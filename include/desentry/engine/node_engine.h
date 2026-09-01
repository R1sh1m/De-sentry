#pragma once
// The CRDT-aware glue layer: everything above this point (API server, P2P
// network layer) talks to a NodeEngine, never to StorageEngine or
// CrdtValue::Merge directly. NodeEngine is what turns "an application sent
// me this JSON" or "a peer sent me this replicated write" into the right
// sequence of CRDT operations and physical storage calls, so both entry
// points share exactly one code path for "how does a write actually get
// applied" -- the same reason a real database has a single execution
// engine underneath both its client protocol and its replication stream.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "desentry/common/status.h"
#include "desentry/crdt/document.h"
#include "desentry/crdt/hlc.h"
#include "desentry/net/identity.h"
#include "desentry/storage/storage_engine.h"

namespace desentry {

struct DigestEntryOut {
  std::string key;
  HLCTimestamp top_ts;
};

// A compact, cheap-to-compute per-collection fingerprint -- the "brain
// file" concept: enough for a peer or an operator to tell at a glance
// whether two nodes' copies of a collection have converged, without
// transferring the collection itself. See api routes.cpp's GET /_brain.
struct CollectionSummary {
  std::string name;
  uint64_t document_count = 0;  // live (non-tombstoned) document count
  std::string checksum_hex;     // SHA-256 over every live key + its CRDT top timestamp, sorted
};

class NodeEngine {
 public:
  struct Options {
    std::string data_dir = "./data";
    size_t buffer_pool_pages = 1024;
  };

  static StatusOr<std::unique_ptr<NodeEngine>> Open(const Options& options);

  const NodeIdentity& identity() const { return *identity_; }
  StorageEngine& storage() { return *storage_; }

  // Called after every successful *local* write (from the API layer) with
  // the collection/key/already-CRDT-encoded bytes, so the network layer
  // can eagerly push it to connected peers. Set once by NetworkManager
  // after construction -- see the header comment on why this is a callback
  // rather than a direct dependency (avoids a NodeEngine <-> NetworkManager
  // circular include).
  void SetLocalWriteHook(std::function<void(const std::string&, const std::string&, const std::string&)> hook) {
    on_local_write_ = std::move(hook);
  }

  // -- local application writes (API layer) --------------------------------
  Status PutDocument(const std::string& collection, const std::string& key, const JsonValue& new_json);
  Status DeleteDocument(const std::string& collection, const std::string& key);
  StatusOr<JsonValue> GetDocument(const std::string& collection, const std::string& key);
  std::vector<std::pair<std::string, JsonValue>> ListDocuments(const std::string& collection,
                                                                 const std::string& start_key, size_t limit);

  // -- replication (network layer) ------------------------------------------
  // Merges a remote peer's encoded document into local state. Folds the
  // remote HLC into our clock for causality (HLC.Observe) but does *not*
  // re-invoke the local-write hook -- see node_engine.cpp for why (avoids
  // rebroadcast loops; documented MVP scope limit on multi-hop relay).
  Status MergeRemote(const std::string& collection, const std::string& key, const std::string& remote_encoded_doc);

  std::vector<DigestEntryOut> LocalDigest(const std::string& collection);
  std::vector<std::string> ListCollections();

  // Cheap per-collection fingerprint for the brain-file-style status
  // endpoint (GET /_brain) -- see CollectionSummary above.
  CollectionSummary Summarize(const std::string& collection);

  // -- hash-chained audit ledger -------------------------------------------
  WriteAheadLog::LedgerTip LedgerTip() const { return storage_->LedgerTip(); }
  WriteAheadLog::VerifyResult VerifyLedger() { return storage_->VerifyLedger(); }
  StatusOr<std::vector<WalRecord>> LedgerEntries(lsn_t from, lsn_t to) { return storage_->LedgerEntries(from, to); }

  // Signs the current ledger chain tip (entry_id + entry_hash) with this
  // node's persistent Ed25519 identity key. A pure SHA-256 hash chain only
  // proves internal self-consistency -- anyone could fabricate a
  // *different* but internally-consistent chain from scratch and claim it.
  // Signing the tip binds it to node_id (== derived from the same public
  // key, net/identity.h), so a peer that already trusts this node's
  // identity can verify the signature and know this exact node attests to
  // this exact tip. This directly closes a gap our design-comparison
  // process flagged as future work in an earlier draft of this ledger
  // concept -- see ARCHITECTURE.md's ledger section.
  std::string SignLedgerTip() const;

  // Raw (still-encoded, still tombstone-carrying) document bytes for a key
  // -- what the gossip/broadcast layer actually ships on the wire, as
  // opposed to GetDocument()'s materialized, tombstone-free JsonValue.
  StatusOr<std::string> GetRawEncoded(const std::string& collection, const std::string& key);

 private:
  NodeEngine() = default;

  std::unique_ptr<StorageEngine> storage_;
  std::unique_ptr<HybridLogicalClock> clock_;
  std::unique_ptr<NodeIdentity> identity_;
  std::function<void(const std::string&, const std::string&, const std::string&)> on_local_write_;
};

}  // namespace desentry
