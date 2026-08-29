#pragma once
// Hybrid Logical Clock (Kulkarni et al.) -- gives every write a timestamp
// that is close to wall-clock time (useful for humans/debugging) while
// still being strictly monotonic per node and causality-respecting across
// nodes: if node A received a message from node B and then produces a new
// timestamp, A's timestamp is guaranteed to be greater than B's, even under
// clock skew between the two machines. This is the timestamp every CRDT
// merge in this engine orders on (see crdt/document.h).

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace desentry {

struct HLCTimestamp {
  uint64_t physical_ms = 0;
  uint32_t logical = 0;
  std::string node_id;  // final tiebreaker if physical+logical are exactly equal

  bool operator<(const HLCTimestamp& o) const {
    if (physical_ms != o.physical_ms) return physical_ms < o.physical_ms;
    if (logical != o.logical) return logical < o.logical;
    return node_id < o.node_id;
  }
  bool operator>(const HLCTimestamp& o) const { return o < *this; }
  bool operator==(const HLCTimestamp& o) const {
    return physical_ms == o.physical_ms && logical == o.logical && node_id == o.node_id;
  }
  bool operator<=(const HLCTimestamp& o) const { return !(o < *this); }
  bool operator>=(const HLCTimestamp& o) const { return !(*this < o); }

  std::string ToString() const {
    return std::to_string(physical_ms) + "." + std::to_string(logical) + "@" + node_id;
  }

  // Wire/storage encoding: fixed 8+4 bytes big-endian + node_id, kept
  // separate from JSON so document_codec.h can pack it compactly.
  std::string Encode() const;
  static HLCTimestamp Decode(const std::string& bytes);
};

class HybridLogicalClock {
 public:
  explicit HybridLogicalClock(std::string node_id) : node_id_(std::move(node_id)) {}

  // Produces a new local timestamp, guaranteed greater than every
  // timestamp this clock has produced or observed so far.
  HLCTimestamp Now();

  // Folds in a timestamp observed from a remote peer (e.g. attached to an
  // incoming replicated write) so that this node's subsequent timestamps
  // are guaranteed causally after it.
  void Observe(const HLCTimestamp& remote);

 private:
  static uint64_t WallClockMs();

  std::mutex mu_;
  std::string node_id_;
  uint64_t last_physical_ = 0;
  uint32_t last_logical_ = 0;
};

}  // namespace desentry
