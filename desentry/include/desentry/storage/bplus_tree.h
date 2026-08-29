#pragma once
// A real, disk-backed B+Tree, keyed by a bounded-length string key, storing
// an RID (page_id, slot_id) per key -- the same physical index structure
// InnoDB (as the clustered index) and Postgres (for secondary indexes) use.
// Every node is exactly one Page, fetched/pinned through the
// BufferPoolManager like any other page, so the tree participates in the
// same buffer pool / eviction / durability story as the rest of the
// engine.
//
// Deliberate MVP scope limits (both documented rather than hidden):
//   * Keys are capped at kMaxKeyBytes. A document's primary key longer than
//     this is rejected at the API layer with a clear error, rather than the
//     index silently truncating it.
//   * Remove() deletes the leaf entry but does not rebalance/merge
//     underfull sibling nodes. This is safe for this engine specifically
//     because a CRDT "delete" is a tombstone *write* (see crdt/document.h)
//     that goes through Insert()'s upsert path, not through Remove() --
///    Remove() exists for completeness (e.g. catalog housekeeping) but is
//     not on the hot path. Full rebalancing is a documented roadmap item.

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "desentry/common/status.h"
#include "desentry/storage/buffer_pool_manager.h"
#include "desentry/storage/page.h"

namespace desentry {

constexpr size_t kMaxKeyBytes = 64;
constexpr int kMaxKeysPerNode = 50;

class BPlusTree {
 public:
  BPlusTree(BufferPoolManager* bpm, page_id_t root_page_id) : bpm_(bpm), root_page_id_(root_page_id) {}

  // Allocates a fresh, empty tree (a single empty leaf as root) and returns
  // its root page id -- callers persist this id in the catalog.
  static StatusOr<page_id_t> CreateNew(BufferPoolManager* bpm);

  // Upsert: inserts a new key, or overwrites the RID of an existing key.
  // Splits nodes (propagating up to a new root if necessary) as needed.
  Status Insert(const std::string& key, const RID& rid);

  bool Search(const std::string& key, RID* out_rid) const;

  // See class comment: leaf-only delete, no rebalancing.
  bool Remove(const std::string& key);

  // Ordered scan starting at `start_key` (inclusive; empty string = start
  // from the beginning), returning up to `limit` entries (0 = unlimited).
  std::vector<std::pair<std::string, RID>> Scan(const std::string& start_key, size_t limit) const;

  page_id_t root_page_id() const { return root_page_id_; }

 private:
  page_id_t FindLeaf(const std::string& key) const;
  Status InsertIntoLeaf(page_id_t leaf_id, const std::string& key, const RID& rid);
  void InsertIntoParentAfterSplit(page_id_t left_id, const std::string& sep_key, page_id_t right_id);

  BufferPoolManager* bpm_;
  page_id_t root_page_id_;
  // Coarse-grained whole-tree lock. Correct but not fine-grained --
  // latch-crabbing (per-node locks, released as soon as a subtree is known
  // safe from a split/merge) is the standard next step for concurrent
  // throughput and is called out in ARCHITECTURE.md's roadmap; a single
  // mutex is the right MVP choice because it is trivially correct and this
  // is not yet the engine's bottleneck.
  mutable std::mutex tree_mu_;
};

}  // namespace desentry
