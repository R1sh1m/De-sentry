#pragma once
// The set of peers this node currently knows about, learned via UDP
// discovery broadcasts (net/udp_discovery.h), mDNS-style hostname
// advertisements, QR-code pairing, and/or the static bootstrap_peers config
// list.
//
// v2 adds **fitness**: a small, continuously-updated set of measurements per
// peer (latency, success rate, ledger freshness, free quota) that the
// placement layer (net/placement.h) uses to break ties and that the app's
// mesh view renders. Two things are worth stating plainly about it:
//
//   * Fitness is **advisory, never authoritative**. It orders candidates; it
//     never decides whether a write is accepted, and no peer's fitness score
//     grants it any authority over another's data. The data plane stays flat
//     -- see docs/comparison.md Sec 2 for why a fitness-ranked coordinator was
//     deliberately not adopted.
//   * Fitness is **locally measured**, not gossiped as a claim. latency_ms
//     and success_rate come from this node's own probes; ledger freshness
//     and free quota come from the peer's signed /_brain, which the peer
//     could inflate -- so those two only ever *lower* a peer's rank in
//     practice (a peer claiming impossible freshness still has to serve the
//     entries when asked, and failing to does hurt its success_rate).
//
// This is still deliberately not a consensus membership protocol: there is
// no SWIM-style failure detector beyond last-seen staleness. See
// ARCHITECTURE.md Sec 9.7 for the Kademlia-DHT upgrade path once cluster size
// outgrows a LAN broadcast domain.

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "desentry/storage/page.h"

namespace desentry {

// The node lifecycle state machine the supervisor drives
// (docs/architecture-v2.md Sec 3.4). A node advances
// discovered -> allocated -> provisioned -> running, may fall to degraded
// and recover, and ends at reclaimed. Transitions are validated by
// ValidNodeTransition() rather than being set freely, so an impossible jump
// (reclaimed -> running) is a caught bug and not a mystery in the UI.
enum class NodeLifecycleState : uint8_t {
  kDiscovered = 0,   // hardware or a peer was found; nothing has been done with it
  kAllocated = 1,    // the user chose it; ports/quota reserved, nothing written
  kProvisioned = 2,  // data_dir, identity.key and node.json exist on disk
  kRunning = 3,      // desentryd is up and answering /_status
  kDegraded = 4,     // reachable but failing health checks (quota, verify, staleness)
  kReclaimed = 5,    // decommissioned; its data has been re-replicated elsewhere
};

const char* NodeLifecycleStateName(NodeLifecycleState state);
bool ValidNodeTransition(NodeLifecycleState from, NodeLifecycleState to);

// Locally-measured (and partly peer-reported) fitness signals. See the
// header comment for which is which and why it matters.
struct PeerFitness {
  double latency_ms = 0.0;             // exponentially-weighted mean of probe RTTs
  double success_rate = 1.0;           // EW mean of probe outcomes, in [0, 1]
  lsn_t ledger_freshness_entry_id = kInvalidLsn;  // peer's reported ledger tip
  uint64_t free_quota_mb = 0;          // peer's reported remaining budget
  // True once the peer actually reported a quota figure (RecordReport).
  // Guards the supervisor's out-of-space marking: without it, "no report
  // yet" (zero) is indistinguishable from "no space left" (zero), and every
  // healthy peer -- including every unlimited-quota node -- reads as full.
  bool quota_reported = false;
  uint64_t probes = 0;
  int64_t updated_ms = 0;

  // A single comparable number in [0, 1], higher is better. Composed so that
  // no term can dominate: reliability is weighted hardest because an
  // unreachable peer's latency is meaningless, and capacity is weighted
  // least because it is the one signal a peer self-reports without proof.
  double Score(lsn_t network_max_entry_id) const;
};

struct PeerInfo {
  std::string node_id;
  std::string ed25519_pubkey;  // empty until we've actually handshaked with them
  std::string host;
  uint16_t p2p_port = 0;
  uint16_t api_port = 0;       // learned from pairing/mDNS; 0 if unknown
  std::string hostname;        // mDNS-style label, e.g. "studio-imac.local"
  int64_t last_seen_ms = 0;
  bool is_supervisor = false;  // app-local supervisors are never placement targets
  NodeLifecycleState state = NodeLifecycleState::kDiscovered;
  PeerFitness fitness;
};

class PeerTable {
 public:
  void Upsert(const PeerInfo& info);
  std::vector<PeerInfo> List() const;
  size_t Size() const;
  bool Contains(const std::string& node_id) const;
  bool Get(const std::string& node_id, PeerInfo* out) const;

  // -- v2 ------------------------------------------------------------------
  // Folds one probe result into a peer's fitness. `ok == false` records a
  // failure without disturbing the latency estimate (a timeout's "latency"
  // is not a measurement of anything).
  void RecordProbe(const std::string& node_id, double latency_ms, bool ok);

  // Records the capacity/freshness figures a peer reported via /_brain.
  void RecordReport(const std::string& node_id, lsn_t ledger_entry_id, uint64_t free_quota_mb);

  // Records only the peer's ledger height, without touching its quota
  // figures. Used by the gossip path, which learns heights from ledger
  // deltas but never sees a quota report there.
  void RecordLedgerHeight(const std::string& node_id, lsn_t ledger_entry_id);

  // Re-keys a `bootstrap#host:port` placeholder under the identity a
  // handshake proved for that address. Without this, bootstrap entries keep
  // their synthetic ids forever: placement skips them (so the peer is never
  // a replica target), staleness polls by real id never match, and the
  // public-key resolver never finds them. Merges into an existing real entry
  // when one is already there rather than duplicating the peer. Returns false
  // when there is nothing to adopt (unknown old id, or already real).
  bool AdoptIdentity(const std::string& old_id, const std::string& real_id);

  void SetState(const std::string& node_id, NodeLifecycleState state);

  // Peers ordered best-first by PeerFitness::Score(). Used for gossip peer
  // selection and as the tie-break in placement.
  std::vector<PeerInfo> Ranked() const;

  // Peers not seen for longer than `stale_ms`. The supervisor marks these
  // degraded; nothing here removes them, because a peer that reappears must
  // keep its identity and its fitness history.
  std::vector<PeerInfo> StalerThan(int64_t stale_ms, int64_t now_ms) const;

  // Highest ledger tip any known peer has reported -- the denominator for
  // the freshness term in Score().
  lsn_t NetworkMaxLedgerEntryId() const;

  // Exponential weighting factor for latency/success updates. 0.25 keeps
  // roughly the last dozen probes in view: responsive enough to notice a
  // peer going bad, damped enough that one slow round does not reorder the
  // whole table.
  static constexpr double kFitnessAlpha = 0.25;

 private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, PeerInfo> peers_;
};

}  // namespace desentry
