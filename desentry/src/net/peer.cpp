#include "desentry/net/peer.h"

namespace desentry {

void PeerTable::Upsert(const PeerInfo& info) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = peers_.find(info.node_id);
  if (it == peers_.end()) {
    peers_[info.node_id] = info;
  } else {
    // Keep whichever info is freshest; don't clobber a known pubkey with
    // an empty one from a discovery broadcast that hasn't handshaked yet.
    it->second.host = info.host;
    it->second.p2p_port = info.p2p_port;
    it->second.last_seen_ms = info.last_seen_ms;
    if (!info.ed25519_pubkey.empty()) it->second.ed25519_pubkey = info.ed25519_pubkey;
  }
}

std::vector<PeerInfo> PeerTable::List() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<PeerInfo> out;
  out.reserve(peers_.size());
  for (auto& [id, info] : peers_) out.push_back(info);
  return out;
}

size_t PeerTable::Size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return peers_.size();
}

bool PeerTable::Contains(const std::string& node_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  return peers_.count(node_id) != 0;
}

}  // namespace desentry
