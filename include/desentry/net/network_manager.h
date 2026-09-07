#pragma once
// Top-level network component: owns the TCP transport, UDP discovery, peer
// table, placement policy and gossip engine, and is the single place that
// dispatches inbound P2P requests against the NodeEngine. It is also what
// wires NodeEngine's local-write hook to eager broadcast -- the only place
// those two layers touch, kept deliberately narrow (see node_engine.h on why
// the hook is a callback rather than a direct include).
//
// v2 responsibilities added here, all of which need both the peer table and
// the engine in scope:
//
//   * **Bounded eager broadcast.** A fixed worker pool instead of a detached
//     thread per peer per write, a message id on every push, and a small
//     relay TTL. Dedup is what makes relaying safe; without it, a mesh
//     re-broadcasts forever.
//   * **Admission control.** A token bucket per authenticated peer, checked
//     before a request is decoded.
//   * **The ACL byte filter.** A peer that is not a reader of a private
//     collection gets ledger *hashes* and nothing else -- no keys, no
//     document bytes. This is enforced here, on the serving side, not by
//     asking the requester to behave.
//   * **The transit flow.** Answering "are you holding anything for me?",
//     serving the bytes, and recording claims.
//   * **Fitness probing.** Periodic pings that feed PeerTable::RecordProbe,
//     and /_brain-derived capacity reports.

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "desentry/common/config.h"
#include "desentry/common/status.h"
#include "desentry/common/worker_pool.h"
#include "desentry/engine/node_engine.h"
#include "desentry/ledger/checkpoint.h"
#include "desentry/net/admission.h"
#include "desentry/net/gossip.h"
#include "desentry/net/peer.h"
#include "desentry/net/placement.h"
#include "desentry/net/tcp_transport.h"
#include "desentry/net/udp_discovery.h"

namespace desentry {

class NetworkManager {
 public:
  NetworkManager(NodeEngine* engine, const NodeConfig& config) : engine_(engine), config_(config) {}
  ~NetworkManager();

  Status Start();
  void Stop();

  PeerTable& peers() { return peer_table_; }
  TcpTransport* transport() { return transport_.get(); }
  PlacementPolicy& placement() { return *placement_; }
  const NodeConfig& config() const { return config_; }

  // Recomputes the placement ring from current membership. Called on
  // membership change rather than per write.
  void RebuildPlacement();

  // Asks every known peer whether it is holding transit bytes for us, pulls
  // what it has, applies it, and reports the claims back. This is what a
  // returning node runs on startup and whenever it rejoins the mesh --
  // step 3 of the offline-owner flow in docs/architecture-v2.md Sec 5.
  struct ClaimReport {
    size_t peers_asked = 0;
    size_t documents_claimed = 0;
    size_t failures = 0;
  };
  ClaimReport ClaimPendingTransit();

  // Collects signed ledger tips from every peer, for the supervisor's
  // checkpoint quorum check.
  std::vector<ReplicaTip> CollectReplicaTips();

  // Broadcast/relay statistics, surfaced by GET /_status so a saturated
  // fan-out is visible rather than silent.
  struct BroadcastStats {
    uint64_t sent = 0;
    uint64_t dropped = 0;
    uint64_t duplicates_suppressed = 0;
    uint64_t rate_limited = 0;
    size_t queued = 0;
  };
  BroadcastStats broadcast_stats() const;

 private:
  WireMessage HandleRequest(const std::string& peer_node_id, const WireMessage& request);
  WireMessage HandleDigest(const std::string& peer_node_id, const DigestPayload& digest);
  WireMessage HandleOpBroadcast(const std::string& peer_node_id, const OpBroadcastPayload& broadcast);
  WireMessage HandleTransitQuery(const std::string& peer_node_id);
  WireMessage HandleTransitClaim(const std::string& peer_node_id, const TransitClaimPayload& claim);
  WireMessage HandleLedgerDigest(const std::string& peer_node_id, const LedgerDigestPayload& digest);

  void BroadcastLocalWrite(const std::string& collection, const std::string& key,
                            const std::string& encoded_doc);
  void FanOut(const OpBroadcastPayload& payload, const std::string& exclude_node_id);
  // Bytes for a key whose placement targets an unreachable owner are held on
  // this node instead of being dropped.
  void HoldForUnreachableOwners(const std::string& collection, const std::string& key,
                                 const std::string& encoded_doc);
  void ProbeLoop();

  NodeEngine* engine_;
  NodeConfig config_;
  PeerTable peer_table_;
  std::unique_ptr<TcpTransport> transport_;
  std::unique_ptr<UdpDiscovery> discovery_;
  std::unique_ptr<GossipEngine> gossip_;
  std::unique_ptr<PlacementPolicy> placement_;
  std::unique_ptr<WorkerPool> broadcast_pool_;
  std::unique_ptr<TokenBucketLimiter> limiter_;
  std::unique_ptr<MessageDedup> dedup_;

  std::atomic<bool> running_{false};
  std::thread probe_thread_;
  std::atomic<uint64_t> broadcasts_sent_{0};
  std::atomic<uint64_t> duplicates_suppressed_{0};
  std::atomic<uint64_t> rate_limited_{0};
};

}  // namespace desentry
