#pragma once
// The app-local supervisor (docs/architecture-v2.md Sec 3).
//
// A supervisor is an ordinary `desentryd` process started with
// `"supervisor": true`. What makes it a supervisor is entirely a matter of
// what it *does* -- and, just as importantly, what it is structurally
// prevented from doing:
//
//   * It binds its API to loopback only. NodeConfig::Validate() rejects any
//     other bind address for a supervisor, so this is enforced at config
//     load, not merely documented.
//   * The placement layer skips it (net/placement.cpp), so no key is ever
//     assigned to it. It holds no replicated data.
//   * Gossip skips it, so it is never a replication hop.
//   * It is never elected, and nothing waits on it. If every supervisor on
//     the network is down, writes, reads, replication and convergence all
//     continue exactly as before; only garbage collection and hardware
//     discovery pause.
//
// That last property is the whole design. docs/comparison.md Sec 2 records
// why a coordinator ("ROOT") node was deliberately not adopted: it
// reintroduces a soft single point of failure on the write path. A
// supervisor is not a coordinator because it is not on the write path at
// all -- it is the control plane the desktop app drives, and the data plane
// stays flat and leaderless underneath it.
//
// Responsibilities:
//   1. Hardware discovery  -- folder scan for candidate data directories,
//                             USB mount detection, LAN peer enumeration.
//   2. Lifecycle           -- driving nodes through discovered -> allocated
//                             -> provisioned -> running -> degraded ->
//                             reclaimed, with validated transitions.
//   3. Key custody         -- recording that a recovery key was exported
//                             (never the key itself).
//   4. Quota enforcement   -- marking nodes degraded when they exceed budget.
//   5. Checkpoint + GC     -- the quorum-gated ledger prune
//                             (ledger/checkpoint.h).

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "desentry/api/http_server.h"
#include "desentry/common/json.h"
#include "desentry/common/status.h"
#include "desentry/engine/node_engine.h"
#include "desentry/net/network_manager.h"

namespace desentry {

// A directory that looks like it either is, or could become, a node's data
// directory.
struct DataDirCandidate {
  std::string path;
  bool has_node_config = false;   // node.json present
  bool has_identity = false;      // identity.key present
  bool has_data_file = false;     // desentry.dsf (or an engine subdirectory) present
  bool has_manifest = false;      // manifest.json -- a portable/USB node
  bool is_removable = false;      // sits under a detected removable mount
  bool encrypted = false;         // manifest declares at-rest encryption
  std::string node_name;          // from node.json, when present
  std::string node_id;            // from manifest.json, when present
  uint64_t free_bytes = 0;
  uint64_t used_bytes = 0;

  // A directory with data but no identity is not a De-Sentry node -- it is
  // someone else's folder that happens to contain a file we recognise. Being
  // strict here is what stops the app offering to "adopt" a user's Documents
  // folder.
  bool LooksLikeExistingNode() const { return has_identity && (has_node_config || has_data_file); }
  bool LooksAdoptable() const { return !LooksLikeExistingNode(); }
  JsonValue ToJson() const;
};

// A mounted volume the app may offer as a home for a portable node.
struct MountPoint {
  std::string path;
  std::string label;
  bool removable = false;
  uint64_t total_bytes = 0;
  uint64_t free_bytes = 0;
  JsonValue ToJson() const;
};

// Enumerates local storage. Deliberately read-only and side-effect free:
// scanning must never create, move or modify anything, so an accidental scan
// of the wrong directory tree is harmless.
class HardwareScanner {
 public:
  // Roots scanned when the caller does not name any: the app data directory,
  // the user's home, and every detected mount point. Depth-limited so a scan
  // of "/" cannot walk an entire filesystem.
  static constexpr size_t kMaxScanDepth = 3;
  static constexpr size_t kMaxCandidates = 512;

  static std::vector<MountPoint> ListMounts();
  static std::vector<DataDirCandidate> ScanRoots(const std::vector<std::string>& roots,
                                                   size_t max_depth = kMaxScanDepth);
  static std::vector<DataDirCandidate> ScanDefaults();

  // Inspects one directory without recursing -- what the creation wizard
  // calls when the user picks a folder by hand.
  static DataDirCandidate Inspect(const std::string& path);

