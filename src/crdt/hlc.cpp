#include "desentry/crdt/hlc.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>

namespace desentry {

uint64_t HybridLogicalClock::WallClockMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

HLCTimestamp HybridLogicalClock::Now() {
  std::lock_guard<std::mutex> lock(mu_);
  uint64_t wall = WallClockMs();
  if (wall > last_physical_) {
    last_physical_ = wall;
    last_logical_ = 0;
  } else {
    last_logical_++;
  }
  return HLCTimestamp{last_physical_, last_logical_, node_id_};
}

bool HybridLogicalClock::Observe(const HLCTimestamp& remote) {
  std::lock_guard<std::mutex> lock(mu_);
  uint64_t wall = WallClockMs();
  // Reject (don't clamp) unbounded future timestamps (C-5): accepting 2^62
  // would pin last_physical_ forever and make the value unoverwritable.
  if (remote.physical_ms > wall + kMaxFutureSkewMs) return false;
  // Refuse rather than wrap the logical counter: wrapping makes timestamps
  // go backwards and LWW ordering incoherent.
  if (last_logical_ == UINT32_MAX || remote.logical == UINT32_MAX) return false;
  uint64_t max_physical = std::max({wall, last_physical_, remote.physical_ms});
  if (max_physical == last_physical_ && max_physical == remote.physical_ms) {
    last_logical_ = std::max(last_logical_, remote.logical) + 1;
  } else if (max_physical == last_physical_) {
    last_logical_ = last_logical_ + 1;
  } else if (max_physical == remote.physical_ms) {
    last_logical_ = remote.logical + 1;
  } else {
    last_logical_ = 0;
  }
  last_physical_ = max_physical;
  return true;
}

void HybridLogicalClock::Restore(uint64_t physical_ms, uint32_t logical) {
  std::lock_guard<std::mutex> lock(mu_);
  if (physical_ms > last_physical_ ||
      (physical_ms == last_physical_ && logical > last_logical_)) {
    last_physical_ = physical_ms;
    last_logical_ = logical;
  }
}

uint64_t HybridLogicalClock::last_physical() const {
  std::lock_guard<std::mutex> lock(mu_);
  return last_physical_;
}

uint32_t HybridLogicalClock::last_logical() const {
  std::lock_guard<std::mutex> lock(mu_);
  return last_logical_;
}

std::string HLCTimestamp::Encode() const {
  // Fixed little-endian (M-3 fix): the old memcpy(host) made the wire format
  // host-endian despite the header's big-endian claim.
  std::string out;
  out.resize(12);
  for (int i = 0; i < 8; ++i) out[i] = static_cast<char>((physical_ms >> (8 * i)) & 0xFF);
  for (int i = 0; i < 4; ++i) out[8 + i] = static_cast<char>((logical >> (8 * i)) & 0xFF);
  out += node_id;
  return out;
}

HLCTimestamp HLCTimestamp::Decode(const std::string& bytes) {
  HLCTimestamp ts;
  if (bytes.size() < 12) return ts;
  // Accepts both the new LE encoding and legacy host-endian (little-endian
  // hosts) bytes -- identical on LE machines, so no migration is needed.
  uint64_t p = 0;
  uint32_t l = 0;
  for (int i = 0; i < 8; ++i) p |= static_cast<uint64_t>(static_cast<unsigned char>(bytes[i])) << (8 * i);
  for (int i = 0; i < 4; ++i) l |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[8 + i])) << (8 * i);
  ts.physical_ms = p;
  ts.logical = l;
  ts.node_id = bytes.substr(12);
  return ts;
}

}  // namespace desentry
