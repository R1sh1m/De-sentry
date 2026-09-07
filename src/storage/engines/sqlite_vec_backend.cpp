// sqlite-vec adapter -- vector search layered on the SQLite core. Compiled
// only under DESENTRY_WITH_SQLITE_VEC (which implies DESENTRY_WITH_SQLITE).
//
// The trade against vector_hnsw_lite is a real one, and the app's engine
// picker states it rather than choosing silently:
//
//   vector_hnsw_lite -- graph index, sub-linear search, memory-resident,
//                       no vendored source. Right for large collections
//                       queried constantly.
//   sqlite_vec       -- brute-force SIMD scan inside SQLite, exact results,
//                       on-disk, and joinable against the same database's
//                       relational tables in one query. Right for
//                       collections up to ~10^5 vectors where "exact" and
//                       "SQL-joinable" beat "sub-linear".
//
// Storage: one vec0 virtual table per collection, plus the same `rows` table
// the SQLite backend uses, so a document with an embedding is one row in
// each and the two are written in a single transaction.
//
// Licence: Apache-2.0 / MIT dual.

#include "desentry/storage/engines/vendored_backends.h"

#if defined(DESENTRY_WITH_SQLITE_VEC) && defined(DESENTRY_WITH_SQLITE)

#include <sqlite3.h>
#include <sqlite-vec.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <set>
#include <vector>

#include "desentry/common/hex.h"
#include "desentry/common/json.h"
#include "desentry/common/logger.h"
#include "desentry/common/platform.h"
#include "desentry/security/crypto.h"
#include "desentry/storage/document_codec.h"
#include "desentry/storage/engines/vector_hnsw_lite.h"

