#include "desentry/storage/router.h"

#include <algorithm>
#include <cstring>

#include "desentry/common/byte_buffer.h"
#include "desentry/common/crc32.h"
#include "desentry/common/logger.h"
#include "desentry/common/platform.h"
#include "desentry/storage/engines/columnar_lite.h"
#include "desentry/storage/engines/graph_adj.h"
#include "desentry/storage/engines/kv_bplus.h"
#include "desentry/storage/engines/ts_rollup.h"
#include "desentry/storage/engines/vector_hnsw_lite.h"
#include "desentry/storage/engines/vendored_backends.h"

namespace desentry {

// ---------------------------------------------------------------------------
// Cross-engine metadata index
// ---------------------------------------------------------------------------
//
// On-disk format: a sequence of length-prefixed, CRC32-checked records, each
// one a complete IndexEntry. Later records supersede earlier ones for the
// same key, exactly like the WAL -- which means an append is one write with
// no read-modify-write, and a truncated tail from a crash costs the last
// record rather than the whole index.
//
//   [u32 body_len][body][u32 crc32(body)]
//   body = Bytes(key) Bytes(collection) Bytes(engine) Bytes(node_id) I64(updated_ms)

namespace {

constexpr uint32_t kIndexRecordCap = 1u << 20;  // 1MiB: absurdly generous for one entry

std::string EncodeIndexEntry(const IndexEntry& entry) {
  ByteWriter w;
  w.Bytes(entry.key);
  w.Bytes(entry.collection);
  w.Bytes(entry.engine);
  w.Bytes(entry.node_id);
  w.I64(entry.updated_ms);
  return w.TakeString();
}

}  // namespace

StatusOr<std::unique_ptr<CrossEngineIndex>> CrossEngineIndex::Open(const std::string& path) {
  std::unique_ptr<CrossEngineIndex> index(new CrossEngineIndex(path));
  Status st = index->Load();
  if (!st.ok()) return st;
  return index;
}

CrossEngineIndex::~CrossEngineIndex() {
  if (file_ && file_->is_open()) {
    file_->flush();
    file_->close();
  }
}

Status CrossEngineIndex::Load() {
  {
    std::ifstream probe(path_, std::ios::binary);
    if (!probe.is_open()) {
      std::ofstream create(path_, std::ios::binary);
      if (!create.is_open()) return Status::IOError("cannot create cross-engine index: " + path_);
    }
  }
  file_ = std::make_unique<std::fstream>(path_,
                                          std::ios::in | std::ios::out | std::ios::binary);
  if (!file_->is_open()) return Status::IOError("cannot open cross-engine index: " + path_);

  std::lock_guard<std::mutex> lock(mu_);
  file_->clear();
  file_->seekg(0);
  for (;;) {
    char len_buf[4];
    file_->read(len_buf, 4);
    if (file_->gcount() < 4) break;
    uint32_t body_len = 0;
    std::memcpy(&body_len, len_buf, 4);
    if (body_len == 0 || body_len > kIndexRecordCap) {
      DSN_LOG_WARN("index", "cross-engine index: implausible record length, stopping load");
      break;
    }
    std::string body(body_len, '\0');
    file_->read(body.data(), static_cast<std::streamsize>(body_len));
    if (static_cast<uint32_t>(file_->gcount()) < body_len) break;  // torn tail
    char crc_buf[4];
    file_->read(crc_buf, 4);
    if (file_->gcount() < 4) break;
    uint32_t stored_crc = 0;
    std::memcpy(&stored_crc, crc_buf, 4);
    if (stored_crc != Crc32(body.data(), body.size())) {
      DSN_LOG_WARN("index", "cross-engine index: CRC mismatch, stopping load at a torn record");
      break;
    }
    try {
      ByteReader r(body);
      IndexEntry entry;
      entry.key = r.Bytes();
      entry.collection = r.Bytes();
      entry.engine = r.Bytes();
      entry.node_id = r.Bytes();
      entry.updated_ms = r.I64();
      entries_[entry.key] = std::move(entry);
    } catch (const std::exception&) {
      break;
    }
  }
  file_->clear();
  return Status::OK();
}

Status CrossEngineIndex::AppendRecord(const IndexEntry& entry) {
  std::string body = EncodeIndexEntry(entry);
  uint32_t body_len = static_cast<uint32_t>(body.size());
  uint32_t crc = Crc32(body.data(), body.size());

  std::string frame;
  frame.resize(4);
  std::memcpy(frame.data(), &body_len, 4);
  frame += body;
  size_t off = frame.size();
  frame.resize(off + 4);
  std::memcpy(frame.data() + off, &crc, 4);

  file_->clear();
  file_->seekp(0, std::ios::end);
  file_->write(frame.data(), static_cast<std::streamsize>(frame.size()));
  if (!file_->good()) return Status::IOError("cross-engine index append failed");
  file_->flush();
  return Status::OK();
}

Status CrossEngineIndex::Upsert(const IndexEntry& entry) {
  std::lock_guard<std::mutex> lock(mu_);
  IndexEntry stored = entry;
  if (stored.updated_ms == 0) stored.updated_ms = NowMs();
  Status st = AppendRecord(stored);
  if (!st.ok()) return st;
  entries_[stored.key] = std::move(stored);
  return Status::OK();
}

StatusOr<IndexEntry> CrossEngineIndex::Lookup(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = entries_.find(key);
  if (it == entries_.end()) return Status::NotFound("key not in cross-engine index: " + key);
  return it->second;
}

std::vector<IndexEntry> CrossEngineIndex::ByEngine(const std::string& engine) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<IndexEntry> out;
  for (const auto& [key, entry] : entries_) {
    (void)key;
    if (entry.engine == engine) out.push_back(entry);
  }
  return out;
}

