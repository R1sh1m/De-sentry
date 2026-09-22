#include "desentry/net/tcp_transport.h"


#include <cstring>

#include "desentry/common/logger.h"
#include "desentry/common/platform.h"
#include "desentry/net/secure_channel.h"
#if !defined(_WIN32)
#include <fcntl.h>
#endif

namespace desentry {

namespace {
constexpr size_t kMaxMessageBytes = 4 * 1024 * 1024;  // 4MiB frame cap (C-4 fix): a legitimate
// batch never needs 64MiB; the old cap let one unauthenticated length prefix
// commit 64MiB resident before any auth.
}  // namespace

TcpTransport::~TcpTransport() { Stop(); }

Status TcpTransport::StartListening(const std::string& bind_addr, RequestHandler handler) {
  handler_ = std::move(handler);

  NetInit();
  // Dual-stack listen (M-11 fix): IPv4 literals behave exactly as before;
  // "::" (or an IPv6 literal) binds AF_INET6 with V6ONLY cleared where the
  // stack allows, so one socket serves both families.
  sockaddr_storage addr{};
  dsn_socklen_t addr_len = 0;
  if (!ParseBindAddr(bind_addr, p2p_port_, &addr, &addr_len)) {
    return Status::InvalidArgument("bad bind address: " + bind_addr);
  }
  dsn_socket_t fd = ::socket(addr.ss_family, SOCK_STREAM, 0);
  if (!SocketValid(fd)) return Status::NetworkError("socket() failed: " + SocketErrorString());

  SetSockOptInt(fd, SOL_SOCKET, SO_REUSEADDR, 1);
  if (addr.ss_family == AF_INET6) TryDualStack(fd);

  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), addr_len) != 0) {
    CloseSocket(fd);
    return Status::NetworkError("bind() failed: " + SocketErrorString());
  }
  if (::listen(fd, 64) != 0) {
    CloseSocket(fd);
    return Status::NetworkError("listen() failed: " + SocketErrorString());
  }

  listen_fd_ = fd;
  running_ = true;
  conn_pool_ = std::make_unique<WorkerPool>(16, 64);
  accept_thread_ = std::thread(&TcpTransport::AcceptLoop, this, fd);
  DSN_LOG_INFO("tcp", "P2P listener up on " << bind_addr << ":" << p2p_port_);
  return Status::OK();
}

void TcpTransport::AcceptLoop(dsn_socket_t listen_fd) {
  while (running_) {
    sockaddr_storage peer_addr{};
    dsn_socklen_t len = sizeof(peer_addr);
    dsn_socket_t client_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer_addr), &len);
    if (!SocketValid(client_fd)) {
      if (!running_) break;
      // Back off on accept failure (e.g. EMFILE): tight-loop spinning here
      // is a 100% CPU self-DoS (C-4).
      SleepMs(10);
      continue;
    }
    if (active_conns_.load() >= kMaxInboundConns) {
      DSN_LOG_WARN("tcp", "inbound connection cap reached; rejecting");
      CloseSocket(client_fd);
      continue;
    }
    if (conn_pool_ && !conn_pool_->Submit([this, client_fd]() { HandleConnection(client_fd); })) {
      DSN_LOG_WARN("tcp", "connection queue full; rejecting");
      CloseSocket(client_fd);
      continue;
    } else if (!conn_pool_) {
      CloseSocket(client_fd);
    }
  }
}

