#pragma once
// Buffer pool: caches fixed-size pages in memory, backed by the
// DiskManager. Eviction policy is a plain LRU replacer (documented in
// ARCHITECTURE.md §9 as the first thing to upgrade to LRU-K/clock once we
// have real access traces to tune against -- LRU is the right starting
// point because it is simple to prove correct, which matters more than
// raw hit-rate for an MVP).

#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "desentry/common/status.h"
#include "desentry/storage/disk_manager.h"
#include "desentry/storage/page.h"

namespace desentry {

using frame_id_t = int32_t;

// Tracks which frames are currently evictable (pin_count == 0) and in what
// order they should be evicted (least-recently-unpinned first).
class LRUReplacer {
 public:
  void RecordAccess(frame_id_t frame_id);   // frame was just unpinned -> now evictable
  void Pin(frame_id_t frame_id);            // frame is in use -> not evictable
  bool Victim(frame_id_t* out_frame_id);    // pick and remove the LRU evictable frame
  size_t Size() const;

 private:
  mutable std::mutex mu_;
  std::list<frame_id_t> lru_list_;  // front = most recently used, back = victim
  std::unordered_map<frame_id_t, std::list<frame_id_t>::iterator> position_;
};

class BufferPoolManager {
 public:
  BufferPoolManager(size_t pool_size, DiskManager* disk_manager);
  ~BufferPoolManager() = default;

  BufferPoolManager(const BufferPoolManager&) = delete;
  BufferPoolManager& operator=(const BufferPoolManager&) = delete;

  // Fetches (loading from disk if necessary) and pins the page. Returns
  // nullptr if the pool is full of pinned pages (caller must retry later --
  // this mirrors real buffer pool back-pressure rather than silently
  // growing unbounded memory).
  Page* FetchPage(page_id_t page_id);

  // Allocates a brand-new page (via the DiskManager), pins it, and returns
  // it plus its id.
  Page* NewPage(page_id_t* out_page_id);

  // Unpins a page; if is_dirty, marks it for eventual flush.
  bool UnpinPage(page_id_t page_id, bool is_dirty);

  // Forces a page's current bytes to disk immediately.
  bool FlushPage(page_id_t page_id);

  // Flushes every dirty page -- called at shutdown / checkpoint time.
  void FlushAllPages();

  size_t PoolSize() const { return pages_.size(); }

 private:
  Page* FindFreeOrVictimFrame(frame_id_t* out_frame);

  std::vector<Page> pages_;
  DiskManager* disk_manager_;
  std::unordered_map<page_id_t, frame_id_t> page_table_;
  std::vector<frame_id_t> free_list_;
  LRUReplacer replacer_;
  std::mutex latch_;  // protects page_table_/free_list_ bookkeeping
};

}  // namespace desentry
