// SQLite adapter -- the relational/metadata core of the OSS backbone, plus
// the JSON1 and FTS5 surfaces. Compiled only under DESENTRY_WITH_SQLITE.
//
// Schema (one table, two virtual companions):
//
//   CREATE TABLE rows(
//     collection TEXT NOT NULL,
//     key        TEXT NOT NULL,
//     doc        BLOB NOT NULL,   -- CrdtValue::Encode() bytes, verbatim
//     json       TEXT,            -- doc materialised via ToJson(), for JSON1
//     top_hlc    TEXT NOT NULL,   -- CrdtValue::MaxTimestamp().ToString()
//     updated_ms INTEGER NOT NULL,
//     PRIMARY KEY(collection, key)) WITHOUT ROWID;
//
//   CREATE VIRTUAL TABLE rows_fts USING fts5(collection, key, body,
//                                            content='', tokenize='unicode61');
//
// The CRDT bytes are the authority; `json` and the FTS index are derived
// projections refreshed on every write. That ordering matters -- if the two
// ever disagree, the blob wins and Verify() reports it, rather than a query
// quietly returning a stale materialisation.
//
// Checksum() is computed with a single SQL statement over (key, top_hlc)
// instead of a full scan, and the unit test asserts it equals
// FingerprintRows() over the same data -- the invariant that keeps /_brain
// parity meaningful across backends.

#include "desentry/storage/engines/vendored_backends.h"

#ifdef DESENTRY_WITH_SQLITE

#include <sqlite3.h>

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

class SqliteStatement {
 public:
  SqliteStatement(sqlite3* db, const char* sql) {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
      DSN_LOG_ERROR("sqlite", "prepare failed: " << sqlite3_errmsg(db) << " for: " << sql);
      stmt_ = nullptr;
    }
  }
  ~SqliteStatement() {
    if (stmt_ != nullptr) sqlite3_finalize(stmt_);
  }
  SqliteStatement(const SqliteStatement&) = delete;
  SqliteStatement& operator=(const SqliteStatement&) = delete;

  bool ok() const { return stmt_ != nullptr; }
  sqlite3_stmt* get() { return stmt_; }

 private:
  sqlite3_stmt* stmt_ = nullptr;
};

Status Exec(sqlite3* db, const char* sql) {
  char* err = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
    std::string message = err ? err : "unknown sqlite error";
    sqlite3_free(err);
    return Status::IOError("sqlite: " + message + " (while running: " + sql + ")");
  }
  return Status::OK();
}

std::string TextColumn(sqlite3_stmt* stmt, int index) {
  const unsigned char* text = sqlite3_column_text(stmt, index);
  int len = sqlite3_column_bytes(stmt, index);
  return text == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(text), len);
}

std::string BlobColumn(sqlite3_stmt* stmt, int index) {
  const void* blob = sqlite3_column_blob(stmt, index);
  int len = sqlite3_column_bytes(stmt, index);
  return blob == nullptr ? std::string() : std::string(static_cast<const char*>(blob), len);
}

// Flattens every scalar leaf of a document into one searchable string. FTS5
// indexes text, not JSON, so the projection has to be explicit -- and being
// explicit is why "why didn't my document match?" is answerable.
void FlattenForFts(const JsonValue& value, std::string* out) {
  switch (value.type()) {
    case JsonType::kString:
      *out += value.AsString();
      out->push_back(' ');
      break;
    case JsonType::kInt:
    case JsonType::kDouble:
    case JsonType::kBool:
      *out += value.Dump();
      out->push_back(' ');
      break;
    case JsonType::kArray:
      for (const JsonValue& item : value.AsArray()) FlattenForFts(item, out);
      break;
    case JsonType::kObject:
      for (const auto& [field, item] : value.AsObject()) {
        *out += field;
        out->push_back(' ');
        FlattenForFts(item, out);
      }
      break;
    case JsonType::kNull:
      break;
  }
}

}  // namespace

class SqliteBackend : public BaseBackend {
 public:
  ~SqliteBackend() override {
    if (db_ != nullptr) sqlite3_close(db_);
  }

  std::string Name() const override { return "sqlite"; }

