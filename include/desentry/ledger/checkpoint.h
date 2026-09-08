#pragma once
// Quorum-gated ledger checkpoint (docs/architecture-v2.md Sec 5.3).
//
// Checkpointing is the one operation in the system that deletes history: it
// prunes settled TRANSIT_INTENT / TRANSIT_CLAIMED pairs out of the ledger
// (WriteAheadLog::Prune) and releases the corresponding transit envelopes.
// It is therefore gated twice:
//
//   1. **Quorum.** 2f+1 matching, self-verified tips with f = (rf-1)/2. At
//      RF=3 that is all three replicas: unanimity among the odd core rather
//      than a bare majority, because the cost of waiting is retained garbage
//      and the cost of being wrong is deleted history.
//   2. **No unclaimed intents.** Every TRANSIT_INTENT strictly below the
//      checkpoint must have a matching TRANSIT_CLAIMED at or below it.
//      Pruning an unclaimed intent would throw away a write an offline node
//      has not collected -- data loss dressed up as garbage collection.
//
// Three refusal cases are distinguished, and the distinction is the point:
//
//   * A conflicting tip (same height, different hash) aborts entirely: two
//     nodes have different histories, and pruning would destroy the evidence
//     needed to work out why. There is deliberately no fallback to a lower
//     checkpoint in this case.
//   * A lagging (or ahead) peer is not a conflict: it simply has not caught
//     up (or has entries we have not seen). It does not vote, but it does
//     not veto either.
//   * Falling short of quorum otherwise refuses with the count attached, so
//     the caller can see how far off agreement it was.
//
// An unverified replica is not a vote: a node reporting the right hash
// without having verified its own chain is asserting agreement it has not
// checked. A tip carrying a non-empty signature that does not verify is not
// a vote either; an unsigned tip whose chain segment verified locally may
// still vote (that is the only kind the ledger-digest path produces --
// NetworkManager::CollectReplicaTips checks the chain links itself rather
// than trusting a boolean, which is the stronger check).
//
// Only supervisors run this (routes.cpp refuses it with 403 elsewhere, and
// Supervisor::AttemptCheckpoint drives the periodic pass). This header is the
// only production caller of WriteAheadLog::Prune, which refuses nothing on
// its own.

#include <cstdint>
#include <string>
#include <vector>

#include "desentry/common/status.h"
#include "desentry/crdt/hlc.h"
#include "desentry/ledger/transit_store.h"
#include "desentry/storage/page.h"
#include "desentry/storage/wal.h"

namespace desentry {

// One replica's reported ledger tip, as collected for a checkpoint vote.
struct ReplicaTip {
  std::string node_id;
  lsn_t entry_id = kInvalidLsn;
  std::string entry_hash;  // 32 raw bytes
  bool self_verified = false;
  bool signature_valid = false;
  std::string signature;  // Ed25519 over "<entry_id>:<entry_hash>"; may be empty
};

// The gate's verdict. On refusal, `reason` always says why and the counts
// say how far off agreement the mesh was.
struct CheckpointDecision {
  bool proceed = false;
  int agreeing = 0;    // replicas matching the local tip (plus the local node itself)
  int required = 0;    // CheckpointQuorumSize(replication_factor)
  int conflicting = 0;  // same height, different hash: aborts unconditionally
  int unverified = 0;  // matching tip but not a countable vote
  lsn_t checkpoint_lsn = kInvalidLsn;  // agreed tip height; valid only when proceed
  std::string agreed_entry_hash;       // agreed tip hash; valid only when proceed
  std::string reason;
};

// The full outcome of a checkpoint attempt: the verdict plus what the prune
// did. Prune-related fields are zero/empty unless decision.proceed.
struct CheckpointOutcome {
  CheckpointDecision decision;
  lsn_t checkpoint_entry_id = kInvalidLsn;  // LSN of the CHECKPOINT marker written
  uint64_t entries_pruned = 0;
  uint64_t envelopes_released = 0;
  std::string previous_tip_hash;  // 32 raw bytes, pre-prune tip
  std::string new_tip_hash;       // 32 raw bytes, post-prune tip
};

// 2f+1 with f = (rf-1)/2 (integer division): 1->1, 3->3, 5->5, 4->3.
int CheckpointQuorumSize(uint32_t replication_factor);

// Pure evaluation of the gate: no I/O, no pruning. Exported so the refusal
// paths are unit-testable without a ledger on disk.
CheckpointDecision EvaluateCheckpoint(const ReplicaTip& local,
                                      const std::vector<ReplicaTip>& replicas,
                                      uint32_t replication_factor);

// key_hash values (32 raw bytes each) of TRANSIT_INTENT entries with
// lsn < checkpoint_lsn that have no matching TRANSIT_CLAIMED entry with
// lsn <= checkpoint_lsn. First-seen order, deduplicated. Empty means the
// prefix is safe to prune.
std::vector<std::string> UnclaimedIntentsBelow(const std::vector<WalRecord>& entries,
                                               lsn_t checkpoint_lsn);

// Evaluates the gate and, on agreement, appends a CHECKPOINT marker, prunes
// settled transit pairs through the agreed tip, and releases the envelopes
// whose intent/claimed pairs the checkpoint covers. `now` stamps the marker.
// Neither `wal` nor `transit` may be null.
StatusOr<CheckpointOutcome> RunCheckpoint(WriteAheadLog* wal, TransitStore* transit,
                                          const ReplicaTip& local,
                                          const std::vector<ReplicaTip>& replicas,
                                          uint32_t replication_factor, const HLCTimestamp& now);

}  // namespace desentry
