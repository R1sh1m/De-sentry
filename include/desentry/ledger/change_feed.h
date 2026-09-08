#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

#include "desentry/common/status.h"
#include "desentry/storage/wal.h"

namespace desentry {

struct ChangeBatch {
  lsn_t from = kInvalidLsn;
  lsn_t tip = kInvalidLsn;
  bool truncated = false;
  std::vector<WalRecord> entries;
};

class ChangeFeed {
 public:
  static constexpr uint32_t kMaxTimeoutMs = 30000;

  explicit ChangeFeed(WriteAheadLog* wal) : wal_(wal) {}

  void Publish(lsn_t tip);
  void Stop();
  StatusOr<ChangeBatch> Wait(lsn_t since, uint32_t timeout_ms, size_t limit);

 private:
  WriteAheadLog* wal_;
  std::mutex mutex_;
  std::condition_variable condition_;
  lsn_t published_tip_ = kInvalidLsn;
  bool stopped_ = false;
};

}  // namespace desentry
