#pragma once
// `kv` -- the generic key/value backend, and the router's default.
//
// This is the v1 physical path (4KiB paged file, LRU buffer pool, slotted
// pages, one disk-backed B+Tree per collection) packaged as a router
// backend. It stays the default for a reason: for an unknown access
// pattern, an ordered index over variable-length records is the shape that
// is never badly wrong, and it is the only backend with no assumptions
// about the *content* of a document. It is also what the transit store uses
// -- bytes held on behalf of an offline peer have no queryable structure by
// definition, so a columnar or vector layout would be pure overhead there.
//
// Deliberately does not own a WAL. Durability belongs to StorageEngine's
// hash-chained ledger, which fsyncs before the router is called at all;
// giving each backend its own redo log would mean five logs to recover, five
// chances to diverge from the ledger, and no single tamper-evident history.

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "desentry/storage/bplus_tree.h"
#include "desentry/storage/buffer_pool_manager.h"
#include "desentry/storage/disk_manager.h"
#include "desentry/storage/engines/engine_common.h"

namespace desentry {

class KvBPlusBackend : public BaseBackend {
 public:
  std::string Name() const override { return "kv"; }

  Status Open(const std::string& data_dir, uint64_t quota_mb) override;
  Status Put(const std::string& collection, const std::string& key,
              const std::string& encoded_doc) override;
  StatusOr<std::string> Get(const std::string& collection, const std::string& key) override;
  std::vector<EngineRow> Scan(const std::string& collection, const std::string& start_key,
                               size_t limit) override;
  Status Verify() override;
  Status Flush() override;
  std::vector<std::string> ListCollections() const override;

 private:
  BPlusTree* IndexFor(const std::string& collection);
  Status SaveRoots();
  Status LoadRoots();

  std::string dir_;
  std::string roots_path_;
  std::unique_ptr<DiskManager> disk_;
  std::unique_ptr<BufferPoolManager> pool_;

  mutable std::mutex mu_;
  std::unordered_map<std::string, std::unique_ptr<BPlusTree>> indexes_;
  std::unordered_map<std::string, page_id_t> roots_;
  // Per-collection bump-allocated current write page, same trade-off the v1
  // engine documents: no free-space map, tombstoned slot bytes are reclaimed
  // by a future compaction pass rather than on every delete.
  std::unordered_map<std::string, page_id_t> write_pages_;
};

}  // namespace desentry
