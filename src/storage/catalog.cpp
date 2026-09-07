#include "desentry/storage/catalog.h"

#include <chrono>
#include <fstream>
#include <sstream>

#include "desentry/common/logger.h"

namespace desentry {

namespace {

uint64_t NowMsUtc() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count());
}

JsonValue EncodeAcl(const CollectionAcl& acl) {
  JsonValue::Object o;
  o.emplace_back("owner_node", JsonValue(acl.owner_node));
  o.emplace_back("private", JsonValue(acl.is_private));
  JsonValue::Array readers;
  for (const std::string& r : acl.readers) readers.emplace_back(JsonValue(r));
  o.emplace_back("readers", JsonValue(std::move(readers)));
  o.emplace_back("parent", JsonValue(acl.parent));
  return JsonValue(std::move(o));
}

CollectionAcl DecodeAcl(const JsonValue& v) {
  CollectionAcl acl;
  if (!v.is_object()) return acl;
  const JsonValue* owner = v.Find("owner_node");
  if (owner && owner->is_string()) acl.owner_node = owner->AsString();
  const JsonValue* priv = v.Find("private");
  if (priv && priv->is_bool()) acl.is_private = priv->AsBool();
  const JsonValue* readers = v.Find("readers");
  if (readers && readers->is_array()) {
    for (const JsonValue& r : readers->AsArray()) {
      if (r.is_string()) acl.readers.push_back(r.AsString());
    }
  }
  const JsonValue* parent = v.Find("parent");
  if (parent && parent->is_string()) acl.parent = parent->AsString();
  return acl;
}

}  // namespace

StatusOr<std::unique_ptr<Catalog>> Catalog::Open(const std::string& catalog_file) {
  std::unique_ptr<Catalog> cat(new Catalog(catalog_file));
  Status st = cat->LoadFromDisk();
  if (!st.ok()) return st;
  return cat;
}

