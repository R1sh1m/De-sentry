#pragma once
// `vector_hnsw_lite` -- a from-scratch approximate-nearest-neighbour backend
// for embedding collections, sized for the 384-dimension MiniLM vectors the
// app produces (app/resources/ + ONNX Runtime sidecar).
//
// Why HNSW and not a flat scan: a flat cosine scan over N vectors is O(N)
// per query and is genuinely fine to a few thousand rows -- which is why
// this backend falls back to it below kHnswFlatThreshold rather than
// pretending a graph helps there. Past that, a hierarchical navigable
// small-world graph turns the query into O(log N) hops, and it is the one
// ANN structure that is both incremental (no rebuild on insert) and simple
// enough to implement correctly in a few hundred lines.
//
// "Lite" is an honest label, and here is exactly what it means:
//   * Neighbour selection is distance-ordered truncation to M, not the full
//     heuristic from the HNSW paper (which additionally prefers neighbours
//     that improve graph diversity). Recall is a little lower on adversarial
//     distributions; the code is a third the size and has no unproven
//     pruning rule.
//   * Deletes are soft: a removed vector stays in the graph as a tombstone
//     and is filtered out of results. Rebuilding the graph on delete is the
//     documented alternative, deferred deliberately.
//   * The graph and its vectors are memory-resident, snapshot-persisted to
//     the SegmentStore on Flush. This is inherent to HNSW rather than a
//     shortcut -- every production HNSW implementation holds the graph in
//     RAM -- and it is bounded by the collection's quota, which is why the
//     app's sizing step shifts +15% of quota into the cache/index bucket
//     when it proposes this engine.
//
// Vectors are L2-normalised on insert, so cosine similarity is a plain dot
// product and the graph's distance function is 1 - dot.
//
// Optional int8 quantization stores each component as a signed byte with a
// per-vector scale: 4x less memory, and for normalised embeddings the recall
// cost is small. It is off by default and enabled per collection, because
// "quietly lossy by default" is the wrong default for a database.

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "desentry/storage/engines/engine_common.h"
#include "desentry/storage/segment_store.h"

namespace desentry {

constexpr size_t kHnswDefaultDim = 384;  // sentence-transformers/all-MiniLM-L6-v2
constexpr size_t kHnswM = 16;            // neighbours per node above level 0
constexpr size_t kHnswM0 = 32;           // neighbours at level 0
constexpr size_t kHnswEfConstruction = 100;
constexpr size_t kHnswEfSearch = 64;
constexpr size_t kHnswFlatThreshold = 1000;  // below this, exhaustive scan wins
constexpr uint32_t kHnswBlobMagic = 0x44535601;  // "DSV" + version 1

struct VectorHit {
  std::string key;
  float score = 0;  // cosine similarity in [-1, 1]; higher is closer
};

class VectorHnswLiteBackend : public BaseBackend {
 public:
  std::string Name() const override { return "vector_hnsw_lite"; }

  Status Open(const std::string& data_dir, uint64_t quota_mb) override;
  Status Put(const std::string& collection, const std::string& key,
              const std::string& encoded_doc) override;
  StatusOr<std::string> Get(const std::string& collection, const std::string& key) override;
  std::vector<EngineRow> Scan(const std::string& collection, const std::string& start_key,
                               size_t limit) override;
  Status Verify() override;
  Status Flush() override;
  std::vector<std::string> ListCollections() const override;

  // -- vector surface ------------------------------------------------------
  // k nearest neighbours of `query` (any length; it is normalised here).
  // Returns kInvalidArgument if the query's dimension disagrees with the
  // collection's -- silently truncating or zero-padding would produce
  // plausible-looking nonsense.
  StatusOr<std::vector<VectorHit>> Search(const std::string& collection,
                                           const std::vector<float>& query, size_t k);

  // Turns int8 quantization on for a collection. Only meaningful before the
  // first insert; afterwards it returns kInvalidArgument rather than
  // silently re-quantizing an existing index.
  Status SetQuantized(const std::string& collection, bool quantized);

  struct Stats {
    uint64_t vectors = 0;
    uint64_t tombstoned = 0;
    size_t dim = 0;
    bool quantized = false;
    size_t max_level = 0;
    bool flat_mode = false;
  };
  Stats StatsFor(const std::string& collection) const;

  // Pulls the embedding out of a CRDT document: the first present array
  // field named `embedding`, `vector` or `values`. Exposed for tests and so
  // the failure mode ("no embedding field") is checkable, not guessed at.
  static bool ExtractEmbedding(const std::string& encoded_doc, std::vector<float>* out);
  static void Normalize(std::vector<float>* v);

 private:
  struct Node {
    std::string key;
    std::vector<float> vec;      // normalised, when not quantized
    std::vector<int8_t> qvec;    // normalised then scaled, when quantized
    float qscale = 0;
    size_t level = 0;
    bool tombstone = false;
    std::vector<std::vector<uint32_t>> neighbours;  // per level
  };

  struct CollectionState {
    std::vector<Node> nodes;
    std::unordered_map<std::string, uint32_t> key_to_node;
    std::map<std::string, std::string> docs;  // full document bytes, by key
    size_t dim = 0;
    bool quantized = false;
    int64_t entry_point = -1;
    size_t max_level = 0;
    page_id_t snapshot_page_id = kInvalidPageId;
    bool dirty = false;
    uint64_t rng_state = 0x9E3779B97F4A7C15ull;
  };

  // All of these assume mu_ is held.
  float Similarity(const CollectionState& state, uint32_t a, const std::vector<float>& q,
                    const std::vector<int8_t>& qq, float qscale) const;
  std::vector<uint32_t> SearchLayer(const CollectionState& state, const std::vector<float>& q,
                                     const std::vector<int8_t>& qq, float qscale,
                                     uint32_t entry, size_t level, size_t ef) const;
  void LinkNode(CollectionState* state, uint32_t node_idx);
  size_t RandomLevel(CollectionState* state) const;
  Status SnapshotLocked(const std::string& collection, CollectionState* state);

  Status SaveManifest();
  Status LoadManifest();
  static std::string EncodeSnapshot(const CollectionState& state);
  static Status DecodeSnapshot(const std::string& blob, CollectionState* out);

  std::string dir_;
  std::string manifest_path_;
  std::unique_ptr<SegmentStore> store_;

  mutable std::mutex mu_;
  std::unordered_map<std::string, CollectionState> collections_;
};

}  // namespace desentry
