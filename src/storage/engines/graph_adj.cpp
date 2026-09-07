#include "desentry/storage/engines/graph_adj.h"

#include <algorithm>
#include <deque>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include "desentry/common/byte_buffer.h"
#include "desentry/common/json.h"
#include "desentry/common/logger.h"
#include "desentry/common/platform.h"
#include "desentry/storage/document_codec.h"

namespace desentry {

namespace {

JsonValue ToJsonSafe(const std::string& encoded_doc) {
  try {
    return DecodeDocument(encoded_doc).ToJson();
  } catch (const std::exception&) {
    return JsonValue();
  }
}

}  // namespace

std::string GraphAdjBackend::DeriveParent(const std::string& encoded_doc) {
  JsonValue json = ToJsonSafe(encoded_doc);
  if (!json.is_object()) return std::string();
  const JsonValue* parent = json.Find("parent");
  if (parent != nullptr && parent->is_string()) return parent->AsString();
  return std::string();
}

std::string GraphAdjBackend::DeriveClass(const std::string& encoded_doc) {
  JsonValue json = ToJsonSafe(encoded_doc);
  if (!json.is_object()) return std::string();
  for (const char* name : {"_class", "class", "type"}) {
    const JsonValue* v = json.Find(name);
    if (v != nullptr && v->is_string() && !v->AsString().empty()) return v->AsString();
  }
  return std::string();
}

std::vector<GraphEdge> GraphAdjBackend::DeriveOutEdges(const std::string& encoded_doc) {
  std::vector<GraphEdge> edges;
  JsonValue json = ToJsonSafe(encoded_doc);
  if (!json.is_object()) return edges;

  const JsonValue* children = json.Find("children");
  if (children != nullptr && children->is_array()) {
    for (const JsonValue& child : children->AsArray()) {
      if (child.is_string() && !child.AsString().empty()) {
        edges.push_back(GraphEdge{child.AsString(), "child", ""});
      }
    }
  }

  const JsonValue* general = json.Find("edges");
  if (general != nullptr && general->is_array()) {
    for (const JsonValue& e : general->AsArray()) {
      if (e.is_string() && !e.AsString().empty()) {
        edges.push_back(GraphEdge{e.AsString(), "", ""});
      } else if (e.is_object()) {
        const JsonValue* to = e.Find("to");
        if (to == nullptr || !to->is_string() || to->AsString().empty()) continue;
        const JsonValue* label = e.Find("label");
        edges.push_back(
            GraphEdge{to->AsString(), label && label->is_string() ? label->AsString() : "", ""});
      }
    }
  }
  return edges;
}

// ---------------------------------------------------------------------------
// Adjacency snapshot codec
// ---------------------------------------------------------------------------

std::string GraphAdjBackend::EncodeAdjacency(const CollectionState& state) {
  ByteWriter w;
  w.U32(kGraphAdjMagic);
  w.U32(static_cast<uint32_t>(state.out_edges.size()));
  for (const auto& [from, edges] : state.out_edges) {
    w.Bytes(from);
    w.U32(static_cast<uint32_t>(edges.size()));
    for (const GraphEdge& e : edges) {
      w.Bytes(e.to);
      w.Bytes(e.label);
      w.Bytes(e.owner);
    }
  }
  w.U32(static_cast<uint32_t>(state.key_class.size()));
  for (const auto& [key, cls] : state.key_class) {
    w.Bytes(key);
    w.Bytes(cls);
  }
  return w.TakeString();
}

Status GraphAdjBackend::DecodeAdjacency(const std::string& blob, CollectionState* out) {
  try {
    ByteReader r(blob);
    if (r.U32() != kGraphAdjMagic) return Status::Corruption("graph_adj: adjacency bad magic");
    out->out_edges.clear();
    out->in_edges.clear();
    out->class_members.clear();
    out->key_class.clear();
    out->declared.clear();

    uint32_t sources = r.U32();
    for (uint32_t i = 0; i < sources; ++i) {
      std::string from = r.Bytes();
      uint32_t count = r.U32();
      std::vector<GraphEdge>& edges = out->out_edges[from];
      edges.reserve(count);
      for (uint32_t j = 0; j < count; ++j) {
        GraphEdge e;
        e.to = r.Bytes();
        e.label = r.Bytes();
        e.owner = r.Bytes();
        // The reverse index and the ownership map are both derived from the
        // stored out-edges, never persisted separately: one authority for
        // the edge set means the three views cannot drift apart on disk.
        out->in_edges[e.to].push_back(GraphEdge{from, e.label, e.owner});
        out->declared[e.owner].emplace_back(from, e.to);
        edges.push_back(std::move(e));
      }
    }
    uint32_t classed = r.U32();
    for (uint32_t i = 0; i < classed; ++i) {
      std::string key = r.Bytes();
      std::string cls = r.Bytes();
      out->key_class[key] = cls;
      out->class_members[cls].insert(key);
    }
  } catch (const std::exception& e) {
    return Status::Corruption(std::string("graph_adj: malformed adjacency snapshot: ") + e.what());
  }
  return Status::OK();
}

// ---------------------------------------------------------------------------
// Backend
// ---------------------------------------------------------------------------

Status GraphAdjBackend::Open(const std::string& data_dir, uint64_t quota_mb) {
  dir_ = data_dir + "/graph_adj";
  if (!MakeDirs(dir_)) return Status::IOError("cannot create backend directory: " + dir_);
  manifest_path_ = dir_ + "/graph.json";

  docs_ = std::make_unique<KvBPlusBackend>();
  // The inner store is opened unlimited: this backend's own Charge() is the
  // single quota authority, so a document is never accepted here and then
  // rejected one layer down.
  Status st = docs_->Open(dir_, 0);
  if (!st.ok()) return st;

  auto store_or = SegmentStore::Open(dir_ + "/adjacency.dsf");
  if (!store_or.ok()) return store_or.status();
  store_ = std::move(store_or.value());

  SetQuotaBytes(quota_mb * 1024ull * 1024ull);
  SetBytesUsed(docs_->QuotaUse() + store_->BytesOnDisk());
  return LoadManifest();
}

Status GraphAdjBackend::LoadManifest() {
  std::ifstream f(manifest_path_);
  if (!f.is_open()) return Status::OK();
  std::ostringstream ss;
  ss << f.rdbuf();
  if (ss.str().empty()) return Status::OK();
  JsonValue root;
  try {
    root = JsonValue::Parse(ss.str());
  } catch (const std::exception& e) {
    return Status::Corruption(std::string("graph_adj manifest parse error: ") + e.what());
  }
  if (!root.is_array()) return Status::OK();

  std::lock_guard<std::mutex> lock(mu_);
  for (const JsonValue& entry : root.AsArray()) {
    const std::string name = entry.Get("collection").AsString();
    const page_id_t page = static_cast<page_id_t>(entry.Get("snapshot_page_id").AsInt());
    if (name.empty()) continue;
    CollectionState state;
    if (page != kInvalidPageId) {
      auto blob = store_->ReadBlob(page);
      if (!blob.ok()) return blob.status();
      Status st = DecodeAdjacency(blob.value(), &state);
      if (!st.ok()) return st;
      state.snapshot_page_id = page;
    }
    collections_[name] = std::move(state);
  }
  return Status::OK();
}

Status GraphAdjBackend::SaveManifest() {
  JsonValue::Array arr;
  for (const auto& [name, state] : collections_) {
    JsonValue::Object o;
    o.emplace_back("collection", JsonValue(name));
    o.emplace_back("snapshot_page_id", JsonValue(static_cast<int64_t>(state.snapshot_page_id)));
    o.emplace_back("nodes", JsonValue(static_cast<int64_t>(state.out_edges.size())));
    arr.emplace_back(std::move(o));
  }
  const std::string tmp = manifest_path_ + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc | std::ios::binary);
    if (!f.is_open()) return Status::IOError("graph_adj: cannot write " + tmp);
    f << JsonValue(std::move(arr)).Dump();
    f.flush();
    if (!f.good()) return Status::IOError("graph_adj: manifest write failed");
  }
  std::remove(manifest_path_.c_str());
  if (std::rename(tmp.c_str(), manifest_path_.c_str()) != 0) {
    return Status::IOError("graph_adj: cannot commit " + manifest_path_);
  }
  return Status::OK();
}

