#include "desentry/net/network_manager.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "desentry/common/hex.h"
#include "desentry/common/logger.h"
#include "desentry/common/platform.h"
#include "desentry/net/identity.h"
#include "desentry/security/at_rest.h"
#include "desentry/security/crypto.h"

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

// Verifies one served transit entry (C-3 fix). The old check recomputed
// LedgerKeyHash from the same attacker-supplied (collection, key) and
// compared it to the attacker-supplied key_hash -- self-consistent by
// construction for any attacker, saying nothing about encoded_doc. This
// checks what actually binds bytes to intent:
//   1. key_hash matches (collection, key) -- keeps the cheap mislabel filter;
//   2. content_hash == SHA-256(encoded_doc) -- the bytes are what the holder
//      attested to, not arbitrary substitution;
//   3. holder_sig verifies under the responding holder's handshake-proven
//      key -- a Sybil that was never designated a holder cannot mint it.
// Entries without an integrity tail (pre-fix holders) are rejected.
bool VerifyTransitEntry(const TransitEntry& entry, const std::string& holder_node,
                        const std::string& holder_pubkey) {
  if (LedgerKeyHash(entry.collection, entry.key) != entry.key_hash &&
      TransitChunkKeyHash(LedgerKeyHash(entry.collection, entry.key), entry.chunk_index) !=
          entry.key_hash) {
    return false;
  }
  if (entry.content_hash.empty() || entry.holder_sig.empty()) return false;
  if (crypto::Sha256(entry.encoded_doc) != entry.content_hash) return false;
  if (holder_pubkey.empty()) return false;
  const std::string msg =
      TransitAttestMessage(entry.key_hash, entry.content_hash, entry.chunk_index, entry.chunk_total);
  return NodeIdentity::Verify(holder_pubkey, msg, entry.holder_sig);
}

}  // namespace

NetworkManager::~NetworkManager() { Stop(); }

namespace {

// Decodes the cluster-membership secret from the environment. Accepted forms:
// 64 hex chars (32 bytes) or any non-empty string used as key material
// directly. Empty/missing = open mesh. Never logged.
bool DecodeClusterSecret(const char* env, std::string* out) {
  if (env == nullptr || *env == '\0') return false;
  const std::string raw(env);
  auto hexval = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  if (raw.size() == 64) {
    std::string bytes;
    bytes.reserve(32);
    for (size_t i = 0; i < 64; i += 2) {
      int hi = hexval(raw[i]);
      int lo = hexval(raw[i + 1]);
      if (hi < 0 || lo < 0) return false;
      bytes.push_back(static_cast<char>((hi << 4) | lo));
    }
    *out = std::move(bytes);
    return true;
  }
  *out = raw;
  return true;
}

}  // namespace

