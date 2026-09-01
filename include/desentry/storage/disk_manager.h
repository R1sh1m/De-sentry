#pragma once
// Raw paged-file I/O. This is the only component allowed to call
// read/write/fsync on the data file -- every other storage component goes
// through the BufferPoolManager, which goes through this.

#include <atomic>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

#include "desentry/common/status.h"
#include "desentry/storage/page.h"

namespace desentry {

class DiskManager {
 public:
  // Opens (creating if necessary) the paged data file at `db_file`.
  static StatusOr<std::unique_ptr<DiskManager>> Open(const std::string& db_file);
  ~DiskManager();

  DiskManager(const DiskManager&) = delete;
  DiskManager& operator=(const DiskManager&) = delete;

  // Reads exactly kPageSize bytes for `page_id` into `out` (must have room
  // for kPageSize bytes). Reading a page beyond EOF returns a zeroed page
  // (this happens the first time a freshly allocated page is read before
  // anything has been written to it).
  Status ReadPage(page_id_t page_id, char* out);

  // Writes exactly kPageSize bytes for `page_id`. Does not fsync -- callers
  // that need durability call Sync() explicitly (the WAL is what's fsync'd
  // on the hot path; data-file writes are fsync'd at checkpoint time).
  Status WritePage(page_id_t page_id, const char* data);

  // Allocates a new page id (does not touch disk -- the page is materialized
  // on first WritePage).
  page_id_t AllocatePage();

  Status Sync();

  int64_t NumAllocatedPages() const { return next_page_id_.load(); }

 private:
  explicit DiskManager(std::fstream file, std::string path, int64_t next_page_id)
      : file_(std::move(file)), path_(std::move(path)), next_page_id_(next_page_id) {}

  std::fstream file_;
  std::string path_;
  std::mutex io_mu_;
  std::atomic<int64_t> next_page_id_;
};

}  // namespace desentry
