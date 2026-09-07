#include "desentry/storage/engines/vector_hnsw_lite.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <queue>
#include <sstream>
#include <unordered_set>

#include "desentry/common/byte_buffer.h"
#include "desentry/common/json.h"
#include "desentry/common/logger.h"
#include "desentry/common/platform.h"
#include "desentry/storage/document_codec.h"
#include "desentry/storage/engines/columnar_lite.h"

namespace desentry {

namespace {

// SplitMix64 -- a deterministic PRNG seeded per collection. Deterministic on
// purpose: two runs over the same insert order build the same graph, which
// is what makes a recall regression reproducible instead of flaky.
uint64_t NextRandom(uint64_t* state) {
  uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

}  // namespace

bool VectorHnswLiteBackend::ExtractEmbedding(const std::string& encoded_doc,
                                              std::vector<float>* out) {
  JsonValue json;
  try {
    json = DecodeDocument(encoded_doc).ToJson();
  } catch (const std::exception&) {
    return false;
  }
  if (!json.is_object()) return false;
  for (const char* name : {"embedding", "vector", "values"}) {
    const JsonValue* v = json.Find(name);
    if (v == nullptr || !v->is_array()) continue;
    out->clear();
    out->reserve(v->AsArray().size());
    for (const JsonValue& component : v->AsArray()) {
      if (!component.is_number()) return false;
      out->push_back(static_cast<float>(component.AsDouble()));
    }
    return !out->empty();
  }
  return false;
}

void VectorHnswLiteBackend::Normalize(std::vector<float>* v) {
  double sum = 0;
  for (float x : *v) sum += static_cast<double>(x) * x;
  if (sum <= 0) return;  // an all-zero vector stays all-zero rather than becoming NaN
  const float inv = static_cast<float>(1.0 / std::sqrt(sum));
  for (float& x : *v) x *= inv;
}

// ---------------------------------------------------------------------------
// Snapshot codec
// ---------------------------------------------------------------------------

std::string VectorHnswLiteBackend::EncodeSnapshot(const CollectionState& state) {
  ByteWriter index;
  index.U32(kHnswBlobMagic);
  index.U32(static_cast<uint32_t>(state.dim));
  index.U8(state.quantized ? 1 : 0);
  index.I64(state.entry_point);
  index.U32(static_cast<uint32_t>(state.max_level));
  index.U32(static_cast<uint32_t>(state.nodes.size()));
  for (const Node& n : state.nodes) {
    index.Bytes(n.key);
    index.U32(static_cast<uint32_t>(n.level));
    index.U8(n.tombstone ? 1 : 0);
    if (state.quantized) {
      index.F64(n.qscale);
      std::string raw(reinterpret_cast<const char*>(n.qvec.data()), n.qvec.size());
      index.Bytes(raw);
    } else {
      std::string raw(reinterpret_cast<const char*>(n.vec.data()), n.vec.size() * sizeof(float));
      index.Bytes(raw);
    }
    index.U32(static_cast<uint32_t>(n.neighbours.size()));
    for (const std::vector<uint32_t>& level : n.neighbours) {
      index.U32(static_cast<uint32_t>(level.size()));
      for (uint32_t id : level) index.U32(id);
    }
  }
  std::string index_bytes = index.TakeString();

  std::vector<EngineRow> rows;
  rows.reserve(state.docs.size());
  for (const auto& [key, doc] : state.docs) rows.emplace_back(key, doc);
  std::string docs_bytes = ColumnarLiteBackend::EncodeSegment(rows);

  ByteWriter w;
  w.U32(kHnswBlobMagic);
  w.U32(static_cast<uint32_t>(index_bytes.size()));
  w.U32(static_cast<uint32_t>(docs_bytes.size()));
  w.RawBytes(index_bytes);
  w.RawBytes(docs_bytes);
  return w.TakeString();
}

Status VectorHnswLiteBackend::DecodeSnapshot(const std::string& blob, CollectionState* out) {
  constexpr size_t kHeaderBytes = 12;
  if (blob.size() < kHeaderBytes) return Status::Corruption("vector: snapshot truncated");
  std::string index_bytes;
  std::string docs_bytes;
  try {
    ByteReader outer(blob);
    if (outer.U32() != kHnswBlobMagic) return Status::Corruption("vector: snapshot bad magic");
    uint32_t index_len = outer.U32();
    uint32_t docs_len = outer.U32();
    if (kHeaderBytes + static_cast<size_t>(index_len) + docs_len != blob.size()) {
      return Status::Corruption("vector: snapshot block lengths do not span the blob");
    }
    index_bytes = outer.RawBytes(index_len);
    docs_bytes = outer.RawBytes(docs_len);

    ByteReader r(index_bytes);
    if (r.U32() != kHnswBlobMagic) return Status::Corruption("vector: index block bad magic");
    out->dim = r.U32();
    out->quantized = r.U8() != 0;
    out->entry_point = r.I64();
    out->max_level = r.U32();
    uint32_t node_count = r.U32();
    out->nodes.clear();
    out->nodes.reserve(node_count);
    out->key_to_node.clear();
    for (uint32_t i = 0; i < node_count; ++i) {
      Node n;
      n.key = r.Bytes();
      n.level = r.U32();
      n.tombstone = r.U8() != 0;
      if (out->quantized) {
        n.qscale = static_cast<float>(r.F64());
        std::string raw = r.Bytes();
        n.qvec.assign(reinterpret_cast<const int8_t*>(raw.data()),
                      reinterpret_cast<const int8_t*>(raw.data()) + raw.size());
      } else {
        std::string raw = r.Bytes();
        if (raw.size() % sizeof(float) != 0) return Status::Corruption("vector: bad vector length");
        n.vec.resize(raw.size() / sizeof(float));
        std::memcpy(n.vec.data(), raw.data(), raw.size());
      }
      uint32_t levels = r.U32();
      n.neighbours.resize(levels);
      for (uint32_t l = 0; l < levels; ++l) {
        uint32_t count = r.U32();
        n.neighbours[l].reserve(count);
        for (uint32_t j = 0; j < count; ++j) n.neighbours[l].push_back(r.U32());
      }
      out->key_to_node[n.key] = i;
      out->nodes.push_back(std::move(n));
    }
  } catch (const std::exception& e) {
    return Status::Corruption(std::string("vector: malformed snapshot: ") + e.what());
  }

  auto rows = ColumnarLiteBackend::DecodeSegment(docs_bytes);
  if (!rows.ok()) return rows.status();
  out->docs.clear();
  for (auto& [key, doc] : rows.value()) out->docs.emplace(key, std::move(doc));

  // Cross-check: every graph node must have a document and vice versa. A
  // snapshot where those disagree is corrupt, and finding out at load time
  // beats finding out when a search returns a key with no row behind it.
  for (const Node& n : out->nodes) {
    if (!n.tombstone && out->docs.find(n.key) == out->docs.end()) {
      return Status::Corruption("vector: snapshot has a graph node with no document: " + n.key);
    }
  }
  return Status::OK();
}

// ---------------------------------------------------------------------------
// Graph
// ---------------------------------------------------------------------------

float VectorHnswLiteBackend::Similarity(const CollectionState& state, uint32_t a,
                                         const std::vector<float>& q,
                                         const std::vector<int8_t>& qq, float qscale) const {
  const Node& n = state.nodes[a];
  if (state.quantized) {
    if (n.qvec.size() != qq.size()) return -2.0f;
    int32_t dot = 0;
    for (size_t i = 0; i < qq.size(); ++i) {
      dot += static_cast<int32_t>(n.qvec[i]) * static_cast<int32_t>(qq[i]);
    }
    // Both sides were normalised before quantization, so the dot product of
    // the de-scaled integers is the cosine similarity.
    return static_cast<float>(dot) * n.qscale * qscale;
  }
  if (n.vec.size() != q.size()) return -2.0f;
  float dot = 0;
  for (size_t i = 0; i < q.size(); ++i) dot += n.vec[i] * q[i];
  return dot;
}

std::vector<uint32_t> VectorHnswLiteBackend::SearchLayer(const CollectionState& state,
                                                           const std::vector<float>& q,
                                                           const std::vector<int8_t>& qq,
                                                           float qscale, uint32_t entry,
                                                           size_t level, size_t ef) const {
  // Standard best-first expansion with two heaps: `candidates` is a
  // max-similarity heap of frontier nodes, `results` a min-similarity heap
  // capped at ef.
  using Scored = std::pair<float, uint32_t>;
  std::priority_queue<Scored> candidates;                                     // best first
  std::priority_queue<Scored, std::vector<Scored>, std::greater<Scored>> results;  // worst first
  std::unordered_set<uint32_t> visited;

  const float entry_score = Similarity(state, entry, q, qq, qscale);
  candidates.emplace(entry_score, entry);
  results.emplace(entry_score, entry);
  visited.insert(entry);

  while (!candidates.empty()) {
    Scored best = candidates.top();
    candidates.pop();
    if (results.size() >= ef && best.first < results.top().first) break;

    const Node& n = state.nodes[best.second];
    if (level >= n.neighbours.size()) continue;
    for (uint32_t neighbour : n.neighbours[level]) {
      if (neighbour >= state.nodes.size()) continue;
      if (!visited.insert(neighbour).second) continue;
      const float score = Similarity(state, neighbour, q, qq, qscale);
      if (results.size() < ef || score > results.top().first) {
        candidates.emplace(score, neighbour);
        results.emplace(score, neighbour);
        if (results.size() > ef) results.pop();
      }
    }
  }

  std::vector<Scored> ordered;
  ordered.reserve(results.size());
  while (!results.empty()) {
    ordered.push_back(results.top());
    results.pop();
  }
  std::sort(ordered.begin(), ordered.end(), [](const Scored& a, const Scored& b) {
    return a.first > b.first;  // most similar first
  });
  std::vector<uint32_t> out;
  out.reserve(ordered.size());
  for (const Scored& s : ordered) out.push_back(s.second);
  return out;
}

size_t VectorHnswLiteBackend::RandomLevel(CollectionState* state) const {
  // Level ~ floor(-ln(U) * mL) with mL = 1/ln(M), the assignment from the
  // HNSW paper: it makes the expected number of nodes per level fall off
  // geometrically, which is what gives the logarithmic search.
  const double mL = 1.0 / std::log(static_cast<double>(kHnswM));
  const double u = (static_cast<double>(NextRandom(&state->rng_state) >> 11) + 1.0) /
                   9007199254740993.0;  // (0, 1]
  const double level = -std::log(u) * mL;
  size_t l = static_cast<size_t>(level);
  return std::min<size_t>(l, 24);  // hard ceiling: 24 levels is ~10^28 nodes
}

void VectorHnswLiteBackend::LinkNode(CollectionState* state, uint32_t node_idx) {
  Node& node = state->nodes[node_idx];
  std::vector<float> q = node.vec;
  std::vector<int8_t> qq = node.qvec;
  float qscale = node.qscale;
  if (state->quantized && q.empty()) {
    // Dequantize once for the construction search; the graph itself stays
    // quantized.
    q.resize(qq.size());
    for (size_t i = 0; i < qq.size(); ++i) q[i] = static_cast<float>(qq[i]) * qscale;
  }

  if (state->entry_point < 0) {
    state->entry_point = node_idx;
    state->max_level = node.level;
    return;
  }

  uint32_t current = static_cast<uint32_t>(state->entry_point);
  // Greedy descent through the layers above this node's own top level.
  for (size_t level = state->max_level; level > node.level; --level) {
    std::vector<uint32_t> nearest = SearchLayer(*state, q, qq, qscale, current, level, 1);
    if (!nearest.empty()) current = nearest.front();
    if (level == 0) break;
  }

  for (size_t level = std::min(node.level, state->max_level) + 1; level-- > 0;) {
    std::vector<uint32_t> candidates =
        SearchLayer(*state, q, qq, qscale, current, level, kHnswEfConstruction);
    const size_t cap = level == 0 ? kHnswM0 : kHnswM;

    std::vector<uint32_t>& mine = state->nodes[node_idx].neighbours[level];
    for (uint32_t candidate : candidates) {
      if (candidate == node_idx) continue;
      if (mine.size() >= cap) break;
      mine.push_back(candidate);

      // Bidirectional link, pruning the neighbour's list back to its cap by
      // similarity when it overflows.
      Node& other = state->nodes[candidate];
      if (level >= other.neighbours.size()) continue;
      std::vector<uint32_t>& theirs = other.neighbours[level];
      theirs.push_back(node_idx);
      if (theirs.size() > cap) {
        std::vector<float> oq = other.vec;
        if (state->quantized && oq.empty()) {
          oq.resize(other.qvec.size());
          for (size_t i = 0; i < other.qvec.size(); ++i) {
            oq[i] = static_cast<float>(other.qvec[i]) * other.qscale;
          }
        }
        std::sort(theirs.begin(), theirs.end(), [&](uint32_t a, uint32_t b) {
          return Similarity(*state, a, oq, other.qvec, other.qscale) >
                 Similarity(*state, b, oq, other.qvec, other.qscale);
        });
        theirs.resize(cap);
      }
    }
    if (!candidates.empty()) current = candidates.front();
    if (level == 0) break;
  }

  if (node.level > state->max_level) {
    state->max_level = node.level;
    state->entry_point = node_idx;
  }
}

// ---------------------------------------------------------------------------
// Backend
// ---------------------------------------------------------------------------

Status VectorHnswLiteBackend::Open(const std::string& data_dir, uint64_t quota_mb) {
  dir_ = data_dir + "/vector_hnsw_lite";
  if (!MakeDirs(dir_)) return Status::IOError("cannot create backend directory: " + dir_);
  manifest_path_ = dir_ + "/index.json";

  auto store_or = SegmentStore::Open(dir_ + "/vector.dsf");
  if (!store_or.ok()) return store_or.status();
  store_ = std::move(store_or.value());

  SetQuotaBytes(quota_mb * 1024ull * 1024ull);
  SetBytesUsed(store_->BytesOnDisk());
  return LoadManifest();
}

Status VectorHnswLiteBackend::LoadManifest() {
  std::ifstream f(manifest_path_);
  if (!f.is_open()) return Status::OK();
  std::ostringstream ss;
  ss << f.rdbuf();
  if (ss.str().empty()) return Status::OK();
  JsonValue root;
  try {
    root = JsonValue::Parse(ss.str());
  } catch (const std::exception& e) {
    return Status::Corruption(std::string("vector manifest parse error: ") + e.what());
  }
  if (!root.is_array()) return Status::OK();

  std::lock_guard<std::mutex> lock(mu_);
  for (const JsonValue& entry : root.AsArray()) {
    const std::string name = entry.Get("collection").AsString();
    const page_id_t page = static_cast<page_id_t>(entry.Get("snapshot_page_id").AsInt());
    if (name.empty() || page == kInvalidPageId) continue;
    auto blob = store_->ReadBlob(page);
    if (!blob.ok()) return blob.status();
    CollectionState state;
    Status st = DecodeSnapshot(blob.value(), &state);
    if (!st.ok()) return st;
    state.snapshot_page_id = page;
    collections_[name] = std::move(state);
  }
  return Status::OK();
}

Status VectorHnswLiteBackend::SaveManifest() {
  JsonValue::Array arr;
  for (const auto& [name, state] : collections_) {
    if (state.snapshot_page_id == kInvalidPageId) continue;
    JsonValue::Object o;
    o.emplace_back("collection", JsonValue(name));
    o.emplace_back("snapshot_page_id", JsonValue(static_cast<int64_t>(state.snapshot_page_id)));
    o.emplace_back("vectors", JsonValue(static_cast<int64_t>(state.nodes.size())));
    o.emplace_back("dim", JsonValue(static_cast<int64_t>(state.dim)));
    o.emplace_back("quantized", JsonValue(state.quantized));
    arr.emplace_back(std::move(o));
  }
  const std::string tmp = manifest_path_ + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc | std::ios::binary);
    if (!f.is_open()) return Status::IOError("vector: cannot write " + tmp);
    f << JsonValue(std::move(arr)).Dump();
    f.flush();
    if (!f.good()) return Status::IOError("vector: manifest write failed");
  }
  std::remove(manifest_path_.c_str());
  if (std::rename(tmp.c_str(), manifest_path_.c_str()) != 0) {
    return Status::IOError("vector: cannot commit " + manifest_path_);
  }
  return Status::OK();
}

