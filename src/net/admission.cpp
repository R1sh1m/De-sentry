#include "desentry/net/admission.h"

#include <algorithm>
#include <atomic>

#include "desentry/common/hex.h"
#include "desentry/common/platform.h"
#include "desentry/security/crypto.h"

namespace desentry {

bool TokenBucketLimiter::Allow(const std::string& key, int64_t now_ms) {
  if (rate_per_sec_ == 0) return true;  // limiting disabled
  std::lock_guard<std::mutex> lock(mu_);
  Bucket& bucket = buckets_[key];
  if (bucket.last_refill_ms == 0) {
    // A peer's first request starts with a full burst allowance rather than
    // an empty bucket: making a brand-new peer wait a second before its
    // first gossip round would slow down exactly the case (a node joining)
    // that most wants to be fast.
    bucket.tokens = static_cast<double>(burst_);
    bucket.last_refill_ms = now_ms;
  } else {
    const double elapsed_s = static_cast<double>(now_ms - bucket.last_refill_ms) / 1000.0;
    if (elapsed_s > 0) {
      bucket.tokens = std::min(static_cast<double>(burst_),
                                bucket.tokens + elapsed_s * static_cast<double>(rate_per_sec_));
      bucket.last_refill_ms = now_ms;
    }
  }
  bucket.last_seen_ms = now_ms;
  if (bucket.tokens < 1.0) return false;
  bucket.tokens -= 1.0;
  return true;
}

size_t TokenBucketLimiter::Sweep(int64_t now_ms, int64_t idle_ms) {
  std::lock_guard<std::mutex> lock(mu_);
  size_t removed = 0;
  for (auto it = buckets_.begin(); it != buckets_.end();) {
    if (now_ms - it->second.last_seen_ms > idle_ms) {
      it = buckets_.erase(it);
      ++removed;
    } else {
      ++it;
    }
  }
  return removed;
}

size_t TokenBucketLimiter::TrackedKeys() const {
  std::lock_guard<std::mutex> lock(mu_);
  return buckets_.size();
}

bool MessageDedup::NoteAndCheckNew(const std::string& message_id) {
  if (message_id.empty()) return true;  // no id: no dedup, but still correct
  std::lock_guard<std::mutex> lock(mu_);
  if (!seen_.insert(message_id).second) return false;
  order_.push_back(message_id);
  while (order_.size() > capacity_) {
    seen_.erase(order_.front());
    order_.pop_front();
  }
  return true;
}

bool MessageDedup::Contains(const std::string& message_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  return seen_.count(message_id) != 0;
}

size_t MessageDedup::Size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return seen_.size();
}

std::string MessageDedup::NewMessageId(const std::string& node_id) {
  static std::atomic<uint64_t> counter{0};
  // node_id + counter would be unique within a run but repeat after a
  // restart, which would make a restarted node's first broadcasts look like
  // duplicates to peers that still remember the old ids. The random suffix
  // is what makes ids unique across restarts too.
  const std::string material = node_id + ":" + std::to_string(counter.fetch_add(1)) + ":" +
                               std::to_string(NowMs()) + ":" + crypto::RandomBytes(8);
  return HexEncode(crypto::Sha256(material)).substr(0, 32);
}

}  // namespace desentry
