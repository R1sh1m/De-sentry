#include "desentry/storage/storage_engine.h"

#include <algorithm>

#include "desentry/common/logger.h"
#include "desentry/common/platform.h"

namespace desentry {

namespace {
Status EnsureDir(const std::string& path) {
  if (!MakeDirs(path)) return Status::IOError("cannot create directory: " + path);
  return Status::OK();
}
}  // namespace

StatusOr<std::unique_ptr<StorageEngine>> StorageEngine::Open(const Options& options) {
  Status dir_st = EnsureDir(options.data_dir);
  if (!dir_st.ok()) return dir_st;

  std::unique_ptr<StorageEngine> engine(new StorageEngine());
  engine->data_dir_ = options.data_dir;
  engine->quota_bytes_ = options.quota_mb * 1024ull * 1024ull;

  auto wal_or = WriteAheadLog::Open(options.data_dir + "/desentry.wal");
  if (!wal_or.ok()) return wal_or.status();
  engine->wal_ = std::move(wal_or.value());

  auto cat_or = Catalog::Open(options.data_dir + "/catalog.json");
  if (!cat_or.ok()) return cat_or.status();
  engine->catalog_ = std::move(cat_or.value());

  StorageRouter::Options router_opts;
  router_opts.data_dir = options.data_dir;
  router_opts.quota_mb = options.quota_mb;
  router_opts.db_share_pct = options.db_share_pct;
  router_opts.engines = options.engines;
  router_opts.default_engine = options.default_engine;
  router_opts.catalog = engine->catalog_.get();
  router_opts.node_id = options.node_id;
  auto router_or = StorageRouter::Open(router_opts);
  if (!router_or.ok()) return router_or.status();
  engine->router_ = std::move(router_or.value());

  Status replay_st = engine->ReplayLedger();
  if (!replay_st.ok()) return replay_st;

  if (engine->wal_->migrated_from_v1()) {
    // The v1 -> v2 ledger migration re-derives the chain (see wal.cpp). That
    // discontinuity is recorded on disk so an auditor comparing this node
    // against a replica that has not yet upgraded can see exactly where the
    // hashes stop matching, and why.
    JsonValue::Object record;
    record.emplace_back("migrated_at_ms", JsonValue(NowMs()));
    record.emplace_back("pre_migration_tip_hash",
                        JsonValue(engine->wal_->pre_migration_tip_hash()));
    record.emplace_back("note",
                        JsonValue("ledger upgraded from the v1 record format; entry hashes were "
                                  "re-derived because the hashed content now covers key_hash, HLC "
                                  "and origin fields that v1 records did not carry"));
    std::ofstream f(options.data_dir + "/ledger_migration.json", std::ios::trunc);
    if (f.is_open()) f << JsonValue(std::move(record)).Dump();
  }

  return engine;
}

Status StorageEngine::ReplayLedger() {
  auto records_or = wal_->ReadAll();
  if (!records_or.ok()) return records_or.status();
  const std::vector<WalRecord>& records = records_or.value();
  if (records.empty()) return Status::OK();

  DSN_LOG_INFO("storage", "replaying " << records.size() << " ledger record(s)...");
  size_t applied = 0;
  for (const WalRecord& rec : records) {
    // Only document mutations rebuild state. Transit and checkpoint entries
    // are ledger bookkeeping -- replaying a TRANSIT_INTENT as a document
    // write would materialise an envelope as if it were user data.
    if (rec.type != WalRecordType::kPut) continue;
    // Replay goes through the router, not the ledger: appending again would
    // duplicate the entry and break the chain's relationship to history.
    Status st = router_->Put(rec.collection, rec.key, rec.document_bytes);
    if (!st.ok()) {
      // One unreplayable record must not abort recovery of everything after
      // it -- that would turn a single bad row into total data loss. It is
      // logged loudly and skipped, and VerifyAll() will report the resulting
      // gap between the ledger and the materialised state.
      DSN_LOG_ERROR("storage", "ledger replay skipped entry " << rec.lsn << " (" << rec.collection
                                                               << "/" << rec.key
                                                               << "): " << st.message());
      continue;
    }
    ++applied;
  }
  Status flush_st = router_->Flush();
  if (!flush_st.ok()) return flush_st;
  DSN_LOG_INFO("storage", "ledger replay complete (" << applied << " document write(s) applied)");
  return Status::OK();
}

void StorageEngine::SetLedgerOrigin(std::string node_id, WriteAheadLog::Signer signer) {
  wal_->SetOrigin(std::move(node_id), std::move(signer));
}

void StorageEngine::SetTipObserver(std::function<void(lsn_t)> observer) {
  std::lock_guard<std::mutex> lock(observer_mu_);
  tip_observer_ = std::move(observer);
}

std::vector<std::string> StorageEngine::ListCollections() const { return catalog_->ListCollections(); }

Status StorageEngine::EnsureCollection(const std::string& collection) {
  if (catalog_->HasCollection(collection)) return Status::OK();
  Status st = catalog_->UpsertRootPageId(collection, kInvalidPageId);
  if (!st.ok()) return st;
  return Status::OK();
}

StorageEngine::QuotaStatus StorageEngine::Quota() const {
  QuotaStatus status;
  status.limit_bytes = quota_bytes_;
  status.ledger_bytes = FileSize(data_dir_ + "/desentry.wal");
  status.used_bytes = router_->QuotaUse() + status.ledger_bytes;
  if (quota_bytes_ > 0) {
    status.used_fraction = static_cast<double>(status.used_bytes) / static_cast<double>(quota_bytes_);
    status.over_limit = status.used_bytes > quota_bytes_;
  }
  return status;
}

Status StorageEngine::PutRaw(const std::string& collection, const std::string& key,
                              const std::string& encoded_doc) {
  // Node-level quota check, before anything is written. The backend checks
  // its own share too; this is the number the user actually set, and it
  // covers the ledger's own growth, which no single backend can see.
  if (quota_bytes_ > 0) {
    const QuotaStatus quota = Quota();
    // The ledger append itself costs roughly the record size, so charge the
    // write twice: once for the log entry, once for the materialised row.
    const uint64_t projected = quota.used_bytes + 2 * (key.size() + encoded_doc.size());
    if (projected > quota_bytes_) {
      return Status::OutOfSpace("node quota exceeded: " + std::to_string(projected) + " > " +
                                 std::to_string(quota_bytes_) +
                                 " bytes (raise the node's quota in the app, or free space)");
    }
  }

  // Durability point: ledger append + fsync strictly before the physical
  // write. The reverse order would let a crash leave a materialised row the
  // ledger never attested to, which no peer could then verify.
  WriteAheadLog::AppendOptions options;
  auto lsn_or = wal_->Append(WalRecordType::kPut, collection, key, encoded_doc, options);
  if (!lsn_or.ok()) return lsn_or.status();

  Status st = router_->Put(collection, key, encoded_doc);
  if (!st.ok()) return st;

  EnsureCollection(collection);
  {
    std::lock_guard<std::mutex> lock(observer_mu_);
    if (tip_observer_) tip_observer_(lsn_or.value());
  }
  return Status::OK();
}

StatusOr<lsn_t> StorageEngine::AppendLedgerOp(WalRecordType type, const std::string& collection,
                                               const std::string& key, const std::string& payload,
                                               const HLCTimestamp& hlc) {
  WriteAheadLog::AppendOptions options;
  options.hlc = hlc;
  auto lsn_or = wal_->Append(type, collection, key, payload, options);
  if (!lsn_or.ok()) return lsn_or.status();
  {
    std::lock_guard<std::mutex> lock(observer_mu_);
    if (tip_observer_) tip_observer_(lsn_or.value());
  }
  return lsn_or.value();
}

StatusOr<std::string> StorageEngine::GetRaw(const std::string& collection, const std::string& key) {
  return router_->Get(collection, key);
}

std::vector<std::pair<std::string, std::string>> StorageEngine::Scan(const std::string& collection,
                                                                       const std::string& start_key,
                                                                       size_t limit) {
  return router_->Scan(collection, start_key, limit);
}

void StorageEngine::Checkpoint() {
  Status st = router_->Flush();
  if (!st.ok()) DSN_LOG_ERROR("storage", "checkpoint flush failed: " << st.message());
  catalog_->Save();
}

StatusOr<std::vector<WalRecord>> StorageEngine::LedgerEntries(lsn_t from, lsn_t to) {
  auto records_or = wal_->ReadAll();
  if (!records_or.ok()) return records_or.status();
  std::vector<WalRecord> out;
  for (const WalRecord& rec : records_or.value()) {
    if (rec.lsn >= from && rec.lsn <= to) out.push_back(rec);
  }
  return out;
}

StorageEngine::VerifyReport StorageEngine::VerifyAll(
    const WriteAheadLog::SignatureVerifier& verify_signature) {
  VerifyReport report;
  report.ledger = wal_->VerifyChain(verify_signature);
  Status backends = router_->Verify();
  report.backends_ok = backends.ok();
  if (!backends.ok()) report.backend_failure = backends.message();
  return report;
}

}  // namespace desentry