void GraphAdjBackend::UnindexDocumentLocked(CollectionState* state, const std::string& key) {
  // Remove only the edges this document declared. An edge into out_edges[key]
  // that some *other* document asserted (a child naming `parent: key`)
  // survives, which is the whole reason edges carry an owner.
  auto declared_it = state->declared.find(key);
  if (declared_it != state->declared.end()) {
    for (const auto& [from, to] : declared_it->second) {
      auto out_it = state->out_edges.find(from);
      if (out_it != state->out_edges.end()) {
        std::vector<GraphEdge>& list = out_it->second;
        list.erase(std::remove_if(list.begin(), list.end(),
                                   [&](const GraphEdge& e) { return e.owner == key && e.to == to; }),
                   list.end());
        // The node itself stays in out_edges (possibly with an empty list)
        // so Roots() and StatsFor() can still see it.
      }
      auto in_it = state->in_edges.find(to);
      if (in_it != state->in_edges.end()) {
        std::vector<GraphEdge>& list = in_it->second;
        list.erase(std::remove_if(list.begin(), list.end(),
                                   [&](const GraphEdge& e) { return e.owner == key && e.to == from; }),
                   list.end());
        if (list.empty()) state->in_edges.erase(in_it);
      }
    }
    state->declared.erase(declared_it);
  }
  auto cls_it = state->key_class.find(key);
  if (cls_it != state->key_class.end()) {
    auto members = state->class_members.find(cls_it->second);
    if (members != state->class_members.end()) {
      members->second.erase(key);
      if (members->second.empty()) state->class_members.erase(members);
    }
    state->key_class.erase(cls_it);
  }
}

