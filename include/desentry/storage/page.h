#pragma once
// Fixed-size page: the atomic unit of I/O and buffering, exactly like every
// production RDBMS (InnoDB defaults to 16KiB, Postgres to 8KiB; we use
// 4KiB, matching the OS page size, which keeps the demo's buffer pool
// small enough to visibly exercise eviction).

#include <cstdint>
#include <cstring>
#include <mutex>

namespace desentry {

using page_id_t = int32_t;
using lsn_t = int64_t;
using slot_id_t = int16_t;

constexpr page_id_t kInvalidPageId = -1;
constexpr lsn_t kInvalidLsn = -1;
constexpr size_t kPageSize = 4096;

// Record identifier: which page, which slot within that page's slot
// directory. This is the value a B+Tree leaf stores for a key.
struct RID {
  page_id_t page_id = kInvalidPageId;
  slot_id_t slot_id = -1;

  bool IsValid() const { return page_id != kInvalidPageId && slot_id >= 0; }
  bool operator==(const RID& o) const { return page_id == o.page_id && slot_id == o.slot_id; }
};

// An in-memory frame holding one page's raw bytes plus buffer-pool
// bookkeeping (pin count, dirty flag, the page_lsn watermark used by the
// WAL to decide whether a page's changes are already durable).
class Page {
 public:
  Page() { ResetMemory(); }

  char* GetData() { return data_; }
  const char* GetData() const { return data_; }
  page_id_t GetPageId() const { return page_id_; }
  int PinCount() const { return pin_count_; }
  bool IsDirty() const { return is_dirty_; }
  lsn_t GetPageLsn() const { return page_lsn_; }
  void SetPageLsn(lsn_t lsn) { page_lsn_ = lsn; }

  void ResetMemory() {
    std::memset(data_, 0, kPageSize);
    page_id_ = kInvalidPageId;
    pin_count_ = 0;
    is_dirty_ = false;
    page_lsn_ = kInvalidLsn;
  }

  std::mutex& latch() { return latch_; }

  // Only the BufferPoolManager mutates these -- kept public-ish via friend
  // to avoid a wall of setters that would just be called from one place.
  page_id_t page_id_ = kInvalidPageId;
  int pin_count_ = 0;
  bool is_dirty_ = false;
  lsn_t page_lsn_ = kInvalidLsn;

 private:
  char data_[kPageSize];
  std::mutex latch_;
};

}  // namespace desentry
