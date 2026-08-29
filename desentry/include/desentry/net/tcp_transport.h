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
#include "desentry/net/identity.h"
#include "desentry/net/wire_protocol.h"

namespace desentry {

// Handles one inbound request and returns the response to send back.
// Receives the *authenticated* peer_node_id (proven by the handshake
// signature, not merely claimed) so handlers can trust it.
using RequestHandler = std::function<WireMessage(const std::string& peer_node_id, const WireMessage& request)>;

class TcpTransport {
 public:
  TcpTransport(const NodeIdentity* identity, uint16_t p2p_port) : identity_(identity), p2p_port_(p2p_port) {}
  ~TcpTransport();

  Status StartListening(const std::string& bind_addr, RequestHandler handler);
  void Stop();

  // Blocking: dials out, handshakes as client, sends one request, waits for
  // one response, closes. Safe to call from multiple threads concurrently
  // (each call owns its own socket).
  StatusOr<WireMessage> SendRequest(const std::string& host, uint16_t port, const WireMessage& request);

  uint16_t port() const { return p2p_port_; }

 private:
  void AcceptLoop(int listen_fd);
  void HandleConnection(int client_fd);

  const NodeIdentity* identity_;
  uint16_t p2p_port_;
  RequestHandler handler_;
  std::atomic<bool> running_{false};
  int listen_fd_ = -1;
  std::thread accept_thread_;
};

}  // namespace desentry
