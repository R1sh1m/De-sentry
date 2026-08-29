#include "desentry/storage/storage_engine.h"

#include <sys/stat.h>

#include "desentry/common/logger.h"
#include "desentry/storage/slotted_page.h"

namespace desentry {

namespace {
Status EnsureDir(const std::string& path) {
  if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
    return Status::IOError("cannot create directory: " + path);
  }
  return Status::OK();
}
}  // namespace

StatusOr<std::unique_ptr<StorageEngine>> StorageEngine::Open(const Options& options) {
  Status dir_st = EnsureDir(options.data_dir);
  if (!dir_st.ok()) return dir_st;

  std::unique_ptr<StorageEngine> engine(new StorageEngine());

  auto dm_or = DiskManager::Open(options.data_dir + "/desentry.dsf");
  if (!dm_or.ok()) return dm_or.status();
  engine->disk_manager_ = std::move(dm_or.value());

  engine->buffer_pool_ = std::make_unique<BufferPoolManager>(options.buffer_pool_pages, engine->disk_manager_.get());

  auto wal_or = WriteAheadLog::Open(options.data_dir + "/desentry.wal");
  if (!wal_or.ok()) return wal_or.status();
  engine->wal_ = std::move(wal_or.value());

  auto cat_or = Catalog::Open(options.data_dir + "/catalog.json");
  if (!cat_or.ok()) return cat_or.status();
  engine->catalog_ = std::move(cat_or.value());

  Status replay_st = engine->ReplayWal();
  if (!replay_st.ok()) return replay_st;

  return engine;
}

Status StorageEngine::ReplayWal() {
  auto records_or = wal_->ReadAll();
  if (!records_or.ok()) return records_or.status();
  auto& records = records_or.value();
  if (records.empty()) return Status::OK();

  DSN_LOG_INFO("storage", "replaying " << records.size() << " WAL record(s)...");

  // Every collection touched by the WAL gets a *freshly allocated* B+Tree
  // rather than reusing whatever root_page_id the catalog last recorded:
  // the catalog is saved eagerly (outside the buffer pool/WAL durability
  // path), so after an unclean shutdown its root_page_id may point at a
  // page that was never actually flushed to the data file. Rebuilding into
  // brand-new pages during replay -- and only then repointing the catalog
  // at them -- sidesteps ever trusting a page whose durability we can't
  // verify. The old pages simply become unreachable garbage (documented
  // "vacuum later" trade-off, same as elsewhere in this engine).
  std::unordered_map<std::string, bool> rebuilt;

  for (auto& rec : records) {
    if (!rebuilt[rec.collection]) {
      auto root_or = BPlusTree::CreateNew(buffer_pool_.get());
      if (!root_or.ok()) return root_or.status();
      {
        std::lock_guard<std::mutex> lock(indexes_mu_);
        indexes_[rec.collection] = std::make_unique<BPlusTree>(buffer_pool_.get(), root_or.value());
      }
      catalog_->UpsertRootPageId(rec.collection, root_or.value());
      rebuilt[rec.collection] = true;
    }
    BPlusTree* index = GetOrCreateIndex(rec.collection);
    if (index == nullptr) continue;

    if (rec.type == WalRecordType::kPut) {
      // Same physical write path as PutRaw(), minus the WAL append (we are
      // replaying the WAL itself -- appending again would duplicate it).
      page_id_t write_page_id;
      {
        std::lock_guard<std::mutex> lock(write_pages_mu_);
        auto it = write_pages_.find(rec.collection);
        if (it == write_pages_.end()) {
          page_id_t new_id;
          Page* p = buffer_pool_->NewPage(&new_id);
          if (p) { SlottedPage::Init(p); buffer_pool_->UnpinPage(new_id, true); }
          write_pages_[rec.collection] = new_id;
          write_page_id = new_id;
        } else {
          write_page_id = it->second;
        }
      }
      Page* page = buffer_pool_->FetchPage(write_page_id);
      slot_id_t slot = SlottedPage::InsertRecord(page, rec.document_bytes);
      if (slot < 0) {
        buffer_pool_->UnpinPage(write_page_id, false);
        page_id_t new_id;
        Page* np = buffer_pool_->NewPage(&new_id);
        SlottedPage::Init(np);
        slot = SlottedPage::InsertRecord(np, rec.document_bytes);
        buffer_pool_->UnpinPage(new_id, true);
        std::lock_guard<std::mutex> lock(write_pages_mu_);
        write_pages_[rec.collection] = new_id;
        write_page_id = new_id;
      } else {
        buffer_pool_->UnpinPage(write_page_id, true);
      }
      index->Insert(rec.key, RID{write_page_id, slot});
    }
    // kDelete records don't appear in this engine's WAL stream today
    // (deletes are modeled as CRDT-tombstoning PUTs -- see the class
    // comment) but the type is handled for forward-compatibility.
  }
  buffer_pool_->FlushAllPages();
  DSN_LOG_INFO("storage", "WAL replay complete");
  return Status::OK();
}

