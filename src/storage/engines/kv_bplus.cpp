#include "desentry/storage/engines/kv_bplus.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "desentry/common/json.h"
#include "desentry/common/logger.h"
#include "desentry/common/platform.h"
#include "desentry/storage/slotted_page.h"

namespace desentry {

Status KvBPlusBackend::Open(const std::string& data_dir, uint64_t quota_mb) {
  dir_ = data_dir + "/kv";
  if (!MakeDirs(dir_)) return Status::IOError("cannot create backend directory: " + dir_);
  roots_path_ = dir_ + "/roots.json";

  auto disk_or = DiskManager::Open(dir_ + "/kv.dsf");
  if (!disk_or.ok()) return disk_or.status();
  disk_ = std::move(disk_or.value());
  pool_ = std::make_unique<BufferPoolManager>(1024, disk_.get());

  SetQuotaBytes(quota_mb * 1024ull * 1024ull);
  SetBytesUsed(static_cast<uint64_t>(std::max<int64_t>(0, disk_->NumAllocatedPages())) * kPageSize);
  return LoadRoots();
}

Status KvBPlusBackend::LoadRoots() {
  std::ifstream f(roots_path_);
  if (!f.is_open()) return Status::OK();
  std::ostringstream ss;
  ss << f.rdbuf();
  if (ss.str().empty()) return Status::OK();
  JsonValue root;
  try {
    root = JsonValue::Parse(ss.str());
  } catch (const std::exception& e) {
    return Status::Corruption(std::string("kv backend: roots.json parse error: ") + e.what());
  }
  if (!root.is_object()) return Status::OK();
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& [name, value] : root.AsObject()) {
    if (value.is_number()) roots_[name] = static_cast<page_id_t>(value.AsInt());
  }
  return Status::OK();
}

Status KvBPlusBackend::SaveRoots() {
  JsonValue::Object obj;
  for (const auto& [name, root] : roots_) {
    obj.emplace_back(name, JsonValue(static_cast<int64_t>(root)));
  }
  const std::string tmp = roots_path_ + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc | std::ios::binary);
    if (!f.is_open()) return Status::IOError("kv backend: cannot write " + tmp);
    f << JsonValue(std::move(obj)).Dump();
    f.flush();
    if (!f.good()) return Status::IOError("kv backend: roots write failed");
  }
  std::remove(roots_path_.c_str());
  if (std::rename(tmp.c_str(), roots_path_.c_str()) != 0) {
    return Status::IOError("kv backend: cannot commit " + roots_path_);
  }
  return Status::OK();
}

// Caller must hold mu_.
BPlusTree* KvBPlusBackend::IndexFor(const std::string& collection) {
  auto it = indexes_.find(collection);
  if (it != indexes_.end()) return it->second.get();

  page_id_t root = kInvalidPageId;
  auto root_it = roots_.find(collection);
  // roots.json is written separately from the pages it names, so a root id
  // can survive a crash that its page did not: the id is in the file, the
  // page was never flushed, and the read comes back zero-filled. Descending
  // into that is how the same class of bug bit the catalog in v1. The id is
  // therefore checked, not trusted -- and a root that is gone means an empty
  // tree, which WAL replay then refills.
  if (root_it != roots_.end() && BPlusTree::RootLooksValid(pool_.get(), root_it->second)) {
    root = root_it->second;
  } else {
    if (root_it != roots_.end() && root_it->second != kInvalidPageId) {
      DSN_LOG_WARN("kv", "root page " << root_it->second << " for collection " << collection
                                       << " is not a valid node; rebuilding the index from the ledger");
    }
    auto root_or = BPlusTree::CreateNew(pool_.get());
    if (!root_or.ok()) return nullptr;
    root = root_or.value();
    roots_[collection] = root;
    SaveRoots();
  }
  auto tree = std::make_unique<BPlusTree>(pool_.get(), root);
  BPlusTree* raw = tree.get();
  indexes_[collection] = std::move(tree);
  return raw;
}

