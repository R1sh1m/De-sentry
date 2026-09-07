// DuckDB adapter -- the columnar / analytical member of the OSS backbone.
// Compiled only under DESENTRY_WITH_DUCKDB.
//
// Where columnar_lite gives a compact segmented layout, DuckDB gives a real
// vectorised execution engine: GROUP BY, window functions, joins across
// collections, and Parquet/CSV export, all in-process with no server. That
// is the whole reason to accept the (large) vendored source -- if the only
// need is compact storage, columnar_lite already covers it and this backend
// should be left out of the build.
//
// Storage shape mirrors the SQLite adapter deliberately, so a collection can
// be moved between them by copy without a schema translation:
//
//   CREATE TABLE rows(collection VARCHAR, key VARCHAR, doc BLOB,
//                     json VARCHAR, top_hlc VARCHAR, updated_ms BIGINT);
//
// The CRDT blob is the authority; `json` is a derived projection that
// DuckDB's JSON functions read. Verify() re-derives it and reports a
// mismatch rather than trusting that every write path refreshed it.
//
// Licence: MIT.

#include "desentry/storage/engines/vendored_backends.h"

#ifdef DESENTRY_WITH_DUCKDB

#include <duckdb.h>

#include <algorithm>
#include <mutex>
#include <vector>

#include "desentry/common/hex.h"
#include "desentry/common/logger.h"
#include "desentry/common/platform.h"
#include "desentry/security/crypto.h"
#include "desentry/storage/document_codec.h"

namespace desentry {

namespace {

// RAII around duckdb_result, which must be destroyed on every path including
// the error one -- forgetting that is the classic C-API leak.
class DuckResult {
 public:
  ~DuckResult() { duckdb_destroy_result(&result_); }
  duckdb_result* get() { return &result_; }
  idx_t rows() { return duckdb_row_count(&result_); }
  std::string Text(idx_t col, idx_t row) {
    char* v = duckdb_value_varchar(&result_, col, row);
    if (v == nullptr) return std::string();
    std::string out(v);
    duckdb_free(v);
    return out;
  }
  std::string Blob(idx_t col, idx_t row) {
    duckdb_blob b = duckdb_value_blob(&result_, col, row);
    std::string out(static_cast<const char*>(b.data), b.size);
    if (b.data != nullptr) duckdb_free(b.data);
    return out;
  }

 private:
  duckdb_result result_{};
};

std::string SqlQuote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "''";
    else out.push_back(c);
  }
  out.push_back('\'');
  return out;
}

// DuckDB's C API has no bind for BLOB literals in a plain query, so a blob
// is written as a hex literal. Correct, and explicit about the cost.
std::string BlobLiteral(const std::string& bytes) {
  return "from_hex('" + HexEncode(bytes) + "')";
}

}  // namespace

class DuckDbBackend : public BaseBackend {
 public:
  ~DuckDbBackend() override {
    if (conn_ != nullptr) duckdb_disconnect(&conn_);
    if (db_ != nullptr) duckdb_close(&db_);
  }

  std::string Name() const override { return "duckdb"; }

