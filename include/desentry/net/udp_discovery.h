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
#include <utility>
#include <cstdint>
#include <string>
#include <thread>

#include "desentry/common/platform.h"
#include "desentry/common/status.h"
#include "desentry/net/identity.h"
#include "desentry/net/peer.h"

namespace desentry {

// What this node advertises about itself beyond the bare minimum. v1 sent
// node_id + public key + P2P port; v2 adds the fields the desktop app's
// sidebar needs to group and label nodes without a second lookup -- an
// mDNS-style hostname so a peer reads as "studio-imac.local" rather than
// "192.168.1.34", the API port for QR-code pairing, and the supervisor
// flag so the placement layer can exclude control-plane nodes on sight.
//
// All of it stays advisory. Discovery is not how trust is established:
// the TCP secure-channel handshake authenticates a peer before any data
// moves, so a spoofed advertisement can at worst waste a connection
// attempt -- and a spoofed `is_supervisor` can only cause a node to be
// *excluded* from placement, never included.
struct UdpAdvertisement {
  uint16_t api_port = 0;
  std::string hostname;
  bool is_supervisor = false;
};


class UdpDiscovery {
 public:
  UdpDiscovery(const NodeIdentity* identity, uint16_t p2p_port, uint16_t discovery_port,
                uint32_t interval_ms, UdpAdvertisement advert = UdpAdvertisement())
      : identity_(identity), p2p_port_(p2p_port), discovery_port_(discovery_port),
        interval_ms_(interval_ms), advert_(std::move(advert)) {}
  ~UdpDiscovery();

  Status Start(PeerTable* peer_table);
  void Stop();

  // Sends one advertisement immediately, outside the periodic loop. The app
  // calls this on a WiFi-change wake-up so a node reappears on a new network
  // without waiting a full interval.
  void AdvertiseNow();

 private:
  void ListenLoop();
  void BroadcastLoop();
  std::string BuildAdvertisement() const;
  void SendAdvertisement(const std::string& payload);

  const NodeIdentity* identity_;
  uint16_t p2p_port_;
  uint16_t discovery_port_;
  uint32_t interval_ms_;
  UdpAdvertisement advert_;
  PeerTable* peer_table_ = nullptr;

  std::atomic<bool> running_{false};
  dsn_socket_t recv_fd_ = kInvalidSocket;
  dsn_socket_t send_fd_ = kInvalidSocket;
  std::thread listen_thread_;
  std::thread broadcast_thread_;
};

}  // namespace desentry
