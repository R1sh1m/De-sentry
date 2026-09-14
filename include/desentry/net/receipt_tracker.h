#pragma once
// Receipt tracker: awaits signed merge receipts from peers for a given
// local write's message_id. Mirrors the ChangeFeed pattern -- no
// persistent state, the waiter is the HTTP request handler itself, and the
// map entry is erased on completion or timeout. This is the zero-residual
// property: a timed-out request leaves nothing to GC.
//
// Receipts ride on the existing request/response transport: HandleOpBroadcast
// returns a MergeReceipt in its kPong payload (distinct from the kPing/kPong
// node_id payload). The worker lambda in FanOut decodes and forwards it here.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "desentry/common/status.h"
#include "desentry/storage/page.h"
#include "desentry/net/wire_protocol.h"

namespace desentry {

class ReceiptTracker {
 public:
  explicit ReceiptTracker(uint32_t capacity = 8192) : capacity_(capacity) {}

  // Waits until `wanted` distinct receipts arrive for `message_id`, or
  // `timeout_ms` elapses, or the tracker is stopped. Returns the receipts
  // received (may be fewer than `wanted` on timeout).
  StatusOr<std::vector<MergeReceipt>> WaitFor(const std::string& message_id,
                                               uint32_t wanted, uint32_t timeout_ms);

  // Called by FanOut's worker lambda when a peer's merge receipt arrives.
  // If the message_id is being waited on, the receipt is stored and waiters
  // are notified. Duplicate receipts (same applier + key_hash) are ignored.
  void NoteReceipt(const MergeReceipt& receipt);

  // Wakes every waiter with an error and makes future WaitFor() calls fail.
  // Called once at engine shutdown.
  void Stop();

 private:
  struct WaitState {
    uint32_t wanted = 0;
    std::vector<MergeReceipt> receipts;
    std::condition_variable cv;
    bool satisfied = false;
    bool stopped = false;
  };

  std::unique_ptr<WaitState> GetOrCreateWait(const std::string& message_id, uint32_t wanted);

  void EvictIfNeeded();

  mutable std::mutex mu_;
  size_t capacity_;
  std::map<std::string, std::unique_ptr<WaitState>> waits_;
  std::deque<std::string> order_;  // FIFO for capacity eviction
};

}  // namespace desentry