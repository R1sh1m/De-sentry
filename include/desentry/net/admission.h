#pragma once
// Admission control for inbound P2P traffic, and de-duplication for
// broadcast fan-out.
//
// ARCHITECTURE.md Sec 8 called out per-peer rate limiting as required before
// any non-loopback deployment and left it as a note. v2 implements it,
// because v2 is the version that actually ships onto a shared LAN with up to
// 50 nodes per user, where a single misbehaving (or merely buggy) peer can
// saturate every other node's thread-per-connection server.
//
// Two independent mechanisms, deliberately kept separate:
//
//   * **TokenBucket** bounds how *often* a peer may ask for work. A bucket
//     per authenticated node_id, refilled at a steady rate with a burst
//     allowance, so normal bursty gossip is unaffected and a flood is
//     rejected at the cheapest possible point -- before the request is
//     decoded, let alone executed.
//
//   * **MessageDedup** bounds how *many times the same work* is done. Eager
//     broadcast with a bounded relay TTL means a node on a well-connected
//     mesh will receive the same write from several neighbours. CRDT merge
//     is idempotent so a duplicate is never *incorrect* -- but at 50 nodes
//     it is a real cost, and dropping it is free.
//
// Neither is a security boundary on its own: an attacker with a valid
// identity can still open many connections. They are there to make the
// common failure -- one peer going wrong -- survivable for everyone else,
// which is the property a LAN mesh actually needs.

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace desentry {

// Per-key token bucket. Keys are authenticated node_ids, so a peer cannot
// escape its own bucket by reconnecting or by claiming a different address.
class TokenBucketLimiter {
 public:
  // `rate_per_sec` tokens accrue per second, up to `burst` in reserve.
  // rate_per_sec == 0 disables limiting entirely (and is logged by the
  // caller, because an unlimited peer path is an operational decision).
  TokenBucketLimiter(uint32_t rate_per_sec, uint32_t burst)
      : rate_per_sec_(rate_per_sec), burst_(burst == 0 ? rate_per_sec : burst) {}

  // Consumes one token for `key`. Returns false when the peer has exceeded
  // its allowance and the request should be refused.
  bool Allow(const std::string& key, int64_t now_ms);

  // Drops buckets untouched for longer than `idle_ms`, so a mesh that has
  // seen thousands of transient peers does not accumulate their buckets
  // forever.
  size_t Sweep(int64_t now_ms, int64_t idle_ms);

  size_t TrackedKeys() const;
  uint32_t rate_per_sec() const { return rate_per_sec_; }

 private:
  struct Bucket {
    double tokens = 0;
    int64_t last_refill_ms = 0;
    int64_t last_seen_ms = 0;
  };

  mutable std::mutex mu_;
  uint32_t rate_per_sec_;
  uint32_t burst_;
  std::unordered_map<std::string, Bucket> buckets_;
};

// A bounded, FIFO-evicting set of recently-seen message ids.
//
// Bounded rather than time-based on purpose: a fixed memory ceiling is the
// property that matters under load, and at any plausible message rate the
// window covered by kDefaultCapacity ids is far longer than the few seconds
// a relayed duplicate takes to arrive.
class MessageDedup {
 public:
  static constexpr size_t kDefaultCapacity = 8192;

  explicit MessageDedup(size_t capacity = kDefaultCapacity) : capacity_(capacity) {}

  // Records `message_id` and returns true if it had NOT been seen before
  // (i.e. the caller should process it). An empty id is always treated as
  // new -- a peer that omits one gets correct behaviour, just no dedup.
  bool NoteAndCheckNew(const std::string& message_id);

  bool Contains(const std::string& message_id) const;
  size_t Size() const;

  // Generates an id for a locally-originated broadcast: node_id + a counter
  // + random bytes, so ids are unique across nodes and across restarts.
  static std::string NewMessageId(const std::string& node_id);

 private:
  mutable std::mutex mu_;
  size_t capacity_;
  std::unordered_set<std::string> seen_;
  std::deque<std::string> order_;
};

}  // namespace desentry