std::vector<IndexEntry> CrossEngineIndex::ByCollection(const std::string& collection) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<IndexEntry> out;
  for (const auto& [key, entry] : entries_) {
    (void)key;
    if (entry.collection == collection) out.push_back(entry);
  }
  return out;
}

size_t CrossEngineIndex::Size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return entries_.size();
}

Status CrossEngineIndex::Flush() {
  std::lock_guard<std::mutex> lock(mu_);
  if (file_) file_->flush();
  return Status::OK();
}

Status CrossEngineIndex::Compact() {
  std::lock_guard<std::mutex> lock(mu_);
  // Write the live set to a sibling file and rename over the original, so a
  // crash mid-compaction leaves the original intact rather than a half-built
  // index.
  const std::string tmp = path_ + ".compact";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return Status::IOError("cannot write " + tmp);
    for (const auto& [key, entry] : entries_) {
      (void)key;
      std::string body = EncodeIndexEntry(entry);
      uint32_t body_len = static_cast<uint32_t>(body.size());
      uint32_t crc = Crc32(body.data(), body.size());
      out.write(reinterpret_cast<const char*>(&body_len), 4);
      out.write(body.data(), static_cast<std::streamsize>(body.size()));
      out.write(reinterpret_cast<const char*>(&crc), 4);
    }
    out.flush();
    if (!out.good()) return Status::IOError("cross-engine index compaction write failed");
  }
  if (file_ && file_->is_open()) file_->close();
  std::remove(path_.c_str());
  if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
    return Status::IOError("cannot commit compacted cross-engine index");
  }
  file_ = std::make_unique<std::fstream>(path_, std::ios::in | std::ios::out | std::ios::binary);
  if (!file_->is_open()) return Status::IOError("cannot reopen cross-engine index after compaction");
  return Status::OK();
}

// ---------------------------------------------------------------------------
// Backend factory
// ---------------------------------------------------------------------------

StatusOr<std::unique_ptr<EngineBackend>> MakeBackend(const std::string& engine_name) {
  if (engine_name == "kv") return std::unique_ptr<EngineBackend>(new KvBPlusBackend());
  if (engine_name == "columnar_lite") return std::unique_ptr<EngineBackend>(new ColumnarLiteBackend());
  if (engine_name == "ts_rollup") return std::unique_ptr<EngineBackend>(new TsRollupBackend());
  if (engine_name == "vector_hnsw_lite") {
    return std::unique_ptr<EngineBackend>(new VectorHnswLiteBackend());
  }
  if (engine_name == "graph_adj") return std::unique_ptr<EngineBackend>(new GraphAdjBackend());
  return MakeVendoredBackend(engine_name);
}