void GraphAdjBackend::IndexDocumentLocked(CollectionState* state, const std::string& key,
                                           const std::string& encoded_doc) {
  UnindexDocumentLocked(state, key);

  std::vector<std::pair<std::string, std::string>>& declared = state->declared[key];
  auto add_edge = [&](const std::string& from, const std::string& to, const std::string& label) {
    state->out_edges[from].push_back(GraphEdge{to, label, key});
    state->in_edges[to].push_back(GraphEdge{from, label, key});
    declared.emplace_back(from, to);
    // Both endpoints must exist as nodes even with no outgoing edges of
    // their own, so Roots()/StatsFor() see the whole vertex set.
    state->out_edges.emplace(to, std::vector<GraphEdge>());
  };

  // A `parent` field is an in-edge on this node, which is the same fact as
  // an out-edge on the parent -- recorded that way so both directions stay
  // derivable from one map.
  const std::string parent = DeriveParent(encoded_doc);
  if (!parent.empty()) add_edge(parent, key, "child");
  for (const GraphEdge& e : DeriveOutEdges(encoded_doc)) add_edge(key, e.to, e.label);
  state->out_edges.emplace(key, std::vector<GraphEdge>());

  const std::string cls = DeriveClass(encoded_doc);
  if (!cls.empty()) {
    state->key_class[key] = cls;
    state->class_members[cls].insert(key);
  }
  state->dirty = true;
}

Status GraphAdjBackend::Put(const std::string& collection, const std::string& key,
                             const std::string& encoded_doc) {
  uint64_t old_cost = 0;
  {
    auto existing = docs_->Get(collection, key);
    if (existing.ok()) old_cost = RecordCost(key, existing.value());
  }
  Status charged = Charge(old_cost, RecordCost(key, encoded_doc));
  if (!charged.ok()) return charged;

  Status st = docs_->Put(collection, key, encoded_doc);
  if (!st.ok()) return st;

  std::lock_guard<std::mutex> lock(mu_);
  IndexDocumentLocked(&collections_[collection], key, encoded_doc);
  return Status::OK();
}

StatusOr<std::string> GraphAdjBackend::Get(const std::string& collection, const std::string& key) {
  return docs_->Get(collection, key);
}

std::vector<EngineRow> GraphAdjBackend::Scan(const std::string& collection,
                                               const std::string& start_key, size_t limit) {
  return docs_->Scan(collection, start_key, limit);
}

std::vector<GraphEdge> GraphAdjBackend::OutEdges(const std::string& collection,
                                                   const std::string& key) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto coll = collections_.find(collection);
  if (coll == collections_.end()) return {};
  auto it = coll->second.out_edges.find(key);
  return it == coll->second.out_edges.end() ? std::vector<GraphEdge>() : it->second;
}

std::vector<GraphEdge> GraphAdjBackend::InEdges(const std::string& collection,
                                                  const std::string& key) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto coll = collections_.find(collection);
  if (coll == collections_.end()) return {};
  auto it = coll->second.in_edges.find(key);
  return it == coll->second.in_edges.end() ? std::vector<GraphEdge>() : it->second;
}

