#include "desentry/net/network_manager.h"

#include <algorithm>
#include <chrono>
#include <unordered_map>

#include "desentry/common/hex.h"
#include "desentry/common/logger.h"
#include "desentry/common/platform.h"

namespace desentry {

namespace {

bool ParseHostPort(const std::string& s, std::string* host, uint16_t* port) {
  auto pos = s.rfind(':');
  if (pos == std::string::npos) return false;
  *host = s.substr(0, pos);
  try {
    *port = static_cast<uint16_t>(std::stoi(s.substr(pos + 1)));
  } catch (...) {
    return false;
  }
  return true;
}

// How long a peer may go unheard-from before it is considered unreachable
// for placement purposes. Three gossip intervals: long enough that one
// missed round does not re-home data, short enough that a laptop closing its
// lid is noticed within seconds rather than minutes.
int64_t StaleThresholdMs(const NodeConfig& config) {
  return static_cast<int64_t>(config.gossip_interval_ms) * 3 + 5000;
}

}  // namespace

NetworkManager::~NetworkManager() { Stop(); }

Status NetworkManager::Start() {
  NetInit();

  limiter_ = std::make_unique<TokenBucketLimiter>(config_.peer_rate_limit_per_sec,
                                                   config_.peer_rate_burst);
  if (config_.peer_rate_limit_per_sec == 0) {
    DSN_LOG_WARN("network",
                 "peer_rate_limit_per_sec is 0: inbound P2P admission control is disabled. "
                 "Do not run this configuration on an untrusted network.");
  }
  dedup_ = std::make_unique<MessageDedup>();
  broadcast_pool_ = std::make_unique<WorkerPool>(config_.max_peer_threads,
                                                  config_.max_peer_threads * 64);

  PlacementOptions placement_opts;
  placement_opts.replication_factor = config_.replication_factor;
  placement_opts.stale_after_ms = StaleThresholdMs(config_);
  placement_ = std::make_unique<PlacementPolicy>(engine_->identity().node_id(), placement_opts);
  placement_->Rebuild(peer_table_);

  // Lets the engine map an origin node_id back to the public key the
  // handshake proved, so ledger signature verification is real rather than
  // "a signature was present".
  engine_->SetPublicKeyResolver([this](const std::string& node_id) -> std::string {
    PeerInfo info;
    if (!peer_table_.Get(node_id, &info)) return std::string();
    return info.ed25519_pubkey;
  });

  transport_ = std::make_unique<TcpTransport>(&engine_->identity(), config_.p2p_port);
  Status listen_st = transport_->StartListening(
      config_.p2p_bind_addr, [this](const std::string& peer_id, const WireMessage& req) {
        return HandleRequest(peer_id, req);
      });
  if (!listen_st.ok()) return listen_st;

  if (config_.discovery_enabled) {
    UdpAdvertisement advert;
    advert.api_port = config_.api_port;
    advert.hostname = config_.advertise_hostname;
    advert.is_supervisor = config_.supervisor;
    discovery_ = std::make_unique<UdpDiscovery>(&engine_->identity(), config_.p2p_port,
                                                 config_.discovery_port, config_.discovery_interval_ms,
                                                 advert);
    discovery_->Start(&peer_table_);
  }

  for (const std::string& spec : config_.bootstrap_peers) {
    std::string host;
    uint16_t port = 0;
    if (!ParseHostPort(spec, &host, &port)) {
      DSN_LOG_WARN("network", "ignoring malformed bootstrap peer spec: " << spec);
      continue;
    }
    // node_id is unknown until we handshake with this address, so it is keyed
    // synthetically for now. The placement layer explicitly skips
    // "bootstrap#" ids (net/placement.cpp) so a not-yet-identified address
    // can never become a replica target.
    PeerInfo info;
    info.node_id = "bootstrap#" + spec;
    info.host = host;
    info.p2p_port = port;
    info.last_seen_ms = NowMs();
    peer_table_.Upsert(info);
    DSN_LOG_INFO("network", "added bootstrap peer " << spec);
  }

  GossipEngine::Options gossip_opts;
  gossip_opts.interval_ms = config_.gossip_interval_ms;
  gossip_opts.jitter_pct = 25;
  gossip_opts.fanout = 2;
  gossip_ = std::make_unique<GossipEngine>(engine_, &peer_table_, transport_.get(), gossip_opts);
  gossip_->Start();

  engine_->SetLocalWriteHook([this](const std::string& collection, const std::string& key,
                                     const std::string& bytes) {
    BroadcastLocalWrite(collection, key, bytes);
  });

  running_ = true;
  probe_thread_ = std::thread(&NetworkManager::ProbeLoop, this);

  DSN_LOG_INFO("network", "network manager started (node_id=" << engine_->identity().node_id()
                                                              << ", rf=" << config_.replication_factor
                                                              << ", broadcast threads="
                                                              << config_.max_peer_threads << ")");
  return Status::OK();
}

void NetworkManager::Stop() {
  running_ = false;
  if (probe_thread_.joinable()) probe_thread_.join();
  if (gossip_) gossip_->Stop();
  if (discovery_) discovery_->Stop();
  if (broadcast_pool_) broadcast_pool_->Stop();
  if (transport_) transport_->Stop();
}

void NetworkManager::RebuildPlacement() {
  if (placement_) placement_->Rebuild(peer_table_);
}

// ---------------------------------------------------------------------------
// Inbound dispatch
// ---------------------------------------------------------------------------

WireMessage NetworkManager::HandleRequest(const std::string& peer_node_id,
                                           const WireMessage& request) {
  // Admission control first: before decoding, before touching the engine.
  // The peer id is the handshake-proven one, so a flooding peer cannot
  // escape its bucket by claiming a different identity.
  if (limiter_ && !limiter_->Allow(peer_node_id, NowMs())) {
    rate_limited_.fetch_add(1);
    return WireMessage{MessageType::kError, "rate limit exceeded"};
  }

  // Any authenticated inbound contact is useful liveness information, and it
  // works even where UDP broadcast does not.
  PeerInfo seen;
  if (peer_table_.Get(peer_node_id, &seen)) {
    seen.last_seen_ms = NowMs();
    peer_table_.Upsert(seen);
  }

  try {
    switch (request.type) {
      case MessageType::kDigest:
        return HandleDigest(peer_node_id, DigestPayload::Decode(request.payload));
      case MessageType::kOpBroadcast:
        return HandleOpBroadcast(peer_node_id, OpBroadcastPayload::Decode(request.payload));
      case MessageType::kTransitQuery:
        return HandleTransitQuery(peer_node_id);
      case MessageType::kTransitClaim:
        return HandleTransitClaim(peer_node_id, TransitClaimPayload::Decode(request.payload));
      case MessageType::kLedgerDigest:
        return HandleLedgerDigest(peer_node_id, LedgerDigestPayload::Decode(request.payload));
      case MessageType::kPing:
        return WireMessage{MessageType::kPong, engine_->identity().node_id()};
      default:
        return WireMessage{MessageType::kError, "unrecognized message type"};
    }
  } catch (const std::exception& e) {
    // A malformed payload from a peer is untrusted input, not a crash.
    DSN_LOG_WARN("network", "malformed " << MessageTypeName(request.type) << " from " << peer_node_id
                                          << ": " << e.what());
    return WireMessage{MessageType::kError, "malformed payload"};
  }
}

WireMessage NetworkManager::HandleDigest(const std::string& peer_node_id,
                                          const DigestPayload& digest) {
  DeltaResponsePayload response;
  response.collection = digest.collection;

  const Requestor who = Requestor::Peer(peer_node_id);
  if (!engine_->CanRead(digest.collection, who)) {
    // The ACL byte filter. A non-reader gets an empty delta -- no keys, no
    // bytes, not even the shape of what it is missing. It still converges on
    // the ledger's entry *hashes* through kLedgerDigest, which is what lets
    // it verify the chain without being able to read the contents.
    DSN_LOG_DEBUG("network", "withholding " << digest.collection << " bytes from non-reader "
                                             << peer_node_id);
    return WireMessage{MessageType::kDeltaResponse, response.Encode()};
  }

  struct Fingerprint {
    HLCTimestamp top_ts;
    std::string content_hash;
  };
  std::unordered_map<std::string, Fingerprint> remote;
  for (const DigestEntry& e : digest.entries) {
    remote[e.key] = Fingerprint{HLCTimestamp::Decode(e.top_ts_encoded), e.content_hash};
  }

  std::unordered_map<std::string, Fingerprint> local;
  for (const DigestEntryOut& e : engine_->LocalDigest(digest.collection)) {
    local[e.key] = Fingerprint{e.top_ts, e.content_hash};
  }

  // Two copies need reconciling when one is newer OR when they carry the same
  // top timestamp but different bytes. The second case is not hypothetical:
  // merging a peer's write can leave a document whose freshest field came
  // from that peer, so both sides report that peer's timestamp while only one
  // of them holds the merged result. Comparing timestamps alone, neither side
  // offers anything and the divergence is permanent. Merge is idempotent and
  // commutative, so exchanging on an inconclusive comparison is always safe.
  auto differs = [](const Fingerprint& a, const Fingerprint& b) {
    return a.top_ts > b.top_ts || (!(b.top_ts > a.top_ts) && a.content_hash != b.content_hash);
  };

  for (const auto& [key, mine] : local) {
    auto it = remote.find(key);
    if (it == remote.end() || differs(mine, it->second)) {
      auto raw_or = engine_->GetRawEncoded(digest.collection, key);
      if (raw_or.ok()) response.pushed.push_back(DocEntry{key, raw_or.value()});
    }
  }
  for (const auto& [key, theirs] : remote) {
    auto it = local.find(key);
    if (it == local.end() || differs(theirs, it->second)) response.wanted_keys.push_back(key);
  }
  return WireMessage{MessageType::kDeltaResponse, response.Encode()};
}

WireMessage NetworkManager::HandleOpBroadcast(const std::string& peer_node_id,
                                               const OpBroadcastPayload& broadcast) {
  if (dedup_ && !dedup_->NoteAndCheckNew(broadcast.message_id)) {
    duplicates_suppressed_.fetch_add(1);
    return WireMessage{MessageType::kPong, ""};
  }

  const Requestor who = Requestor::Peer(peer_node_id);
  for (const DocEntry& doc : broadcast.docs) {
    Status st = engine_->MergeRemote(broadcast.collection, doc.key, doc.encoded_doc, who);
    if (!st.ok()) {
      DSN_LOG_WARN("network", "merge from " << peer_node_id << " rejected for "
                                             << broadcast.collection << "/" << doc.key << ": "
                                             << st.message());
    }
  }

  // Bounded relay. v1 refused to relay at all, because without dedup a relay
  // is a broadcast storm. With a message id and a TTL, one extra hop cuts
  // convergence latency on a partially-connected mesh and terminates by
  // construction.
  if (broadcast.ttl > 0) {
    OpBroadcastPayload relayed = broadcast;
    relayed.ttl = static_cast<uint8_t>(broadcast.ttl - 1);
    FanOut(relayed, peer_node_id);
  }
  return WireMessage{MessageType::kPong, ""};
}

WireMessage NetworkManager::HandleTransitQuery(const std::string& peer_node_id) {
  TransitResponsePayload response;
  // The requester is the handshake-authenticated peer, so it can only ever
  // ask for bytes held for *itself*. There is no owner field in the query
  // for exactly that reason.
  constexpr size_t kMaxEntriesPerResponse = 256;
  for (const TransitEnvelope& envelope : engine_->PendingTransitFor(peer_node_id)) {
    if (response.entries.size() >= kMaxEntriesPerResponse) {
      response.truncated = true;
      break;
    }
    TransitEntry entry;
    entry.collection = envelope.collection;
    entry.key = envelope.key;
    entry.key_hash = envelope.key_hash;
    entry.encoded_doc = envelope.encoded_doc;
    entry.intent_lsn = envelope.intent_lsn;
    entry.holder_node = envelope.holder_node;
    response.entries.push_back(std::move(entry));
  }
  if (!response.entries.empty()) {
    DSN_LOG_INFO("transit", "serving " << response.entries.size() << " held document(s) to returning "
                                        << peer_node_id);
  }
  return WireMessage{MessageType::kTransitResponse, response.Encode()};
}

WireMessage NetworkManager::HandleTransitClaim(const std::string& peer_node_id,
                                                const TransitClaimPayload& claim) {
  // The claimer is the authenticated peer, not whatever the payload says --
  // otherwise any peer could claim on another's behalf and cause its held
  // bytes to be released.
  size_t recorded = 0;
  for (const std::string& key_hash : claim.key_hashes) {
    Status st = engine_->RecordRemoteClaim(peer_node_id, key_hash);
    if (st.ok()) ++recorded;
  }
  DSN_LOG_INFO("transit", "recorded " << recorded << " claim(s) from " << peer_node_id);
  return WireMessage{MessageType::kPong, ""};
}

WireMessage NetworkManager::HandleLedgerDigest(const std::string& peer_node_id,
                                                const LedgerDigestPayload& digest) {
  LedgerDeltaPayload response;
  const Requestor who = Requestor::Peer(peer_node_id);

  const lsn_t local_tip = engine_->LedgerTip().entry_id;
  if (digest.tip_entry_id >= local_tip) {
    // The peer is at or ahead of us; nothing to send.
    return WireMessage{MessageType::kLedgerDelta, response.Encode()};
  }

  auto entries_or = engine_->LedgerEntries(digest.tip_entry_id + 1, local_tip);
  if (!entries_or.ok()) return WireMessage{MessageType::kError, entries_or.status().message()};

  constexpr size_t kMaxDeltaEntries = 1024;
  for (const WalRecord& rec : entries_or.value()) {
    if (response.entries.size() >= kMaxDeltaEntries) break;
    LedgerEntrySummary summary;
    summary.entry_id = rec.lsn;
    summary.operation = static_cast<uint8_t>(rec.type);
    summary.key_hash = rec.key_hash;
    summary.entry_hash = rec.entry_hash;
    summary.prev_hash = rec.prev_hash;
    summary.origin_node_id = rec.origin_node_id;
    summary.origin_signature = rec.origin_signature;
    summary.hlc_physical_ms = static_cast<int64_t>(rec.hlc.physical_ms);
    summary.hlc_logical = rec.hlc.logical;
    // The hash-only property: a peer that is not a reader of the collection
    // still receives the entry hash, the chain links and the origin
    // signature -- everything it needs to verify the chain -- but neither
    // the collection name nor the key. That is what makes the ledger safe to
    // converge on network-wide while the bytes stay private.
    if (engine_->CanRead(rec.collection, who)) {
      summary.collection = rec.collection;
      summary.key = rec.key;
    } else {
      response.hashes_only = true;
    }
    response.entries.push_back(std::move(summary));
  }
  return WireMessage{MessageType::kLedgerDelta, response.Encode()};
}

// ---------------------------------------------------------------------------
// Outbound
// ---------------------------------------------------------------------------

void NetworkManager::FanOut(const OpBroadcastPayload& payload, const std::string& exclude_node_id) {
  WireMessage msg{MessageType::kOpBroadcast, payload.Encode()};
  // Ranked order matters under a bounded pool: if the queue fills, the peers
  // that get dropped should be the least reliable ones, not whichever the
  // hash map happened to iterate last.
  for (const PeerInfo& peer : peer_table_.Ranked()) {
    if (peer.node_id == exclude_node_id) continue;
    if (peer.is_supervisor) continue;  // supervisors are never on the data path
    if (peer.p2p_port == 0) continue;
    const std::string host = peer.host;
    const uint16_t port = peer.p2p_port;
    const std::string node_id = peer.node_id;
    TcpTransport* transport = transport_.get();
    const bool queued = broadcast_pool_->Submit([this, transport, host, port, node_id, msg]() {
      const int64_t started = MonotonicMs();
      auto result = transport->SendRequest(host, port, msg);
      peer_table_.RecordProbe(node_id, static_cast<double>(MonotonicMs() - started), result.ok());
      if (result.ok()) broadcasts_sent_.fetch_add(1);
    });
    if (!queued) {
      // Dropping an eager push is safe: gossip anti-entropy is the
      // correctness backstop and will carry the write on its next round.
      // It is counted so a saturated fan-out is visible in /_status rather
      // than showing up as mysteriously slow convergence.
      DSN_LOG_DEBUG("network", "broadcast queue full; dropping eager push to " << node_id);
    }
  }
}

void NetworkManager::BroadcastLocalWrite(const std::string& collection, const std::string& key,
                                          const std::string& encoded_doc) {
  OpBroadcastPayload payload;
  payload.collection = collection;
  payload.docs.push_back(DocEntry{key, encoded_doc});
  payload.message_id = MessageDedup::NewMessageId(engine_->identity().node_id());
  payload.ttl = 1;
  // Record our own id so a relay coming back to us is recognised as a
  // duplicate rather than merged a second time.
  if (dedup_) dedup_->NoteAndCheckNew(payload.message_id);

  FanOut(payload, std::string());
  HoldForUnreachableOwners(collection, key, encoded_doc);
}

void NetworkManager::HoldForUnreachableOwners(const std::string& collection, const std::string& key,
                                               const std::string& encoded_doc) {
  if (placement_ == nullptr) return;
  // Transit envelopes themselves are never held for anyone: that would be a
  // recursion with no termination condition.
  if (collection == kTransitCollection) return;

  CollectionMeta meta;
  std::string shard_value;
  if (engine_->storage().catalog().GetCopy(collection, &meta) && !meta.shard_key.empty()) {
    auto raw = engine_->GetDocument(collection, key, engine_->SelfRequestor());
    if (raw.ok() && raw.value().is_object()) {
      const JsonValue* v = raw.value().Find(meta.shard_key);
      if (v != nullptr && v->is_string()) shard_value = v->AsString();
    }
  }

  const PlacementPlan plan = placement_->Place(collection, key, shard_value);
  const int64_t stale_ms = StaleThresholdMs(config_);
  const int64_t now = NowMs();

  // Two sources, because an owner can be absent in two different ways.
  // `displaced_owners` is the important one: a peer that has been away long
  // enough is dropped from the ring entirely, so it never appears in
  // `replicas` again -- and holding bytes only for unreachable *replicas*
  // would mean holding them for nobody, which is how this whole path came to
  // be unreachable in the first place. `replicas` still has to be scanned
  // too, for an owner that is on the ring but has just stopped answering.
  std::vector<std::string> candidates = plan.displaced_owners;
  candidates.insert(candidates.end(), plan.replicas.begin(), plan.replicas.end());

  for (const std::string& replica : candidates) {
    if (replica == engine_->identity().node_id()) continue;
    PeerInfo info;
    if (!peer_table_.Get(replica, &info)) continue;
    const bool unreachable = info.last_seen_ms == 0 || (now - info.last_seen_ms) > stale_ms ||
                             info.state == NodeLifecycleState::kDegraded;
    if (!unreachable) continue;
    // This replica should hold these bytes but cannot be reached. Hold them
    // here and record the intent, so the owner discovers them on return
    // instead of the write simply never arriving.
    Status st = engine_->HoldForOfflineOwner(replica, collection, key, encoded_doc);
    if (!st.ok()) {
      DSN_LOG_WARN("transit", "could not hold bytes for offline owner " << replica << ": "
                                                                        << st.message());
    }
  }
}

NetworkManager::ClaimReport NetworkManager::ClaimPendingTransit() {
  ClaimReport report;
  const std::string self = engine_->identity().node_id();

  for (const PeerInfo& peer : peer_table_.Ranked()) {
    if (peer.p2p_port == 0 || peer.node_id == self) continue;
    ++report.peers_asked;

    auto response = transport_->SendRequest(peer.host, peer.p2p_port,
                                             WireMessage{MessageType::kTransitQuery, ""});
    if (!response.ok() || response.value().type != MessageType::kTransitResponse) {
      ++report.failures;
      continue;
    }

    TransitResponsePayload payload;
    try {
      payload = TransitResponsePayload::Decode(response.value().payload);
    } catch (const std::exception&) {
      ++report.failures;
      continue;
    }
    if (payload.entries.empty()) continue;

    TransitClaimPayload claim;
    claim.claimer_node = self;
    for (const TransitEntry& entry : payload.entries) {
      // Verify the bytes actually hash to the key they are labelled with:
      // a holder that returned the wrong document for a key_hash would
      // otherwise be applied verbatim.
      if (LedgerKeyHash(entry.collection, entry.key) != entry.key_hash) {
        DSN_LOG_WARN("transit", "holder " << peer.node_id << " returned an entry whose key does not "
                                           << "match its key_hash; ignoring");
        ++report.failures;
        continue;
      }
      Status st = engine_->ApplyClaimedTransit(entry.collection, entry.key, entry.encoded_doc);
      if (!st.ok()) {
        DSN_LOG_WARN("transit", "could not apply held document " << entry.collection << "/"
                                                                  << entry.key << ": " << st.message());
        ++report.failures;
        continue;
      }
      claim.key_hashes.push_back(entry.key_hash);
      ++report.documents_claimed;
    }

    if (!claim.key_hashes.empty()) {
      // Telling the holder is what lets it record TRANSIT_CLAIMED and
      // eventually release the bytes. A failure here is not fatal: the
      // envelope simply expires on its TTL instead.
      transport_->SendRequest(peer.host, peer.p2p_port,
                              WireMessage{MessageType::kTransitClaim, claim.Encode()});
    }
  }

  if (report.documents_claimed > 0) {
    DSN_LOG_INFO("transit", "claimed " << report.documents_claimed << " held document(s) from "
                                        << report.peers_asked << " peer(s)");
  }
  return report;
}

std::vector<ReplicaTip> NetworkManager::CollectReplicaTips() {
  std::vector<ReplicaTip> tips;
  for (const PeerInfo& peer : peer_table_.Ranked()) {
    if (peer.p2p_port == 0) continue;
    LedgerDigestPayload digest;
    digest.tip_entry_id = -1;  // "tell me everything you have"
    auto response = transport_->SendRequest(peer.host, peer.p2p_port,
                                             WireMessage{MessageType::kLedgerDigest, digest.Encode()});
    if (!response.ok() || response.value().type != MessageType::kLedgerDelta) continue;

    LedgerDeltaPayload delta;
    try {
      delta = LedgerDeltaPayload::Decode(response.value().payload);
    } catch (const std::exception&) {
      continue;
    }
    if (delta.entries.empty()) continue;

    ReplicaTip tip;
    tip.node_id = peer.node_id;
    tip.entry_id = delta.entries.back().entry_id;
    tip.entry_hash = delta.entries.back().entry_hash;
    // A peer's *self-reported* verification result is not evidence on its
    // own; what we can check locally is that the chain segment it sent links
    // correctly and that its origin signatures verify against the public keys
    // the handshake proved. That is a stronger check than trusting a boolean.
    tip.self_verified = true;
    std::string expected_prev;
    for (const LedgerEntrySummary& entry : delta.entries) {
      if (!expected_prev.empty() && entry.prev_hash != expected_prev) {
        tip.self_verified = false;
        break;
      }
      expected_prev = entry.entry_hash;
    }
    tip.signature_valid = engine_->VerifyPeerTip(peer.node_id, tip.entry_id, tip.entry_hash,
                                                  tip.signature);
    tips.push_back(std::move(tip));
  }
  return tips;
}

void NetworkManager::ProbeLoop() {
  // Periodic liveness + capacity probing. Deliberately separate from gossip:
  // a gossip round only touches a couple of peers, while fitness needs a view
  // of everyone, and a probe is far cheaper than a digest exchange.
  while (running_) {
    const int64_t deadline = MonotonicMs() + config_.discovery_interval_ms * 2;
    for (const PeerInfo& peer : peer_table_.List()) {
      if (!running_) break;
      if (peer.p2p_port == 0) continue;
      const int64_t started = MonotonicMs();
      auto response = transport_->SendRequest(peer.host, peer.p2p_port,
                                               WireMessage{MessageType::kPing, ""});
      const double rtt = static_cast<double>(MonotonicMs() - started);
      const bool ok = response.ok() && response.value().type == MessageType::kPong;
      peer_table_.RecordProbe(peer.node_id, rtt, ok);
      if (ok) {
        PeerInfo seen = peer;
        seen.last_seen_ms = NowMs();
        if (seen.state == NodeLifecycleState::kDegraded) seen.state = NodeLifecycleState::kRunning;
        peer_table_.Upsert(seen);
      } else if (peer.state == NodeLifecycleState::kRunning) {
        peer_table_.SetState(peer.node_id, NodeLifecycleState::kDegraded);
      }
    }
    RebuildPlacement();

    while (running_ && MonotonicMs() < deadline) SleepMs(100);
  }
}

NetworkManager::BroadcastStats NetworkManager::broadcast_stats() const {
  BroadcastStats stats;
  stats.sent = broadcasts_sent_.load();
  stats.duplicates_suppressed = duplicates_suppressed_.load();
  stats.rate_limited = rate_limited_.load();
  if (broadcast_pool_) {
    stats.dropped = broadcast_pool_->dropped();
    stats.queued = broadcast_pool_->queued();
  }
  return stats;
}

}  // namespace desentry