Status VectorHnswLiteBackend::SetQuantized(const std::string& collection, bool quantized) {
  std::lock_guard<std::mutex> lock(mu_);
  CollectionState& state = collections_[collection];
  if (!state.nodes.empty() && state.quantized != quantized) {
    return Status::InvalidArgument(
        "cannot change quantization on a populated vector collection: create a new collection");
  }
  state.quantized = quantized;
  state.dirty = true;
  return Status::OK();
}

Status VectorHnswLiteBackend::Put(const std::string& collection, const std::string& key,
                                   const std::string& encoded_doc) {
  uint64_t old_cost = 0;
  {
    auto existing = Get(collection, key);
    if (existing.ok()) old_cost = RecordCost(key, existing.value());
  }
  Status charged = Charge(old_cost, RecordCost(key, encoded_doc));
  if (!charged.ok()) return charged;

  std::vector<float> vec;
  const bool has_vector = ExtractEmbedding(encoded_doc, &vec);

  std::lock_guard<std::mutex> lock(mu_);
  CollectionState& state = collections_[collection];
  state.docs[key] = encoded_doc;
  state.dirty = true;

  if (!has_vector) {
    // A row with no embedding is stored and readable, just not searchable.
    // Rejecting it would make a vector collection unable to hold the
    // metadata rows that usually sit alongside the embeddings.
    return Status::OK();
  }

  if (state.dim == 0) {
    state.dim = vec.size();
  } else if (vec.size() != state.dim) {
    return Status::InvalidArgument("embedding dimension " + std::to_string(vec.size()) +
                                    " does not match collection dimension " +
                                    std::to_string(state.dim));
  }
  Normalize(&vec);

  auto existing_node = state.key_to_node.find(key);
  if (existing_node != state.key_to_node.end()) {
    // An update replaces the vector in place and leaves the graph edges
    // alone. Edges become slightly stale; recall degrades gracefully rather
    // than the index becoming wrong. A rebuild pass is Compact()'s job.
    Node& n = state.nodes[existing_node->second];
    if (state.quantized) {
      n.qvec.resize(vec.size());
      for (size_t i = 0; i < vec.size(); ++i) {
        n.qvec[i] = static_cast<int8_t>(std::lround(std::max(-1.0f, std::min(1.0f, vec[i])) * 127.0f));
      }
      n.qscale = 1.0f / 127.0f;
      n.vec.clear();
    } else {
      n.vec = vec;
    }
    n.tombstone = false;
    return Status::OK();
  }

  Node node;
  node.key = key;
  node.level = RandomLevel(&state);
  node.neighbours.resize(node.level + 1);
  if (state.quantized) {
    node.qvec.resize(vec.size());
    for (size_t i = 0; i < vec.size(); ++i) {
      node.qvec[i] = static_cast<int8_t>(std::lround(std::max(-1.0f, std::min(1.0f, vec[i])) * 127.0f));
    }
    node.qscale = 1.0f / 127.0f;
  } else {
    node.vec = vec;
  }

  const uint32_t idx = static_cast<uint32_t>(state.nodes.size());
  state.nodes.push_back(std::move(node));
  state.key_to_node[key] = idx;
  LinkNode(&state, idx);
  return Status::OK();
}