Status NetworkManager::Start() {
  NetInit();

  // Cluster membership: a shared secret (never on disk, never logged) that
  // authenticates HELLOs and discovery beacons. Without it the mesh is open:
  // anyone on the LAN can join, handshake, and gossip. With it, outsiders
  // fail the handshake and their beacons are ignored. Mixed rollout is
  // fail-open per direction (an unconfigured node accepts everything), so
  // configure it on every node to actually close the mesh.
  std::string cluster_secret;
  if (DecodeClusterSecret(std::getenv("DESENTRY_CLUSTER_SECRET"), &cluster_secret)) {
    cluster_secret_ = cluster_secret;
    at_rest::Zeroize(cluster_secret);
    DSN_LOG_INFO("network", "cluster membership required (secret configured)");
  } else {
    DSN_LOG_WARN("network", "no cluster secret: mesh is OPEN, any LAN peer may join. "
                            "Set DESENTRY_CLUSTER_SECRET (the desktop sidecar does) to close it.");
  }

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
  receipt_tracker_ = &engine_->receipt_tracker();

  PlacementOptions placement_opts;
  placement_opts.replication_factor = config_.replication_factor;
  placement_opts.stale_after_ms = StaleThresholdMs(config_);
  placement_opts.is_supervisor = config_.supervisor;
  placement_ = std::make_unique<PlacementPolicy>(engine_->identity().node_id(), placement_opts);
  placement_->Rebuild(peer_table_);

  // Lets the engine map an origin node_id back to the public key the
  // handshake proved, so ledger signature verification is real rather than
  // "a signature was present".
  engine_->SetPublicKeyResolver([this](const std::string& node_id) -> std::string {
    PeerInfo info;
    if (!peer_table_.Get(node_id, &info)) return std::string();
    // Only handshake-proven keys are trust anchors (C-2): a discovery-only
    // entry must never verify ledger signatures.
    if (!info.handshake_proven) return std::string();
    return info.ed25519_pubkey;
  });

  transport_ = std::make_unique<TcpTransport>(&engine_->identity(), config_.p2p_port);
  transport_->SetClusterSecret(cluster_secret_);
  transport_->SetHandshakeCallback([this](const std::string& node_id, const std::string& pubkey,
                                          uint16_t p2p_port) {
    // Both dial directions land here; the HELLO already proved
    // node_id == SHA-256(pubkey), so this binding is trustworthy.
    peer_table_.MarkHandshakeProven(node_id, pubkey, "", p2p_port);
  });
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
    discovery_->SetClusterSecret(cluster_secret_);
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
    peer_table_.Upsert(info, PeerSource::kBootstrap);
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
  at_rest::Zeroize(cluster_secret_);
  cluster_secret_.clear();
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
    // Handshake-proven id: liveness refresh only; Upsert gates the rest.
    peer_table_.Upsert(seen, PeerSource::kHandshake);
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
  // construction. The TTL is clamped to our own maximum (H-9 fix): a sender
  // claiming ttl=255 must not buy a 255-hop storm at our expense.
  constexpr uint8_t kMaxRelayTtl = 3;
  if (broadcast.ttl > 0) {
    OpBroadcastPayload relayed = broadcast;
    if (relayed.ttl > kMaxRelayTtl) relayed.ttl = kMaxRelayTtl;
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
    // Sign (M-7 fix): domain-separated so a held-ack signature is not
    // replayable as a merge receipt or vice versa.
    const std::string message = std::string("DSN-RECEIPT-v1") + receipt.message_id + receipt.key_hash +
                                std::to_string(receipt.applied_lsn);
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
    entry.content_hash = envelope.content_hash;
    entry.holder_sig = envelope.holder_sig;
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
    if (rec.type == WalRecordType::kAcl) {
      // Replicated ACLs (H-1) are metadata, not user bytes: the collection
      // name and the signed envelope go to every peer so confidentiality
      // converges along with the data. The envelope verifies without any
      // other record content (owner self-attestation).
      summary.collection = rec.collection;
      summary.acl_json = rec.document_bytes;
    }
    response.entries.push_back(std::move(summary));
  }
  // Attest the delta's tip (C-6): the collector verifies this against our
  // handshake-proven key. Signs entries.back (which may lag our current tip
  // under truncation), never an unrelated height.
  if (!response.entries.empty()) {
    const LedgerEntrySummary& back = response.entries.back();
    response.tip_signature = engine_->SignTipFor(back.entry_id, back.entry_hash);
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
  hb.is_supervisor = config_.supervisor;
  return hb;
}

WireMessage NetworkManager::HandleHeartbeat(const std::string& peer_node_id,
                                            const HeartbeatPayload& request) {
  // One round trip updates both sides: fold the requester's figures in,
  // answer with our own. Advisory only -- see the header comment.
  peer_table_.RecordReport(peer_node_id, request.ledger_tip_entry_id, request.free_quota_mb,
                           request.quota_limited, request.transit_bytes_held,
                           request.transit_budget_bytes);
  if (request.is_supervisor) {
    // Heartbeat arrived over the authenticated channel, so the sender really
    // is peer_node_id; supervisor flag from here is handshake-adjacent.
    PeerInfo info;
    if (peer_table_.Get(peer_node_id, &info) && !info.is_supervisor) {
      info.is_supervisor = true;
      peer_table_.Upsert(info, PeerSource::kProbe);
      if (placement_) placement_->Rebuild(peer_table_);
    }
  }
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
      if (hb.is_supervisor) {
        peer.is_supervisor = true;
      }
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
  if (peer.is_supervisor) seen.is_supervisor = true;
  // Fast recovery, slow condemn: one answered probe clears a degraded
  // marking (the peer proved it is back); condemning takes sustained
  // failure via the suspicion tiers above.
  if (seen.state == NodeLifecycleState::kDegraded) seen.state = NodeLifecycleState::kRunning;
  peer_table_.Upsert(seen, PeerSource::kProbe);
  if (seen.is_supervisor && placement_) placement_->Rebuild(peer_table_);
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
              const std::string message = std::string("DSN-RECEIPT-v1") + receipt.message_id +
                                          receipt.key_hash + std::to_string(receipt.applied_lsn);
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

thread_local std::string NetworkManager::thread_next_broadcast_id_;

void NetworkManager::BroadcastLocalWrite(const std::string& collection, const std::string& key,
                                          const std::string& encoded_doc) {
  OpBroadcastPayload payload;
  payload.collection = collection;
  payload.docs.push_back(DocEntry{key, encoded_doc});
  // Prefer the API-assigned id for this thread (durability wait shares it);
  // otherwise mint one (replication-path writes, outbox flush).
  if (!thread_next_broadcast_id_.empty()) {
    payload.message_id = std::move(thread_next_broadcast_id_);
    thread_next_broadcast_id_.clear();
  } else {
    payload.message_id = MessageDedup::NewMessageId(engine_->identity().node_id());
  }
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

  RebuildPlacement();
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
    // An offline owner cannot hold bytes for itself.
    std::vector<std::string> active_candidates;
    active_candidates.reserve(holder_candidates.size());
    for (const auto& h : holder_candidates) {
      if (h != replica) active_candidates.push_back(h);
    }
    // This replica should hold these bytes but cannot be reached. Hold the
    // chunks this node is assigned by deterministic rank -- at most
    // max_holders copies mesh-wide instead of one per online replica -- and
    // record the intent, so the owner discovers them on return instead of
    // the write simply never arriving. Chunks rotate across holders, so a
    // striped document spreads rather than stacking on the same top-H.
    std::vector<uint32_t> my_chunks;
    for (uint32_t c = 0; c < chunk_total; ++c) {
      const std::vector<std::string> holders = SelectTransitHolders(
          replica, doc_key_hash, active_candidates, max_holders, c);
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
    // No held-ack dial here (M-10 fix). The old code dialled `replica` --
    // the offline owner, unreachable by definition (each dial was a doomed
    // connection attempt), under a comment confusing the writer with the
    // owner. On this path (local writes only -- the sole caller is
    // BroadcastLocalWrite) the writer is this node itself, and the durable
    // record of the hold is the TRANSIT_INTENT just appended to our ledger
    // plus the envelope (which preserves message_id for correlation). The
    // owner discovers the hold via intents on return and claims it; nothing
    // is sent to an unreachable peer. Worse, the old loop read
    // TransitIntentsForSelf -- intents where WE are the owner, not the
    // holder -- and matched them to these chunks by chunk_index alone, so it
    // could ack unrelated intents. Deleted, not repointed.
    DSN_LOG_INFO("transit", "held " << my_chunks.size() << " chunk(s) of " << collection << "/"
                                    << key << " for offline owner " << replica
                                    << " (intent on our ledger; no ack dial)");
  }
}

size_t NetworkManager::ApplyHolderEntries(const std::string& holder_node,
                                           const std::vector<TransitEntry>& entries,
                                           ClaimReport& report, TransitClaimPayload& claim_out) {
  // The holder must be handshake-known: its signature is the trust root for
  // these bytes, and an unproven key verifies nothing (C-2/C-3).
  PeerInfo holder_info;
  std::string holder_pubkey;
  if (peer_table_.Get(holder_node, &holder_info) && holder_info.handshake_proven) {
    holder_pubkey = holder_info.ed25519_pubkey;
  }
  if (holder_pubkey.empty()) {
    DSN_LOG_WARN("transit", "ignoring " << entries.size() << " entries from unverified holder "
                                        << holder_node);
    report.failures += entries.size();
    return 0;
  }
  // Local intents are the cross-check: which (key_hash, chunk_total, holder)
  // we actually expect. Entries for anything else are refused.
  std::map<std::string, NodeEngine::TransitIntent> intent_by_hash;
  for (const auto& intent : engine_->TransitIntentsForSelf()) {
    intent_by_hash[intent.key_hash] = intent;
  }
  // Ring-designation inputs, built once per response. Mirrors the hold-time
  // candidate construction (HoldForUnreachableOwners): self plus dialable,
  // non-supervisor, non-reclaimed, non-dead peers, minus the owner (self).
  const std::string self = engine_->identity().node_id();
  const int64_t threshold = config_.liveness_threshold_ms > 0
                                ? static_cast<int64_t>(config_.liveness_threshold_ms)
                                : 5000;
  const int64_t now = NowMs();
  std::vector<std::string> holder_candidates;
  for (const PeerInfo& peer : peer_table_.List()) {
    if (peer.node_id == self || peer.p2p_port == 0) continue;
    if (peer.node_id.rfind("bootstrap#", 0) == 0) continue;
    if (peer.is_supervisor) continue;
    if (peer.state == NodeLifecycleState::kReclaimed) continue;
    if (peer.Suspicion(now, threshold) == PeerSuspicion::kDead) continue;
    holder_candidates.push_back(peer.node_id);
  }
  const uint32_t max_holders = config_.transit_max_holders > 0 ? config_.transit_max_holders : 3;
  // Group verified chunks by document.
  std::map<std::string, std::vector<const TransitEntry*>> by_doc;
  for (const TransitEntry& entry : entries) {
    if (!VerifyTransitEntry(entry, holder_node, holder_pubkey)) {
      DSN_LOG_WARN("transit", "holder " << holder_node
                                        << " served unverifiable bytes; ignoring entry");
      ++report.failures;
      continue;
    }
    // Ring-designation check (C-3): the responder must be a deterministically
    // designated holder for this chunk (SelectTransitHolders over the same
    // inputs the hold path used). An off-ring Sybil -- even a
    // handshake-proven mesh member -- cannot inject bytes; designated
    // holders already receive plaintext via broadcast, so passing this check
    // grants no new trust. A locally-held intent disagreeing on
    // holder/chunk_total is still refused below (defense in depth).
    const std::string doc_hash = LedgerKeyHash(entry.collection, entry.key);
    const std::vector<std::string> designated =
        SelectTransitHolders(self, doc_hash, holder_candidates, max_holders, entry.chunk_index);
    if (std::find(designated.begin(), designated.end(), holder_node) == designated.end()) {
      DSN_LOG_WARN("transit", "holder " << holder_node << " is not a designated holder for "
                                        << entry.collection << "/" << entry.key << " chunk "
                                        << entry.chunk_index << "; ignoring");
      ++report.failures;
      continue;
    }
    auto it = intent_by_hash.find(entry.key_hash);
    if (it != intent_by_hash.end()) {
      const auto& intent = it->second;
      if (intent.holder_node != holder_node || intent.chunk_total != entry.chunk_total) {
        DSN_LOG_WARN("transit", "holder " << holder_node << " served entry disagreeing with local "
                                          << "intent (holder/chunk_total); ignoring");
        ++report.failures;
        continue;
      }
    }
    std::string doc_key = entry.collection;
    doc_key.push_back('\0');
    doc_key += entry.key;
    by_doc[doc_key].push_back(&entry);
  }
  size_t applied = 0;
  for (const auto& [doc_key, chunks] : by_doc) {
    const TransitEntry* first = chunks.front();
    std::string full_bytes;
    if (first->chunk_total == 1) {
      if (chunks.size() != 1) {
        ++report.failures;
        continue;
      }
      full_bytes = first->encoded_doc;
    } else {
      // Reassemble striped document (C-3): every chunk 0..total-1 exactly
      // once, concatenated size matching the attested doc_size.
      const uint32_t total = first->chunk_total;
      if (total > 4096 || first->doc_size_bytes > (64u << 20)) {
        ++report.failures;
        continue;
      }
      std::vector<const TransitEntry*> ordered(total, nullptr);
      bool ok = true;
      for (const TransitEntry* e : chunks) {
        if (e->chunk_total != total || e->doc_size_bytes != first->doc_size_bytes ||
            e->chunk_index >= total || ordered[e->chunk_index] != nullptr) {
          ok = false;
          break;
        }
        ordered[e->chunk_index] = e;
      }
      for (const TransitEntry* e : ordered) {
        if (e == nullptr) { ok = false; break; }
      }
      if (!ok) {
        ++report.failures;
        continue;
      }
      size_t sum = 0;
      for (const TransitEntry* e : ordered) sum += e->encoded_doc.size();
      if (sum != first->doc_size_bytes) {
        ++report.failures;
        continue;
      }
      full_bytes.reserve(sum);
      for (const TransitEntry* e : ordered) full_bytes += e->encoded_doc;
    }
    Status st = engine_->ApplyClaimedTransit(first->collection, first->key, full_bytes);
    if (!st.ok()) {
      DSN_LOG_WARN("transit", "could not apply held document " << first->collection << "/"
                                                               << first->key << ": " << st.message());
      ++report.failures;
      continue;
    }
    for (const TransitEntry* e : chunks) claim_out.key_hashes.push_back(e->key_hash);
    ++report.documents_claimed;
    ++applied;
  }
  return applied;
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
        const size_t applied =
            ApplyHolderEntries(holder, payload.entries, report, claim);
        if (applied > 0) {
          transport_->SendRequest(holder_info.host, holder_info.p2p_port,
                                  WireMessage{MessageType::kTransitClaim, claim.Encode()});
        }
        if (!payload.truncated) break;
        offset = payload.next_offset;
      }
    }
  }

  // Phase 2: ask-everyone sweep for holders not captured by local intents
  // (e.g. intents from a peer we haven't synced with yet -- the returning
  // owner that was offline for the write has NO local intent). Sound because
  // ApplyHolderEntries verifies per entry: content hash + holder attestation
  // against the responder's proven key AND ring-designation
  // (SelectTransitHolders). An off-ring Sybil's bytes are ignored no matter
  // what they attest.
  {
    std::map<std::string, bool> queried;
    for (const auto& intent : local_intents) queried[intent.holder_node] = true;
    for (const PeerInfo& peer : peer_table_.Ranked()) {
      if (peer.p2p_port == 0 || peer.node_id == self) continue;
      if (queried.count(peer.node_id) != 0) continue;  // asked in Phase 1
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
      ApplyHolderEntries(peer.node_id, payload.entries, report, claim);

      if (!claim.key_hashes.empty()) {
        // Telling the holder is what lets it record TRANSIT_CLAIMED and
        // eventually release the bytes. A failure here is not fatal: the
        // envelope simply expires on its TTL instead.
        transport_->SendRequest(peer.host, peer.p2p_port,
                                WireMessage{MessageType::kTransitClaim, claim.Encode()});
      }
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
    tip.signature = delta.tip_signature;
    // A peer's *self-reported* verification result is not evidence on its
    // own. What we check locally: the segment links to itself, AND the
    // responder's tip signature verifies against its handshake-proven key
    // (C-6 fix -- previously the signature field was never populated, so an
    // empty string was "verified"). Origin signatures cannot be recomputed
    // from summaries (document bytes are stripped for non-readers), so a tip
    // without a valid signature is not a vote; see checkpoint.h.
    tip.self_verified = true;
    std::string expected_prev;
    for (const LedgerEntrySummary& entry : delta.entries) {
      if (!expected_prev.empty() && entry.prev_hash != expected_prev) {
        tip.self_verified = false;
        break;
      }
      expected_prev = entry.entry_hash;
    }
    tip.signature_valid = !tip.signature.empty() &&
                          engine_->VerifyPeerTip(peer.node_id, tip.entry_id, tip.entry_hash,
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
