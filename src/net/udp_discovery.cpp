#include "desentry/net/udp_discovery.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

#include "desentry/common/byte_buffer.h"
#include "desentry/common/logger.h"

namespace desentry {

namespace {
constexpr uint32_t kMagic = 0x44534E31;  // "DSN1"

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}  // namespace

UdpDiscovery::~UdpDiscovery() { Stop(); }

Status UdpDiscovery::Start(PeerTable* peer_table) {
  peer_table_ = peer_table;

  recv_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (recv_fd_ < 0) return Status::NetworkError("discovery: socket() failed");
  int opt = 1;
  ::setsockopt(recv_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
  ::setsockopt(recv_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(discovery_port_);
  if (::bind(recv_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    DSN_LOG_WARN("discovery", "bind() failed on discovery port " << discovery_port_ << ": " << std::strerror(errno)
                                                                    << " -- discovery disabled, rely on bootstrap_peers");
    ::close(recv_fd_);
    recv_fd_ = -1;
    return Status::OK();  // non-fatal: static bootstrap peers still work
  }

  send_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (send_fd_ >= 0) {
    int bcast = 1;
    ::setsockopt(send_fd_, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));
  }

  running_ = true;
  listen_thread_ = std::thread(&UdpDiscovery::ListenLoop, this);
  broadcast_thread_ = std::thread(&UdpDiscovery::BroadcastLoop, this);
  DSN_LOG_INFO("discovery", "UDP discovery active on port " << discovery_port_);
  return Status::OK();
}

void UdpDiscovery::ListenLoop() {
  char buf[2048];
  while (running_) {
    sockaddr_in from{};
    socklen_t fromlen = sizeof(from);
    ssize_t n = ::recvfrom(recv_fd_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromlen);
    if (n <= 0) {
      if (!running_) break;
      continue;
    }
    try {
      ByteReader r(buf, static_cast<size_t>(n));
      uint32_t magic = r.U32();
      if (magic != kMagic) continue;
      std::string node_id = r.Bytes();
      std::string pubkey = r.Bytes();
      uint16_t p2p_port = r.U16();
      if (node_id == identity_->node_id()) continue;  // hearing our own broadcast

      char ip[INET_ADDRSTRLEN];
      ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));

      PeerInfo info;
      info.node_id = node_id;
      info.ed25519_pubkey = pubkey;
      info.host = ip;
      info.p2p_port = p2p_port;
      info.last_seen_ms = NowMs();
      peer_table_->Upsert(info);
    } catch (const std::exception&) {
      continue;  // malformed datagram -- ignore, not fatal
    }
  }
}

void UdpDiscovery::BroadcastLoop() {
  ByteWriter w;
  w.U32(kMagic);
  w.Bytes(identity_->node_id());
  w.Bytes(identity_->public_key());
  w.U16(p2p_port_);
  std::string payload = w.TakeString();

  auto send_to = [&](const char* dest_ip) {
    if (send_fd_ < 0) return;
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(discovery_port_);
    ::inet_pton(AF_INET, dest_ip, &dest.sin_addr);
    ::sendto(send_fd_, payload.data(), payload.size(), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
  };

  while (running_) {
    // Loopback broadcast covers the primary demo shape (several node
    // processes on one machine); general broadcast covers a real LAN.
    // Sending both is harmless -- discovery is advisory, not trust-bearing.
    send_to("127.255.255.255");
    send_to("255.255.255.255");
    for (uint32_t waited = 0; waited < interval_ms_ && running_; waited += 100) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

void UdpDiscovery::Stop() {
  if (!running_ && recv_fd_ < 0 && send_fd_ < 0) return;
  running_ = false;
  if (recv_fd_ >= 0) { ::shutdown(recv_fd_, SHUT_RDWR); ::close(recv_fd_); recv_fd_ = -1; }
  if (send_fd_ >= 0) { ::close(send_fd_); send_fd_ = -1; }
  if (listen_thread_.joinable()) listen_thread_.join();
  if (broadcast_thread_.joinable()) broadcast_thread_.join();
}

}  // namespace desentry