StatusOr<std::string> VectorHnswLiteBackend::Get(const std::string& collection,
                                                   const std::string& key) {
  std::lock_guard<std::mutex> lock(mu_);
  auto coll_it = collections_.find(collection);
  if (coll_it == collections_.end()) return Status::NotFound("no such collection: " + collection);
  auto it = coll_it->second.docs.find(key);
  if (it == coll_it->second.docs.end()) return Status::NotFound("no such key: " + key);
  return it->second;
}

std::vector<EngineRow> VectorHnswLiteBackend::Scan(const std::string& collection,
                                                     const std::string& start_key, size_t limit) {
  std::lock_guard<std::mutex> lock(mu_);
  auto coll_it = collections_.find(collection);
  if (coll_it == collections_.end()) return {};
  std::vector<EngineRow> out;
  const std::map<std::string, std::string>& docs = coll_it->second.docs;
  for (auto it = docs.lower_bound(start_key); it != docs.end(); ++it) {
    out.emplace_back(it->first, it->second);
    if (limit != 0 && out.size() >= limit) break;
  }
  return out;
}

StatusOr<std::vector<VectorHit>> VectorHnswLiteBackend::Search(const std::string& collection,
                                                                 const std::vector<float>& query,
                                                                 size_t k) {
  if (k == 0) return std::vector<VectorHit>();
  std::lock_guard<std::mutex> lock(mu_);
  auto coll_it = collections_.find(collection);
  if (coll_it == collections_.end()) return Status::NotFound("no such collection: " + collection);
  CollectionState& state = coll_it->second;
  if (state.nodes.empty()) return std::vector<VectorHit>();
  if (query.size() != state.dim) {
    return Status::InvalidArgument("query dimension " + std::to_string(query.size()) +
                                    " does not match collection dimension " +
                                    std::to_string(state.dim));
  }

  std::vector<float> q = query;
  Normalize(&q);
  std::vector<int8_t> qq;
  float qscale = 0;
  if (state.quantized) {
    qq.resize(q.size());
    for (size_t i = 0; i < q.size(); ++i) {
      qq[i] = static_cast<int8_t>(std::lround(std::max(-1.0f, std::min(1.0f, q[i])) * 127.0f));
    }
    qscale = 1.0f / 127.0f;
  }

  std::vector<std::pair<float, uint32_t>> scored;
  if (state.nodes.size() < kHnswFlatThreshold) {
    // Below the threshold an exhaustive scan is both faster and exact, so
    // the graph is not consulted at all. Saying this out loud beats
    // pretending the ANN structure earns its keep at 40 rows.
    scored.reserve(state.nodes.size());
    for (uint32_t i = 0; i < state.nodes.size(); ++i) {
      if (state.nodes[i].tombstone) continue;
      scored.emplace_back(Similarity(state, i, q, qq, qscale), i);
    }
  } else {
    uint32_t current = static_cast<uint32_t>(std::max<int64_t>(0, state.entry_point));
    for (size_t level = state.max_level; level > 0; --level) {
      std::vector<uint32_t> nearest = SearchLayer(state, q, qq, qscale, current, level, 1);
      if (!nearest.empty()) current = nearest.front();
    }
    const size_t ef = std::max(kHnswEfSearch, k);
    for (uint32_t idx : SearchLayer(state, q, qq, qscale, current, 0, ef)) {
      if (state.nodes[idx].tombstone) continue;
      scored.emplace_back(Similarity(state, idx, q, qq, qscale), idx);
    }
  }

  std::sort(scored.begin(), scored.end(),
            [](const std::pair<float, uint32_t>& a, const std::pair<float, uint32_t>& b) {
              return a.first > b.first;
            });
  std::vector<VectorHit> hits;
  hits.reserve(std::min(k, scored.size()));
  for (size_t i = 0; i < scored.size() && hits.size() < k; ++i) {
    hits.push_back(VectorHit{state.nodes[scored[i].second].key, scored[i].first});
  }
  return hits;
}

