#pragma once
// A paged blob store: the shared physical substrate under columnar_lite,
// ts_rollup and vector_hnsw_lite.
//
// Those three engines all have the same physical need -- "write an opaque,
// already-encoded segment of arbitrary size, get an identifier back, read it
// again later, and prove it wasn't corrupted" -- and none of them needs the
// ordered key lookup a B+Tree provides (they keep their own in-memory
// segment indexes, which is the whole point of a segmented columnar layout).
// Implementing that three times would be three chances to get chained-page
// bookkeeping wrong.
//
// Deliberately built on the existing DiskManager + BufferPoolManager rather
// than on private files: a segment page participates in the same 4KiB
// paging, the same LRU eviction and the same checkpoint discipline as every
// other page in the engine, so buffer pool pressure is a single global
// number rather than one per engine.
//
// On-disk layout of one chained page:
//   [ u32 magic | u32 chunk_len | i32 next_page_id | u32 crc32(chunk) | chunk bytes ]
// A blob is the concatenation of its chain's chunks; the chain ends at
// next_page_id == kInvalidPageId. Each page carries its own CRC so a torn
// write is detected at the page that was torn, not silently absorbed into
// the decoded segment.

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "desentry/common/status.h"
#include "desentry/storage/buffer_pool_manager.h"
#include "desentry/storage/disk_manager.h"
#include "desentry/storage/page.h"

namespace desentry {

constexpr uint32_t kSegmentPageMagic = 0x44534753;  // "DSGS"
constexpr size_t kSegmentPageHeaderBytes = 16;
constexpr size_t kSegmentPagePayload = kPageSize - kSegmentPageHeaderBytes;

class SegmentStore {
 public:
  static StatusOr<std::unique_ptr<SegmentStore>> Open(const std::string& file_path,
                                                        size_t buffer_pool_pages = 256);
  ~SegmentStore();

  // Writes `bytes` as a page chain and returns the first page id. An empty
  // blob still consumes one page, so every blob has a valid identifier.
  StatusOr<page_id_t> AppendBlob(const std::string& bytes);

  // Reads back a blob written by AppendBlob(). Returns kCorruption if any
  // page in the chain fails its magic or CRC check.
  StatusOr<std::string> ReadBlob(page_id_t first_page_id) const;

  // Checks a chain without materializing it (cheaper for Verify() over many
  // segments -- it never allocates the decoded blob).
  Status VerifyBlob(page_id_t first_page_id) const;

  // Bytes on disk, i.e. allocated pages * kPageSize. This is what the
  // engines charge against their quota, so quota accounting reflects real
  // disk footprint including chaining overhead rather than a payload count
  // that understates it.
  uint64_t BytesOnDisk() const;

  Status Flush();

 private:
  SegmentStore() = default;

  std::unique_ptr<DiskManager> disk_;
  std::unique_ptr<BufferPoolManager> pool_;
  mutable std::mutex mu_;
};

}  // namespace desentry
