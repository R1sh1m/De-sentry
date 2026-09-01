#include "desentry/net/gossip.h"

#include <chrono>
#include <random>
#include <unordered_map>

#include "desentry/common/logger.h"

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

void GossipEngine::Loop() {
  std::mt19937 rng(std::random_device{}());
  while (running_) {
    auto known = peers_->List();
    if (!known.empty()) {
      std::uniform_int_distribution<size_t> dist(0, known.size() - 1);
      RunRoundWith(known[dist(rng)]);
    }
    for (uint32_t waited = 0; waited < interval_ms_ && running_; waited += 100) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

void GossipEngine::RunRoundWith(const PeerInfo& peer) {
  for (auto& collection : engine_->ListCollections()) {
    DigestPayload digest;
    digest.collection = collection;
    for (auto& e : engine_->LocalDigest(collection)) {
      digest.entries.push_back(DigestEntry{e.key, e.top_ts.Encode()});
    }

    auto resp_or = transport_->SendRequest(peer.host, peer.p2p_port,
                                            WireMessage{MessageType::kDigest, digest.Encode()});
    if (!resp_or.ok()) {
      DSN_LOG_DEBUG("gossip", "round with " << peer.node_id << " (" << collection << ") failed: " << resp_or.status().ToString());
      continue;
    }
    if (resp_or.value().type != MessageType::kDeltaResponse) continue;

    DeltaResponsePayload delta = DeltaResponsePayload::Decode(resp_or.value().payload);
    for (auto& doc : delta.pushed) {
      engine_->MergeRemote(collection, doc.key, doc.encoded_doc);
    }
    DSN_LOG_DEBUG("gossip", "merged " << delta.pushed.size() << " doc(s) from " << peer.node_id << "/" << collection);

    if (!delta.wanted_keys.empty()) {
      OpBroadcastPayload fulfill;
      fulfill.collection = collection;
      for (auto& key : delta.wanted_keys) {
        auto raw_or = engine_->GetRawEncoded(collection, key);
        if (raw_or.ok()) fulfill.docs.push_back(DocEntry{key, raw_or.value()});
      }
      if (!fulfill.docs.empty()) {
        transport_->SendRequest(peer.host, peer.p2p_port, WireMessage{MessageType::kOpBroadcast, fulfill.Encode()});
      }
    }
  }
}

}  // namespace desentry
