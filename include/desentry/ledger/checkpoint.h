#pragma once

#include <string>
#include <vector>

#include "desentry/common/status.h"
#include "desentry/crdt/hlc.h"
#include "desentry/ledger/transit_store.h"
#include "desentry/storage/wal.h"

namespace desentry {

struct ReplicaTip {
  std::string node_id;
  lsn_t entry_id = kInvalidLsn;
  std::string entry_hash;
  std::string signature;
  bool self_verified = false;
  bool signature_valid = false;
};

struct CheckpointDecision {
  bool proceed = false;
  size_t agreeing = 0;
  size_t required = 0;
  size_t conflicting = 0;
  size_t unverified = 0;
  lsn_t checkpoint_lsn = kInvalidLsn;
  std::string agreed_entry_hash;
  std::string reason;
};

size_t CheckpointQuorumSize(uint32_t replication_factor);
CheckpointDecision EvaluateCheckpoint(const ReplicaTip& local,
                                      const std::vector<ReplicaTip>& replicas,
                                      uint32_t replication_factor);
std::vector<std::string> UnclaimedIntentsThrough(const std::vector<WalRecord>& entries,
                                                 lsn_t checkpoint_lsn);

struct CheckpointOutcome {
  CheckpointDecision decision;
  lsn_t checkpoint_entry_id = kInvalidLsn;
  uint64_t entries_pruned = 0;
  uint64_t envelopes_released = 0;
  std::string previous_tip_hash;
  std::string new_tip_hash;
};

StatusOr<CheckpointOutcome> RunCheckpoint(WriteAheadLog* wal, TransitStore* transit,
                                          const ReplicaTip& local,
                                          const std::vector<ReplicaTip>& replicas,
                                          uint32_t replication_factor,
                                          const HLCTimestamp& hlc);

}  // namespace desentry
