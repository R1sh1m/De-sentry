#pragma once
// Shared machinery every from-scratch router backend inherits.
//
// Two of the EngineBackend contract's obligations are not really per-backend
// decisions at all, and implementing them five times would be five chances
// to break a cross-node invariant:
//
//   * MergeRemote() must be `CrdtValue::Merge(local, remote)` -- the merge
//     semantics belong to the document type, never to a storage layout.
//   * Checksum() must hash the same sorted (key, top-HLC) fingerprint list
//     that NodeEngine::Summarize() produces, or `/_brain` parity between
//     two peers would report a false divergence purely because they bound a
//     collection to different backends.
//
// BaseBackend implements both once, in terms of the subclass's Get/Put/Scan,
// and adds the quota accounting every backend must enforce. A subclass
// therefore implements only its physical layout.

#include <atomic>
#include <string>
#include <vector>

#include "desentry/storage/router.h"

namespace desentry {

class BaseBackend : public EngineBackend {
 public:
  Status MergeRemote(const std::string& collection, const std::string& key,
                      const std::string& remote_encoded_doc) override;

  std::string Checksum(const std::string& collection) override;

  uint64_t QuotaUse() const override { return bytes_used_.load(); }
  uint64_t QuotaLimit() const override { return quota_bytes_; }

  // Fraction of the budget a replication merge is allowed to overshoot by.
  // See Charge() for why this exists at all.
  static constexpr uint64_t kMergeOverdraftPercent = 5;

 protected:
  // Accounts for a record whose on-disk cost is changing from `old_cost`
  // (0 for a brand-new key) to `new_cost`. Shrinking or unchanged records
  // always succeed; growth is charged against the budget and returns
  // kOutOfSpace *before* the subclass touches disk, so a rejected write
  // leaves no partial state.
  //
  // Replication merges get a bounded overdraft. Refusing a merge for want
  // of quota would leave two peers permanently divergent -- a correctness
  // failure -- to avoid a few kilobytes of overshoot, which is a resource
  // failure the supervisor's quota sweep already handles. Bounding the
  // overdraft to kMergeOverdraftPercent keeps that from becoming an
  // unlimited bypass; past it, the merge really is refused and the node is
  // reported degraded.
  Status Charge(uint64_t old_cost, uint64_t new_cost);

  void SetQuotaBytes(uint64_t bytes) { quota_bytes_ = bytes; }
  void SetBytesUsed(uint64_t bytes) { bytes_used_.store(bytes); }

  // Bytes a stored record costs on disk, including this engine's per-record
  // framing. Subclasses with different framing override it.
  virtual uint64_t RecordCost(const std::string& key, const std::string& encoded_doc) const {
    return key.size() + encoded_doc.size() + 32;
  }

  // True while this thread is inside MergeRemote(), i.e. doing convergence
  // work rather than accepting new user data.
  static bool InMergeScope();

  uint64_t quota_bytes_ = 0;
  std::atomic<uint64_t> bytes_used_{0};
};

// RAII marker consulted by Charge(). Public so the vendored adapters, which
// do not inherit BaseBackend's Put path, can mark their own merge windows
// identically.
class MergeScope {
 public:
  MergeScope();
  ~MergeScope();
  MergeScope(const MergeScope&) = delete;
  MergeScope& operator=(const MergeScope&) = delete;
};

// Computes the canonical collection fingerprint from already-materialized
// rows. Exposed (rather than kept private to BaseBackend) so the vendored
// SQLite/DuckDB adapters -- which can compute the same digest with a query
// instead of a full scan -- can be checked against it in tests.
std::string FingerprintRows(const std::vector<EngineRow>& rows);

}  // namespace desentry