  Status Open(const std::string& data_dir, uint64_t quota_mb) override {
    dir_ = data_dir + "/duckdb";
    if (!MakeDirs(dir_)) return Status::IOError("cannot create backend directory: " + dir_);
    path_ = dir_ + "/desentry.duckdb";

    if (duckdb_open(path_.c_str(), &db_) != DuckDBSuccess) {
      return Status::IOError("duckdb: cannot open " + path_);
    }
    if (duckdb_connect(db_, &conn_) != DuckDBSuccess) {
      return Status::IOError("duckdb: cannot connect to " + path_);
    }
    Status st = Exec(
        "CREATE TABLE IF NOT EXISTS rows("
        " collection VARCHAR NOT NULL, key VARCHAR NOT NULL, doc BLOB NOT NULL,"
        " json VARCHAR, top_hlc VARCHAR NOT NULL, updated_ms BIGINT NOT NULL,"
        " PRIMARY KEY(collection, key));");
    if (!st.ok()) return st;

    SetQuotaBytes(quota_mb * 1024ull * 1024ull);
    SetBytesUsed(FileSize(path_));
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

    std::string json_text;
    std::string top_hlc;
    try {
      CrdtValue doc = DecodeDocument(encoded_doc);
      json_text = doc.ToJson().Dump();
      top_hlc = doc.MaxTimestamp().ToString();
    } catch (const std::exception& e) {
      return Status::Corruption(std::string("duckdb: undecodable document: ") + e.what());
    }

    std::lock_guard<std::mutex> lock(mu_);
    // DELETE-then-INSERT rather than upsert: DuckDB's ON CONFLICT support
    // has varied across releases, and two statements inside one transaction
    // are portable across every version we might vendor.
    Status st = Exec("BEGIN TRANSACTION;");
    if (!st.ok()) return st;
    st = Exec("DELETE FROM rows WHERE collection = " + SqlQuote(collection) + " AND key = " +
              SqlQuote(key) + ";");
    if (!st.ok()) {
      Exec("ROLLBACK;");
      return st;
    }
    st = Exec("INSERT INTO rows VALUES(" + SqlQuote(collection) + ", " + SqlQuote(key) + ", " +
              BlobLiteral(encoded_doc) + ", " + SqlQuote(json_text) + ", " + SqlQuote(top_hlc) +
              ", " + std::to_string(NowMs()) + ");");
    if (!st.ok()) {
      Exec("ROLLBACK;");
      return st;
    }
    st = Exec("COMMIT;");
    SetBytesUsed(FileSize(path_));
    return st;
  }

  StatusOr<std::string> Get(const std::string& collection, const std::string& key) override {
    std::lock_guard<std::mutex> lock(mu_);
    DuckResult result;
    if (duckdb_query(conn_,
                     ("SELECT doc FROM rows WHERE collection = " + SqlQuote(collection) +
                      " AND key = " + SqlQuote(key) + ";")
                         .c_str(),
                     result.get()) != DuckDBSuccess) {
      return Status::IOError("duckdb: select failed");
    }
    if (result.rows() == 0) return Status::NotFound("no such key: " + key);
    return result.Blob(0, 0);
  }

  std::vector<EngineRow> Scan(const std::string& collection, const std::string& start_key,
                               size_t limit) override {
    std::vector<EngineRow> out;
    std::lock_guard<std::mutex> lock(mu_);
    std::string sql = "SELECT key, doc FROM rows WHERE collection = " + SqlQuote(collection) +
                      " AND key >= " + SqlQuote(start_key) + " ORDER BY key ASC";
    if (limit != 0) sql += " LIMIT " + std::to_string(limit);
    sql += ";";
    DuckResult result;
    if (duckdb_query(conn_, sql.c_str(), result.get()) != DuckDBSuccess) return out;
    const idx_t rows = result.rows();
    out.reserve(rows);
    for (idx_t i = 0; i < rows; ++i) out.emplace_back(result.Text(0, i), result.Blob(1, i));
    return out;
  }

  std::string Checksum(const std::string& collection) override {
    std::lock_guard<std::mutex> lock(mu_);
    DuckResult result;
    if (duckdb_query(conn_,
                     ("SELECT key, top_hlc FROM rows WHERE collection = " + SqlQuote(collection) +
                      " AND json <> '{}' ORDER BY key ASC;")
                         .c_str(),
                     result.get()) != DuckDBSuccess) {
      return std::string();
    }
    std::vector<std::string> fingerprints;
    const idx_t rows = result.rows();
    fingerprints.reserve(rows);
    for (idx_t i = 0; i < rows; ++i) {
      fingerprints.push_back(result.Text(0, i) + "=" + result.Text(1, i));
    }
    std::sort(fingerprints.begin(), fingerprints.end());
    std::string joined;
    for (const std::string& f : fingerprints) {
      joined += f;
      joined += '\n';
    }
    return HexEncode(crypto::Sha256(joined));
  }

