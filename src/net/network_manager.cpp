#include "desentry/net/network_manager.h"

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

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
  // Liveness probes get their own bounded pool: a write burst must never
  // starve failure detection, and a mesh full of silent peers must never
  // starve eager broadcast. Four threads is plenty -- probes are short
  // request/response exchanges, and the pool only bounds concurrency, not
  // the peer count (overflow drops with a counter, counted in ProbeStats).
probe_pool_ = std::make_unique<WorkerPool>(4, 64);
  receipt_tracker_ = std::make_unique<ReceiptTracker>();

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

  engine_->SetReachabilityProvider([this]() -> bool {
    // Node is reachable if it has at least one peer with an established
    // P2P connection (handshake complete, port known, seen recently).
    const int64_t stale_ms = StaleThresholdMs(config_);
    const int64_t now = NowMs();
    for (const auto& peer : peer_table_.List()) {
      if (peer.p2p_port == 0) continue;
      if (peer.node_id == engine_->identity().node_id()) continue;
      if (now - peer.last_seen_ms > stale_ms) continue;
      return true;  // at least one reachable peer
    }
    return false;  // no reachable peers = isolated
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
  if (probe_pool_) probe_pool_->Stop();
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
        // Old peers send an empty query (no cursor); Decode maps that to
        // offset 0, so they keep working unchanged.
        return HandleTransitQuery(peer_node_id, TransitQueryPayload::Decode(request.payload));
      case MessageType::kTransitClaim:
        return HandleTransitClaim(peer_node_id, TransitClaimPayload::Decode(request.payload));
      case MessageType::kLedgerDigest:
        return HandleLedgerDigest(peer_node_id, LedgerDigestPayload::Decode(request.payload));
      case MessageType::kHeartbeat:
        return HandleHeartbeat(peer_node_id, HeartbeatPayload::Decode(request.payload));
      case MessageType::kTransitHeld:
        // A holder confirms it has stored bytes for an offline owner.
        // The holder is the authenticated peer; we just log it.
        return HandleTransitHeld(peer_node_id, TransitHeldPayload::Decode(request.payload));
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

  // Only the original eager broadcast (ttl == 1) expects a signed merge receipt.
  // Relays (ttl == 0) and gossip fulfilments (ttl == 0) do not send receipts.
  if (broadcast.ttl == 1 && !broadcast.docs.empty()) {
    DSN_LOG_DEBUG("network", "HandleOpBroadcast: sending receipt for message_id=" << broadcast.message_id
        << " collection=" << broadcast.collection << " key=" << broadcast.docs[0].key);
    MergeReceipt receipt;
    receipt.message_id = broadcast.message_id;
    receipt.key_hash = LedgerKeyHash(broadcast.collection, broadcast.docs[0].key);
    receipt.applier_node = engine_->identity().node_id();
    receipt.applied_lsn = engine_->LedgerTip().entry_id;
    // Sign: Ed25519 over (message_id || key_hash || applied_lsn)
    const std::string message = receipt.message_id + receipt.key_hash + std::to_string(receipt.applied_lsn);
    receipt.signature = engine_->identity().Sign(message);
    DSN_LOG_DEBUG("network", "HandleOpBroadcast: receipt encoded, size=" << receipt.Encode().size());
    return WireMessage{MessageType::kPong, receipt.Encode()};
  }
  DSN_LOG_DEBUG("network", "HandleOpBroadcast: no receipt (ttl=" << static_cast<int>(broadcast.ttl) << ")");
  return WireMessage{MessageType::kPong, ""};
}

WireMessage NetworkManager::HandleTransitQuery(const std::string& peer_node_id,
                                                const TransitQueryPayload& query) {
  TransitResponsePayload response;
  // The requester is the handshake-authenticated peer, so it can only ever
  // ask for bytes held for *itself*. There is no owner field in the query
  // for exactly that reason.
  constexpr size_t kMaxEntriesPerResponse = 256;
  // Stable order (intent_lsn) so the offset cursor stays meaningful across
  // calls: page N always resumes where page N-1 stopped, even as new
  // envelopes arrive (they append at higher LSNs, behind the cursor).
  std::vector<TransitEnvelope> pending = engine_->PendingTransitFor(peer_node_id);
  std::sort(pending.begin(), pending.end(),
            [](const TransitEnvelope& a, const TransitEnvelope& b) {
              return a.intent_lsn < b.intent_lsn;
            });
  size_t skipped = 0;
  for (const TransitEnvelope& envelope : pending) {
    if (skipped < query.offset) {
      ++skipped;
      continue;
    }
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
    entry.doc_size_bytes = envelope.doc_size_bytes;
    entry.chunk_index = envelope.chunk_index;
    entry.chunk_total = envelope.chunk_total;
    response.entries.push_back(std::move(entry));
  }
  response.next_offset = query.offset + response.entries.size();
  if (!response.entries.empty()) {
    DSN_LOG_INFO("transit", "serving " << response.entries.size() << " held document(s) to returning "
                                        << peer_node_id << " (offset " << query.offset << ")");
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

WireMessage NetworkManager::HandleTransitHeld(const std::string& peer_node_id,
                                               const TransitHeldPayload& held) {
  // The holder (peer_node_id) confirms it has stored bytes for an offline
  // owner. We verify the signature and log it. The held-ack is primarily
  // for the writer's durability tracking -- the receipt_tracker will pick
  // it up if it matches a pending message_id. Here we just log.
  DSN_LOG_INFO("transit", "held-ack from " << peer_node_id
                                            << " for key_hash " << HexEncode(held.key_hash)
                                            << " intent_lsn=" << held.intent_lsn);
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
    summary.transit_holder = rec.transit_holder;
    summary.transit_size_bytes = rec.transit_size_bytes;
    summary.transit_chunk_index = rec.transit_chunk_index;
    summary.transit_chunk_total = rec.transit_chunk_total;
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

HeartbeatPayload NetworkManager::OwnHeartbeat() const {
  HeartbeatPayload hb;
  hb.node_id = engine_->identity().node_id();
  hb.ledger_tip_entry_id = engine_->LedgerTip().entry_id;
  const StorageEngine::QuotaStatus quota = engine_->Quota();
  hb.quota_limited = quota.limit_bytes > 0;
  hb.free_quota_mb =
      quota.limit_bytes > quota.used_bytes ? (quota.limit_bytes - quota.used_bytes) / (1024 * 1024) : 0;
  hb.transit_bytes_held = engine_->transit().BytesHeld();
  hb.transit_budget_bytes = config_.quota_split.TransitBytes(config_.quota_mb);
  // Self-assessed servability, not lifecycle: a node serving heartbeats is
  // alive; it reports degraded only when its own quota says writes would
  // fail. Lifecycle transitions stay supervisor-driven.
  hb.lifecycle_state = static_cast<uint8_t>(
      quota.over_limit ? NodeLifecycleState::kDegraded : NodeLifecycleState::kRunning);
  return hb;
}

WireMessage NetworkManager::HandleHeartbeat(const std::string& peer_node_id,
                                            const HeartbeatPayload& request) {
  // One round trip updates both sides: fold the requester's figures in,
  // answer with our own. Advisory only -- see the header comment.
  peer_table_.RecordReport(peer_node_id, request.ledger_tip_entry_id, request.free_quota_mb,
                           request.quota_limited, request.transit_bytes_held,
                           request.transit_budget_bytes);
  return WireMessage{MessageType::kHeartbeat, OwnHeartbeat().Encode()};
}

void NetworkManager::ProbePeer(PeerInfo peer) {
  const std::string self = engine_->identity().node_id();
  if (peer.p2p_port == 0 || peer.node_id == self) return;
  const int64_t threshold =
      config_.liveness_threshold_ms > 0 ? static_cast<int64_t>(config_.liveness_threshold_ms) : 5000;

  const int64_t started = MonotonicMs();
  auto response = transport_->SendRequest(peer.host, peer.p2p_port,
                                          WireMessage{MessageType::kHeartbeat, OwnHeartbeat().Encode()});

  // Mixed-version fallback: a v1 peer answers kHeartbeat with kError (no
  // such message type). It is alive but speaks the old protocol -- fall
  // back to kPing rather than condemning it for that. Only old peers pay
  // the second round trip.
  std::string proven_id;
  bool legacy = false;
  if (response.ok() && response.value().type == MessageType::kError) {
    legacy = true;
    response = transport_->SendRequest(peer.host, peer.p2p_port, WireMessage{MessageType::kPing, ""});
  }
  const double rtt = static_cast<double>(MonotonicMs() - started);
  const bool ok = response.ok() && (response.value().type == MessageType::kHeartbeat ||
                                    (legacy && response.value().type == MessageType::kPong));
  peer_table_.RecordProbe(peer.node_id, rtt, ok);
  if (!ok) {
    // Slow condemn, matching the graded tiers: a single failed probe makes
    // a peer suspect (its score already drops via success_rate); only a
    // dead reading -- silent past 3x threshold or persistently failing --
    // flips lifecycle state, which is what excludes it from placement.
    PeerInfo current;
    if (peer_table_.Get(peer.node_id, &current) &&
        current.Suspicion(NowMs(), threshold) == PeerSuspicion::kDead) {
      peer_table_.SetState(peer.node_id, NodeLifecycleState::kDegraded);
    }
    return;
  }

  std::string key = peer.node_id;
  if (!legacy) {
    try {
      const HeartbeatPayload hb = HeartbeatPayload::Decode(response.value().payload);
      peer_table_.RecordReport(peer.node_id, hb.ledger_tip_entry_id, hb.free_quota_mb,
                               hb.quota_limited, hb.transit_bytes_held, hb.transit_budget_bytes);
      proven_id = hb.node_id;
    } catch (const std::exception&) {
      // Reachable but garbled: liveness counts (last_seen_ms below), the
      // figures don't. A peer whose heartbeats never decode keeps its old
      // fitness numbers, which is the honest reading of "no new data".
    }
  } else {
    // The kPing response carries the responder's handshake-proven node_id,
    // so a successful legacy probe is also an identification: retire a
    // `bootstrap#host:port` placeholder under the real identity. Trusted
    // because it arrived over the authenticated channel (same reasoning as
    // the old ping loop).
    proven_id = response.value().payload;
  }
  if (!proven_id.empty() && proven_id != key) {
    if (peer_table_.AdoptIdentity(key, proven_id)) key = proven_id;
  }
  PeerInfo seen;
  if (!peer_table_.Get(key, &seen)) seen = peer;
  seen.node_id = key;
  seen.last_seen_ms = NowMs();
  // Fast recovery, slow condemn: one answered probe clears a degraded
  // marking (the peer proved it is back); condemning takes sustained
  // failure via the suspicion tiers above.
  if (seen.state == NodeLifecycleState::kDegraded) seen.state = NodeLifecycleState::kRunning;
  peer_table_.Upsert(seen);
}

// ---------------------------------------------------------------------------
// Outbound
// ---------------------------------------------------------------------------

void NetworkManager::FanOut(const OpBroadcastPayload& payload, const std::string& exclude_node_id) {
  WireMessage msg{MessageType::kOpBroadcast, payload.Encode()};
  DSN_LOG_DEBUG("network", "FanOut called: message_id=" << payload.message_id 
      << " collection=" << payload.collection << " ttl=" << static_cast<int>(payload.ttl)
      << " docs=" << payload.docs.size() << " peers=" << peer_table_.Ranked().size());
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
    const bool queued = broadcast_pool_->Submit([this, transport, host, port, node_id, msg, payload]() {
      const int64_t started = MonotonicMs();
      auto result = transport->SendRequest(host, port, msg);
      peer_table_.RecordProbe(node_id, static_cast<double>(MonotonicMs() - started), result.ok());
      if (result.ok()) {
        broadcasts_sent_.fetch_add(1);
        // Check for a merge receipt in the kPong response.
        if (result.value().type == MessageType::kPong) {
          try {
            const MergeReceipt receipt = MergeReceipt::Decode(result.value().payload);
            // Verify the signature matches the applier's public key.
            const std::string public_key = ResolvePublicKey(receipt.applier_node);
            if (!public_key.empty() && NodeIdentity::DeriveNodeId(public_key) == receipt.applier_node) {
              const std::string message = receipt.message_id + receipt.key_hash +
                                          std::to_string(receipt.applied_lsn);
              if (NodeIdentity::Verify(public_key, message, receipt.signature)) {
                if (receipt_tracker_) receipt_tracker_->NoteReceipt(receipt);
              }
            }
          } catch (const std::exception&) {
            // Not a receipt (e.g., legacy kPong with node_id string), ignore.
          }
        }
      }
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
  HoldForUnreachableOwners(collection, key, encoded_doc, payload.message_id);
}

void NetworkManager::HoldForUnreachableOwners(const std::string& collection, const std::string& key,
                                                const std::string& encoded_doc, const std::string& message_id) {
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

  const std::string self = engine_->identity().node_id();
  const uint32_t max_holders =
      config_.transit_max_holders > 0 ? config_.transit_max_holders : 3;
  const int64_t threshold = config_.liveness_threshold_ms > 0
                                ? static_cast<int64_t>(config_.liveness_threshold_ms)
                                : 5000;

  // Holder candidates: this node plus every dialable peer. Mirrors the
  // placement ring's exclusions (no supervisors, no reclaimed nodes, no
  // un-handshaked placeholders) and additionally skips dead-suspicion peers
  // -- a holder that cannot be reached cannot serve the bytes back. Ranked
  // deterministically per (owner, key) by SelectTransitHolders, so every
  // replica independently picks the same top-H without a coordination round.
  std::vector<std::string> holder_candidates;
  holder_candidates.push_back(self);
  for (const PeerInfo& peer : peer_table_.List()) {
    if (peer.node_id == self || peer.p2p_port == 0) continue;
    if (peer.node_id.rfind("bootstrap#", 0) == 0) continue;
    if (peer.is_supervisor) continue;
    if (peer.state == NodeLifecycleState::kReclaimed) continue;
    if (peer.Suspicion(now, threshold) == PeerSuspicion::kDead) continue;
    holder_candidates.push_back(peer.node_id);
  }

  const std::string doc_key_hash = LedgerKeyHash(collection, key);
  const uint32_t chunk_total =
      TransitChunkCount(encoded_doc.size(), config_.transit_chunk_bytes);

  for (const std::string& replica : candidates) {
    if (replica == self) continue;
    PeerInfo info;
    if (!peer_table_.Get(replica, &info)) continue;
    const bool unreachable = info.last_seen_ms == 0 || (now - info.last_seen_ms) > stale_ms ||
                             info.state == NodeLifecycleState::kDegraded;
    if (!unreachable) continue;
    // This replica should hold these bytes but cannot be reached. Hold the
    // chunks this node is assigned by deterministic rank -- at most
    // max_holders copies mesh-wide instead of one per online replica -- and
    // record the intent, so the owner discovers them on return instead of
    // the write simply never arriving. Chunks rotate across holders, so a
    // striped document spreads rather than stacking on the same top-H.
    std::vector<uint32_t> my_chunks;
    for (uint32_t c = 0; c < chunk_total; ++c) {
      const std::vector<std::string> holders = SelectTransitHolders(
          replica, doc_key_hash, holder_candidates, max_holders, c);
      if (std::find(holders.begin(), holders.end(), self) != holders.end()) {
        my_chunks.push_back(c);
      }
    }
    if (my_chunks.empty()) continue;  // not in any chunk's holder set
    Status st = engine_->HoldForOfflineOwner(replica, collection, key, encoded_doc, &my_chunks);
    if (!st.ok()) {
      DSN_LOG_WARN("transit", "could not hold bytes for offline owner " << replica << ": "
                                                                        << st.message());
      continue;
    }
    // Send held-ack to the writer (the offline owner) so they can count it
    // toward their durability target. The intent LSNs were just appended
    // with our holder_node; fetch them and send a signed ack per chunk.
    auto intents = engine_->TransitIntentsForSelf();
    for (const auto& intent : intents) {
      // TransitIntentsForSelf already filters to intents where collection == self.
      // Check if this intent is for one of our chunks.
      bool is_ours = false;
      for (uint32_t c : my_chunks) {
        if (intent.chunk_index == c) { is_ours = true; break; }
      }
      if (!is_ours) continue;
      // Fetch the envelope to get the original message_id.
      auto env_or = engine_->transit().Lookup(replica, intent.key_hash);
      if (!env_or.ok()) continue;
      const TransitEnvelope& env = env_or.value();
      // Send held-ack to the writer (replica is the owner_node)
      PeerInfo holder_info;
      if (!peer_table_.Get(replica, &holder_info) || holder_info.p2p_port == 0) continue;
      TransitHeldPayload held;
      held.message_id = env.message_id;
      held.key_hash = intent.key_hash;
      held.holder_node = self;
      held.intent_lsn = intent.intent_lsn;
      // Sign the held-ack
      const std::string message = held.message_id + held.key_hash + std::to_string(held.intent_lsn);
      held.signature = engine_->identity().Sign(message);
      transport_->SendRequest(holder_info.host, holder_info.p2p_port,
                              WireMessage{MessageType::kTransitHeld, held.Encode()});
    }
  }
}

NetworkManager::ClaimReport NetworkManager::ClaimPendingTransit() {
  ClaimReport report;
  const std::string self = engine_->identity().node_id();

  // Phase 1: targeted claim from locally-synced TRANSIT_INTENTs.
  // The ledger entries naming this node as owner tell us exactly which
  // holders have bytes for us and which chunks they hold. We query them
  // directly instead of polling all peers.
  const std::vector<NodeEngine::TransitIntent> local_intents = engine_->TransitIntentsForSelf();
  if (!local_intents.empty()) {
    DSN_LOG_INFO("transit", "targeted claim: found " << local_intents.size()
                                                     << " local intent(s) naming us as owner");
    // Group intents by holder to make one query per holder.
    std::map<std::string, std::vector<NodeEngine::TransitIntent>> by_holder;
    for (const auto& intent : local_intents) {
      if (!intent.holder_node.empty()) by_holder[intent.holder_node].push_back(intent);
    }
    for (const auto& [holder, intents] : by_holder) {
      if (holder == self) continue;  // can't hold for self
      PeerInfo holder_info;
      if (!peer_table_.Get(holder, &holder_info)) continue;
      if (holder_info.p2p_port == 0) continue;

      // Build a targeted query for just these chunks. The holder's
      // TransitQuery supports offset, but since we know exactly which
      // intents we want, we can ask for them in batches.
      uint64_t offset = 0;
      while (true) {
        TransitQueryPayload query;
        query.offset = offset;
        auto response = transport_->SendRequest(holder_info.host, holder_info.p2p_port,
                                                WireMessage{MessageType::kTransitQuery, query.Encode()});
        ++report.peers_asked;
        if (!response.ok() || response.value().type != MessageType::kTransitResponse) {
          ++report.failures;
          break;
        }
        TransitResponsePayload payload;
        try {
          payload = TransitResponsePayload::Decode(response.value().payload);
        } catch (const std::exception&) {
          ++report.failures;
          break;
        }
        if (payload.entries.empty()) break;

        TransitClaimPayload claim;
        claim.claimer_node = self;
        bool any = false;
        for (const TransitEntry& entry : payload.entries) {
          if (LedgerKeyHash(entry.collection, entry.key) != entry.key_hash) {
            DSN_LOG_WARN("transit", "holder " << holder << " returned an entry whose key does not "
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
          any = true;
        }
        if (any) {
          transport_->SendRequest(holder_info.host, holder_info.p2p_port,
                                  WireMessage{MessageType::kTransitClaim, claim.Encode()});
        }
        if (!payload.truncated) break;
        offset = payload.next_offset;
      }
    }
  }

  // Phase 2: fallback ask-everyone sweep for any holders not captured
  // by local intents (e.g., intents from a peer we haven't synced with
  // yet, or a new holder since our last gossip round).
  for (const PeerInfo& peer : peer_table_.Ranked()) {
    if (peer.p2p_port == 0 || peer.node_id == self) continue;
    // Skip holders we already queried in Phase 1.
    if (!local_intents.empty()) {
      bool skip = false;
      for (const auto& intent : local_intents) {
        if (intent.holder_node == peer.node_id) { skip = true; break; }
      }
      if (skip) continue;
    }
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
  // Decoupled liveness, in two lanes. Deliberately separate from gossip: a
  // gossip round only touches a couple of peers, while fitness needs a view
  // of everyone, and a heartbeat is far cheaper than a digest exchange.
  //
  //   * **Fast lane (passive-first).** The loop thread only compares
  //     timestamps; an RPC is spent solely on peers with nothing heard from
  //     in the last liveness_threshold_ms. Gossip, discovery and handshakes
  //     all refresh last_seen_ms for free, so a healthy mesh costs almost no
  //     active probes.
  //   * **Slow lane (measured sweep).** Every fitness_probe_interval_ms each
  //     dialable peer gets a full timed heartbeat, refreshing RTT even for
  //     chatty peers whose last_seen_ms is fresh from passive traffic.
  //
  // All RPCs run on the bounded probe pool -- the loop thread never blocks
  // on a peer, so one hung node costs a pool slot instead of stalling
  // placement rebuilds, outbox flushes and every other peer's liveness.
  const int64_t threshold = config_.liveness_threshold_ms > 0
                                ? static_cast<int64_t>(config_.liveness_threshold_ms)
                                : 5000;
  const int64_t sweep_interval = config_.fitness_probe_interval_ms > 0
                                     ? static_cast<int64_t>(config_.fitness_probe_interval_ms)
                                     : 20000;
  // Check often enough to notice a silence promptly, seldom enough that the
  // check itself is noise: a quarter of the threshold, clamped.
  const uint32_t fast_tick_ms =
      static_cast<uint32_t>(std::max<int64_t>(100, std::min<int64_t>(threshold / 4, 2000)));
  const int64_t stale_ms = StaleThresholdMs(config_);
  const std::string self = engine_->identity().node_id();

  int64_t last_slow_sweep = 0;
  int64_t last_outbox_flush = 0;
  while (running_) {
    const int64_t now = NowMs();
    std::unordered_set<std::string> submitted;

    auto submit_probe = [this, &submitted](const PeerInfo& peer) {
      if (!running_ || peer.p2p_port == 0) return;
      if (!submitted.insert(peer.node_id).second) return;  // already queued this tick
      const bool queued = probe_pool_->Submit([this, peer]() { ProbePeer(peer); });
      if (!queued) {
        // Dropping a probe is safe: the peer stays at its current suspicion
        // tier and is retried next tick. Counted in ProbeStats so a
        // saturated pool reads as pool pressure, never as peer failure.
        DSN_LOG_DEBUG("network", "probe pool full; skipping probe of " << peer.node_id);
      }
    };

    bool had_reachable_peer = false;
    for (const PeerInfo& peer : peer_table_.List()) {
      if (!running_) break;
      if (peer.p2p_port == 0 || peer.node_id == self) continue;
      if (peer.last_seen_ms != 0 && now - peer.last_seen_ms <= stale_ms) {
        had_reachable_peer = true;
      }
      // Fast lane: only spend an RPC on silence. Fresh peers cost nothing.
      if (peer.last_seen_ms == 0 || now - peer.last_seen_ms > threshold) {
        submit_probe(peer);
      }
    }

    // Slow lane: full measured sweep at the relaxed cadence, skipping peers
    // the fast lane already queued this tick.
    if (now - last_slow_sweep >= sweep_interval) {
      last_slow_sweep = now;
      for (const PeerInfo& peer : peer_table_.List()) {
        if (!running_) break;
        if (peer.p2p_port == 0 || peer.node_id == self) continue;
        submit_probe(peer);
      }
    }
    RebuildPlacement();

    // Periodically flush the outbox when we have reachable peers.
    if (had_reachable_peer && now - last_outbox_flush >= 30000) {  // every 30s
      const size_t replayed = engine_->FlushOutbox();
      if (replayed > 0) {
        DSN_LOG_INFO("outbox", "periodic flush replayed " << replayed << " staged write(s)");
      }
      last_outbox_flush = now;
    }

    const int64_t deadline = MonotonicMs() + fast_tick_ms;
    while (running_ && MonotonicMs() < deadline) SleepMs(100);
  }
}

std::string NetworkManager::ResolvePublicKey(const std::string& node_id) const {
  PeerInfo info;
  if (!peer_table_.Get(node_id, &info)) return std::string();
  return info.ed25519_pubkey;
}

NetworkManager::ProbeStats NetworkManager::probe_stats() const {
  ProbeStats stats;
  if (probe_pool_) {
    stats.completed = probe_pool_->completed();
    stats.dropped = probe_pool_->dropped();
    stats.queued = probe_pool_->queued();
  }
  return stats;
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
