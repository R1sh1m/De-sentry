#pragma once
// Live change feed: a long poll over the ledger tip (docs/architecture-v2.md
// Sec 5.4).
//
// GET /_changes?since= blocks until entries newer than `since` exist (or the
// timeout elapses) instead of polling /_brain on a timer. The storage layer
// notifies the feed after every ledger append via the tip observer
// (StorageEngine::SetTipObserver), so a write anywhere in the mesh that
// reaches this node's ledger wakes waiters within its round trip.
//
// `truncated: true` means the caller's cursor predates a prune: entries the
// cursor names no longer exist (or the log was renumbered beneath it), so the
// client must drop the cursor and re-read from scratch rather than pretend
// continuity across a gap. A false `truncated` only costs a re-sync; a false
// negative would silently skip history, so the feed biases towards reporting
// truncation whenever the tip regressed below a previously seen height.

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "desentry/common/status.h"
#include "desentry/storage/page.h"
#include "desentry/storage/wal.h"

namespace desentry {

struct ChangeBatch {
  std::vector<WalRecord> entries;  // records with lsn in (from, tip], oldest first
  lsn_t from = kInvalidLsn;        // the `since` cursor this batch answers
  lsn_t tip = kInvalidLsn;         // ledger tip when the batch was read
  bool truncated = false;          // cursor predates a prune: re-sync, entries is empty
};

class ChangeFeed {
 public:
  // Upper bound on a single long poll, in milliseconds. The route clamps the
  // caller's timeout_ms into [0, kMaxTimeoutMs].
  static constexpr int64_t kMaxTimeoutMs = 120000;

  // `wal` is borrowed (owned by StorageEngine) and must outlive the feed.
  explicit ChangeFeed(WriteAheadLog* wal) : wal_(wal) {}

  // Called after every ledger append with the new tip. Wakes waiters.
  void Publish(lsn_t tip);

  // Wakes every waiter with an error and makes future Wait() calls fail.
  // Called once at engine shutdown, before the ledger goes away.
  void Stop();

  // Returns records with lsn in (since, tip], oldest first, at most `limit`
  // of them (0 == unlimited). If no such records exist yet, blocks until
  // some arrive, the feed is stopped, or `timeout_ms` elapses -- whichever
  // comes first. `since == kInvalidLsn` (-1) reads from the beginning.
  StatusOr<ChangeBatch> Wait(lsn_t since, uint32_t timeout_ms, size_t limit);

 private:
  ChangeBatch Snapshot(lsn_t since, size_t limit);

  WriteAheadLog* wal_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool stopped_ = false;
  lsn_t max_tip_ever_ = kInvalidLsn;  // highest tip observed; prune detection
};

}  // namespace desentry
