#pragma once
// The gossip anti-entropy replicator (ARCHITECTURE.md Sec 7.5): the
// convergence *backstop*. Eager broadcast (net/network_manager.h) is what
// makes replication feel instant in the common case, but it is best-effort --
// a peer that was offline, a dropped packet, or a full broadcast queue means
// an eager push never arrives. This periodic round is what guarantees
// eventual consistency regardless.
//
// v2 changes three things, all driven by the 50-nodes-on-one-LAN target:
//
//   * **Ranked peer selection.** v1 picked a uniformly random peer. v2 picks
//     from the fitness-ranked head of the table most of the time, but
//     deliberately keeps an exploration fraction that picks uniformly --
//     without it, a peer that had one bad round could be starved of contact
//     forever and never get a chance to prove it recovered.
//
//   * **Jittered intervals.** Fifty nodes started by the same script with
//     the same interval will synchronise into a thundering herd. Jittering
//     each node's sleep by +/- jitter_pct breaks that up. This is the same
//     reason every production gossip implementation jitters.
//
//   * **Ledger digest exchange.** Alongside the per-collection document
//     digest, peers exchange ledger tips and entry hashes. This is what
//     makes the *hash set* converge even between peers that share no
//     readable collections at all -- the property that lets a node verify
//     the network's history without being able to read its contents.

#include <atomic>
#include <cstdint>
#include <random>
#include <thread>

#include "desentry/engine/node_engine.h"
#include "desentry/net/peer.h"
#include "desentry/net/tcp_transport.h"

namespace desentry {

class GossipEngine {
 public:
  struct Options {
    uint32_t interval_ms = 2000;
    // Percentage of the interval to jitter by, in each direction.
    uint32_t jitter_pct = 25;
    // Peers contacted per round. Two is the standard choice: one is fragile
    // to a single unlucky pick, and more than a few costs bandwidth without
    // materially improving the (already logarithmic) convergence time.
    uint32_t fanout = 2;
    // Fraction of picks (in percent) made uniformly at random rather than
    // from the ranked head. See the header comment on why this is not zero.
    uint32_t exploration_pct = 20;
  };

  GossipEngine(NodeEngine* engine, PeerTable* peers, TcpTransport* transport, const Options& options)
      : engine_(engine), peers_(peers), transport_(transport), options_(options) {}
  ~GossipEngine();

  void Start();
  void Stop();

  // Runs one full round against a specific peer immediately (used by the
  // loop, and exposed for tests and CLI-triggered manual sync).
  void RunRoundWith(const PeerInfo& peer);

  // The two halves of a round, separately callable so a test can exercise
  // ledger convergence without document convergence and vice versa.
  void ExchangeDocuments(const PeerInfo& peer);
  void ExchangeLedger(const PeerInfo& peer);

  uint64_t rounds_completed() const { return rounds_.load(); }

  // Battery/idle throttling: the app's background mode multiplies the
  // interval rather than stopping gossip, so a laptop on battery still
  // converges, just less eagerly. A factor of 1 is normal operation.
  void SetThrottleFactor(uint32_t factor);
  uint32_t throttle_factor() const { return throttle_.load(); }

 private:
  void Loop();
  std::vector<PeerInfo> SelectPeers(std::mt19937* rng);
  uint32_t NextSleepMs(std::mt19937* rng) const;

  NodeEngine* engine_;
  PeerTable* peers_;
  TcpTransport* transport_;
  Options options_;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> rounds_{0};
  std::atomic<uint32_t> throttle_{1};
  std::thread thread_;
};

}  // namespace desentry