namespace desentry {

namespace {

Status Exec(sqlite3* db, const std::string& sql) {
  char* err = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
    std::string message = err ? err : "unknown sqlite error";
    sqlite3_free(err);
    return Status::IOError("sqlite_vec: " + message + " (while running: " + sql + ")");
  }
  return Status::OK();
}

// vec0 table names are interpolated, not bound, so the collection name has
// to be constrained to an identifier-safe alphabet. Rejecting rather than
// escaping keeps the rule obvious.
bool SafeIdentifier(const std::string& name) {
  if (name.empty() || name.size() > 96) return false;
  for (char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

std::string VecTable(const std::string& collection) {
  std::string safe = collection;
  std::replace(safe.begin(), safe.end(), '-', '_');
  return "vec_" + safe;
}

}  // namespace

class SqliteVecBackend : public BaseBackend {
 public:
  ~SqliteVecBackend() override {
    if (db_ != nullptr) sqlite3_close(db_);
  }

  std::string Name() const override { return "sqlite_vec"; }

  Status Open(const std::string& data_dir, uint64_t quota_mb) override {
    dir_ = data_dir + "/sqlite_vec";
    if (!MakeDirs(dir_)) return Status::IOError("cannot create backend directory: " + dir_);
    path_ = dir_ + "/vectors.sqlite3";

    // The extension is statically linked and registered as an auto-extension
    // *before* the connection is opened, so no runtime .so/.dll is ever
    // loaded -- which is what keeps the offline/airplane-mode guarantee true
    // for this backend too.
    sqlite3_auto_extension(reinterpret_cast<void (*)(void)>(sqlite3_vec_init));

    if (sqlite3_open(path_.c_str(), &db_) != SQLITE_OK) {
      std::string message = db_ ? sqlite3_errmsg(db_) : "unknown";
      return Status::IOError("sqlite_vec: cannot open " + path_ + ": " + message);
    }
    Status st = Exec(db_, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
    if (!st.ok()) return st;
    st = Exec(db_,
              "CREATE TABLE IF NOT EXISTS rows("
              " collection TEXT NOT NULL, key TEXT NOT NULL, doc BLOB NOT NULL,"
              " top_hlc TEXT NOT NULL, has_vector INTEGER NOT NULL DEFAULT 0,"
              " PRIMARY KEY(collection, key)) WITHOUT ROWID;");
    if (!st.ok()) return st;
    st = Exec(db_,
              "CREATE TABLE IF NOT EXISTS vec_rowids("
              " collection TEXT NOT NULL, key TEXT NOT NULL, rowid_value INTEGER NOT NULL,"
              " PRIMARY KEY(collection, key)) WITHOUT ROWID;");
    if (!st.ok()) return st;

    // Resume the rowid sequence where the last run left off. Restarting at
    // zero after a reopen would hand a fresh document the rowid of an
    // existing vector and silently return the wrong document for a search
    // hit -- the kind of bug that only shows up after a restart.
    {
      sqlite3_stmt* stmt = nullptr;
      if (sqlite3_prepare_v2(db_, "SELECT COALESCE(MAX(rowid_value), 0) FROM vec_rowids;", -1, &stmt,
                             nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) next_rowid_ = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
      }
    }

    SetQuotaBytes(quota_mb * 1024ull * 1024ull);
    SetBytesUsed(FileSize(path_));
    return Status::OK();
  }

  Status Put(const std::string& collection, const std::string& key,
              const std::string& encoded_doc) override {
    if (!SafeIdentifier(collection)) {
      return Status::InvalidArgument(
          "sqlite_vec: collection names must be [A-Za-z0-9_-] to back a vec0 table: " + collection);
    }

    uint64_t old_cost = 0;
    {
      auto existing = Get(collection, key);
      if (existing.ok()) old_cost = RecordCost(key, existing.value());
    }
    Status charged = Charge(old_cost, RecordCost(key, encoded_doc));
    if (!charged.ok()) return charged;

    std::string top_hlc;
    try {
      top_hlc = DecodeDocument(encoded_doc).MaxTimestamp().ToString();
    } catch (const std::exception& e) {
      return Status::Corruption(std::string("sqlite_vec: undecodable document: ") + e.what());
    }

    std::vector<float> vec;
    const bool has_vector = VectorHnswLiteBackend::ExtractEmbedding(encoded_doc, &vec);
    if (has_vector) VectorHnswLiteBackend::Normalize(&vec);

    std::lock_guard<std::mutex> lock(mu_);
    if (has_vector) {
      Status st = EnsureVecTable(collection, vec.size());
      if (!st.ok()) return st;
    }

    Status st = Exec(db_, "BEGIN IMMEDIATE;");
    if (!st.ok()) return st;

    sqlite3_stmt* stmt = nullptr;
    const char* upsert =
        "INSERT INTO rows(collection, key, doc, top_hlc, has_vector) VALUES(?1, ?2, ?3, ?4, ?5)"
        " ON CONFLICT(collection, key) DO UPDATE SET doc=excluded.doc,"
        " top_hlc=excluded.top_hlc, has_vector=excluded.has_vector;";
    if (sqlite3_prepare_v2(db_, upsert, -1, &stmt, nullptr) != SQLITE_OK) {
      Exec(db_, "ROLLBACK;");
      return Status::Internal("sqlite_vec: cannot prepare upsert");
    }
    sqlite3_bind_text(stmt, 1, collection.data(), static_cast<int>(collection.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 3, encoded_doc.data(), static_cast<int>(encoded_doc.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, top_hlc.data(), static_cast<int>(top_hlc.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, has_vector ? 1 : 0);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
      Exec(db_, "ROLLBACK;");
      return Status::IOError(std::string("sqlite_vec: upsert failed: ") + sqlite3_errmsg(db_));
    }

    if (has_vector) {
      st = UpsertVector(collection, key, vec);
      if (!st.ok()) {
        Exec(db_, "ROLLBACK;");
        return st;
      }
    }
    st = Exec(db_, "COMMIT;");
    SetBytesUsed(FileSize(path_));
    return st;
  }

  StatusOr<std::string> Get(const std::string& collection, const std::string& key) override {
    std::lock_guard<std::mutex> lock(mu_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT doc FROM rows WHERE collection = ?1 AND key = ?2;", -1, &stmt,
                           nullptr) != SQLITE_OK) {
      return Status::Internal("sqlite_vec: cannot prepare select");
    }
    sqlite3_bind_text(stmt, 1, collection.data(), static_cast<int>(collection.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
    std::string out;
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      const void* blob = sqlite3_column_blob(stmt, 0);
      out.assign(static_cast<const char*>(blob), sqlite3_column_bytes(stmt, 0));
      found = true;
    }
    sqlite3_finalize(stmt);
    if (!found) return Status::NotFound("no such key: " + key);
    return out;
  }

  std::vector<EngineRow> Scan(const std::string& collection, const std::string& start_key,
                               size_t limit) override {
    std::vector<EngineRow> out;
    std::lock_guard<std::mutex> lock(mu_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_, "SELECT key, doc FROM rows WHERE collection = ?1 AND key >= ?2 ORDER BY key ASC;",
            -1, &stmt, nullptr) != SQLITE_OK) {
      return out;
    }
    sqlite3_bind_text(stmt, 1, collection.data(), static_cast<int>(collection.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, start_key.data(), static_cast<int>(start_key.size()), SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const unsigned char* key_text = sqlite3_column_text(stmt, 0);
      const void* blob = sqlite3_column_blob(stmt, 1);
      out.emplace_back(std::string(reinterpret_cast<const char*>(key_text), sqlite3_column_bytes(stmt, 0)),
                       std::string(static_cast<const char*>(blob), sqlite3_column_bytes(stmt, 1)));
      if (limit != 0 && out.size() >= limit) break;
    }
    sqlite3_finalize(stmt);
    return out;
  }

  Status Verify() override {
    std::lock_guard<std::mutex> lock(mu_);
    Status st = Exec(db_, "PRAGMA quick_check;");
    if (!st.ok()) return st;
    // Every row claiming a vector must actually have one in vec_rowids --
    // that mapping is the only thing tying a search hit back to a document.
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT r.collection, r.key FROM rows r WHERE r.has_vector = 1"
                           " AND NOT EXISTS (SELECT 1 FROM vec_rowids v"
                           "                 WHERE v.collection = r.collection AND v.key = r.key);",
                           -1, &stmt, nullptr) != SQLITE_OK) {
      return Status::Internal("sqlite_vec: cannot prepare verify query");
    }
    Status result = Status::OK();
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      result = Status::Corruption(
          std::string("sqlite_vec: row claims a vector but has no vec0 entry: ") +
          reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)) + "/" +
          reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
    }
    sqlite3_finalize(stmt);
    return result;
  }

  Status Flush() override {
    std::lock_guard<std::mutex> lock(mu_);
    Status st = Exec(db_, "PRAGMA wal_checkpoint(TRUNCATE);");
    SetBytesUsed(FileSize(path_));
    return st;
  }

  std::vector<std::string> ListCollections() const override {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lock(mu_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT DISTINCT collection FROM rows ORDER BY collection ASC;", -1,
                           &stmt, nullptr) != SQLITE_OK) {
      return out;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      out.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    }
    sqlite3_finalize(stmt);
    return out;
  }