Status Catalog::LoadFromDisk() {
  std::ifstream f(path_);
  if (!f.is_open()) {
    DSN_LOG_INFO("catalog", "no existing catalog at " << path_ << ", starting empty");
    return Status::OK();
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  std::string text = ss.str();
  if (text.empty()) return Status::OK();

  JsonValue root;
  try {
    root = JsonValue::Parse(text);
  } catch (const std::exception& e) {
    return Status::Corruption(std::string("catalog parse error: ") + e.what());
  }
  if (!root.is_array()) return Status::OK();

  std::lock_guard<std::mutex> lock(mu_);
  for (auto& entry : root.AsArray()) {
    CollectionMeta meta;
    meta.name = entry.Get("name").AsString();
    meta.root_page_id = static_cast<page_id_t>(entry.Get("root_page_id").AsInt());
    meta.created_at_ms = static_cast<uint64_t>(entry.Get("created_at_ms").AsInt());
    const JsonValue* schema = entry.Find("schema");
    if (schema && !schema->is_null()) {
      meta.has_schema = true;
      meta.schema = *schema;
    }
    // v2 fields; absent in a v1 catalog, which is exactly why every one of
    // them has a meaningful default (a v1 data directory opens unchanged).
    const JsonValue* engine = entry.Find("engine");
    if (engine && engine->is_string()) meta.engine = engine->AsString();
    const JsonValue* acl = entry.Find("acl");
    if (acl != nullptr) meta.acl = DecodeAcl(*acl);
    const JsonValue* retention = entry.Find("retention_days");
    if (retention && retention->is_number()) meta.retention_days = static_cast<uint32_t>(retention->AsInt());
    const JsonValue* shard_key = entry.Find("shard_key");
    if (shard_key && shard_key->is_string()) meta.shard_key = shard_key->AsString();
    const JsonValue* rf = entry.Find("replication_factor");
    if (rf && rf->is_number()) meta.replication_factor = static_cast<uint32_t>(rf->AsInt());
    const JsonValue* idx = entry.Find("secondary_indexes");
    if (idx && idx->is_array()) {
      for (const JsonValue& i : idx->AsArray()) {
        if (i.is_string()) meta.secondary_indexes.push_back(i.AsString());
      }
    }
    collections_[meta.name] = std::move(meta);
  }
  DSN_LOG_INFO("catalog", "loaded " << collections_.size() << " collection(s) from " << path_);
  return Status::OK();
}

Status Catalog::SaveLocked() {
  JsonValue::Array arr;
  for (auto& [name, meta] : collections_) {
    JsonValue::Object obj;
    obj.emplace_back("name", JsonValue(meta.name));
    obj.emplace_back("root_page_id", JsonValue(static_cast<int64_t>(meta.root_page_id)));
    obj.emplace_back("created_at_ms", JsonValue(static_cast<int64_t>(meta.created_at_ms)));
    obj.emplace_back("schema", meta.has_schema ? meta.schema : JsonValue(nullptr));
    obj.emplace_back("engine", JsonValue(meta.engine));
    obj.emplace_back("acl", EncodeAcl(meta.acl));
    obj.emplace_back("retention_days", JsonValue(static_cast<int64_t>(meta.retention_days)));
    obj.emplace_back("shard_key", JsonValue(meta.shard_key));
    obj.emplace_back("replication_factor", JsonValue(static_cast<int64_t>(meta.replication_factor)));
    JsonValue::Array idx;
    for (const std::string& i : meta.secondary_indexes) idx.emplace_back(JsonValue(i));
    obj.emplace_back("secondary_indexes", JsonValue(std::move(idx)));
    arr.emplace_back(std::move(obj));
  }
  // Write-then-rename so a crash mid-save can never leave a truncated
  // catalog on disk: the old file stays intact until the new one is
  // complete. (v1 wrote in place with std::ios::trunc, which had a real
  // window where the catalog was zero bytes.)
  const std::string tmp_path = path_ + ".tmp";
  {
    std::ofstream f(tmp_path, std::ios::trunc | std::ios::binary);
    if (!f.is_open()) return Status::IOError("cannot write catalog: " + tmp_path);
    f << JsonValue(std::move(arr)).Dump();
    f.flush();
    if (!f.good()) return Status::IOError("catalog write failed: " + tmp_path);
  }
  std::remove(path_.c_str());  // Windows rename() refuses to clobber
  if (std::rename(tmp_path.c_str(), path_.c_str()) != 0) {
    return Status::IOError("cannot commit catalog: " + path_);
  }
  return Status::OK();
}

Status Catalog::Save() {
  std::lock_guard<std::mutex> lock(mu_);
  return SaveLocked();
}

bool Catalog::HasCollection(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mu_);
  return collections_.count(name) != 0;
}

const CollectionMeta* Catalog::Get(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = collections_.find(name);
  return it == collections_.end() ? nullptr : &it->second;
}

bool Catalog::GetCopy(const std::string& name, CollectionMeta* out) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = collections_.find(name);
  if (it == collections_.end()) return false;
  *out = it->second;
  return true;
}

std::vector<std::string> Catalog::ListCollections() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::string> names;
  names.reserve(collections_.size());
  for (auto& [name, meta] : collections_) names.push_back(name);
  return names;
}

Status Catalog::CreateCollection(const std::string& name, page_id_t root_page_id) {
  std::lock_guard<std::mutex> lock(mu_);
  if (collections_.count(name)) return Status::AlreadyExists("collection exists: " + name);
  CollectionMeta meta;
  meta.name = name;
  meta.root_page_id = root_page_id;
  meta.created_at_ms = NowMsUtc();
  collections_[name] = std::move(meta);
  return SaveLocked();
}

Status Catalog::UpsertRootPageId(const std::string& name, page_id_t root_page_id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = collections_.find(name);
  if (it != collections_.end()) {
    it->second.root_page_id = root_page_id;
  } else {
    CollectionMeta meta;
    meta.name = name;
    meta.root_page_id = root_page_id;
    meta.created_at_ms = NowMsUtc();
    collections_[name] = std::move(meta);
  }
  return SaveLocked();
}

