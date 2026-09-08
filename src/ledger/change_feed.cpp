// Live change-feed implementation: a tip-published long poll with
// prune-aware truncation. See the header for the contract.

#include "desentry/ledger/change_feed.h"

#include <algorithm>
#include <chrono>

#include "desentry/common/platform.h"

namespace desentry {

void ChangeFeed::Publish(lsn_t tip) {
  std::lock_guard<std::mutex> lock(mu_);
  if (tip > max_tip_ever_) max_tip_ever_ = tip;
  cv_.notify_all();
}

void ChangeFeed::Stop() {
  std::lock_guard<std::mutex> lock(mu_);
  stopped_ = true;
  cv_.notify_all();
}

ChangeBatch ChangeFeed::Snapshot(lsn_t since, size_t limit) {
  ChangeBatch batch;
  batch.from = since;

  auto all_or = wal_->ReadAll();
  if (!all_or.ok()) {
    // A ledger that cannot be read is not "no changes": signal a re-sync so
    // the caller treats its cursor as suspect rather than as current.
    batch.tip = wal_->LastLsn();
    batch.truncated = true;
    return batch;
  }
  const std::vector<WalRecord>& all = all_or.value();
  const lsn_t tip = wal_->LastLsn();
  batch.tip = tip;

  const lsn_t oldest = all.empty() ? tip + 1 : all.front().lsn;
  // Three ways a cursor goes stale, all of which mean "re-sync":
  //   * it points past the tip (the log was pruned/renumbered beneath it);
  //   * the oldest surviving entry is newer than the cursor (prefix dropped);
  //   * the tip regressed below a previously seen height while the cursor is
  //     behind (renumbering swapped which entries the cursor's range names).
  // The initial cursor (-1) is never stale.
  const bool stale = since >= 0 && (since > tip || oldest > since + 1 ||
                                    (since < tip && tip < max_tip_ever_));
  if (since >= tip) {
    // The caller is known-current: whatever prunes happened before are no
    // longer observable through this cursor, so the high-water mark resets
    // rather than crying truncation on every subsequent poll.
    max_tip_ever_ = tip;
  } else if (tip > max_tip_ever_) {
    max_tip_ever_ = tip;
  }
  if (stale) {
    batch.truncated = true;
    return batch;
  }

  batch.entries.reserve(std::min<size_t>(all.size(), limit == 0 ? all.size() : limit));
  for (const WalRecord& rec : all) {
    if (rec.lsn <= since) continue;
    if (limit != 0 && batch.entries.size() >= limit) break;
    batch.entries.push_back(rec);
  }
  return batch;
}

StatusOr<ChangeBatch> ChangeFeed::Wait(lsn_t since, uint32_t timeout_ms, size_t limit) {
  std::unique_lock<std::mutex> lock(mu_);
  if (stopped_) return Status::Internal("change feed is stopped");

  const int64_t deadline = MonotonicMs() + static_cast<int64_t>(timeout_ms);
  for (;;) {
    // Snapshot takes the WAL lock while this thread holds the feed lock;
    // that order is safe because Publish()/Stop() never touch the WAL while
    // holding the feed lock.
    ChangeBatch batch = Snapshot(since, limit);
    if (!batch.entries.empty() || batch.truncated) return batch;
    if (stopped_) return Status::Internal("change feed is stopped");

    const int64_t remaining = deadline - MonotonicMs();
    if (remaining <= 0) return batch;  // empty, current, not truncated
    cv_.wait_for(lock, std::chrono::milliseconds(remaining));
  }
}

}  // namespace desentry
