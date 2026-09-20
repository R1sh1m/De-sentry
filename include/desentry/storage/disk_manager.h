#pragma once
// Raw paged-file I/O. This is the only component allowed to call
// read/write/fsync on the data file -- every other storage component goes
// through the BufferPoolManager, which goes through this.
//
// At-rest encryption (see security/at_rest.h) lives here, so every paged
// backend (kv's B+Tree file, all SegmentStore files) inherits it without
// knowing: plaintext files use a 4096-byte stride, sealed files a
// 4124-byte stride ([12B nonce][4096B ct][16B tag] with a fresh random nonce
// per write, AAD binding file_tag + page_id). A random per-file tag in the
// `<db>.enc` sidecar header (non-secret) doubles as the encrypted/plaintext
// detector, so detection is never heuristic:
//
//   * encrypted open + data file present + no header  -> Corruption
//     ("plaintext data file ... migrate with desentryd --re-encrypt");
//   * plaintext open + header present                 -> Corruption
//     ("encrypted data file opened without encryption");
//   * full-stride page failing authentication         -> Corruption (wrong
//     key, swapped or tampered page -- never a zeroed page pretending to
//     be data). A short/torn final page still zero-fills exactly like the
//     plaintext path, and WAL replay heals it.

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
  // Opens (creating if necessary) the paged data file at `db_file`. `dek`
  // is the node's 32-byte data-encryption key; empty means plaintext.
  static StatusOr<std::unique_ptr<DiskManager>> Open(const std::string& db_file,
                                                     const std::string& dek = "");
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
  bool encrypted() const { return encrypted_; }

 private:
  DiskManager(std::fstream file, std::string path, int64_t next_page_id, std::string subkey,
              std::string file_tag, bool encrypted, size_t stride)
      : file_(std::move(file)),
        path_(std::move(path)),
        next_page_id_(next_page_id),
        subkey_(std::move(subkey)),
        file_tag_(std::move(file_tag)),
        encrypted_(encrypted),
        stride_(stride) {}

  std::fstream file_;
  std::string path_;
  std::mutex io_mu_;
  std::atomic<int64_t> next_page_id_;
  // At-rest state. Empty subkey when plaintext; the subkey (not the DEK) is
  // what pages are sealed with, so one DEK serves every file differently.
  std::string subkey_;
  std::string file_tag_;
  bool encrypted_ = false;
  size_t stride_ = kPageSize;
};

}  // namespace desentry
