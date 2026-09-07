#include "desentry/net/udp_discovery.h"


#include <chrono>
#include <cstring>

#include "desentry/common/byte_buffer.h"
#include "desentry/common/logger.h"
#include "desentry/common/platform.h"

namespace desentry {

namespace {
constexpr uint32_t kMagic = 0x44534E31;  // "DSN1"

}  // namespace

UdpDiscovery::~UdpDiscovery() { Stop(); }

Status UdpDiscovery::Start(PeerTable* peer_table) {
  peer_table_ = peer_table;

  NetInit();
  recv_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (!SocketValid(recv_fd_)) return Status::NetworkError("discovery: socket() failed");
  SetSockOptInt(recv_fd_, SOL_SOCKET, SO_REUSEADDR, 1);
#ifdef SO_REUSEPORT
  SetSockOptInt(recv_fd_, SOL_SOCKET, SO_REUSEPORT, 1);
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(discovery_port_);
  if (::bind(recv_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    DSN_LOG_WARN("discovery", "bind() failed on discovery port " << discovery_port_ << ": " << SocketErrorString()
                                                                    << " -- discovery disabled, rely on bootstrap_peers");
    CloseSocket(recv_fd_);
    recv_fd_ = kInvalidSocket;
    return Status::OK();  // non-fatal: static bootstrap peers still work
  }

  send_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (SocketValid(send_fd_)) {
    SetSockOptInt(send_fd_, SOL_SOCKET, SO_BROADCAST, 1);
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
      // v2 fields. A v1 peer's datagram simply ends after p2p_port, so they
      // are read only if bytes remain -- a mixed-version LAN keeps working
      // and the older peers just show up without a hostname.
      if (r.remaining() > 0) info.api_port = r.U16();
      if (r.remaining() > 0) info.hostname = r.Bytes();
      if (r.remaining() > 0) info.is_supervisor = r.U8() != 0;
      peer_table_->Upsert(info);
    } catch (const std::exception&) {
      continue;  // malformed datagram -- ignore, not fatal
    }
  }
}

std::string UdpDiscovery::BuildAdvertisement() const {
  ByteWriter w;
  w.U32(kMagic);
  w.Bytes(identity_->node_id());
  w.Bytes(identity_->public_key());
  w.U16(p2p_port_);
  w.U16(advert_.api_port);
  w.Bytes(advert_.hostname);
  w.U8(advert_.is_supervisor ? 1 : 0);
  return w.TakeString();
}

void UdpDiscovery::SendAdvertisement(const std::string& payload) {
  if (!SocketValid(send_fd_)) return;
  auto send_to = [&](const char* dest_ip) {
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(discovery_port_);
    ::inet_pton(AF_INET, dest_ip, &dest.sin_addr);
    SocketSendTo(send_fd_, payload.data(), payload.size(), reinterpret_cast<sockaddr*>(&dest),
                 sizeof(dest));
  };
  // Loopback broadcast covers several node processes on one machine; general
  // broadcast covers a real LAN. Sending both is harmless -- discovery is
  // advisory, not trust-bearing.
  send_to("127.255.255.255");
  send_to("255.255.255.255");
}

void UdpDiscovery::AdvertiseNow() { SendAdvertisement(BuildAdvertisement()); }

void UdpDiscovery::BroadcastLoop() {
  const std::string payload = BuildAdvertisement();
  while (running_) {
    SendAdvertisement(payload);
    for (uint32_t waited = 0; waited < interval_ms_ && running_; waited += 100) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

void UdpDiscovery::Stop() {
  if (!running_ && !SocketValid(recv_fd_) && !SocketValid(send_fd_)) return;
  running_ = false;
  if (SocketValid(recv_fd_)) { ShutdownSocket(recv_fd_); CloseSocket(recv_fd_); recv_fd_ = kInvalidSocket; }
  if (SocketValid(send_fd_)) { CloseSocket(send_fd_); send_fd_ = kInvalidSocket; }
  if (listen_thread_.joinable()) listen_thread_.join();
  if (broadcast_thread_.joinable()) broadcast_thread_.join();
}

}  // namespace desentry
