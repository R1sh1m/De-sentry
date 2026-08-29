#pragma once
// The set of peers this node currently knows about, learned via UDP
// discovery broadcasts (net/udp_discovery.h) and/or the static
// bootstrap_peers config list. A thin, thread-safe table -- deliberately
// not itself a consensus membership protocol (no SWIM-style failure
// detection beyond a simple last-seen staleness check); see
// ARCHITECTURE.md §9.7 for the Kademlia-DHT upgrade path once cluster size
// outgrows a LAN broadcast domain.

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace desentry {

struct PeerInfo {
  std::string node_id;
  std::string ed25519_pubkey;  // empty until we've actually handshaked with them
  std::string host;
  uint16_t p2p_port = 0;
  int64_t last_seen_ms = 0;
};

class PeerTable {
 public:
  void Upsert(const PeerInfo& info);
  std::vector<PeerInfo> List() const;
  size_t Size() const;
  bool Contains(const std::string& node_id) const;

 private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, PeerInfo> peers_;
};

}  // namespace desentry