Status KvBPlusBackend::Put(const std::string& collection, const std::string& key,
                            const std::string& encoded_doc) {
  if (key.size() > kMaxKeyBytes) {
    return Status::InvalidArgument("key exceeds " + std::to_string(kMaxKeyBytes) + " bytes: " + key);
  }

  // Quota is charged before any page is touched, so a rejected write leaves
  // no partial state (see BaseBackend::Charge).
  uint64_t old_cost = 0;
  {
    auto existing = Get(collection, key);
    if (existing.ok()) old_cost = RecordCost(key, existing.value());
  }
  Status charged = Charge(old_cost, RecordCost(key, encoded_doc));
  if (!charged.ok()) return charged;

  std::lock_guard<std::mutex> lock(mu_);
  BPlusTree* index = IndexFor(collection);
  if (index == nullptr) return Status::Internal("kv backend: no index for " + collection);

  page_id_t write_page_id;
  auto wp = write_pages_.find(collection);
  if (wp == write_pages_.end()) {
    page_id_t new_id = kInvalidPageId;
    Page* p = pool_->NewPage(&new_id);
    if (p == nullptr) return Status::OutOfSpace("kv backend: buffer pool exhausted");
    SlottedPage::Init(p);
    pool_->UnpinPage(new_id, true);
    write_pages_[collection] = new_id;
    write_page_id = new_id;
  } else {
    write_page_id = wp->second;
  }

  Page* page = pool_->FetchPage(write_page_id);
  if (page == nullptr) return Status::OutOfSpace("kv backend: buffer pool exhausted");
  slot_id_t slot = SlottedPage::InsertRecord(page, encoded_doc);
  if (slot < 0) {
    pool_->UnpinPage(write_page_id, false);
    page_id_t new_id = kInvalidPageId;
    Page* np = pool_->NewPage(&new_id);
    if (np == nullptr) return Status::OutOfSpace("kv backend: buffer pool exhausted");
    SlottedPage::Init(np);
    slot = SlottedPage::InsertRecord(np, encoded_doc);
    if (slot < 0) {
      pool_->UnpinPage(new_id, true);
      return Status::InvalidArgument("document too large for a page (" +
                                      std::to_string(encoded_doc.size()) + " bytes)");
    }
    pool_->UnpinPage(new_id, true);
    write_pages_[collection] = new_id;
    write_page_id = new_id;
  } else {
    pool_->UnpinPage(write_page_id, true);
  }

  SetBytesUsed(static_cast<uint64_t>(std::max<int64_t>(0, disk_->NumAllocatedPages())) * kPageSize);
  return index->Insert(key, RID{write_page_id, slot});
}

StatusOr<std::string> KvBPlusBackend::Get(const std::string& collection, const std::string& key) {
  std::lock_guard<std::mutex> lock(mu_);
  BPlusTree* index = IndexFor(collection);
  if (index == nullptr) return Status::NotFound("no such collection: " + collection);
  RID rid;
  if (!index->Search(key, &rid)) return Status::NotFound("no such key: " + key);
  Page* page = pool_->FetchPage(rid.page_id);
  if (page == nullptr) return Status::Internal("kv backend: cannot fetch data page");
  std::string bytes;
  bool found = SlottedPage::GetRecord(page, rid.slot_id, &bytes);
  pool_->UnpinPage(rid.page_id, false);
  if (!found) return Status::NotFound("dangling index entry for key: " + key);
  return bytes;
}

std::vector<EngineRow> KvBPlusBackend::Scan(const std::string& collection,
                                              const std::string& start_key, size_t limit) {
  std::vector<EngineRow> out;
  std::lock_guard<std::mutex> lock(mu_);
  BPlusTree* index = IndexFor(collection);
  if (index == nullptr) return out;
  for (auto& [key, rid] : index->Scan(start_key, limit)) {
    Page* page = pool_->FetchPage(rid.page_id);
    if (page == nullptr) continue;
    std::string bytes;
    if (SlottedPage::GetRecord(page, rid.slot_id, &bytes)) out.emplace_back(key, std::move(bytes));
    pool_->UnpinPage(rid.page_id, false);
  }
  return out;
}

Status KvBPlusBackend::Verify() {
  // Every key the index reports must resolve to a readable record. This
  // catches exactly the failure the v1 crash-recovery bug produced (an
  // index entry pointing at a page that was never flushed) rather than
  // waiting for a reader to hit it.
  for (const std::string& collection : ListCollections()) {
    std::lock_guard<std::mutex> lock(mu_);
    BPlusTree* index = IndexFor(collection);
    if (index == nullptr) return Status::Corruption("kv backend: missing index for " + collection);
    for (auto& [key, rid] : index->Scan("", 0)) {
      Page* page = pool_->FetchPage(rid.page_id);
      if (page == nullptr) {
        return Status::Corruption("kv backend: unreadable page " + std::to_string(rid.page_id) +
                                   " for key " + key);
      }
      std::string bytes;
      bool ok = SlottedPage::GetRecord(page, rid.slot_id, &bytes);
      pool_->UnpinPage(rid.page_id, false);
      if (!ok) {
        return Status::Corruption("kv backend: dangling index entry for key " + key + " in " + collection);
      }
    }
  }
  return Status::OK();
}

Status KvBPlusBackend::Flush() {
  std::lock_guard<std::mutex> lock(mu_);
  pool_->FlushAllPages();
  Status st = SaveRoots();
  if (!st.ok()) return st;
  return disk_->Sync();
}

std::vector<std::string> KvBPlusBackend::ListCollections() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::string> names;
  names.reserve(roots_.size());
  for (const auto& [name, root] : roots_) names.push_back(name);
  std::sort(names.begin(), names.end());
  return names;
}

}  // namespace desentry