// ---------------------------------------------------------------------------
// Router
// ---------------------------------------------------------------------------

StatusOr<std::unique_ptr<StorageRouter>> StorageRouter::Open(const Options& options) {
  std::unique_ptr<StorageRouter> router(new StorageRouter());
  router->data_dir_ = options.data_dir + "/engines";
  router->default_engine_ = options.default_engine;
  router->catalog_ = options.catalog;
  router->node_id_ = options.node_id;

  if (!MakeDirs(router->data_dir_)) {
    return Status::IOError("cannot create engines directory: " + router->data_dir_);
  }

  const uint64_t db_bytes = options.quota_mb == 0
                                ? 0
                                : options.quota_mb * 1024ull * 1024ull * options.db_share_pct / 100;
  router->quota_bytes_ = db_bytes;

  // The data-plane budget is split evenly across the configured engines
  // rather than being handed to each in full. Handing each backend the whole
  // budget would let five engines collectively use five times the quota,
  // which would make the number meaningless.
  std::vector<std::string> engines = options.engines;
  if (engines.empty()) engines.push_back("kv");
  if (std::find(engines.begin(), engines.end(), options.default_engine) == engines.end()) {
    engines.push_back(options.default_engine);
  }
  std::sort(engines.begin(), engines.end());
  engines.erase(std::unique(engines.begin(), engines.end()), engines.end());

  const uint64_t per_engine = db_bytes == 0 ? 0 : db_bytes / engines.size();
  for (const std::string& name : engines) {
    Status st = router->RegisterBackend(name, router->data_dir_, per_engine);
    if (!st.ok()) return st;
  }

  auto index_or = CrossEngineIndex::Open(router->data_dir_ + "/cross_engine_index.log");
  if (!index_or.ok()) return index_or.status();
  router->index_ = std::move(index_or.value());

  DSN_LOG_INFO("router", "storage router ready with " << engines.size() << " engine(s), default="
                                                       << router->default_engine_);
  return router;
}

StorageRouter::~StorageRouter() = default;

Status StorageRouter::RegisterBackend(const std::string& name, const std::string& data_dir,
                                       uint64_t bytes) {
  auto backend_or = MakeBackend(name);
  if (!backend_or.ok()) return backend_or.status();
  std::unique_ptr<EngineBackend> backend = std::move(backend_or.value());
  // Backends take a MiB budget; round up so a tiny quota never becomes zero,
  // which would read as "unlimited".
  const uint64_t mb = bytes == 0 ? 0 : std::max<uint64_t>(1, bytes / (1024ull * 1024ull));
  Status st = backend->Open(data_dir, mb);
  if (!st.ok()) return st;
  std::lock_guard<std::mutex> lock(mu_);
  backends_[name] = std::move(backend);
  return Status::OK();
}

EngineBackend* StorageRouter::Backend(const std::string& engine_name) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = backends_.find(engine_name);
  return it == backends_.end() ? nullptr : it->second.get();
}