VectorHnswLiteBackend::Stats VectorHnswLiteBackend::StatsFor(const std::string& collection) const {
  Stats stats;
  std::lock_guard<std::mutex> lock(mu_);
  auto it = collections_.find(collection);
  if (it == collections_.end()) return stats;
  const CollectionState& state = it->second;
  stats.vectors = state.nodes.size();
  for (const Node& n : state.nodes) {
    if (n.tombstone) ++stats.tombstoned;
  }
  stats.dim = state.dim;
  stats.quantized = state.quantized;
  stats.max_level = state.max_level;
  stats.flat_mode = state.nodes.size() < kHnswFlatThreshold;
  return stats;
}

Status VectorHnswLiteBackend::SnapshotLocked(const std::string& collection, CollectionState* state) {
  if (!state->dirty) return Status::OK();
  std::string blob = EncodeSnapshot(*state);
  auto page_or = store_->AppendBlob(blob);
  if (!page_or.ok()) return page_or.status();
  state->snapshot_page_id = page_or.value();
  state->dirty = false;
  (void)collection;
  SetBytesUsed(store_->BytesOnDisk());
  return Status::OK();
}

Status VectorHnswLiteBackend::Verify() {
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& [name, state] : collections_) {
    // Graph integrity: every neighbour id must be in range, and every live
    // node must have a document.
    for (const Node& n : state.nodes) {
      if (!n.tombstone && state.docs.find(n.key) == state.docs.end()) {
        return Status::Corruption("vector: node without a document in " + name + ": " + n.key);
      }
      if (n.neighbours.size() != n.level + 1) {
        return Status::Corruption("vector: node " + n.key + " has " +
                                   std::to_string(n.neighbours.size()) + " levels but level " +
                                   std::to_string(n.level));
      }
      for (const std::vector<uint32_t>& level : n.neighbours) {
        for (uint32_t id : level) {
          if (id >= state.nodes.size()) {
            return Status::Corruption("vector: out-of-range neighbour id in " + name);
          }
        }
      }
    }
    if (state.snapshot_page_id != kInvalidPageId) {
      Status st = store_->VerifyBlob(state.snapshot_page_id);
      if (!st.ok()) return Status::Corruption("vector: snapshot of " + name + ": " + st.message());
    }
  }
  return Status::OK();
}

Status VectorHnswLiteBackend::Flush() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& [name, state] : collections_) {
      Status st = SnapshotLocked(name, &state);
      if (!st.ok()) return st;
    }
    Status st = SaveManifest();
    if (!st.ok()) return st;
  }
  return store_->Flush();
}

std::vector<std::string> VectorHnswLiteBackend::ListCollections() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::string> names;
  names.reserve(collections_.size());
  for (const auto& [name, state] : collections_) {
    (void)state;
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

}  // namespace desentry
