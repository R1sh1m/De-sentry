#include "desentry/net/mdns_discovery.h"

#include <chrono>
#include <cstring>

#include "desentry/common/byte_buffer.h"
#include "desentry/common/logger.h"
#include "desentry/common/platform.h"

namespace desentry {

namespace {
constexpr uint32_t kMagic = 0x4D445352; // "MDSR"
constexpr const char* kMulticastAddr = "224.0.0.251";
constexpr uint16_t kMulticastPort = 5353;
}  // namespace

MdnsDiscovery::~MdnsDiscovery() { Stop(); }

Status MdnsDiscovery::Start(const std::string& node_id, uint16_t p2p_port, uint16_t api_port) {
  node_id_ = node_id;
  p2p_port_ = p2p_port;
  api_port_ = api_port;

  NetInit();
  recv_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (!SocketValid(recv_fd_))
    return Status::NetworkError("mdns: socket() failed");
  SetSockOptInt(recv_fd_, SOL_SOCKET, SO_REUSEADDR, 1);
#ifdef SO_REUSEPORT
  SetSockOptInt(recv_fd_, SOL_SOCKET, SO_REUSEPORT, 1);
#endif
  // Bind to the multicast port on all interfaces.
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(kMulticastPort);
  if (::bind(recv_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    DSN_LOG_WARN("mdns", "bind() failed on port " << kMulticastPort << ": " << SocketErrorString()
               << " -- mdns disabled");
    CloseSocket(recv_fd_);
    recv_fd_ = kInvalidSocket;
    return Status::OK();
  }
  // Join multicast group.
  ip_mreq mreq{};
  ::inet_pton(AF_INET, kMulticastAddr, &mreq.imr_multiaddr);
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  if (::setsockopt(recv_fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq), sizeof(mreq)) != 0) {
    DSN_LOG_WARN("mdns", "setsockopt IP_ADD_MEMBERSHIP failed: " << SocketErrorString()
               << " -- mdns disabled");
    CloseSocket(recv_fd_);
    recv_fd_ = kInvalidSocket;
    return Status::OK();
  }

  send_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (SocketValid(send_fd_)) {
    // Enable multicast TTL = 1 (local network only).
    uint8_t ttl = 1;
    ::setsockopt(send_fd_, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
  }

  running_ = true;
  listen_thread_ = std::thread(&MdnsDiscovery::ListenLoop, this);
  broadcast_thread_ = std::thread(&MdnsDiscovery::BroadcastLoop, this);
  DSN_LOG_INFO("mdns", "mDNS discovery active on " << kMulticastAddr << ":" << kMulticastPort);
  return Status::OK();
}

void MdnsDiscovery::ListenLoop() {
  char buf[2048];
  while (running_) {
    sockaddr_in from{};
    dsn_socklen_t fromlen = sizeof(from);
    dsn_iolen_t n = SocketRecvFrom(recv_fd_, buf, sizeof(buf), reinterpret_cast<sockaddr*>(&from), &fromlen);
    if (n <= 0) {
      if (!running_) break;
      continue;
    }
    try {
      ByteReader r(buf, static_cast<size_t>(n));
      uint32_t magic = r.U32();
      if (magic != kMagic) continue;
      std::string node_id = r.Bytes();
      uint16_t p2p_port = r.U16();
      uint16_t api_port = r.U16();
      if (node_id == node_id_) continue; // ignore own packets
      if (node_id.size() != 32) continue;
      char ip[INET_ADDRSTRLEN];
      ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
      // Invoke callback.
      if (callback_) {
        callback_(node_id, std::string(ip), p2p_port, api_port);
      }
    } catch (const std::exception&) {
      continue; // malformed packet
    }
  }
}

std::string MdnsDiscovery::BuildAdvertisement() const {
  ByteWriter w;
  w.U32(kMagic);
  w.Bytes(node_id_);
  w.U16(p2p_port_);
  w.U16(api_port_);
  return w.TakeString();
}

void MdnsDiscovery::SendAdvertisement(const std::string& payload) {
  if (!SocketValid(send_fd_)) return;
  sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(kMulticastPort);
  ::inet_pton(AF_INET, kMulticastAddr, &dest.sin_addr);
  SocketSendTo(send_fd_, payload.data(), payload.size(), reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
}

void MdnsDiscovery::AdvertiseNow() { SendAdvertisement(BuildAdvertisement()); }

void MdnsDiscovery::BroadcastLoop() {
  const std::string payload = BuildAdvertisement();
  while (running_) {
    SendAdvertisement(payload);
    // 30 seconds interval
    for (uint32_t waited = 0; waited < 30000 && running_; waited += 100) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

void MdnsDiscovery::Stop() {
  if (!running_ && !SocketValid(recv_fd_) && !SocketValid(send_fd_)) return;
  running_ = false;
  if (SocketValid(recv_fd_)) { ShutdownSocket(recv_fd_); CloseSocket(recv_fd_); recv_fd_ = kInvalidSocket; }
  if (SocketValid(send_fd_)) { CloseSocket(send_fd_); send_fd_ = kInvalidSocket; }
  if (listen_thread_.joinable()) listen_thread_.join();
  if (broadcast_thread_.joinable()) broadcast_thread_.join();
}

}  // namespace desentry