void TcpTransport::HandleConnection(dsn_socket_t client_fd) {
  active_conns_.fetch_add(1);
  // Inbound timeout before any read (C-4 slowloris fix): a peer that
  // connects and sends nothing must not hold a worker forever.
  SetSocketTimeoutMs(client_fd, kInboundTimeoutMs);
  auto hs = ServerHandshake(client_fd, *identity_, p2p_port_, cluster_secret_);
  if (!hs.ok()) {
    DSN_LOG_WARN("tcp", "inbound handshake failed: " << hs.status().ToString());
    CloseSocket(client_fd);
    active_conns_.fetch_sub(1);
    return;
  }
  SessionKeys keys = hs.value().keys;

  auto req_or = RecvEncrypted(client_fd, &keys, kMaxMessageBytes);
  if (!req_or.ok()) {
    DSN_LOG_WARN("tcp", "failed to read request from " << hs.value().peer_node_id << ": " << req_or.status().ToString());
    CloseSocket(client_fd);
    active_conns_.fetch_sub(1);
    return;
  }
  // Key confirmation (H-5 fix): only bind node_id -> pubkey after the peer
  // proves session-key possession by producing a valid encrypted frame. A
  // replayed HELLO verifies (it is a real signature) but its replayer knows
  // neither ephemeral secret, so no valid frame follows and nothing is
  // registered -- the replay poisons nothing.
  if (handshake_cb_) {
    handshake_cb_(hs.value().peer_node_id, hs.value().peer_ed25519_pubkey,
                  hs.value().peer_p2p_port);
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
  active_conns_.fetch_sub(1);
}

StatusOr<WireMessage> TcpTransport::SendRequest(const std::string& host, uint16_t port, const WireMessage& request) {
  NetInit();
  // Dual-stack dial (M-11 fix): IPv4 literals behave exactly as before;
  // IPv6 literals, bracketed forms and hostnames resolving AAAA all work.
  sockaddr_storage addr{};
  dsn_socklen_t addr_len = 0;
  if (!ResolvePeerAddr(host, port, &addr, &addr_len)) {
    return Status::NetworkError("cannot resolve host: " + host);
  }
  dsn_socket_t fd = ::socket(addr.ss_family, SOCK_STREAM, 0);
  if (!SocketValid(fd)) return Status::NetworkError("socket() failed: " + SocketErrorString());

  // Bounded connect (C-4 fix): SO_RCVTIMEO/SO_SNDTIMEO do NOT bound
  // connect(2) on POSIX, so use non-blocking connect + select with a 2s
  // deadline instead of relying on the (ineffective) socket timeout.
  SetSocketTimeoutMs(fd, 2000);
#if defined(_WIN32)
  {
    u_long mode = 1;
    ::ioctlsocket(fd, FIONBIO, &mode);
  }
#else
  {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
#endif
  int conn_rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), addr_len);
  if (conn_rc != 0) {
#if defined(_WIN32)
    int wsa_err = ::WSAGetLastError();
    bool in_progress = (wsa_err == WSAEWOULDBLOCK || wsa_err == WSAEINPROGRESS);
#else
    bool in_progress = (errno == EINPROGRESS);
#endif
    if (!in_progress) {
      std::string why = SocketErrorString();
      CloseSocket(fd);
      return Status::NetworkError("connect() to " + host + ":" + std::to_string(port) + " failed: " + why);
    }
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    int sel = ::select(static_cast<int>(fd) + 1, nullptr, &wfds, nullptr, &tv);
    if (sel <= 0) {
      CloseSocket(fd);
      return Status::NetworkError("connect() to " + host + ":" + std::to_string(port) +
                                  " timed out after 2s");
    }
    int so_err = 0;
    dsn_socklen_t so_len = sizeof(so_err);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR,
#if defined(_WIN32)
                 reinterpret_cast<char*>(&so_err),
#else
                 &so_err,
#endif
                 &so_len);
    if (so_err != 0) {
      CloseSocket(fd);
      return Status::NetworkError("connect() to " + host + ":" + std::to_string(port) + " failed");
    }
  }
#if defined(_WIN32)
  {
    u_long mode = 0;
    ::ioctlsocket(fd, FIONBIO, &mode);
  }
#else
  {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
  }
#endif

  auto hs = ClientHandshake(fd, *identity_, p2p_port_, cluster_secret_);
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
  // Key confirmation (H-5 fix, client side): binding happens only after the
  // server proves session-key possession with a valid encrypted response.
  if (resp_or.ok() && handshake_cb_) {
    handshake_cb_(hs.value().peer_node_id, hs.value().peer_ed25519_pubkey,
                  hs.value().peer_p2p_port);
  }
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
  // Drain the bounded pool so no connection worker outlives us (H-11 fix).
  // Workers hold only this + socket fds; Stop() joins them all.
  if (conn_pool_) conn_pool_->Stop();
}

}  // namespace desentry
