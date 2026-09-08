#include "desentry/ledger/change_feed.h"

#include <algorithm>
#include <chrono>

namespace desentry {

void ChangeFeed::Publish(lsn_t tip) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    published_tip_ = std::max(published_tip_, tip);
  }
  condition_.notify_all();
}

void ChangeFeed::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
  }
  condition_.notify_all();
}

StatusOr<ChangeBatch> ChangeFeed::Wait(lsn_t since, uint32_t timeout_ms, size_t limit) {
  auto read = [this, since, limit]() -> StatusOr<ChangeBatch> {
    auto records_or = wal_->ReadAll();
    if (!records_or.ok()) return records_or.status();

    ChangeBatch batch;
    batch.from = since;
    batch.tip = wal_->Tip().entry_id;
    const lsn_t first = records_or.value().empty() ? kInvalidLsn : records_or.value().front().lsn;
    batch.truncated = since >= 0 && first >= 0 && since < first - 1;
    for (const WalRecord& record : records_or.value()) {
      if (record.lsn <= since) continue;
      if (limit != 0 && batch.entries.size() >= limit) break;
      batch.entries.push_back(record);
    }
    return batch;
  };

  auto initial = read();
  if (!initial.ok() || !initial.value().entries.empty() || initial.value().tip > since) {
    return initial;
  }

  std::unique_lock<std::mutex> lock(mutex_);
  condition_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
    return stopped_ || published_tip_ > since;
  });
  lock.unlock();
  return read();
}

}  // namespace desentry
