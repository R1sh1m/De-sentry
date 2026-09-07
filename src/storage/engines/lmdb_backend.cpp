// LMDB adapter -- the generic and transit key/value backbone. Compiled only
// under DESENTRY_WITH_LMDB.
//
// LMDB earns its place here for one reason the from-scratch `kv` backend
// cannot match without a lot more code: a memory-mapped, copy-on-write
// B+Tree with single-writer/many-reader MVCC, so a long gossip scan never
// blocks an API write and a crash can never leave a torn page (the mapping
// is only ever advanced by an atomic meta-page flip). For the transit store
// -- which is written by replication and read by a returning owner at the
// same time -- that property is worth a vendored dependency.
//
// One environment, one named database per collection. LMDB caps the number
// of named databases at open time, so kMaxNamedDbs is explicit rather than
// discovered when the 128th collection fails to open.
//
// Licence: OpenLDAP Public Licence -- permissive, static-link safe, and
// specifically not copyleft, which is why LMDB rather than an
// LGPL/GPL-licensed embedded KV.

#include "desentry/storage/engines/vendored_backends.h"

#ifdef DESENTRY_WITH_LMDB

#include <lmdb.h>

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "desentry/common/logger.h"
#include "desentry/common/platform.h"

namespace desentry {

namespace {

constexpr unsigned int kMaxNamedDbs = 256;
// Map size is a virtual-address reservation, not an allocation: LMDB grows
// the file lazily inside it. Reserving generously avoids MDB_MAP_FULL, which
// is otherwise the classic LMDB operational surprise.
constexpr size_t kDefaultMapSizeBytes = 8ull << 30;  // 8GiB of address space

std::string LmdbError(int rc) { return mdb_strerror(rc); }

MDB_val ToVal(const std::string& s) {
  MDB_val v;
  v.mv_size = s.size();
  v.mv_data = const_cast<char*>(s.data());
  return v;
}

std::string FromVal(const MDB_val& v) {
  return std::string(static_cast<const char*>(v.mv_data), v.mv_size);
}

}  // namespace

class LmdbBackend : public BaseBackend {
 public:
  ~LmdbBackend() override {
    if (env_ != nullptr) mdb_env_close(env_);
  }

  std::string Name() const override { return "lmdb"; }

  Status Open(const std::string& data_dir, uint64_t quota_mb) override {
    dir_ = data_dir + "/lmdb";
    if (!MakeDirs(dir_)) return Status::IOError("cannot create backend directory: " + dir_);

    int rc = mdb_env_create(&env_);
    if (rc != MDB_SUCCESS) return Status::IOError("lmdb: env_create: " + LmdbError(rc));
    rc = mdb_env_set_maxdbs(env_, kMaxNamedDbs);
    if (rc != MDB_SUCCESS) return Status::IOError("lmdb: set_maxdbs: " + LmdbError(rc));

    // A quota, when set, also bounds the map: an engine that cannot exceed
    // its budget on disk should not reserve address space it can never use.
    size_t map_size = quota_mb == 0 ? kDefaultMapSizeBytes
                                     : std::max<size_t>(quota_mb * 1024ull * 1024ull * 2, 16ull << 20);
    rc = mdb_env_set_mapsize(env_, map_size);
    if (rc != MDB_SUCCESS) return Status::IOError("lmdb: set_mapsize: " + LmdbError(rc));

    // MDB_NOSYNC: the hash-chained ledger one layer up already fsync'd this
    // write before the router was called, so a second fsync here is pure
    // latency. LMDB's meta-page flip still guarantees the mapping is never
    // torn -- what is at risk on power loss is the last few transactions,
    // and those are exactly what WAL replay restores.
    rc = mdb_env_open(env_, dir_.c_str(), MDB_NOSYNC, 0600);
    if (rc != MDB_SUCCESS) return Status::IOError("lmdb: env_open " + dir_ + ": " + LmdbError(rc));

    SetQuotaBytes(quota_mb * 1024ull * 1024ull);
    SetBytesUsed(FileSize(dir_ + "/data.mdb"));
    return Status::OK();
  }

  Status Put(const std::string& collection, const std::string& key,
              const std::string& encoded_doc) override {
    uint64_t old_cost = 0;
    {
      auto existing = Get(collection, key);
      if (existing.ok()) old_cost = RecordCost(key, existing.value());
    }
    Status charged = Charge(old_cost, RecordCost(key, encoded_doc));
    if (!charged.ok()) return charged;

    std::lock_guard<std::mutex> lock(mu_);
    MDB_txn* txn = nullptr;
    int rc = mdb_txn_begin(env_, nullptr, 0, &txn);
    if (rc != MDB_SUCCESS) return Status::IOError("lmdb: txn_begin: " + LmdbError(rc));

    MDB_dbi dbi = 0;
    rc = mdb_dbi_open(txn, collection.c_str(), MDB_CREATE, &dbi);
    if (rc != MDB_SUCCESS) {
      mdb_txn_abort(txn);
      return Status::IOError("lmdb: dbi_open " + collection + ": " + LmdbError(rc));
    }
    MDB_val k = ToVal(key);
    MDB_val v = ToVal(encoded_doc);
    rc = mdb_put(txn, dbi, &k, &v, 0);
    if (rc != MDB_SUCCESS) {
      mdb_txn_abort(txn);
      if (rc == MDB_MAP_FULL) return Status::OutOfSpace("lmdb: map full");
      return Status::IOError("lmdb: put: " + LmdbError(rc));
    }
    rc = mdb_txn_commit(txn);
    if (rc != MDB_SUCCESS) return Status::IOError("lmdb: commit: " + LmdbError(rc));

    SetBytesUsed(FileSize(dir_ + "/data.mdb"));
    return Status::OK();
  }

