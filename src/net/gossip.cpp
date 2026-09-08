#include "desentry/net/gossip.h"

#include <algorithm>
#include <unordered_map>

#include "desentry/common/logger.h"
#include "desentry/common/platform.h"
#include "desentry/net/admission.h"

namespace desentry {

GossipEngine::~GossipEngine() { Stop(); }

void GossipEngine::Start() {
  running_ = true;
  thread_ = std::thread(&GossipEngine::Loop, this);
}

void GossipEngine::Stop() {
  if (!running_) return;
  running_ = false;
  if (thread_.joinable()) thread_.join();
}

void GossipEngine::SetThrottleFactor(uint32_t factor) {
  throttle_.store(factor == 0 ? 1 : factor);
}

uint32_t GossipEngine::NextSleepMs(std::mt19937* rng) const {
  const uint32_t base = options_.interval_ms * throttle_.load();
  if (options_.jitter_pct == 0) return base;
  // Fifty nodes launched by one script with one interval will otherwise
  // synchronise into a thundering herd that spikes every interval and idles
  // in between. Jitter spreads the load flat.
  const uint32_t spread = base * options_.jitter_pct / 100;
  if (spread == 0) return base;
  std::uniform_int_distribution<int32_t> dist(-static_cast<int32_t>(spread),
                                               static_cast<int32_t>(spread));
  const int32_t jittered = static_cast<int32_t>(base) + dist(*rng);
  return static_cast<uint32_t>(std::max(50, jittered));
}

std::vector<PeerInfo> GossipEngine::SelectPeers(std::mt19937* rng) {
  std::vector<PeerInfo> ranked = peers_->Ranked();
  // Supervisors are control-plane only and hold no replicated data; gossiping
  // with one would be a wasted round every time.
  ranked.erase(std::remove_if(ranked.begin(), ranked.end(),
                               [](const PeerInfo& p) { return p.is_supervisor || p.p2p_port == 0; }),
               ranked.end());
  if (ranked.empty()) return {};

  std::vector<PeerInfo> chosen;
  std::uniform_int_distribution<uint32_t> percent(0, 99);
  std::uniform_int_distribution<size_t> anywhere(0, ranked.size() - 1);
  const size_t want = std::min<size_t>(options_.fanout, ranked.size());
  // The ranked head is the top third (at least one peer): good enough to bias
  // toward healthy peers without collapsing onto a single favourite.
  const size_t head = std::max<size_t>(1, ranked.size() / 3);
  std::uniform_int_distribution<size_t> from_head(0, head - 1);

  std::vector<bool> taken(ranked.size(), false);
  for (size_t attempts = 0; chosen.size() < want && attempts < ranked.size() * 4; ++attempts) {
    // Exploration matters: a peer that had one bad round would otherwise sink
    // in the ranking and never be contacted again, so it could never prove it
    // had recovered. A uniform pick some of the time keeps every peer
    // reachable.
    const size_t index = percent(*rng) < options_.exploration_pct ? anywhere(*rng) : from_head(*rng);
    if (taken[index]) continue;
    taken[index] = true;
    chosen.push_back(ranked[index]);
  }
  return chosen;
}

void GossipEngine::Loop() {
  std::mt19937 rng(std::random_device{}());
  while (running_) {
    for (const PeerInfo& peer : SelectPeers(&rng)) {
      if (!running_) break;
      RunRoundWith(peer);
    }
    rounds_.fetch_add(1);

    const uint32_t sleep_ms = NextSleepMs(&rng);
    for (uint32_t waited = 0; waited < sleep_ms && running_; waited += 100) SleepMs(100);
  }
}

void GossipEngine::RunRoundWith(const PeerInfo& peer) {
  ExchangeDocuments(peer);
  ExchangeLedger(peer);
}

void GossipEngine::ExchangeDocuments(const PeerInfo& peer) {
  const Requestor who = Requestor::Peer(peer.node_id);
  // Only offer digests for collections this peer may read. Sending a digest
  // for a private collection would leak its key set even though the bytes
  // would later be withheld -- the filter has to start at the digest.
  for (const std::string& collection : engine_->ReadableCollections(who)) {
    DigestPayload digest;
    digest.collection = collection;
    for (const DigestEntryOut& e : engine_->LocalDigest(collection)) {
      digest.entries.push_back(DigestEntry{e.key, e.top_ts.Encode(), e.content_hash});
    }

    const int64_t started = MonotonicMs();
    auto resp_or = transport_->SendRequest(peer.host, peer.p2p_port,
                                            WireMessage{MessageType::kDigest, digest.Encode()});
    peers_->RecordProbe(peer.node_id, static_cast<double>(MonotonicMs() - started), resp_or.ok());
    if (!resp_or.ok()) {
      DSN_LOG_DEBUG("gossip", "round with " << peer.node_id << " (" << collection
                                             << ") failed: " << resp_or.status().ToString());
      return;  // the peer is unreachable; the remaining collections would fail too
    }
    if (resp_or.value().type != MessageType::kDeltaResponse) continue;

    DeltaResponsePayload delta;
    try {
      delta = DeltaResponsePayload::Decode(resp_or.value().payload);
    } catch (const std::exception& e) {
      DSN_LOG_WARN("gossip", "malformed delta from " << peer.node_id << ": " << e.what());
      continue;
    }

    for (const DocEntry& doc : delta.pushed) {
      Status st = engine_->MergeRemote(collection, doc.key, doc.encoded_doc, who);
      if (!st.ok()) {
        DSN_LOG_WARN("gossip", "merge rejected for " << collection << "/" << doc.key << ": "
                                                      << st.message());
      }
    }
    if (!delta.pushed.empty()) {
      DSN_LOG_DEBUG("gossip", "merged " << delta.pushed.size() << " doc(s) from " << peer.node_id
                                         << "/" << collection);
    }

    if (!delta.wanted_keys.empty()) {
      OpBroadcastPayload fulfill;
      fulfill.collection = collection;
      fulfill.message_id = MessageDedup::NewMessageId(engine_->identity().node_id());
      // A gossip fulfilment is a direct reply to a specific request, so it
      // must not be relayed onward -- the requester is the only peer that
      // asked for it.
      fulfill.ttl = 0;
      for (const std::string& key : delta.wanted_keys) {
        auto raw_or = engine_->GetRawEncoded(collection, key);
        if (raw_or.ok()) fulfill.docs.push_back(DocEntry{key, raw_or.value()});
      }
      if (!fulfill.docs.empty()) {
        transport_->SendRequest(peer.host, peer.p2p_port,
                                 WireMessage{MessageType::kOpBroadcast, fulfill.Encode()});
      }
    }
  }
}

void GossipEngine::ExchangeLedger(const PeerInfo& peer) {
  // The hash-only half of a round. This runs regardless of what collections
  // the two peers share, because the ledger's entry-hash set is the thing
  // every node converges on -- it is a grow-only set, so union is merge, and
  // union is exactly what exchanging missing entries computes.
  const WriteAheadLog::LedgerTip tip = engine_->LedgerTip();

  LedgerDigestPayload digest;
  digest.tip_entry_id = tip.entry_id;
  digest.tip_entry_hash = tip.entry_hash;
  digest.tip_signature = engine_->SignLedgerTip();
  digest.from_entry_id = tip.entry_id;

  auto resp_or = transport_->SendRequest(peer.host, peer.p2p_port,
                                          WireMessage{MessageType::kLedgerDigest, digest.Encode()});
  if (!resp_or.ok() || resp_or.value().type != MessageType::kLedgerDelta) return;

  LedgerDeltaPayload delta;
  try {
    delta = LedgerDeltaPayload::Decode(resp_or.value().payload);
  } catch (const std::exception&) {
    return;
  }
  if (delta.entries.empty()) return;

  // Record what the peer's ledger height is, which feeds the freshness term
  // of its fitness score and the supervisor's checkpoint quorum.
  peers_->RecordReport(peer.node_id, delta.entries.back().entry_id, 0);

  // Transit intents naming this node are the trigger for a claim pass: the
  // whole point of the hash-only exchange is that a returning node discovers
  // bytes are waiting for it without anyone having tracked who was down.
  const std::string self = engine_->identity().node_id();
  bool intents_for_us = false;
  for (const LedgerEntrySummary& entry : delta.entries) {
    if (static_cast<WalRecordType>(entry.operation) != WalRecordType::kTransitIntent) continue;
    // A TRANSIT_INTENT records the owner in its collection field. A peer that
    // cannot read the collection sees an empty one -- but an intent's
    // "collection" is a node_id, not a real collection, so it is never
    // withheld and the discovery works for every node.
    if (entry.collection == self) {
      intents_for_us = true;
      break;
    }
  }
  if (intents_for_us) {
    DSN_LOG_INFO("gossip", "peer " << peer.node_id << " is holding transit bytes for us");
  }
}

}  // namespace desentry
