#pragma once
// The gossip anti-entropy replicator (ARCHITECTURE.md §7.5): the
// convergence *backstop*. Eager broadcast (net/network_manager.h) is what
// makes replication feel instant in the common case, but it's
// best-effort -- a peer that was offline, or a dropped packet, means an
// eager push never arrives. This periodic round is what guarantees
// eventual consistency regardless: every `interval_ms`, pick a random
// known peer and, per collection, exchange a digest of what each side has
// so both sides can pull whatever they're missing.

#include <atomic>
#include <cstdint>
#include <thread>

#include "desentry/engine/node_engine.h"
#include "desentry/net/peer.h"
#include "desentry/net/tcp_transport.h"

namespace desentry {

class GossipEngine {
 public:
  GossipEngine(NodeEngine* engine, PeerTable* peers, TcpTransport* transport, uint32_t interval_ms)
      : engine_(engine), peers_(peers), transport_(transport), interval_ms_(interval_ms) {}
  ~GossipEngine();

  void Start();
  void Stop();

  // Runs one round against a specific peer immediately (used by Start()'s
  // loop, and exposed for tests/CLI-triggered manual sync).
  void RunRoundWith(const PeerInfo& peer);

 private:
  void Loop();

  NodeEngine* engine_;
  PeerTable* peers_;
  TcpTransport* transport_;
  uint32_t interval_ms_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace desentry