  StatusOr<std::string> Get(const std::string& collection, const std::string& key) override {
    std::lock_guard<std::mutex> lock(mu_);
    MDB_txn* txn = nullptr;
    int rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn);
    if (rc != MDB_SUCCESS) return Status::IOError("lmdb: read txn_begin: " + LmdbError(rc));
    MDB_dbi dbi = 0;
    rc = mdb_dbi_open(txn, collection.c_str(), 0, &dbi);
    if (rc != MDB_SUCCESS) {
      mdb_txn_abort(txn);
      return Status::NotFound("no such collection: " + collection);
    }
    MDB_val k = ToVal(key);
    MDB_val v;
    rc = mdb_get(txn, dbi, &k, &v);
    if (rc == MDB_NOTFOUND) {
      mdb_txn_abort(txn);
      return Status::NotFound("no such key: " + key);
    }
    if (rc != MDB_SUCCESS) {
      mdb_txn_abort(txn);
      return Status::IOError("lmdb: get: " + LmdbError(rc));
    }
    // The value points into the read transaction's mapping, so it must be
    // copied before the transaction is aborted -- reading it afterwards is
    // the single most common LMDB use-after-free.
    std::string out = FromVal(v);
    mdb_txn_abort(txn);
    return out;
  }

  std::vector<EngineRow> Scan(const std::string& collection, const std::string& start_key,
                               size_t limit) override {
    std::vector<EngineRow> out;
    std::lock_guard<std::mutex> lock(mu_);
    MDB_txn* txn = nullptr;
    if (mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn) != MDB_SUCCESS) return out;
    MDB_dbi dbi = 0;
    if (mdb_dbi_open(txn, collection.c_str(), 0, &dbi) != MDB_SUCCESS) {
      mdb_txn_abort(txn);
      return out;
    }
    MDB_cursor* cursor = nullptr;
    if (mdb_cursor_open(txn, dbi, &cursor) != MDB_SUCCESS) {
      mdb_txn_abort(txn);
      return out;
    }
    MDB_val k = ToVal(start_key);
    MDB_val v;
    int rc = start_key.empty() ? mdb_cursor_get(cursor, &k, &v, MDB_FIRST)
                                : mdb_cursor_get(cursor, &k, &v, MDB_SET_RANGE);
    while (rc == MDB_SUCCESS) {
      out.emplace_back(FromVal(k), FromVal(v));
      if (limit != 0 && out.size() >= limit) break;
      rc = mdb_cursor_get(cursor, &k, &v, MDB_NEXT);
    }
    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);
    return out;
  }

  Status Verify() override {
    // LMDB's mapping is either coherent or unopenable -- there is no
    // half-committed state to check for -- so the honest verification is
    // that every named database opens and every row is readable end to end.
    for (const std::string& collection : ListCollections()) {
      std::vector<EngineRow> rows = Scan(collection, "", 0);
      for (const auto& [key, doc] : rows) {
        if (doc.empty()) {
          return Status::Corruption("lmdb: empty value for " + collection + "/" + key);
        }
      }
    }
    return Status::OK();
  }

  Status Flush() override {
    std::lock_guard<std::mutex> lock(mu_);
    int rc = mdb_env_sync(env_, 1);
    SetBytesUsed(FileSize(dir_ + "/data.mdb"));
    if (rc != MDB_SUCCESS) return Status::IOError("lmdb: env_sync: " + LmdbError(rc));
    return Status::OK();
  }

  std::vector<std::string> ListCollections() const override {
    // LMDB's unnamed root database lists every named database, which is the
    // only way to enumerate collections without keeping a second index.
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lock(mu_);
    MDB_txn* txn = nullptr;
    if (mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn) != MDB_SUCCESS) return out;
    MDB_dbi root = 0;
    if (mdb_dbi_open(txn, nullptr, 0, &root) != MDB_SUCCESS) {
      mdb_txn_abort(txn);
      return out;
    }
    MDB_cursor* cursor = nullptr;
    if (mdb_cursor_open(txn, root, &cursor) != MDB_SUCCESS) {
      mdb_txn_abort(txn);
      return out;
    }
    MDB_val k, v;
    int rc = mdb_cursor_get(cursor, &k, &v, MDB_FIRST);
    while (rc == MDB_SUCCESS) {
      out.push_back(FromVal(k));
      rc = mdb_cursor_get(cursor, &k, &v, MDB_NEXT);
    }
    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);
    std::sort(out.begin(), out.end());
    return out;
  }

 private:
  std::string dir_;
  MDB_env* env_ = nullptr;
  mutable std::mutex mu_;
};

std::unique_ptr<EngineBackend> MakeLmdbBackend() { return std::make_unique<LmdbBackend>(); }

}  // namespace desentry

#endif  // DESENTRY_WITH_LMDB