std::vector<std::string> StorageRouter::AvailableEngines() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::string> names;
  names.reserve(backends_.size());
  for (const auto& [name, backend] : backends_) {
    (void)backend;
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::string StorageRouter::EngineNameFor(const std::string& collection) const {
  if (catalog_ != nullptr) {
    CollectionMeta meta;
    if (catalog_->GetCopy(collection, &meta) && !meta.engine.empty()) return meta.engine;
  }
  return default_engine_;
}

EngineBackend* StorageRouter::BackendFor(const std::string& collection) {
  const std::string engine = EngineNameFor(collection);
  EngineBackend* backend = Backend(engine);
  if (backend != nullptr) return backend;
  // A collection bound to an engine this build does not have is a real
  // configuration error, not something to paper over by writing its rows
  // into a different layout -- that would strand them where nothing looks.
  DSN_LOG_ERROR("router", "collection '" << collection << "' is bound to engine '" << engine
                                          << "' which is not available in this build");
  return nullptr;
}

Status StorageRouter::BindCollection(const std::string& collection, const std::string& engine_name) {
  if (Backend(engine_name) == nullptr) {
    const std::string option = VendoredBackendOption(engine_name);
    if (!option.empty() && !VendoredBackendAvailable(engine_name)) {
      return Status::InvalidArgument("engine '" + engine_name +
                                      "' is not compiled into this build (rebuild with -D" + option +
                                      "=ON)");
    }
    return Status::InvalidArgument("engine '" + engine_name +
                                    "' is not in this node's configured engines list");
  }
  if (catalog_ == nullptr) return Status::Internal("router has no catalog to record the binding in");
  if (!catalog_->HasCollection(collection)) {
    Status st = catalog_->UpsertRootPageId(collection, kInvalidPageId);
    if (!st.ok()) return st;
  }
  return catalog_->SetEngine(collection, engine_name);
}

Status StorageRouter::Put(const std::string& collection, const std::string& key,
                           const std::string& encoded_doc) {
  EngineBackend* backend = BackendFor(collection);
  if (backend == nullptr) {
    return Status::InvalidArgument("no available backend for collection " + collection);
  }
  Status st = backend->Put(collection, key, encoded_doc);
  if (!st.ok()) return st;

  IndexEntry entry;
  entry.key = key;
  entry.collection = collection;
  entry.engine = backend->Name();
  entry.node_id = node_id_;
  entry.updated_ms = NowMs();
  // An index append that fails must not fail the write: the document is
  // already durable in the ledger and the backend, and the index is a
  // lookup accelerator that Compact()/rebuild can restore. Losing the write
  // to protect a cache would be exactly backwards.
  Status index_st = index_->Upsert(entry);
  if (!index_st.ok()) {
    DSN_LOG_WARN("router", "cross-engine index update failed for " << collection << "/" << key << ": "
                                                                    << index_st.message());
  }
  return Status::OK();
}

StatusOr<std::string> StorageRouter::Get(const std::string& collection, const std::string& key) {
  EngineBackend* backend = BackendFor(collection);
  if (backend == nullptr) {
    return Status::InvalidArgument("no available backend for collection " + collection);
  }
  return backend->Get(collection, key);
}

std::vector<EngineRow> StorageRouter::Scan(const std::string& collection,
                                             const std::string& start_key, size_t limit) {
  EngineBackend* backend = BackendFor(collection);
  if (backend == nullptr) return {};
  return backend->Scan(collection, start_key, limit);
}

Status StorageRouter::MergeRemote(const std::string& collection, const std::string& key,
                                   const std::string& remote_encoded_doc) {
  EngineBackend* backend = BackendFor(collection);
  if (backend == nullptr) {
    return Status::InvalidArgument("no available backend for collection " + collection);
  }
  return backend->MergeRemote(collection, key, remote_encoded_doc);
}

std::string StorageRouter::Checksum(const std::string& collection) {
  EngineBackend* backend = BackendFor(collection);
  if (backend == nullptr) return std::string();
  return backend->Checksum(collection);
}

uint64_t StorageRouter::QuotaUse() const {
  std::lock_guard<std::mutex> lock(mu_);
  uint64_t total = 0;
  for (const auto& [name, backend] : backends_) {
    (void)name;
    total += backend->QuotaUse();
  }
  return total;
}

Status StorageRouter::Verify() {
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& [name, backend] : backends_) {
    Status st = backend->Verify();
    if (!st.ok()) return Status::Corruption("engine '" + name + "': " + st.message());
  }
  return Status::OK();
}

Status StorageRouter::Flush() {
  std::vector<EngineBackend*> to_flush;
  {
    std::lock_guard<std::mutex> lock(mu_);
    to_flush.reserve(backends_.size());
    for (const auto& [name, backend] : backends_) {
      (void)name;
      to_flush.push_back(backend.get());
    }
  }
  for (EngineBackend* backend : to_flush) {
    Status st = backend->Flush();
    if (!st.ok()) return st;
  }
  return index_->Flush();
}

}  // namespace desentry
