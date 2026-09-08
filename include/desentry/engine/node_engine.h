#pragma once
// The CRDT-aware glue layer: everything above this point (API server, P2P
// network layer, supervisors) talks to a NodeEngine, never to StorageEngine
// or CrdtValue::Merge directly. NodeEngine is what turns "an application
// sent me this JSON" or "a peer sent me this replicated write" into the
// right sequence of CRDT operations and physical storage calls, so both
// entry points share exactly one code path for how a write is applied --
// the same reason a real database has one execution engine underneath both
// its client protocol and its replication stream.
//
// v2 adds four responsibilities, all of which have to live here because
// each needs both the CRDT layer and the identity layer in scope:
//
//   * **Access control.** Every read and write carries the *authenticated*
//     node_id of whoever asked (the local node for API calls, the handshake
//     identity for peer calls) and is checked against the collection's ACL.
//   * **The transit flow.** Holding bytes for an offline owner, recording
//     TRANSIT_INTENT, serving them back on return, and recording
//     TRANSIT_CLAIMED (docs/architecture-v2.md Sec 5).
//   * **Per-entry ledger signing and verification.** The engine installs the
//     signing callback on the ledger and supplies the verifier that maps a
//     node_id back to a public key.
//   * **The live change feed.** Every ledger append publishes its new tip so
//     GET /_changes long-polls wake immediately.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "desentry/common/status.h"
#include "desentry/crdt/document.h"
#include "desentry/crdt/hlc.h"
#include "desentry/ledger/change_feed.h"
#include "desentry/ledger/transit_store.h"
#include "desentry/net/identity.h"
#include "desentry/storage/storage_engine.h"

namespace desentry {

struct DigestEntryOut {
  std::string key;
  HLCTimestamp top_ts;
  // Short fingerprint of the stored bytes; see DigestEntry in
  // net/wire_protocol.h for why a timestamp is not enough on its own.
  std::string content_hash;
};

// A compact, cheap-to-compute per-collection fingerprint -- the "brain file"
// concept: enough for a peer or an operator to tell at a glance whether two
// nodes' copies of a collection have converged, without transferring the
// collection itself. See api routes.cpp's GET /_brain.
struct CollectionSummary {
  std::string name;
  uint64_t document_count = 0;  // live (non-tombstoned) document count
  std::string checksum_hex;     // SHA-256 over every live key + its CRDT top timestamp, sorted
  std::string engine;           // which router backend holds it
  bool is_private = false;
};

// Who is asking. Constructed from the local node's own id for API calls, and
// from the handshake-proven peer id for replication calls -- never from
// anything a caller can assert about itself.
struct Requestor {
  std::string node_id;
  bool is_local = false;  // came in over the loopback API rather than the mesh

  static Requestor Local(const std::string& node_id) { return Requestor{node_id, true}; }
  static Requestor Peer(const std::string& node_id) { return Requestor{node_id, false}; }
};

class NodeEngine {
 public:
  struct Options {
    std::string data_dir = "./data";
    size_t buffer_pool_pages = 1024;

    // -- v2 --------------------------------------------------------------
    uint64_t quota_mb = 0;
    uint32_t db_share_pct = 60;
    std::vector<std::string> engines{"kv"};
    std::string default_engine = "kv";
    uint32_t transit_ttl_seconds = 7 * 24 * 3600;
    uint32_t replication_factor = 3;
    bool supervisor = false;
  };

  static StatusOr<std::unique_ptr<NodeEngine>> Open(const Options& options);
  ~NodeEngine();

  const NodeIdentity& identity() const { return *identity_; }
  StorageEngine& storage() { return *storage_; }
  TransitStore& transit() { return *transit_; }
  ChangeFeed& changes() { return *changes_; }
  bool is_supervisor() const { return options_.supervisor; }
  uint32_t replication_factor() const { return options_.replication_factor; }
  Requestor SelfRequestor() const { return Requestor::Local(identity_->node_id()); }

  // Called after every successful *local* write with the
  // collection/key/already-CRDT-encoded bytes, so the network layer can
  // eagerly push it to connected peers. Set once by NetworkManager -- a
  // callback rather than a direct dependency, to avoid a NodeEngine <->
  // NetworkManager circular include.
  void SetLocalWriteHook(std::function<void(const std::string&, const std::string&, const std::string&)> hook) {
    on_local_write_ = std::move(hook);
  }

  // Supplied by NetworkManager so ledger verification can map an origin
  // node_id back to the public key the handshake proved. Without it,
  // VerifyLedger() still checks the hash chain but reports signatures as
  // merely present rather than valid.
  void SetPublicKeyResolver(std::function<std::string(const std::string&)> resolver) {
    resolve_public_key_ = std::move(resolver);
  }

