#include "desentry/storage/catalog.h"

#include <chrono>
#include <fstream>
#include <sstream>

#include "desentry/common/logger.h"

namespace desentry {

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
    collections_[meta.name] = std::move(meta);
  }
  DSN_LOG_INFO("catalog", "loaded " << collections_.size() << " collection(s) from " << path_);
  return Status::OK();
}

Status Catalog::Save() {
  std::lock_guard<std::mutex> lock(mu_);
  JsonValue::Array arr;
  for (auto& [name, meta] : collections_) {
    JsonValue::Object obj;
    obj.emplace_back("name", JsonValue(meta.name));
    obj.emplace_back("root_page_id", JsonValue(static_cast<int64_t>(meta.root_page_id)));
    obj.emplace_back("created_at_ms", JsonValue(static_cast<int64_t>(meta.created_at_ms)));
    obj.emplace_back("schema", meta.has_schema ? meta.schema : JsonValue(nullptr));
    arr.emplace_back(std::move(obj));
  }
  std::ofstream f(path_, std::ios::trunc);
  if (!f.is_open()) return Status::IOError("cannot write catalog: " + path_);
  f << JsonValue(std::move(arr)).Dump();
  return Status::OK();
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

std::vector<std::string> Catalog::ListCollections() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::string> names;
  names.reserve(collections_.size());
  for (auto& [name, meta] : collections_) names.push_back(name);
  return names;
}

Status Catalog::CreateCollection(const std::string& name, page_id_t root_page_id) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (collections_.count(name)) return Status::AlreadyExists("collection exists: " + name);
    CollectionMeta meta;
    meta.name = name;
    meta.root_page_id = root_page_id;
    meta.created_at_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    collections_[name] = std::move(meta);
  }
  return Save();
}

Status Catalog::UpsertRootPageId(const std::string& name, page_id_t root_page_id) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = collections_.find(name);
    if (it != collections_.end()) {
      it->second.root_page_id = root_page_id;
    } else {
      CollectionMeta meta;
      meta.name = name;
      meta.root_page_id = root_page_id;
      meta.created_at_ms = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count());
      collections_[name] = std::move(meta);
    }
  }
  return Save();
}

Status Catalog::SetSchema(const std::string& name, const JsonValue& schema) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = collections_.find(name);
    if (it == collections_.end()) return Status::NotFound("no such collection: " + name);
    it->second.has_schema = true;
    it->second.schema = schema;
  }
  return Save();
}

Status Catalog::DropSchema(const std::string& name) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = collections_.find(name);
    if (it == collections_.end()) return Status::NotFound("no such collection: " + name);
    it->second.has_schema = false;
  }
  return Save();
}

}  // namespace desentry
