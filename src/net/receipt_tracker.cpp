#include "desentry/net/receipt_tracker.h"

#include <algorithm>
#include <chrono>

#include "desentry/common/logger.h"

namespace desentry {

void ReceiptTracker::EvictIfNeeded() {
  if (waits_.size() < capacity_) return;
  // Drop the oldest waiting entry that has already been satisfied or
  // timed out (stopped == true for shutdown). An active wait is never
  // evicted -- it would wake with an error, which is worse than a timeout.
  while (waits_.size() >= capacity_ && !order_.empty()) {
    const std::string& front = order_.front();
    auto it = waits_.find(front);
    if (it != waits_.end() && (it->second->satisfied || it->second->stopped)) {
      waits_.erase(it);
      order_.pop_front();
    } else {
      break;  // all remaining are active waits
    }
  }
}

StatusOr<std::vector<MergeReceipt>> ReceiptTracker::WaitFor(
    const std::string& message_id, uint32_t wanted, uint32_t timeout_ms) {
  if (wanted == 0) return std::vector<MergeReceipt>{};
  if (timeout_ms == 0) timeout_ms = 1;  // clamp to at least 1ms

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = waits_.find(message_id);
    if (it != waits_.end()) {
      if (wanted > it->second->wanted) it->second->wanted = wanted;
    } else {
      EvictIfNeeded();
      auto state = std::make_unique<WaitState>();
      state->wanted = wanted;
      waits_.emplace(message_id, std::move(state));
      order_.push_back(message_id);
    }
  }

  auto wait_state = waits_.find(message_id);
  if (wait_state == waits_.end()) {
    return Status::Internal("wait state disappeared after creation");
  }
  WaitState* ws = wait_state->second.get();

  std::unique_lock<std::mutex> lock(mu_);
  if (ws->receipts.size() >= wanted) {
    auto result = ws->receipts;
    return result;
  }

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  while (!ws->satisfied && !ws->stopped && ws->receipts.size() < wanted) {
    auto status = ws->cv.wait_until(lock, deadline);
    if (status == std::cv_status::timeout) break;
  }

  if (ws->stopped) {
    return Status::NetworkError("receipt tracker stopped");
  }
  if (ws->receipts.size() >= wanted) {
    ws->satisfied = true;
    auto result = ws->receipts;
    // Mark satisfied so the entry can be evicted later.
    ws->satisfied = true;
    lock.unlock();
    {
      std::lock_guard<std::mutex> l(mu_);
      waits_.erase(message_id);
      if (!order_.empty() && order_.front() == message_id) order_.pop_front();
    }
    return result;
  }
  // Timeout: return what we have.
  auto result = ws->receipts;
  lock.unlock();
  {
    std::lock_guard<std::mutex> l(mu_);
    waits_.erase(message_id);
    if (!order_.empty() && order_.front() == message_id) order_.pop_front();
  }
  return result;
}

void ReceiptTracker::NoteReceipt(const MergeReceipt& receipt) {
  if (receipt.message_id.empty() || receipt.applier_node.empty() ||
      receipt.key_hash.size() != 32) {
    return;  // malformed, ignore
  }
  std::unique_lock<std::mutex> lock(mu_);
  auto it = waits_.find(receipt.message_id);
  if (it == waits_.end()) return;
  WaitState* ws = it->second.get();
  // Dedup by (applier_node, key_hash): same peer applying the same key
  // twice is a duplicate, not a new receipt.
  for (const MergeReceipt& existing : ws->receipts) {
    if (existing.applier_node == receipt.applier_node &&
        existing.key_hash == receipt.key_hash) {
      return;
    }
  }
  ws->receipts.push_back(receipt);
  if (ws->receipts.size() >= ws->wanted) {
    ws->satisfied = true;
    ws->cv.notify_all();
  }
}

void ReceiptTracker::Stop() {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto& [id, ws] : waits_) {
    ws->stopped = true;
    ws->cv.notify_all();
  }
}

}  // namespace desentry