  // Reads a portable node's manifest.json, if present.
  static StatusOr<JsonValue> ReadManifest(const std::string& dir);
  // Writes one. Called when the app provisions a node onto removable media,
  // so the same USB stick plugged into another machine is recognisable.
  static Status WriteManifest(const std::string& dir, const JsonValue& manifest);
};

// Bookkeeping the supervisor keeps about each node it manages. Persisted as
// JSON alongside the supervisor's own data directory, so the app's node list
// survives a restart of either the app or the supervisor.
struct ManagedNode {
  std::string node_id;
  std::string node_name;
  std::string data_dir;
  uint16_t api_port = 0;
  uint16_t p2p_port = 0;
  NodeLifecycleState state = NodeLifecycleState::kDiscovered;
  bool removable = false;
  bool encrypted = false;
  // Key custody: whether the user has exported a recovery key for this node,
  // and when. The key itself is never stored here or anywhere else on disk --
  // there is no escrow (docs/architecture-v2.md Sec 6.3). This records only
  // that the export happened, so the app can refuse to finish creating a node
  // whose recovery key was never taken.
  bool recovery_key_exported = false;
  int64_t recovery_key_exported_ms = 0;
  std::string keychain_ref;
  int64_t created_ms = 0;
  int64_t last_seen_ms = 0;
  std::string last_error;

  JsonValue ToJson() const;
  static ManagedNode FromJson(const JsonValue& v);
};

class Supervisor {
 public:
  struct Options {
    // How often the housekeeping pass runs. Everything it does is idempotent
    // and cheap; the interval is about responsiveness, not correctness.
    uint32_t interval_ms = 15000;
    // Attempt a checkpoint at most this often. Checkpointing costs a full
    // ledger read plus a round of tip collection, so it runs far less often
    // than the housekeeping pass.
    uint32_t checkpoint_interval_ms = 300000;  // 5 minutes
    // A node over this fraction of its quota is marked degraded, so the app
    // can warn before writes start failing rather than after.
    double quota_degraded_fraction = 0.90;
    std::string registry_path;  // where ManagedNode records are persisted
  };

  Supervisor(NodeEngine* engine, NetworkManager* network, const Options& options);
  ~Supervisor();

  Status Start();
  void Stop();

  // Runs one housekeeping pass immediately. Exposed so the app can force a
  // pass (and so tests do not have to wait for a timer).
  struct PassReport {
    size_t peers_marked_degraded = 0;
    size_t transit_expired = 0;
    size_t nodes_over_quota = 0;
    bool checkpoint_attempted = false;
    bool checkpoint_proceeded = false;
    std::string checkpoint_reason;
    uint64_t entries_pruned = 0;
  };
  PassReport RunOnce();

  // -- managed node registry -------------------------------------------------
  std::vector<ManagedNode> Nodes() const;
  StatusOr<ManagedNode> GetNode(const std::string& node_id) const;
  Status UpsertNode(const ManagedNode& node);
  // Advances a node's lifecycle state, rejecting an invalid transition rather
  // than silently accepting it (see ValidNodeTransition in net/peer.h).
  Status TransitionNode(const std::string& node_id, NodeLifecycleState to);
  Status RecordRecoveryKeyExport(const std::string& node_id);
  Status RemoveNode(const std::string& node_id);

  Status SaveRegistry();
  Status LoadRegistry();

 private:
  void Loop();
  size_t MarkStalePeersDegraded();
  size_t EnforceQuota();
  void AttemptCheckpoint(PassReport* report);

  NodeEngine* engine_;
  NetworkManager* network_;
  Options options_;

  std::atomic<bool> running_{false};
  std::thread thread_;
  int64_t last_checkpoint_ms_ = 0;

  mutable std::mutex mu_;
  std::vector<ManagedNode> nodes_;
};

// Registers the supervisor's own REST surface on top of the ordinary node
// routes. Only called when NodeConfig::supervisor is true.
void RegisterSupervisorRoutes(HttpServer* server, Supervisor* supervisor, NodeEngine* engine,
                               NetworkManager* network);

}  // namespace desentry
