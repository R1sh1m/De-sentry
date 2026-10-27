#pragma once
// LAN peer discovery via mDNS (multicast DNS) compatible packets.
// This is a simplified, lightweight protocol that sends/receives minimal
// TXT records over multicast to 224.0.0.251:5353 (standard mDNS address).
//
// Unlike UdpDiscovery which uses broadcast, this uses multicast which is
// more network-friendly and works better on modern networks that may
// restrict broadcast traffic.
//
// Protocol format (custom, not full DNS wire format):
//   - Magic: 4 bytes "MDSR" (0x4D445352)
//   - node_id: 32 bytes
//   - p2p_port: 2 bytes (little-endian)
//   - api_port: 2 bytes (little-endian)
//
// This is advisory only -- trust is established through the TCP secure-channel
// handshake, not through discovery packets.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "desentry/common/platform.h"
#include "desentry/common/status.h"

namespace desentry {

// Callback type for discovered peers
using MdnsPeerCallback = std::function<void(const std::string& node_id, 
                                             const std::string& host,
                                             uint16_t p2p_port,
                                             uint16_t api_port)>;

class MdnsDiscovery {
 public:
  explicit MdnsDiscovery(MdnsPeerCallback callback)
      : callback_(std::move(callback)) {}
  ~MdnsDiscovery();

  // Start multicast discovery: open socket, begin periodic announcements.
  // node_id: this node's identifier (32 bytes)
  // p2p_port: this node's P2P port
  // api_port: this node's API port
  Status Start(const std::string& node_id, uint16_t p2p_port, uint16_t api_port);

  void Stop();

  // Sends one advertisement immediately, outside the periodic loop.
  void AdvertiseNow();

 private:
  void ListenLoop();
  void BroadcastLoop();
  std::string BuildAdvertisement() const;
  void SendAdvertisement(const std::string& payload);

  MdnsPeerCallback callback_;
  std::string node_id_;
  uint16_t p2p_port_ = 0;
  uint16_t api_port_ = 0;

  std::atomic<bool> running_{false};
  dsn_socket_t recv_fd_ = kInvalidSocket;
  dsn_socket_t send_fd_ = kInvalidSocket;
  std::thread listen_thread_;
  std::thread broadcast_thread_;
};

}  // namespace desentry