#include "desentry/net/tcp_transport.h"


#include <cstring>

#include "desentry/common/logger.h"
#include "desentry/common/platform.h"
#include "desentry/net/secure_channel.h"

namespace desentry {

namespace {
constexpr size_t kMaxMessageBytes = 64 * 1024 * 1024;  // 64MiB frame cap: generous for a document batch, small enough to bound a hostile peer's memory pressure
}  // namespace

TcpTransport::~TcpTransport() { Stop(); }

Status TcpTransport::StartListening(const std::string& bind_addr, RequestHandler handler) {
  handler_ = std::move(handler);

  NetInit();
  dsn_socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (!SocketValid(fd)) return Status::NetworkError("socket() failed: " + SocketErrorString());

  SetSockOptInt(fd, SOL_SOCKET, SO_REUSEADDR, 1);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(p2p_port_);
  if (bind_addr.empty() || bind_addr == "0.0.0.0") {
    addr.sin_addr.s_addr = INADDR_ANY;
  } else if (::inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
    CloseSocket(fd);
    return Status::InvalidArgument("bad bind address: " + bind_addr);
  }

  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    CloseSocket(fd);
    return Status::NetworkError("bind() failed: " + SocketErrorString());
  }
  if (::listen(fd, 64) != 0) {
    CloseSocket(fd);
    return Status::NetworkError("listen() failed: " + SocketErrorString());
  }

  listen_fd_ = fd;
  running_ = true;
  accept_thread_ = std::thread(&TcpTransport::AcceptLoop, this, fd);
  DSN_LOG_INFO("tcp", "P2P listener up on " << bind_addr << ":" << p2p_port_);
  return Status::OK();
}

void TcpTransport::AcceptLoop(dsn_socket_t listen_fd) {
  while (running_) {
    sockaddr_in peer_addr{};
    dsn_socklen_t len = sizeof(peer_addr);
    dsn_socket_t client_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer_addr), &len);
    if (!SocketValid(client_fd)) {
      if (!running_) break;
      continue;
    }
    std::thread(&TcpTransport::HandleConnection, this, client_fd).detach();
  }
}

void TcpTransport::HandleConnection(dsn_socket_t client_fd) {
  auto hs = ServerHandshake(client_fd, *identity_, p2p_port_);
  if (!hs.ok()) {
    DSN_LOG_WARN("tcp", "inbound handshake failed: " << hs.status().ToString());
    CloseSocket(client_fd);
    return;
  }
  SessionKeys keys = hs.value().keys;

  auto req_or = RecvEncrypted(client_fd, &keys, kMaxMessageBytes);
  if (!req_or.ok()) {
    DSN_LOG_WARN("tcp", "failed to read request from " << hs.value().peer_node_id << ": " << req_or.status().ToString());
    CloseSocket(client_fd);
    return;
  }

  WireMessage response;
  if (handler_) {
    response = handler_(hs.value().peer_node_id, req_or.value());
  } else {
    response = WireMessage{MessageType::kError, "no handler installed"};
  }

  auto st = SendEncrypted(client_fd, &keys, response);
  if (!st.ok()) {
    DSN_LOG_WARN("tcp", "failed to send response to " << hs.value().peer_node_id << ": " << st.ToString());
  }
  CloseSocket(client_fd);
}

StatusOr<WireMessage> TcpTransport::SendRequest(const std::string& host, uint16_t port, const WireMessage& request) {
  NetInit();
  dsn_socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (!SocketValid(fd)) return Status::NetworkError("socket() failed: " + SocketErrorString());

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    // Not a literal IP -- try to resolve as a hostname.
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) {
      CloseSocket(fd);
      return Status::NetworkError("cannot resolve host: " + host);
    }
    addr.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
    ::freeaddrinfo(res);
  }

  // Bound connect timeout so a dead/unreachable peer doesn't stall an
  // entire gossip round -- important once the demo cluster includes peers
  // that have gone offline.
  SetSocketTimeoutMs(fd, 2000);

  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::string why = SocketErrorString();
    CloseSocket(fd);
    return Status::NetworkError("connect() to " + host + ":" + std::to_string(port) + " failed: " + why);
  }

  auto hs = ClientHandshake(fd, *identity_, p2p_port_);
  if (!hs.ok()) {
    CloseSocket(fd);
    return hs.status();
  }
  SessionKeys keys = hs.value().keys;

  Status send_st = SendEncrypted(fd, &keys, request);
  if (!send_st.ok()) {
    CloseSocket(fd);
    return send_st;
  }

  auto resp_or = RecvEncrypted(fd, &keys, kMaxMessageBytes);
  CloseSocket(fd);
  return resp_or;
}

void TcpTransport::Stop() {
  if (!running_) return;
  running_ = false;
  if (SocketValid(listen_fd_)) {
    ShutdownSocket(listen_fd_);
    CloseSocket(listen_fd_);
    listen_fd_ = kInvalidSocket;
  }
  if (accept_thread_.joinable()) accept_thread_.join();
}

}  // namespace desentry