std::vector<std::string> GraphAdjBackend::Descendants(const std::string& collection,
                                                        const std::string& root,
                                                        size_t max_depth) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto coll = collections_.find(collection);
  if (coll == collections_.end()) return {};
  const CollectionState& state = coll->second;

  const size_t depth_limit = max_depth == 0 ? kGraphMaxWalkDepth : std::min(max_depth, kGraphMaxWalkDepth);
  std::vector<std::string> out;
  std::unordered_set<std::string> seen{root};
  std::deque<std::pair<std::string, size_t>> queue{{root, 0}};
  while (!queue.empty()) {
    auto [key, depth] = queue.front();
    queue.pop_front();
    if (depth >= depth_limit) continue;
    auto it = state.out_edges.find(key);
    if (it == state.out_edges.end()) continue;
    for (const GraphEdge& e : it->second) {
      // `seen` is what makes a cyclic graph terminate rather than hang --
      // this is a general graph store, not a tree store, so cycles are legal
      // input, not corruption.
      if (!seen.insert(e.to).second) continue;
      out.push_back(e.to);
      queue.emplace_back(e.to, depth + 1);
    }
  }
  return out;
}

std::vector<std::string> GraphAdjBackend::Ancestors(const std::string& collection,
                                                      const std::string& node) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto coll = collections_.find(collection);
  if (coll == collections_.end()) return {};
  const CollectionState& state = coll->second;

  std::vector<std::string> out;
  std::unordered_set<std::string> seen{node};
  std::deque<std::string> queue{node};
  size_t depth = 0;
  while (!queue.empty() && depth++ < kGraphMaxWalkDepth) {
    const size_t level_size = queue.size();
    for (size_t i = 0; i < level_size; ++i) {
      const std::string key = queue.front();
      queue.pop_front();
      auto it = state.in_edges.find(key);
      if (it == state.in_edges.end()) continue;
      for (const GraphEdge& e : it->second) {
        if (!seen.insert(e.to).second) continue;
        out.push_back(e.to);
        queue.push_back(e.to);
      }
    }
  }
  return out;
}

std::vector<std::string> GraphAdjBackend::Roots(const std::string& collection) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto coll = collections_.find(collection);
  if (coll == collections_.end()) return {};
  const CollectionState& state = coll->second;
  std::vector<std::string> roots;
  for (const auto& [key, edges] : state.out_edges) {
    (void)edges;
    if (state.in_edges.find(key) == state.in_edges.end()) roots.push_back(key);
  }
  std::sort(roots.begin(), roots.end());
  return roots;
}

std::vector<std::string> GraphAdjBackend::Classes(const std::string& collection) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto coll = collections_.find(collection);
  if (coll == collections_.end()) return {};
  std::vector<std::string> names;
  names.reserve(coll->second.class_members.size());
  for (const auto& [cls, members] : coll->second.class_members) {
    (void)members;
    names.push_back(cls);
  }
  return names;
}

StatusOr<ObjectTable> GraphAdjBackend::ObjectView(const std::string& collection,
                                                    const std::string& class_name) {
  std::vector<std::string> members;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto coll = collections_.find(collection);
    if (coll == collections_.end()) return Status::NotFound("no such collection: " + collection);
    auto it = coll->second.class_members.find(class_name);
    if (it == coll->second.class_members.end()) {
      return Status::NotFound("no such class in " + collection + ": " + class_name);
    }
    members.assign(it->second.begin(), it->second.end());
  }

  // Column discovery: the union of every member's scalar fields, in sorted
  // order so the table is stable across calls (a UI binding column indexes
  // to a header row cannot tolerate a set-iteration-order table).
  std::set<std::string> columns;
  std::vector<std::pair<std::string, JsonValue>> docs;
  docs.reserve(members.size());
  for (const std::string& key : members) {
    auto raw = docs_->Get(collection, key);
    if (!raw.ok()) continue;
    JsonValue json = ToJsonSafe(raw.value());
    if (!json.is_object()) continue;
    for (const auto& [field, value] : json.AsObject()) {
      if (value.is_array() || value.is_object()) continue;  // relations, not columns
      columns.insert(field);
    }
    docs.emplace_back(key, std::move(json));
  }

  ObjectTable table;
  table.class_name = class_name;
  table.columns.assign(columns.begin(), columns.end());
  // Two synthetic relational columns make the object graph queryable the way
  // a foreign key is: who owns this row, and how many rows it owns.
  table.columns.push_back("_parent");
  table.columns.push_back("_child_count");

  std::sort(docs.begin(), docs.end(),
            [](const std::pair<std::string, JsonValue>& a, const std::pair<std::string, JsonValue>& b) {
              return a.first < b.first;
            });

  for (const auto& [key, json] : docs) {
    std::vector<std::string> row;
    row.reserve(table.columns.size());
    for (const std::string& column : columns) {
      const JsonValue* v = json.Find(column);
      row.push_back(v == nullptr ? std::string("null") : v->Dump());
    }
    std::vector<GraphEdge> in = InEdges(collection, key);
    row.push_back(in.empty() ? std::string("null") : JsonValue(in.front().to).Dump());
    row.push_back(std::to_string(OutEdges(collection, key).size()));
    table.keys.push_back(key);
    table.rows.push_back(std::move(row));
  }
  return table;
}