  Status Verify() override {
    std::lock_guard<std::mutex> lock(mu_);
    DuckResult result;
    if (duckdb_query(conn_, "SELECT collection, key, doc, json FROM rows;", result.get()) !=
        DuckDBSuccess) {
      return Status::IOError("duckdb: verify scan failed");
    }
    const idx_t rows = result.rows();
    for (idx_t i = 0; i < rows; ++i) {
      const std::string collection = result.Text(0, i);
      const std::string key = result.Text(1, i);
      const std::string doc = result.Blob(2, i);
      const std::string json = result.Text(3, i);
      try {
        if (DecodeDocument(doc).ToJson().Dump() != json) {
          return Status::Corruption("duckdb: derived json disagrees with the blob for " + collection +
                                     "/" + key);
        }
      } catch (const std::exception& e) {
        return Status::Corruption("duckdb: undecodable blob for " + collection + "/" + key + ": " +
                                   e.what());
      }
    }
    return Status::OK();
  }

  Status Flush() override {
    std::lock_guard<std::mutex> lock(mu_);
    Status st = Exec("CHECKPOINT;");
    SetBytesUsed(FileSize(path_));
    return st;
  }

  std::vector<std::string> ListCollections() const override {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lock(mu_);
    DuckResult result;
    if (duckdb_query(conn_, "SELECT DISTINCT collection FROM rows ORDER BY collection ASC;",
                     result.get()) != DuckDBSuccess) {
      return out;
    }
    const idx_t rows = result.rows();
    out.reserve(rows);
    for (idx_t i = 0; i < rows; ++i) out.push_back(result.Text(0, i));
    return out;
  }

  // -- analytical surface, reached through StorageRouter::Backend("duckdb") --
  // Runs an arbitrary read-only SQL statement against the `rows` table and
  // returns the result as text rows. Write statements are rejected: the CRDT
  // blob column is the replication authority, and a SQL UPDATE that bypassed
  // the ledger would produce a row no peer could ever converge on.
  StatusOr<std::vector<std::vector<std::string>>> Analytical(const std::string& sql) {
    std::string upper;
    upper.reserve(sql.size());
    for (char c : sql) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    for (const char* forbidden : {"INSERT", "UPDATE", "DELETE", "DROP", "ALTER", "CREATE", "ATTACH"}) {
      if (upper.find(forbidden) != std::string::npos) {
        return Status::InvalidArgument(
            "duckdb: only read-only statements are accepted here; writes must go through the ledger");
      }
    }
    std::lock_guard<std::mutex> lock(mu_);
    DuckResult result;
    if (duckdb_query(conn_, sql.c_str(), result.get()) != DuckDBSuccess) {
      return Status::InvalidArgument("duckdb: query failed");
    }
    std::vector<std::vector<std::string>> out;
    const idx_t rows = result.rows();
    const idx_t cols = duckdb_column_count(result.get());
    out.reserve(rows);
    for (idx_t r = 0; r < rows; ++r) {
      std::vector<std::string> row;
      row.reserve(cols);
      for (idx_t c = 0; c < cols; ++c) row.push_back(result.Text(c, r));
      out.push_back(std::move(row));
    }
    return out;
  }

 private:
  Status Exec(const std::string& sql) {
    DuckResult result;
    if (duckdb_query(conn_, sql.c_str(), result.get()) != DuckDBSuccess) {
      const char* err = duckdb_result_error(result.get());
      return Status::IOError(std::string("duckdb: ") + (err ? err : "query failed") +
                             " (while running: " + sql + ")");
    }
    return Status::OK();
  }

  std::string dir_;
  std::string path_;
  duckdb_database db_ = nullptr;
  duckdb_connection conn_ = nullptr;
  mutable std::mutex mu_;
};

std::unique_ptr<EngineBackend> MakeDuckDbBackend() { return std::make_unique<DuckDbBackend>(); }

}  // namespace desentry

#endif  // DESENTRY_WITH_DUCKDB
