#pragma once
// Top-level local storage engine: the thing a single peer's node_engine
// talks to. It owns three things and delegates everything physical:
//
//   * the **hash-chained ledger** (storage/wal.h) -- the durability point
//     and the replication log, fsync'd before any write is acknowledged;
//   * the **catalog** (storage/catalog.h) -- collection metadata, schemas,
//     ACLs, engine bindings;
//   * the **storage router** (storage/router.h) -- which dispatches the
//     physical write to whichever backend the collection is bound to.
//
// v1 held the DiskManager, BufferPoolManager and B+Trees here directly. In
// v2 those live inside the `kv` backend, which is still the default and
// still the exact same code path -- the move is what lets a collection be
// bound to a columnar, time-series, vector or graph layout without any
// caller knowing. The ordering guarantee is unchanged and is the load-bearing
// invariant: **ledger append and fsync happen before the router is called**,
// so the ledger is always at least as complete as the materialised state,
// never behind it.
//
// The interface stays physical and CRDT-unaware: PutRaw/GetRaw/Scan move
// already-encoded document bytes (see storage/document_codec.h). CRDT
// semantics -- merging against the previous version, generating HLC
// timestamps, turning a delete into a tombstoning update -- live one layer
// up in engine/node_engine.h. That split keeps this layer testable
// independently of the distributed-systems layer above it, the same way
// InnoDB does not know what SQL is.
//
// Crash recovery: on Open(), the ledger is replayed from the beginning
// through the router's Put path (idempotent -- every mutation is a CRDT
// merge). Known limitation, stated rather than glossed over: this protects
// documents that were logged but not yet flushed; it does not use physical
// page checksums to repair a page torn mid-write during a B+Tree structural
// split. The segment-based backends do checksum every page they write
// (storage/segment_store.h), so that gap is now specific to the `kv`
// backend's B+Tree rather than engine-wide.

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "desentry/common/status.h"
#include "desentry/storage/catalog.h"
#include "desentry/storage/router.h"
#include "desentry/storage/wal.h"

namespace desentry {

class StorageEngine {
 public:
  struct Options {
    std::string data_dir = "./data";
    size_t buffer_pool_pages = 1024;

    // -- v2 --------------------------------------------------------------
    uint64_t quota_mb = 0;        // 0 == unlimited
    uint32_t db_share_pct = 60;   // NodeConfig::quota_split.db_pct
    std::vector<std::string> engines{"kv"};
    std::string default_engine = "kv";
    std::string node_id;          // stamped into ledger entries and the index
  };

  static StatusOr<std::unique_ptr<StorageEngine>> Open(const Options& options);

  // Ensures a collection exists (idempotent).
  Status EnsureCollection(const std::string& collection);
  std::vector<std::string> ListCollections() const;
  Catalog& catalog() { return *catalog_; }
  StorageRouter& router() { return *router_; }

  // Installs the identity used to stamp and sign ledger entries. Called once
  // by NodeEngine after it has loaded the node's key.
  void SetLedgerOrigin(std::string node_id, WriteAheadLog::Signer signer);

  // Called after every ledger append with the new tip, so the live change
  // feed can wake its waiters. Set by NodeEngine; null is fine.
  void SetTipObserver(std::function<void(lsn_t)> observer);

  // Physical upsert: writes `encoded_doc` durably (ledger-fsync'd) and
  // hands it to the collection's backend.
  //
  // Quota is enforced here as well as inside each backend. Two layers is
  // deliberate rather than redundant: the backend knows its own on-disk
  // cost, while this layer knows the *node's* total budget across every
  // backend plus the ledger itself -- and it is the node-level number the
  // user set in the app.
  Status PutRaw(const std::string& collection, const std::string& key, const std::string& encoded_doc);

  // Same, but for a specific ledger operation type -- how the transit flow
  // records TRANSIT_INTENT / TRANSIT_CLAIMED without going through the
  // document write path.
  StatusOr<lsn_t> AppendLedgerOp(WalRecordType type, const std::string& collection,
                                  const std::string& key, const std::string& payload,
                                  const HLCTimestamp& hlc);

  StatusOr<std::string> GetRaw(const std::string& collection, const std::string& key);

  // Ordered scan for the collection listing / query endpoints.
  std::vector<std::pair<std::string, std::string>> Scan(const std::string& collection,
                                                          const std::string& start_key, size_t limit);

  void Checkpoint();  // flush every backend and the catalog to disk

  // -- quota ----------------------------------------------------------------
  struct QuotaStatus {
    uint64_t limit_bytes = 0;   // 0 == unlimited
    uint64_t used_bytes = 0;
    uint64_t ledger_bytes = 0;
    double used_fraction = 0.0;
    bool over_limit = false;
  };
  QuotaStatus Quota() const;

  // -- hash-chained audit ledger -------------------------------------------
  WriteAheadLog::LedgerTip LedgerTip() const { return wal_->Tip(); }
  WriteAheadLog::VerifyResult VerifyLedger(
      const WriteAheadLog::SignatureVerifier& verify_signature = nullptr) {
    return wal_->VerifyChain(verify_signature);
  }
  // Bounded range read for GET /_ledger/entries; callers (routes.cpp) clamp
  // [from, to] to a sane page size before calling.
  StatusOr<std::vector<WalRecord>> LedgerEntries(lsn_t from, lsn_t to);
  StatusOr<std::vector<WalRecord>> AllLedgerEntries() { return wal_->ReadAll(); }
  WriteAheadLog* wal() { return wal_.get(); }

  // Full integrity check: the ledger chain plus every backend's own
  // structures. This is what POST /_verify runs.
  struct VerifyReport {
    WriteAheadLog::VerifyResult ledger;
    bool backends_ok = true;
    std::string backend_failure;
  };
  VerifyReport VerifyAll(const WriteAheadLog::SignatureVerifier& verify_signature = nullptr);

 private:
  StorageEngine() = default;
  Status ReplayLedger();

  std::string data_dir_;
  uint64_t quota_bytes_ = 0;
  std::unique_ptr<WriteAheadLog> wal_;
  std::unique_ptr<Catalog> catalog_;
  std::unique_ptr<StorageRouter> router_;
  std::function<void(lsn_t)> tip_observer_;
  mutable std::mutex observer_mu_;
};

}  // namespace desentry