  // -- local application writes (API layer) --------------------------------
  Status PutDocument(const std::string& collection, const std::string& key, const JsonValue& new_json,
                      const Requestor& who);
  Status DeleteDocument(const std::string& collection, const std::string& key, const Requestor& who);
  StatusOr<JsonValue> GetDocument(const std::string& collection, const std::string& key,
                                   const Requestor& who);
  std::vector<std::pair<std::string, JsonValue>> ListDocuments(const std::string& collection,
                                                                 const std::string& start_key,
                                                                 size_t limit, const Requestor& who);

  // -- replication (network layer) ------------------------------------------
  // Merges a remote peer's encoded document into local state. Folds the
  // remote HLC into our clock for causality (HLC::Observe). Does not
  // re-invoke the local-write hook: see node_engine.cpp for why (bounded
  // relay with message-id dedup happens in NetworkManager instead).
  Status MergeRemote(const std::string& collection, const std::string& key,
                      const std::string& remote_encoded_doc, const Requestor& who);

  std::vector<DigestEntryOut> LocalDigest(const std::string& collection);
  std::vector<std::string> ListCollections();
  // Collections `who` is allowed to read -- what a digest exchange with a
  // non-reader peer is restricted to.
  std::vector<std::string> ReadableCollections(const Requestor& who);
  bool CanRead(const std::string& collection, const Requestor& who) const;
  bool CanWrite(const std::string& collection, const Requestor& who) const;

  CollectionSummary Summarize(const std::string& collection);

  // -- transit (offline-owner flow) ----------------------------------------
  // Called on a replica when a write lands for a key whose placement targets
  // an unreachable owner: stores the bytes and appends TRANSIT_INTENT.
  Status HoldForOfflineOwner(const std::string& owner_node, const std::string& collection,
                              const std::string& key, const std::string& encoded_doc);

  // Envelopes this node is holding for `owner_node` -- what a kTransitQuery
  // from that peer is answered with.
  std::vector<TransitEnvelope> PendingTransitFor(const std::string& owner_node);

  // Called on the returning owner: applies a held document and appends
  // TRANSIT_CLAIMED to its own ledger.
  Status ApplyClaimedTransit(const std::string& collection, const std::string& key,
                              const std::string& encoded_doc);

  // Called on the holder when the owner reports having applied the bytes.
  // Appends TRANSIT_CLAIMED locally; the envelope itself is only released
  // once a quorum-verified checkpoint covers the pair (ledger/checkpoint.h).
  Status RecordRemoteClaim(const std::string& owner_node, const std::string& key_hash);

  // -- ledger ---------------------------------------------------------------
  WriteAheadLog::LedgerTip LedgerTip() const { return storage_->LedgerTip(); }
  WriteAheadLog::VerifyResult VerifyLedger();
  StorageEngine::VerifyReport VerifyEverything();
  StatusOr<std::vector<WalRecord>> LedgerEntries(lsn_t from, lsn_t to) {
    return storage_->LedgerEntries(from, to);
  }

  // Signs the current ledger chain tip (entry_id + entry_hash) with this
  // node's persistent Ed25519 identity key, binding the tip to a node_id a
  // peer already trusts. Per-entry signatures (storage/wal.h) make the same
  // guarantee for individual entries; this remains the cheap whole-history
  // attestation a peer checks first.
  std::string SignLedgerTip() const;
  static std::string LedgerTipMessage(lsn_t entry_id, const std::string& entry_hash);
  bool VerifyPeerTip(const std::string& node_id, lsn_t entry_id, const std::string& entry_hash,
                      const std::string& signature) const;

  // Raw (still-encoded, still tombstone-carrying) document bytes for a key --
  // what the gossip/broadcast layer ships, as opposed to GetDocument()'s
  // materialised, tombstone-free JsonValue.
  StatusOr<std::string> GetRawEncoded(const std::string& collection, const std::string& key);

  StorageEngine::QuotaStatus Quota() const { return storage_->Quota(); }
  HybridLogicalClock& clock() { return *clock_; }

 private:
  NodeEngine() = default;
  Status WriteThrough(const std::string& collection, const std::string& key,
                       const std::string& encoded_doc, bool notify_hook);

  Options options_;
  std::unique_ptr<StorageEngine> storage_;
  std::unique_ptr<HybridLogicalClock> clock_;
  std::unique_ptr<NodeIdentity> identity_;
  std::unique_ptr<TransitStore> transit_;
  std::unique_ptr<ChangeFeed> changes_;
  std::function<void(const std::string&, const std::string&, const std::string&)> on_local_write_;
  std::function<std::string(const std::string&)> resolve_public_key_;
};

}  // namespace desentry