Status Catalog::SetSchema(const std::string& name, const JsonValue& schema) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = collections_.find(name);
  if (it == collections_.end()) return Status::NotFound("no such collection: " + name);
  it->second.has_schema = true;
  it->second.schema = schema;
  return SaveLocked();
}

Status Catalog::DropSchema(const std::string& name) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = collections_.find(name);
  if (it == collections_.end()) return Status::NotFound("no such collection: " + name);
  it->second.has_schema = false;
  return SaveLocked();
}

Status Catalog::SetEngine(const std::string& name, const std::string& engine) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = collections_.find(name);
  if (it == collections_.end()) return Status::NotFound("no such collection: " + name);
  if (!it->second.engine.empty() && it->second.engine != engine) {
    // Rebinding a populated collection would strand its existing rows in
    // the old backend. The app's flow for this is "create a new collection
    // on the new engine and copy", which is explicit about the cost.
    return Status::InvalidArgument("collection '" + name + "' is already bound to engine '" +
                                    it->second.engine + "'; create a new collection to change engines");
  }
  it->second.engine = engine;
  return SaveLocked();
}

Status Catalog::SetAcl(const std::string& name, const CollectionAcl& acl) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = collections_.find(name);
  if (it == collections_.end()) return Status::NotFound("no such collection: " + name);
  if (acl.parent == name) return Status::InvalidArgument("acl.parent cannot be the collection itself");
  it->second.acl = acl;
  return SaveLocked();
}

Status Catalog::SetRetentionDays(const std::string& name, uint32_t days) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = collections_.find(name);
  if (it == collections_.end()) return Status::NotFound("no such collection: " + name);
  it->second.retention_days = days;
  return SaveLocked();
}

Status Catalog::SetPlacement(const std::string& name, const std::string& shard_key,
                              uint32_t replication_factor) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = collections_.find(name);
  if (it == collections_.end()) return Status::NotFound("no such collection: " + name);
  it->second.shard_key = shard_key;
  it->second.replication_factor = replication_factor;
  return SaveLocked();
}

CollectionAcl Catalog::EffectiveAcl(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::string cursor = name;
  for (int depth = 0; depth < kMaxAclParentDepth; ++depth) {
    auto it = collections_.find(cursor);
    if (it == collections_.end()) return CollectionAcl();  // unknown collection: public default
    const CollectionAcl& acl = it->second.acl;
    // A collection that names a parent and asserts nothing itself inherits;
    // one that sets its own owner/private/readers is authoritative.
    bool asserts_own = acl.is_private || !acl.owner_node.empty() || !acl.readers.empty();
    if (acl.parent.empty() || asserts_own) return acl;
    cursor = acl.parent;
  }
  // A cycle (only reachable via a hand-edited catalog.json) must not hang
  // or silently open access: fail closed.
  DSN_LOG_WARN("catalog", "ACL parent chain for '" << name << "' exceeded depth limit; denying");
  CollectionAcl denied;
  denied.is_private = true;
  denied.owner_node = "\x01invalid";  // matches no real node_id (hex-only)
  return denied;
}

bool Catalog::CanRead(const std::string& collection, const std::string& node_id) const {
  return EffectiveAcl(collection).AllowsReader(node_id);
}

bool Catalog::CanWrite(const std::string& collection, const std::string& node_id) const {
  CollectionAcl acl = EffectiveAcl(collection);
  // Non-private collections stay multi-writer: that is the whole point of
  // the CRDT model, and restricting writes by default would break it. A
  // private collection accepts writes only from its owner; `readers` is
  // read access, as the field name says.
  if (!acl.is_private) return true;
  return !acl.owner_node.empty() && node_id == acl.owner_node;
}

}  // namespace desentry
