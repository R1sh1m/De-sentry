#pragma once
// P2P TCP transport. Deliberately short-lived-connection, request/response
// (not a persistent multiplexed stream): every exchange is
// connect -> secure handshake -> one encrypted request -> one encrypted
// response -> close. See wire_protocol.h's header comment for why -- it
// trades a little connect()/handshake overhead per exchange for a
// concurrency model simple enough to reason about with confidence
// (thread-per-connection, no shared mutable stream state), which matters
// more than that overhead at the MVP's scale. Pooling persistent
// connections is the natural, explicitly-deferred v2 optimization.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "desentry/common/status.h"
#include "desentry/common/worker_pool.h"
#include "desentry/net/identity.h"
#include "desentry/net/wire_protocol.h"

namespace desentry {

// Handles one inbound request and returns the response to send back.
// Receives the *authenticated* peer_node_id (proven by the handshake
// signature, not merely claimed) so handlers can trust it.
using RequestHandler = std::function<WireMessage(const std::string& peer_node_id, const WireMessage& request)>;

class TcpTransport {
 public:
  // Called after a successful handshake (both directions) with the proven
  // peer identity. Used to bind node_id -> pubkey in the peer table (C-2).
  using HandshakeCallback =
      std::function<void(const std::string& node_id, const std::string& pubkey, uint16_t p2p_port)>;
  TcpTransport(const NodeIdentity* identity, uint16_t p2p_port) : identity_(identity), p2p_port_(p2p_port) {}
  ~TcpTransport();

  Status StartListening(const std::string& bind_addr, RequestHandler handler);
  void SetHandshakeCallback(HandshakeCallback cb) { handshake_cb_ = std::move(cb); }
  // Cluster-membership secret (empty = open mesh). Must be set before
  // StartListening; consulted on every handshake.
  void SetClusterSecret(std::string secret) { cluster_secret_ = std::move(secret); }
  void Stop();

  // Blocking: dials out, handshakes as client, sends one request, waits for
  // one response, closes. Safe to call from multiple threads concurrently
  // (each call owns its own socket).
  StatusOr<WireMessage> SendRequest(const std::string& host, uint16_t port, const WireMessage& request);

  uint16_t port() const { return p2p_port_; }

 private:
  void AcceptLoop(dsn_socket_t listen_fd);
  void HandleConnection(dsn_socket_t client_fd);

  const NodeIdentity* identity_;
  uint16_t p2p_port_;
  RequestHandler handler_;
  HandshakeCallback handshake_cb_;
  // Bounded inbound handling (C-4/H-11 fix): fixed pool instead of
  // thread-per-connection; Stop() shuts the pool down so no detached thread
  // outlives this object.
  std::unique_ptr<WorkerPool> conn_pool_;
  std::string cluster_secret_;
  std::atomic<int> active_conns_{0};
  static constexpr int kMaxInboundConns = 64;
  static constexpr int kInboundTimeoutMs = 5000;
  std::atomic<bool> running_{false};
  dsn_socket_t listen_fd_ = kInvalidSocket;
  std::thread accept_thread_;
};

}  // namespace desentry