BPlusTree* StorageEngine::GetOrCreateIndex(const std::string& collection) {
  std::lock_guard<std::mutex> lock(indexes_mu_);
  auto it = indexes_.find(collection);
  if (it != indexes_.end()) return it->second.get();

  const CollectionMeta* meta = catalog_->Get(collection);
  page_id_t root;
  if (meta != nullptr && meta->root_page_id != kInvalidPageId) {
    root = meta->root_page_id;
  } else {
    auto root_or = BPlusTree::CreateNew(buffer_pool_.get());
    if (!root_or.ok()) return nullptr;
    root = root_or.value();
    catalog_->CreateCollection(collection, root);
  }
  auto tree = std::make_unique<BPlusTree>(buffer_pool_.get(), root);
  BPlusTree* raw = tree.get();
  indexes_[collection] = std::move(tree);
  return raw;
}

Status StorageEngine::EnsureCollection(const std::string& collection) {
  if (GetOrCreateIndex(collection) == nullptr) {
    return Status::Internal("failed to create index for collection " + collection);
  }
  return Status::OK();
}

std::vector<std::string> StorageEngine::ListCollections() const { return catalog_->ListCollections(); }

Status StorageEngine::PutRaw(const std::string& collection, const std::string& key, const std::string& encoded_doc) {
  BPlusTree* index = GetOrCreateIndex(collection);
  if (index == nullptr) return Status::Internal("no index for collection " + collection);

  // Durability point: fsync'd WAL append before the page mutation.
  auto lsn_or = wal_->Append(WalRecordType::kPut, collection, key, encoded_doc);
  if (!lsn_or.ok()) return lsn_or.status();

  page_id_t write_page_id;
  {
    std::lock_guard<std::mutex> lock(write_pages_mu_);
    auto it = write_pages_.find(collection);
    if (it == write_pages_.end()) {
      page_id_t new_id;
      Page* p = buffer_pool_->NewPage(&new_id);
      if (p == nullptr) return Status::OutOfSpace("buffer pool exhausted allocating data page");
      SlottedPage::Init(p);
      buffer_pool_->UnpinPage(new_id, true);
      write_pages_[collection] = new_id;
      write_page_id = new_id;
    } else {
      write_page_id = it->second;
    }
  }

  Page* page = buffer_pool_->FetchPage(write_page_id);
  if (page == nullptr) return Status::OutOfSpace("buffer pool exhausted");
  slot_id_t slot = SlottedPage::InsertRecord(page, encoded_doc);
  if (slot < 0) {
    // Current write page is full: allocate a fresh one and retry there.
    buffer_pool_->UnpinPage(write_page_id, false);
    page_id_t new_id;
    Page* np = buffer_pool_->NewPage(&new_id);
    if (np == nullptr) return Status::OutOfSpace("buffer pool exhausted allocating data page");
    SlottedPage::Init(np);
    slot = SlottedPage::InsertRecord(np, encoded_doc);
    if (slot < 0) {
      buffer_pool_->UnpinPage(new_id, true);
      return Status::InvalidArgument("document too large to fit in a page (" + std::to_string(encoded_doc.size()) + " bytes)");
    }
    buffer_pool_->UnpinPage(new_id, true);
    std::lock_guard<std::mutex> lock(write_pages_mu_);
    write_pages_[collection] = new_id;
    write_page_id = new_id;
  } else {
    buffer_pool_->UnpinPage(write_page_id, true);
  }

  return index->Insert(key, RID{write_page_id, slot});
}

StatusOr<std::string> StorageEngine::GetRaw(const std::string& collection, const std::string& key) {
  BPlusTree* index = GetOrCreateIndex(collection);
  if (index == nullptr) return Status::Internal("no index for collection " + collection);

  RID rid;
  if (!index->Search(key, &rid)) {
    return Status::NotFound("no such key: " + key);
  }
  Page* page = buffer_pool_->FetchPage(rid.page_id);
  if (page == nullptr) return Status::Internal("failed to fetch data page");
  std::string bytes;
  bool found = SlottedPage::GetRecord(page, rid.slot_id, &bytes);
  buffer_pool_->UnpinPage(rid.page_id, false);
  if (!found) return Status::NotFound("dangling index entry for key: " + key);
  return bytes;
}

std::vector<std::pair<std::string, std::string>> StorageEngine::Scan(const std::string& collection,
                                                                       const std::string& start_key, size_t limit) {
  std::vector<std::pair<std::string, std::string>> results;
  BPlusTree* index = GetOrCreateIndex(collection);
  if (index == nullptr) return results;

  auto entries = index->Scan(start_key, limit);
  for (auto& [key, rid] : entries) {
    Page* page = buffer_pool_->FetchPage(rid.page_id);
    if (page == nullptr) continue;
    std::string bytes;
    if (SlottedPage::GetRecord(page, rid.slot_id, &bytes)) {
      results.emplace_back(key, std::move(bytes));
    }
    buffer_pool_->UnpinPage(rid.page_id, false);
  }
  return results;
}

void StorageEngine::Checkpoint() { buffer_pool_->FlushAllPages(); }

StatusOr<std::vector<WalRecord>> StorageEngine::LedgerEntries(lsn_t from, lsn_t to) {
  auto records_or = wal_->ReadAll();
  if (!records_or.ok()) return records_or.status();
  std::vector<WalRecord> out;
  for (auto& rec : records_or.value()) {
    if (rec.lsn >= from && rec.lsn <= to) out.push_back(rec);
  }
  return out;
}

}  // namespace desentry
