#pragma once
// Top-level local storage engine: the thing a single peer's node_engine
// talks to. Owns the DiskManager, BufferPoolManager, WriteAheadLog and
// Catalog, and exposes a small, physical (not CRDT-aware) key/value
// interface per collection: PutRaw/GetRaw/Scan operate on already-encoded
// document bytes (see storage/document_codec.h). CRDT semantics -- merging
// a write against the previous version, generating HLC timestamps, turning
// a delete into a tombstoning update -- live one layer up, in
// engine/node_engine.h, which is what actually calls CrdtValue::Merge /
// ApplyJsonUpdate before handing bytes down to this layer. This split
// keeps the storage engine testable and reasoned-about independent of the
// distributed-systems layer above it, same as InnoDB doesn't know what SQL
// is.
//
// Crash recovery: on Open(), the WAL is replayed from the beginning
// through this same Put/Delete path (idempotent -- see wal.h). Known MVP
// limitation, stated plainly rather than glossed over: this protects
// against losing *documents* that were WAL'd but not yet flushed, assuming
// each individual page write to the data file is not torn; it does not yet
// use physical page checksums to detect/repair a page torn mid-write during
// a B+Tree structural split. That hardening (checksummed physical redo, or
// rebuild-into-fresh-pages-then-atomic-swap recovery) is a stated
// ARCHITECTURE.md roadmap item, not silently assumed away.

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "desentry/common/status.h"
#include "desentry/storage/bplus_tree.h"
#include "desentry/storage/buffer_pool_manager.h"
#include "desentry/storage/catalog.h"
#include "desentry/storage/disk_manager.h"
#include "desentry/storage/wal.h"

namespace desentry {

class StorageEngine {
 public:
  struct Options {
    std::string data_dir = "./data";
    size_t buffer_pool_pages = 1024;
  };

  static StatusOr<std::unique_ptr<StorageEngine>> Open(const Options& options);

  // Ensures a collection exists (idempotent).
  Status EnsureCollection(const std::string& collection);
  std::vector<std::string> ListCollections() const;
  Catalog& catalog() { return *catalog_; }

  // Physical upsert: writes `encoded_doc` (see document_codec.h) durably
  // (WAL-fsync'd) and indexes it under `key`, replacing whatever was there.
  Status PutRaw(const std::string& collection, const std::string& key, const std::string& encoded_doc);

  StatusOr<std::string> GetRaw(const std::string& collection, const std::string& key);

  // Ordered scan for the collection listing / query endpoints.
  std::vector<std::pair<std::string, std::string>> Scan(const std::string& collection,
                                                          const std::string& start_key, size_t limit);

  void Checkpoint();  // flush all dirty pages to disk

  // -- hash-chained audit ledger (see storage/wal.h) --------------------------
  // Thin pass-throughs to the WAL's chain so callers only ever depend on
  // StorageEngine, never reach into the WAL directly (same layering
  // discipline as the rest of this class).
  WriteAheadLog::LedgerTip LedgerTip() const { return wal_->Tip(); }
  WriteAheadLog::VerifyResult VerifyLedger() { return wal_->VerifyChain(); }
  // Bounded range read for GET /_ledger/entries; callers (routes.cpp) are
  // responsible for clamping [from, to] to a sane page size before calling.
  StatusOr<std::vector<WalRecord>> LedgerEntries(lsn_t from, lsn_t to);

 private:
  StorageEngine() = default;
  BPlusTree* GetOrCreateIndex(const std::string& collection);
  Status ReplayWal();

  std::unique_ptr<DiskManager> disk_manager_;
  std::unique_ptr<BufferPoolManager> buffer_pool_;
  std::unique_ptr<WriteAheadLog> wal_;
  std::unique_ptr<Catalog> catalog_;

  std::mutex indexes_mu_;
  std::unordered_map<std::string, std::unique_ptr<BPlusTree>> indexes_;

  // Per-collection "current write page" that new PutRaw() calls try first
  // before allocating a fresh page -- a simple bump allocator, not a free
  // space map (documented roadmap: reclaim tombstoned slot space via
  // compaction instead of always bumping forward).
  std::mutex write_pages_mu_;
  std::unordered_map<std::string, page_id_t> write_pages_;
};

}  // namespace desentry