  // -- vector surface, identical in shape to VectorHnswLiteBackend::Search --
  StatusOr<std::vector<VectorHit>> Search(const std::string& collection,
                                           const std::vector<float>& query, size_t k) {
    if (k == 0) return std::vector<VectorHit>();
    if (!SafeIdentifier(collection)) return Status::InvalidArgument("sqlite_vec: unsafe collection name");
    std::vector<float> q = query;
    VectorHnswLiteBackend::Normalize(&q);

    std::lock_guard<std::mutex> lock(mu_);
    const std::string sql =
        "SELECT v.key, m.distance FROM (SELECT rowid, distance FROM " + VecTable(collection) +
        " WHERE embedding MATCH ?1 AND k = ?2) m"
        " JOIN vec_rowids v ON v.rowid_value = m.rowid AND v.collection = ?3"
        " ORDER BY m.distance ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
      return Status::NotFound("sqlite_vec: no vector table for collection " + collection);
    }
    sqlite3_bind_blob(stmt, 1, q.data(), static_cast<int>(q.size() * sizeof(float)), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(k));
    sqlite3_bind_text(stmt, 3, collection.data(), static_cast<int>(collection.size()), SQLITE_TRANSIENT);

    std::vector<VectorHit> hits;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      VectorHit hit;
      hit.key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
      // vec0 reports L2 distance over unit vectors; for normalised inputs
      // cosine = 1 - d^2/2, so the score matches vector_hnsw_lite's scale
      // and the two backends are directly comparable.
      const double d = sqlite3_column_double(stmt, 1);
      hit.score = static_cast<float>(1.0 - (d * d) / 2.0);
      hits.push_back(std::move(hit));
    }
    sqlite3_finalize(stmt);
    return hits;
  }

 private:
  Status EnsureVecTable(const std::string& collection, size_t dim) {
    if (created_.count(collection) != 0) return Status::OK();
    Status st = Exec(db_, "CREATE VIRTUAL TABLE IF NOT EXISTS " + VecTable(collection) +
                              " USING vec0(embedding float[" + std::to_string(dim) + "]);");
    if (!st.ok()) return st;
    created_.insert(collection);
    return Status::OK();
  }

  Status UpsertVector(const std::string& collection, const std::string& key,
                       const std::vector<float>& vec) {
    // vec0 has no upsert, so an existing rowid is deleted then reinserted --
    // both inside the caller's transaction, so a search never observes the
    // gap.
    sqlite3_int64 rowid = 0;
    bool existing = false;
    {
      sqlite3_stmt* stmt = nullptr;
      if (sqlite3_prepare_v2(db_,
                             "SELECT rowid_value FROM vec_rowids WHERE collection = ?1 AND key = ?2;",
                             -1, &stmt, nullptr) != SQLITE_OK) {
        return Status::Internal("sqlite_vec: cannot prepare rowid lookup");
      }
      sqlite3_bind_text(stmt, 1, collection.data(), static_cast<int>(collection.size()), SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
        rowid = sqlite3_column_int64(stmt, 0);
        existing = true;
      }
      sqlite3_finalize(stmt);
    }
    if (existing) {
      Status st = Exec(db_, "DELETE FROM " + VecTable(collection) + " WHERE rowid = " +
                                std::to_string(rowid) + ";");
      if (!st.ok()) return st;
    } else {
      rowid = ++next_rowid_;
    }

    sqlite3_stmt* ins = nullptr;
    const std::string sql = "INSERT INTO " + VecTable(collection) + "(rowid, embedding) VALUES(?1, ?2);";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &ins, nullptr) != SQLITE_OK) {
      return Status::Internal("sqlite_vec: cannot prepare vector insert");
    }
    sqlite3_bind_int64(ins, 1, rowid);
    sqlite3_bind_blob(ins, 2, vec.data(), static_cast<int>(vec.size() * sizeof(float)), SQLITE_TRANSIENT);
    const int rc = sqlite3_step(ins);
    sqlite3_finalize(ins);
    if (rc != SQLITE_DONE) {
      return Status::IOError(std::string("sqlite_vec: vector insert failed: ") + sqlite3_errmsg(db_));
    }

    if (!existing) {
      sqlite3_stmt* map = nullptr;
      if (sqlite3_prepare_v2(db_,
                             "INSERT INTO vec_rowids(collection, key, rowid_value) VALUES(?1, ?2, ?3);",
                             -1, &map, nullptr) != SQLITE_OK) {
        return Status::Internal("sqlite_vec: cannot prepare rowid mapping insert");
      }
      sqlite3_bind_text(map, 1, collection.data(), static_cast<int>(collection.size()), SQLITE_TRANSIENT);
      sqlite3_bind_text(map, 2, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
      sqlite3_bind_int64(map, 3, rowid);
      const int map_rc = sqlite3_step(map);
      sqlite3_finalize(map);
      if (map_rc != SQLITE_DONE) return Status::IOError("sqlite_vec: rowid mapping insert failed");
    }
    return Status::OK();
  }

  std::string dir_;
  std::string path_;
  sqlite3* db_ = nullptr;
  mutable std::mutex mu_;
  std::set<std::string> created_;
  sqlite3_int64 next_rowid_ = 0;
};

std::unique_ptr<EngineBackend> MakeSqliteVecBackend() { return std::make_unique<SqliteVecBackend>(); }

}  // namespace desentry

#endif  // DESENTRY_WITH_SQLITE_VEC && DESENTRY_WITH_SQLITE
