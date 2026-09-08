#include "desentry/ledger/checkpoint.h"

#include <algorithm>

namespace desentry {

size_t CheckpointQuorumSize(uint32_t replication_factor) {
  if (replication_factor <= 1) return 1;
  return replication_factor % 2 == 0 ? replication_factor / 2 + 1 : replication_factor;
}

CheckpointDecision EvaluateCheckpoint(const ReplicaTip& local,
                                      const std::vector<ReplicaTip>& replicas,
                                      uint32_t replication_factor) {
  CheckpointDecision result;
  result.required = CheckpointQuorumSize(replication_factor);
  result.checkpoint_lsn = local.entry_id;
  result.agreed_entry_hash = local.entry_hash;

  if (!local.self_verified || !local.signature_valid) {
    result.unverified = 1;
    result.reason = "local ledger is not verified";
    return result;
  }
  result.agreeing = 1;
  for (const ReplicaTip& replica : replicas) {
    if (!replica.self_verified || !replica.signature_valid) {
      ++result.unverified;
    } else if (replica.entry_id == local.entry_id && replica.entry_hash == local.entry_hash) {
      ++result.agreeing;
    } else if (replica.entry_id == local.entry_id) {
      ++result.conflicting;
    }
  }
  if (result.conflicting != 0) {
    result.reason = "replicas report conflicting ledger tips";
  } else if (result.agreeing < result.required) {
    result.reason = "not enough verified replicas agree on the ledger tip";
  } else {
    result.proceed = true;
    result.reason = "quorum agrees on the ledger tip";
  }
  return result;
}

std::vector<std::string> UnclaimedIntentsThrough(const std::vector<WalRecord>& entries,
                                                 lsn_t checkpoint_lsn) {
  std::vector<std::string> intents;
  for (const WalRecord& entry : entries) {
    if (entry.lsn > checkpoint_lsn || entry.type != WalRecordType::kTransitIntent) continue;
    if (std::find_if(entries.begin(), entries.end(), [&](const WalRecord& claimed) {
          return claimed.type == WalRecordType::kTransitClaimed &&
                 claimed.key_hash == entry.key_hash;
        }) == entries.end()) {
      if (std::find(intents.begin(), intents.end(), entry.key_hash) == intents.end()) {
        intents.push_back(entry.key_hash);
      }
    }
  }
  return intents;
}

StatusOr<CheckpointOutcome> RunCheckpoint(WriteAheadLog* wal, TransitStore* transit,
                                          const ReplicaTip& local,
                                          const std::vector<ReplicaTip>& replicas,
                                          uint32_t replication_factor,
                                          const HLCTimestamp& hlc) {
  if (wal == nullptr || transit == nullptr) {
    return Status::InvalidArgument("checkpoint requires a WAL and transit store");
  }
  CheckpointOutcome outcome;
  outcome.decision = EvaluateCheckpoint(local, replicas, replication_factor);
  if (!outcome.decision.proceed) return outcome;
  outcome.previous_tip_hash = wal->Tip().entry_hash;

  auto checkpoint_or =
      wal->Append(WalRecordType::kCheckpoint, "", "", "", WriteAheadLog::AppendOptions{hlc, {}});
  if (!checkpoint_or.ok()) return checkpoint_or.status();
  outcome.checkpoint_entry_id = checkpoint_or.value();
  auto pruned_or = wal->Prune(outcome.decision.checkpoint_lsn);
  if (!pruned_or.ok()) return pruned_or.status();
  outcome.entries_pruned = pruned_or.value().dropped;
  outcome.new_tip_hash = pruned_or.value().new_tip_hash;
  return outcome;
}

}  // namespace desentry
