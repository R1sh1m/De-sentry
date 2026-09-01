#include "desentry/crdt/hlc.h"

#include <algorithm>
#include <chrono>
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

void HybridLogicalClock::Observe(const HLCTimestamp& remote) {
  std::lock_guard<std::mutex> lock(mu_);
  uint64_t wall = WallClockMs();
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
}

std::string HLCTimestamp::Encode() const {
  std::string out;
  out.resize(12);
  uint64_t p = physical_ms;
  uint32_t l = logical;
  std::memcpy(out.data(), &p, 8);
  std::memcpy(out.data() + 8, &l, 4);
  out += node_id;
  return out;
}

HLCTimestamp HLCTimestamp::Decode(const std::string& bytes) {
  HLCTimestamp ts;
  if (bytes.size() < 12) return ts;
  std::memcpy(&ts.physical_ms, bytes.data(), 8);
  std::memcpy(&ts.logical, bytes.data() + 8, 4);
  ts.node_id = bytes.substr(12);
  return ts;
}

}  // namespace desentry
