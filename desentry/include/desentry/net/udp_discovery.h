#pragma once
// LAN peer discovery via periodic UDP broadcast (ARCHITECTURE.md §7.4).
// Each peer periodically shouts a small unsigned "I exist" datagram
// (node_id + public key + P2P port); anyone who hears it adds/refreshes an
// entry in the shared PeerTable. This is intentionally *not* how trust is
// established -- a discovered peer is just an address to try dialing;
// the TCP secure-channel handshake (net/secure_channel.h) is what actually
// authenticates a peer before any data is exchanged, so a spoofed discovery
// broadcast can at worst waste a connection attempt, never inject data.
//
// Static config (NodeConfig::bootstrap_peers) works alongside this and is
// the more robust option in constrained network environments (some
// container/sandbox networks restrict broadcast traffic) -- discovery is a
// convenience layered on top, not the only way to join the mesh.

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "desentry/common/status.h"
#include "desentry/net/identity.h"
#include "desentry/net/peer.h"

namespace desentry {

class UdpDiscovery {
 public:
  UdpDiscovery(const NodeIdentity* identity, uint16_t p2p_port, uint16_t discovery_port, uint32_t interval_ms)
      : identity_(identity), p2p_port_(p2p_port), discovery_port_(discovery_port), interval_ms_(interval_ms) {}
  ~UdpDiscovery();

  Status Start(PeerTable* peer_table);
  void Stop();

 private:
  void ListenLoop();
  void BroadcastLoop();

  const NodeIdentity* identity_;
  uint16_t p2p_port_;
  uint16_t discovery_port_;
  uint32_t interval_ms_;
  PeerTable* peer_table_ = nullptr;

  std::atomic<bool> running_{false};
  int recv_fd_ = -1;
  int send_fd_ = -1;
  std::thread listen_thread_;
  std::thread broadcast_thread_;
};

}  // namespace desentry
