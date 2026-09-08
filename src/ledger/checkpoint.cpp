// Quorum-gated checkpoint implementation. EvaluateCheckpoint() is pure
// gate logic (unit-testable without a ledger); RunCheckpoint() is the only
// production path that prunes. See the header for the gate's rules.

#include "desentry/ledger/checkpoint.h"

#include <set>

#include "desentry/common/logger.h"

namespace desentry {

int CheckpointQuorumSize(uint32_t replication_factor) {
  if (replication_factor == 0) return 1;
  const int64_t f = (static_cast<int64_t>(replication_factor) - 1) / 2;
  return static_cast<int>(2 * f + 1);
}

CheckpointDecision EvaluateCheckpoint(const ReplicaTip& local,
                                      const std::vector<ReplicaTip>& replicas,
                                      uint32_t replication_factor) {
  CheckpointDecision decision;
  decision.required = CheckpointQuorumSize(replication_factor);

  if (local.entry_id < 0) {
    decision.reason = "ledger is empty: nothing to checkpoint";
    return decision;
  }
  if (!local.self_verified) {
    // Pruning a chain this node has not verified would delete history on the
    // authority of an unchecked ledger -- the exact failure the gate exists
    // to prevent.
    decision.reason = "local ledger did not verify: refusing to prune an unchecked chain";
    return decision;
  }

  // The local node votes for its own verified tip.
  decision.agreeing = 1;
  for (const ReplicaTip& replica : replicas) {
    if (replica.entry_id != local.entry_id) continue;  // lagging or ahead: neither vote nor veto
    if (replica.entry_hash != local.entry_hash) {
      // Same height, different hash: divergent histories. This aborts rather
      // than falling back to a lower checkpoint, because pruning would
      // destroy the evidence needed to work out what happened. Counted
      // whether or not the reporter verified: a dissenting vote is
      // dissenting even from a node that has not checked itself.
      ++decision.conflicting;
      continue;
    }
    if (replica.self_verified && (replica.signature.empty() || replica.signature_valid)) {
      ++decision.agreeing;
    } else {
      ++decision.unverified;
    }
  }

  if (decision.conflicting > 0) {
    decision.reason = "conflicting tip(s) at entry " + std::to_string(local.entry_id) +
                      ": replicas disagree on history; refusing to prune";
    return decision;
  }
  if (decision.agreeing < decision.required) {
    decision.reason = "quorum not reached: " + std::to_string(decision.agreeing) + " agreeing of " +
                      std::to_string(decision.required) + " required";
    return decision;
  }

  decision.proceed = true;
  decision.checkpoint_lsn = local.entry_id;
  decision.agreed_entry_hash = local.entry_hash;
  return decision;
}

std::vector<std::string> UnclaimedIntentsBelow(const std::vector<WalRecord>& entries,
                                               lsn_t checkpoint_lsn) {
  std::set<std::string> claimed;
  for (const WalRecord& rec : entries) {
    if (rec.lsn > checkpoint_lsn) break;
    if (rec.type == WalRecordType::kTransitClaimed) claimed.insert(rec.key_hash);
  }
  std::vector<std::string> unclaimed;
  std::set<std::string> seen;
  for (const WalRecord& rec : entries) {
    if (rec.lsn >= checkpoint_lsn) break;
    if (rec.type != WalRecordType::kTransitIntent) continue;
    if (claimed.count(rec.key_hash) != 0) continue;
    if (seen.insert(rec.key_hash).second) unclaimed.push_back(rec.key_hash);
  }
  return unclaimed;
}

StatusOr<CheckpointOutcome> RunCheckpoint(WriteAheadLog* wal, TransitStore* transit,
                                          const ReplicaTip& local,
                                          const std::vector<ReplicaTip>& replicas,
                                          uint32_t replication_factor, const HLCTimestamp& now) {
  if (wal == nullptr || transit == nullptr) {
    return Status::InvalidArgument("checkpoint requires a ledger and a transit store");
  }

  CheckpointOutcome outcome;
  outcome.decision = EvaluateCheckpoint(local, replicas, replication_factor);
  if (!outcome.decision.proceed) return outcome;

  auto entries_or = wal->ReadAll();
  if (!entries_or.ok()) return entries_or.status();
  const std::vector<WalRecord>& entries = entries_or.value();

  // Second gate: an unclaimed intent holds the prune back. Prune() itself
  // would keep the unclaimed entry, but checkpointing *past* it -- marking a
  // prefix settled while bytes in that prefix are still outstanding -- would
  // be data loss dressed up as garbage collection.
  const std::vector<std::string> unclaimed =
      UnclaimedIntentsBelow(entries, outcome.decision.checkpoint_lsn);
  if (!unclaimed.empty()) {
    outcome.decision.proceed = false;
    outcome.decision.reason = std::to_string(unclaimed.size()) +
                              " unclaimed transit intent(s) at or below entry " +
                              std::to_string(outcome.decision.checkpoint_lsn) +
                              ": an offline owner has not collected its bytes yet";
    outcome.decision.checkpoint_lsn = kInvalidLsn;
    outcome.decision.agreed_entry_hash.clear();
    return outcome;
  }

  // The marker first, so the prune that follows is itself part of the
  // audited history rather than an unrecorded mutation.
  WriteAheadLog::AppendOptions options;
  options.hlc = now;
  auto marker_or = wal->Append(WalRecordType::kCheckpoint, "", "checkpoint", "", options);
  if (!marker_or.ok()) return marker_or.status();
  outcome.checkpoint_entry_id = marker_or.value();

  auto prune_or = wal->Prune(outcome.decision.checkpoint_lsn);
  if (!prune_or.ok()) return prune_or.status();
  outcome.entries_pruned = prune_or.value().dropped;
  outcome.previous_tip_hash = prune_or.value().previous_tip_hash;
  outcome.new_tip_hash = prune_or.value().new_tip_hash;

  // Release the envelopes whose intent/claimed pairs the checkpoint covers.
  // Computed from the pre-prune snapshot: intent and claim both at or below
  // the agreed tip, matching Prune()'s own droppable rule.
  std::set<std::string> claimed_below;
  for (const WalRecord& rec : entries) {
    if (rec.lsn > outcome.decision.checkpoint_lsn) break;
    if (rec.type == WalRecordType::kTransitClaimed) claimed_below.insert(rec.key_hash);
  }
  std::vector<std::string> settled;
  {
    std::set<std::string> seen;
    for (const WalRecord& rec : entries) {
      if (rec.lsn > outcome.decision.checkpoint_lsn) break;
      if (rec.type != WalRecordType::kTransitIntent) continue;
      if (claimed_below.count(rec.key_hash) == 0) continue;
      if (seen.insert(rec.key_hash).second) settled.push_back(rec.key_hash);
    }
  }
  auto released_or = transit->Release(settled);
  // A release failure must not fail the checkpoint: the prune already
  // happened, and the envelopes expire on their TTL regardless.
  outcome.envelopes_released = released_or.ok() ? released_or.value() : 0;
  if (!released_or.ok()) {
    DSN_LOG_WARN("checkpoint", "envelope release failed: " << released_or.status().message());
  }

  DSN_LOG_INFO("checkpoint", "checkpointed through entry " << outcome.decision.checkpoint_lsn
                                                            << " (pruned " << outcome.entries_pruned
                                                            << ", released "
                                                            << outcome.envelopes_released << ")");
  return outcome;
}

}  // namespace desentry