  Status Open(const std::string& data_dir, uint64_t quota_mb) override {
    dir_ = data_dir + "/sqlite";
    if (!MakeDirs(dir_)) return Status::IOError("cannot create backend directory: " + dir_);
    path_ = dir_ + "/desentry.sqlite3";

    if (sqlite3_open(path_.c_str(), &db_) != SQLITE_OK) {
      std::string message = db_ ? sqlite3_errmsg(db_) : "unknown";
      return Status::IOError("sqlite: cannot open " + path_ + ": " + message);
    }
    // WAL + NORMAL matches this engine's durability story: the hash-chained
    // ledger one layer up is already fsync'd before we are called, so paying
    // for a second fsync per row here buys nothing.
    Status st = Exec(db_, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;");
    if (!st.ok()) return st;
    st = Exec(db_,
              "CREATE TABLE IF NOT EXISTS rows("
              " collection TEXT NOT NULL,"
              " key TEXT NOT NULL,"
              " doc BLOB NOT NULL,"
              " json TEXT,"
              " top_hlc TEXT NOT NULL,"
              " updated_ms INTEGER NOT NULL,"
              " PRIMARY KEY(collection, key)) WITHOUT ROWID;");
    if (!st.ok()) return st;
    st = Exec(db_, "CREATE INDEX IF NOT EXISTS rows_by_collection ON rows(collection, key);");
    if (!st.ok()) return st;

    // FTS5 is a compile-time option in some SQLite builds. Its absence
    // degrades full-text search, not storage, so it is reported and skipped
    // rather than made fatal.
    fts_available_ = Exec(db_,
                          "CREATE VIRTUAL TABLE IF NOT EXISTS rows_fts USING fts5("
                          " collection, key, body, tokenize='unicode61');")
                         .ok();
    if (!fts_available_) {
      DSN_LOG_WARN("sqlite", "FTS5 unavailable in this SQLite build; full-text search disabled");
    }

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
    std::string fts_body;
    try {
      CrdtValue doc = DecodeDocument(encoded_doc);
      JsonValue json = doc.ToJson();
      json_text = json.Dump();
      top_hlc = doc.MaxTimestamp().ToString();
      FlattenForFts(json, &fts_body);
    } catch (const std::exception& e) {
      return Status::Corruption(std::string("sqlite: undecodable document: ") + e.what());
    }

    std::lock_guard<std::mutex> lock(mu_);
    SqliteStatement stmt(db_,
                         "INSERT INTO rows(collection, key, doc, json, top_hlc, updated_ms)"
                         " VALUES(?1, ?2, ?3, ?4, ?5, ?6)"
                         " ON CONFLICT(collection, key) DO UPDATE SET"
                         " doc=excluded.doc, json=excluded.json,"
                         " top_hlc=excluded.top_hlc, updated_ms=excluded.updated_ms;");
    if (!stmt.ok()) return Status::Internal("sqlite: cannot prepare upsert");
    sqlite3_bind_text(stmt.get(), 1, collection.data(), static_cast<int>(collection.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt.get(), 3, encoded_doc.data(), static_cast<int>(encoded_doc.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 4, json_text.data(), static_cast<int>(json_text.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 5, top_hlc.data(), static_cast<int>(top_hlc.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.get(), 6, NowMs());
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
      return Status::IOError(std::string("sqlite: upsert failed: ") + sqlite3_errmsg(db_));
    }

    if (fts_available_) {
      SqliteStatement del(db_, "DELETE FROM rows_fts WHERE collection = ?1 AND key = ?2;");
      if (del.ok()) {
        sqlite3_bind_text(del.get(), 1, collection.data(), static_cast<int>(collection.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(del.get(), 2, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
        sqlite3_step(del.get());
      }
      SqliteStatement ins(db_, "INSERT INTO rows_fts(collection, key, body) VALUES(?1, ?2, ?3);");
      if (ins.ok()) {
        sqlite3_bind_text(ins.get(), 1, collection.data(), static_cast<int>(collection.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(ins.get(), 2, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(ins.get(), 3, fts_body.data(), static_cast<int>(fts_body.size()), SQLITE_TRANSIENT);
        sqlite3_step(ins.get());
      }
    }

    SetBytesUsed(FileSize(path_));
    return Status::OK();
  }

  StatusOr<std::string> Get(const std::string& collection, const std::string& key) override {
    std::lock_guard<std::mutex> lock(mu_);
    SqliteStatement stmt(db_, "SELECT doc FROM rows WHERE collection = ?1 AND key = ?2;");
    if (!stmt.ok()) return Status::Internal("sqlite: cannot prepare select");
    sqlite3_bind_text(stmt.get(), 1, collection.data(), static_cast<int>(collection.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) return Status::NotFound("no such key: " + key);
    return BlobColumn(stmt.get(), 0);
  }

  std::vector<EngineRow> Scan(const std::string& collection, const std::string& start_key,
                               size_t limit) override {
    std::vector<EngineRow> out;
    std::lock_guard<std::mutex> lock(mu_);
    SqliteStatement stmt(db_,
                         "SELECT key, doc FROM rows WHERE collection = ?1 AND key >= ?2"
                         " ORDER BY key ASC;");
    if (!stmt.ok()) return out;
    sqlite3_bind_text(stmt.get(), 1, collection.data(), static_cast<int>(collection.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, start_key.data(), static_cast<int>(start_key.size()), SQLITE_TRANSIENT);
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
      out.emplace_back(TextColumn(stmt.get(), 0), BlobColumn(stmt.get(), 1));
      if (limit != 0 && out.size() >= limit) break;
    }
    return out;
  }

  std::string Checksum(const std::string& collection) override {
    // Same fingerprint rule as FingerprintRows(), computed in SQL: the
    // sorted "key=top_hlc\n" list, hashed. Tombstoned documents are
    // excluded exactly as they are there -- a CRDT tombstone materialises
    // to an empty object, which is what json='{}' detects.
    std::lock_guard<std::mutex> lock(mu_);
    SqliteStatement stmt(db_,
                         "SELECT key, top_hlc FROM rows WHERE collection = ?1 AND json <> '{}'"
                         " ORDER BY key ASC;");
    if (!stmt.ok()) return std::string();
    sqlite3_bind_text(stmt.get(), 1, collection.data(), static_cast<int>(collection.size()), SQLITE_TRANSIENT);
    std::vector<std::string> fingerprints;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
      fingerprints.push_back(TextColumn(stmt.get(), 0) + "=" + TextColumn(stmt.get(), 1));
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
    Status st = Exec(db_, "PRAGMA quick_check;");
    if (!st.ok()) return st;
    // The blob is the authority; assert the derived json column still
    // matches it rather than trusting that every write path refreshed it.
    SqliteStatement stmt(db_, "SELECT collection, key, doc, json FROM rows;");
    if (!stmt.ok()) return Status::Internal("sqlite: cannot prepare verify scan");
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
      const std::string collection = TextColumn(stmt.get(), 0);
      const std::string key = TextColumn(stmt.get(), 1);
      const std::string doc = BlobColumn(stmt.get(), 2);
      const std::string json = TextColumn(stmt.get(), 3);
      try {
        if (DecodeDocument(doc).ToJson().Dump() != json) {
          return Status::Corruption("sqlite: derived json disagrees with the document blob for " +
                                     collection + "/" + key);
        }
      } catch (const std::exception& e) {
        return Status::Corruption("sqlite: undecodable blob for " + collection + "/" + key + ": " + e.what());
      }
    }
    return Status::OK();
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
    SqliteStatement stmt(db_, "SELECT DISTINCT collection FROM rows ORDER BY collection ASC;");
    if (!stmt.ok()) return out;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) out.push_back(TextColumn(stmt.get(), 0));
    return out;
  }

  // -- SQL surface, reached through StorageRouter::Backend("sqlite") -------
  // Full-text search over the FTS5 projection.
  StatusOr<std::vector<std::string>> FullTextSearch(const std::string& collection,
                                                     const std::string& query, size_t limit) {
    if (!fts_available_) return Status::InvalidArgument("FTS5 is not available in this SQLite build");
    std::lock_guard<std::mutex> lock(mu_);
    SqliteStatement stmt(db_,
                         "SELECT key FROM rows_fts WHERE collection = ?1 AND rows_fts MATCH ?2"
                         " ORDER BY rank LIMIT ?3;");
    if (!stmt.ok()) return Status::Internal("sqlite: cannot prepare FTS query");
    sqlite3_bind_text(stmt.get(), 1, collection.data(), static_cast<int>(collection.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, query.data(), static_cast<int>(query.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.get(), 3, static_cast<sqlite3_int64>(limit == 0 ? 100 : limit));
    std::vector<std::string> keys;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) keys.push_back(TextColumn(stmt.get(), 0));
    return keys;
  }

  // JSON1 path filter, e.g. JsonQuery("users", "$.role", "admin").
  StatusOr<std::vector<std::string>> JsonQuery(const std::string& collection,
                                                const std::string& json_path,
                                                const std::string& equals) {
    std::lock_guard<std::mutex> lock(mu_);
    SqliteStatement stmt(db_,
                         "SELECT key FROM rows WHERE collection = ?1"
                         " AND json_extract(json, ?2) = ?3 ORDER BY key ASC;");
    if (!stmt.ok()) return Status::InvalidArgument("JSON1 is not available in this SQLite build");
    sqlite3_bind_text(stmt.get(), 1, collection.data(), static_cast<int>(collection.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, json_path.data(), static_cast<int>(json_path.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, equals.data(), static_cast<int>(equals.size()), SQLITE_TRANSIENT);
    std::vector<std::string> keys;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) keys.push_back(TextColumn(stmt.get(), 0));
    return keys;
  }

  sqlite3* handle() { return db_; }

 private:
  std::string dir_;
  std::string path_;
  sqlite3* db_ = nullptr;
  bool fts_available_ = false;
  mutable std::mutex mu_;
};

std::unique_ptr<EngineBackend> MakeSqliteBackend() { return std::make_unique<SqliteBackend>(); }

}  // namespace desentry

#endif  // DESENTRY_WITH_SQLITE
