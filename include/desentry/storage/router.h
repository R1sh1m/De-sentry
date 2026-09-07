#pragma once
// The polyglot storage router (docs/architecture-v2.md Sec 4).
//
// v1 had exactly one physical shape for every collection: slotted pages
// indexed by a B+Tree. That is the right general-purpose shape and it stays
// the default -- but a 20-million-point sensor stream, a 384-dimension
// embedding table and a parent/child DOM tree each pay a real cost for
// being stored as independent key/value documents. The router lets a
// collection be *bound* to a backend whose physical layout matches its
// access pattern, without any layer above the router knowing which backend
// it got.
//
// Two invariants make this safe rather than a pile of incompatible stores:
//
//   1. **Every backend speaks CRDT bytes.** Put/Get/Scan move
//      `CrdtValue::Encode()` blobs, and MergeRemote() is implemented in
//      terms of `CrdtValue::Merge` on all of them. Convergence therefore
//      does not depend on which backend a peer happens to have bound a
//      collection to -- two peers can even disagree about the binding and
//      still converge, because the merge semantics live in the document
//      type, not the storage layout.
//   2. **Every backend answers Checksum() the same way.** The hex digest is
//      computed over the same sorted (key, top-HLC) fingerprint list that
//      NodeEngine::Summarize() uses, so `/_brain` parity between two peers
//      is a statement about *data*, never about storage layout.
//
// Backends fall into two families:
//
//   * Vendored OSS backbone -- SQLite (public domain; relational/metadata
//     core, JSON1, FTS5), DuckDB (MIT; columnar + analytical), LMDB
//     (OpenLDAP licence; generic/transit KV), sqlite-vec (Apache-2.0/MIT;
//     vector search). These are compiled in only when their amalgamation is
//     present under third_party/ (see third_party/README.md) and are gated
//     behind DESENTRY_WITH_SQLITE / _DUCKDB / _LMDB / _SQLITE_VEC. A tree
//     with no third_party/ sources still builds and runs completely --
//     that is the zero-fetched-dependencies requirement, and it is why the
//     from-scratch family below is not merely a demo.
//
//   * From-scratch, always compiled -- kv (the v1 B+Tree engine),
//     columnar_lite (RLE + delta over paged segments), ts_rollup (chunked
//     time segments + downsampled rollups + retention), vector_hnsw_lite
//     (HNSW-lite over 384-dim embeddings, optional int8 quantization) and
//     graph_adj (adjacency KV + an object view). All five reuse the
//     existing DiskManager / BufferPoolManager / WriteAheadLog / HLC
//     primitives rather than opening private file formats behind the
//     engine's back.
//
// Licence note, deliberate: no Xapian (GPL) and no TDengine (AGPL) anywhere
// in the tree, and no ClickHouse/QuestDB -- those are servers, and a server
// dependency would break the "one desktop app, no daemon to install"
// property this product is built around.

#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "desentry/common/status.h"
#include "desentry/storage/catalog.h"

namespace desentry {

// One (key, encoded-document) pair, as returned by Scan().
using EngineRow = std::pair<std::string, std::string>;

// -- the backend contract ---------------------------------------------------
// Deliberately narrow: five data operations plus three introspection
// operations. Anything a backend wants to do that isn't expressible here
// (a vector similarity search, a time-range rollup read) is exposed through
// a backend-specific interface that routes.cpp reaches via
// StorageRouter::Backend(name) and a dynamic_cast -- so the *common* path
// stays uniform and the specialised paths stay honest about being
// specialised, instead of bloating this interface with methods four of the
// five backends would have to stub out.
class EngineBackend {
 public:
  virtual ~EngineBackend() = default;

  // Stable identifier, matching the name used in node.json's `engines` list
  // and in CollectionMeta::engine.
  virtual std::string Name() const = 0;

  // Prepares the backend's own subdirectory under `data_dir` and adopts a
  // byte budget of `quota_mb` MiB (0 == unlimited). Idempotent.
  virtual Status Open(const std::string& data_dir, uint64_t quota_mb) = 0;

  // Physical upsert of already-CRDT-encoded document bytes. Returns
  // kOutOfSpace (never a partial write) when the write would exceed quota.
  virtual Status Put(const std::string& collection, const std::string& key,
                      const std::string& encoded_doc) = 0;

  virtual StatusOr<std::string> Get(const std::string& collection, const std::string& key) = 0;

  // Ordered scan from `start_key` (inclusive; empty == from the beginning),
  // at most `limit` rows (0 == unlimited).
  virtual std::vector<EngineRow> Scan(const std::string& collection, const std::string& start_key,
                                       size_t limit) = 0;

  // Merges a peer's encoded document into local state via CrdtValue::Merge
  // and stores the result. Separate from Put() because a backend may be
  // able to merge in place more cheaply than read-modify-write, and because
  // the two have different quota semantics (a merge that shrinks a document
  // must not be refused for want of quota).
  virtual Status MergeRemote(const std::string& collection, const std::string& key,
                              const std::string& remote_encoded_doc) = 0;

  // Deterministic hex fingerprint of one collection's live contents. Two
  // converged peers must produce identical digests regardless of physical
  // layout or insertion order -- see the header comment.
  virtual std::string Checksum(const std::string& collection) = 0;