Status GraphAdjBackend::Rebuild(const std::string& collection) {
  std::vector<EngineRow> rows = docs_->Scan(collection, "", 0);
  std::lock_guard<std::mutex> lock(mu_);
  CollectionState& state = collections_[collection];
  state.out_edges.clear();
  state.in_edges.clear();
  state.class_members.clear();
  state.key_class.clear();
  state.declared.clear();
  for (const auto& [key, doc] : rows) IndexDocumentLocked(&state, key, doc);
  state.dirty = true;
  return Status::OK();
}

GraphAdjBackend::Stats GraphAdjBackend::StatsFor(const std::string& collection) const {
  Stats stats;
  std::lock_guard<std::mutex> lock(mu_);
  auto coll = collections_.find(collection);
  if (coll == collections_.end()) return stats;
  const CollectionState& state = coll->second;
  stats.nodes = state.out_edges.size();
  for (const auto& [key, edges] : state.out_edges) {
    (void)key;
    stats.edges += edges.size();
  }
  for (const auto& [key, edges] : state.out_edges) {
    (void)edges;
    if (state.in_edges.find(key) == state.in_edges.end()) ++stats.roots;
  }
  stats.classes = state.class_members.size();
  return stats;
}

Status GraphAdjBackend::SnapshotLocked(CollectionState* state) {
  if (!state->dirty) return Status::OK();
  std::string blob = EncodeAdjacency(*state);
  auto page_or = store_->AppendBlob(blob);
  if (!page_or.ok()) return page_or.status();
  state->snapshot_page_id = page_or.value();
  state->dirty = false;
  return Status::OK();
}

Status GraphAdjBackend::Verify() {
  Status st = docs_->Verify();
  if (!st.ok()) return st;

  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& [name, state] : collections_) {
    // The two adjacency directions must agree: every out-edge must have a
    // matching in-edge. This is the check that catches an
    // IndexDocumentLocked/UnindexDocumentLocked mismatch, which is the only
    // way the two maps can diverge.
    for (const auto& [from, edges] : state.out_edges) {
      for (const GraphEdge& e : edges) {
        auto in_it = state.in_edges.find(e.to);
        bool found = false;
        if (in_it != state.in_edges.end()) {
          for (const GraphEdge& r : in_it->second) {
            if (r.to == from && r.owner == e.owner) {
              found = true;
              break;
            }
          }
        }
        if (!found) {
          return Status::Corruption("graph_adj: edge " + from + " -> " + e.to + " in " + name +
                                     " has no matching reverse edge");
        }
      }
    }
    if (state.snapshot_page_id != kInvalidPageId) {
      Status blob_st = store_->VerifyBlob(state.snapshot_page_id);
      if (!blob_st.ok()) {
        return Status::Corruption("graph_adj: adjacency snapshot of " + name + ": " + blob_st.message());
      }
    }
  }
  return Status::OK();
}

Status GraphAdjBackend::Flush() {
  Status st = docs_->Flush();
  if (!st.ok()) return st;
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& [name, state] : collections_) {
      (void)name;
      Status snap = SnapshotLocked(&state);
      if (!snap.ok()) return snap;
    }
    Status saved = SaveManifest();
    if (!saved.ok()) return saved;
  }
  SetBytesUsed(docs_->QuotaUse() + store_->BytesOnDisk());
  return store_->Flush();
}

std::vector<std::string> GraphAdjBackend::ListCollections() const {
  return docs_->ListCollections();
}

}  // namespace desentry
