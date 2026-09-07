#pragma once
// `graph_adj` -- adjacency-list storage for parent/child and general graph
// collections (DOM trees, org charts, dependency graphs, scene graphs), plus
// the object-view projection that makes an object graph queryable the way a
// relational table is.
//
// The shape it fixes: under `kv`, "give me the children of node X" is a full
// scan, because parenthood lives *inside* documents and nothing indexes it.
// graph_adj derives the edges on write and keeps two adjacency maps -- out
// (parent -> children) and in (child -> parents) -- so both directions are a
// hash lookup, and a subtree walk costs one lookup per node rather than one
// scan per level.
//
// Edges are derived, never separately authored. That matters: a document is
// still the single source of truth, so replication and CRDT merge are
// unchanged, and the adjacency maps are a pure function of the documents
// that can always be rebuilt (Rebuild()). An edge model with its own
// independently-writable edge records would need its own conflict
// resolution, which is exactly the complexity the CRDT document model
// exists to avoid.
//
// Edge fields, in priority order:
//   * `parent`   -- a string key; contributes one in-edge.
//   * `children` -- an array of string keys; contributes out-edges.
//   * `edges`    -- an array of either string keys or
//                   {"to": key, "label": string} objects, for labelled
//                   general graphs.
//
// Object view (the "OOPS-RDBMS" mapping): documents carrying a `_class`
// field are projected as a table whose columns are the union of that class's
// scalar fields, with graph edges surfaced as foreign-key-shaped columns.
// When the vendored SQLite backbone is compiled in, ObjectView() is backed
// by a real SQLite table so JSON1/FTS5 queries work against it; without it,
// the same projection is computed on demand from the adjacency maps and the
// documents. Both paths return the identical ObjectTable, and the unit test
// asserts that rather than trusting it.
//
// Document bytes themselves are delegated to an embedded KvBPlusBackend, so
// graph collections get the same paged/buffer-pooled/B+Tree-indexed storage
// as everything else instead of a private file format.

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "desentry/storage/engines/engine_common.h"
#include "desentry/storage/engines/kv_bplus.h"
#include "desentry/storage/segment_store.h"

namespace desentry {

constexpr uint32_t kGraphAdjMagic = 0x44534701;  // "DSG" + version 1
constexpr size_t kGraphMaxWalkDepth = 64;

struct GraphEdge {
  std::string to;
  std::string label;
  // The document that *declared* this edge. Both a node's own `children`
  // list and a child's `parent` field produce an edge into the same
  // adjacency slot, so without an owner, re-indexing either document would
  // silently drop the other's contribution. Ownership is what makes an
  // update to one document remove exactly the edges that document asserted.
  std::string owner;
};

// A class projection: column names plus one row per object of that class.
// Values are rendered as JSON text so a heterogeneous field keeps its type
// information instead of being flattened to a string.
struct ObjectTable {
  std::string class_name;
  std::vector<std::string> columns;
  std::vector<std::vector<std::string>> rows;  // parallel to `columns`
  std::vector<std::string> keys;               // primary key per row
};

class GraphAdjBackend : public BaseBackend {
 public:
  std::string Name() const override { return "graph_adj"; }

  Status Open(const std::string& data_dir, uint64_t quota_mb) override;
  Status Put(const std::string& collection, const std::string& key,
              const std::string& encoded_doc) override;
  StatusOr<std::string> Get(const std::string& collection, const std::string& key) override;
  std::vector<EngineRow> Scan(const std::string& collection, const std::string& start_key,
                               size_t limit) override;
  Status Verify() override;
  Status Flush() override;
  std::vector<std::string> ListCollections() const override;

  // -- graph surface -------------------------------------------------------
  std::vector<GraphEdge> OutEdges(const std::string& collection, const std::string& key) const;
  std::vector<GraphEdge> InEdges(const std::string& collection, const std::string& key) const;
  // Breadth-first walk from `root`, up to `max_depth` levels (0 == unlimited,
  // still bounded by kGraphMaxWalkDepth so a cycle cannot hang a request).
  std::vector<std::string> Descendants(const std::string& collection, const std::string& root,
                                        size_t max_depth) const;
  std::vector<std::string> Ancestors(const std::string& collection, const std::string& node) const;
  // Keys with no in-edges: the roots of a forest, which is what a tree view
  // needs to render without scanning.
  std::vector<std::string> Roots(const std::string& collection) const;

  // -- object view ---------------------------------------------------------
  std::vector<std::string> Classes(const std::string& collection) const;
  StatusOr<ObjectTable> ObjectView(const std::string& collection, const std::string& class_name);

  // Recomputes every adjacency map from the stored documents. Used by
  // Verify()'s repair path and after a bulk import.
  Status Rebuild(const std::string& collection);

  struct Stats {
    uint64_t nodes = 0;
    uint64_t edges = 0;
    uint64_t roots = 0;
    uint64_t classes = 0;
  };
  Stats StatsFor(const std::string& collection) const;

  // Exposed for tests: the documented edge-derivation rules, as code.
  static std::vector<GraphEdge> DeriveOutEdges(const std::string& encoded_doc);
  static std::string DeriveParent(const std::string& encoded_doc);
  static std::string DeriveClass(const std::string& encoded_doc);

 private:
  struct CollectionState {
    std::unordered_map<std::string, std::vector<GraphEdge>> out_edges;
    std::unordered_map<std::string, std::vector<GraphEdge>> in_edges;
    std::map<std::string, std::set<std::string>> class_members;  // class -> keys
    std::unordered_map<std::string, std::string> key_class;
    // doc key -> the (from, to) pairs it declared, so un-indexing is exact
    // rather than a scan of every adjacency list.
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> declared;
    page_id_t snapshot_page_id = kInvalidPageId;
    bool dirty = false;
  };

  void IndexDocumentLocked(CollectionState* state, const std::string& key,
                            const std::string& encoded_doc);
  void UnindexDocumentLocked(CollectionState* state, const std::string& key);
  Status SnapshotLocked(CollectionState* state);
  Status SaveManifest();
  Status LoadManifest();
  static std::string EncodeAdjacency(const CollectionState& state);
  static Status DecodeAdjacency(const std::string& blob, CollectionState* out);

  std::string dir_;
  std::string manifest_path_;
  std::unique_ptr<KvBPlusBackend> docs_;
  std::unique_ptr<SegmentStore> store_;

  mutable std::mutex mu_;
  std::unordered_map<std::string, CollectionState> collections_;
};

}  // namespace desentry