  // Bytes currently attributed to this backend on disk.
  virtual uint64_t QuotaUse() const = 0;
  virtual uint64_t QuotaLimit() const = 0;

  // Full self-check of the backend's own on-disk structures (segment
  // headers, index/heap agreement, per-record CRCs). This is the backend's
  // half of POST /_ledger/verify: the ledger proves history wasn't
  // rewritten, Verify() proves the materialised state is readable.
  virtual Status Verify() = 0;

  // Forces everything durable. Called at checkpoint and shutdown.
  virtual Status Flush() = 0;

  // Collections this backend currently holds rows for.
  virtual std::vector<std::string> ListCollections() const = 0;
};

// -- cross-engine metadata index -------------------------------------------
// "Which engine, on which node, holds this key?" -- the question a client
// asks when it knows a key but not where it lives. In a deployment with the
// SQLite backbone compiled in this is a real SQLite table
// (`cross_engine_index(key TEXT PRIMARY KEY, engine TEXT, node_id TEXT,
// collection TEXT, updated_ms INTEGER)`); without it, the same rows live in
// a small fsync'd append log with an in-memory map over it. Both
// implementations satisfy the same interface and the same test.
struct IndexEntry {
  std::string key;
  std::string collection;
  std::string engine;
  std::string node_id;
  int64_t updated_ms = 0;
};

class CrossEngineIndex {
 public:
  static StatusOr<std::unique_ptr<CrossEngineIndex>> Open(const std::string& path);
  ~CrossEngineIndex();

  Status Upsert(const IndexEntry& entry);
  StatusOr<IndexEntry> Lookup(const std::string& key) const;
  std::vector<IndexEntry> ByEngine(const std::string& engine) const;
  std::vector<IndexEntry> ByCollection(const std::string& collection) const;
  size_t Size() const;
  Status Flush();

  // Rewrites the log with one record per live key, dropping superseded
  // versions. Called by the supervisor's housekeeping pass, never on the
  // write path.
  Status Compact();

 private:
  explicit CrossEngineIndex(std::string path) : path_(std::move(path)) {}
  Status Load();
  Status AppendRecord(const IndexEntry& entry);

  mutable std::mutex mu_;
  std::string path_;
  std::unordered_map<std::string, IndexEntry> entries_;
  std::unique_ptr<std::fstream> file_;
};

// -- the router -------------------------------------------------------------
class StorageRouter {
 public:
  struct Options {
    std::string data_dir = "./data";
    uint64_t quota_mb = 0;         // total node budget for this process
    // Percentage of quota_mb the data plane may use, i.e. NodeConfig's
    // quota_split.db_pct. Passed as a bare number rather than pulling
    // common/config.h into a storage header -- the router needs one value,
    // not the whole NodeConfig.
    uint32_t db_share_pct = 60;
    std::vector<std::string> engines{"kv"};
    std::string default_engine = "kv";
    Catalog* catalog = nullptr;    // borrowed; used to resolve collection -> engine
    std::string node_id;           // recorded in the cross-engine index
  };

  static StatusOr<std::unique_ptr<StorageRouter>> Open(const Options& options);
  ~StorageRouter();

  // Resolves the backend a collection is bound to, falling back to the
  // node's default engine. Never returns nullptr for a configured engine
  // name; an unknown name is rejected at BindCollection() time.
  EngineBackend* BackendFor(const std::string& collection);
  EngineBackend* Backend(const std::string& engine_name);
  std::vector<std::string> AvailableEngines() const;

  // Binds a collection to a backend and records it in the catalog. Refuses
  // an engine that isn't in the node's configured `engines` list, and
  // refuses a rebind of a populated collection (see Catalog::SetEngine).
  Status BindCollection(const std::string& collection, const std::string& engine_name);
  std::string EngineNameFor(const std::string& collection) const;

  // -- routed data operations ---------------------------------------------
  Status Put(const std::string& collection, const std::string& key, const std::string& encoded_doc);
  StatusOr<std::string> Get(const std::string& collection, const std::string& key);
  std::vector<EngineRow> Scan(const std::string& collection, const std::string& start_key, size_t limit);
  Status MergeRemote(const std::string& collection, const std::string& key,
                      const std::string& remote_encoded_doc);

  std::string Checksum(const std::string& collection);
  uint64_t QuotaUse() const;
  uint64_t QuotaLimit() const { return quota_bytes_; }
  Status Verify();
  Status Flush();

  CrossEngineIndex& index() { return *index_; }

 private:
  StorageRouter() = default;
  Status RegisterBackend(const std::string& name, const std::string& data_dir, uint64_t bytes);

  std::string data_dir_;
  std::string default_engine_;
  std::string node_id_;
  uint64_t quota_bytes_ = 0;
  Catalog* catalog_ = nullptr;

  mutable std::mutex mu_;
  std::unordered_map<std::string, std::unique_ptr<EngineBackend>> backends_;
  std::unique_ptr<CrossEngineIndex> index_;
};

// Factory used by StorageRouter and by the per-engine unit tests, so tests
// construct backends exactly the way production does.
StatusOr<std::unique_ptr<EngineBackend>> MakeBackend(const std::string& engine_name);

}  // namespace desentry
