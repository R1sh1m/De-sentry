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
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "desentry/common/status.h"
#include "desentry/crdt/document.h"
#include "desentry/crdt/hlc.h"
#include "desentry/ledger/change_feed.h"
#include "desentry/ledger/outbox_store.h"
#include "desentry/ledger/transit_store.h"
#include "desentry/net/identity.h"
#include "desentry/net/receipt_tracker.h"
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
    // Transit hold budget in bytes, 0 == unbounded (quota_mb == 0). Wired
    // from quota_split.transit_store in node.json; the capacity gate in
    // HoldForOfflineOwner refuses bytes past this rather than silently
    // overfilling the store.
    uint64_t transit_budget_bytes = 0;
    // Max chunk size for striped transit envelopes (config
    // transit_chunk_bytes). Documents bigger than this are held as one
    // envelope per chunk, each with its own ledger intent.
    uint32_t transit_chunk_bytes = 256 * 1024;
    // Node DEK (empty = plaintext). Seals identity.key, the WAL, catalog,
    // every built-in backend, transit/outbox logs and the cross-engine
    // index. Never written to disk; arrives from the sidecar on stdin.
    std::string dek;
  };

  static StatusOr<std::unique_ptr<NodeEngine>> Open(const Options& options);
  ~NodeEngine();

  const NodeIdentity& identity() const { return *identity_; }
  StorageEngine& storage() { return *storage_; }
  TransitStore& transit() { return *transit_; }
  OutboxStore& outbox() { return *outbox_; }
  ChangeFeed& changes() { return *changes_; }
  ReceiptTracker& receipt_tracker() { return *receipt_tracker_; }
  bool is_supervisor() const { return options_.supervisor; }
  uint32_t replication_factor() const { return options_.replication_factor; }
  // True when this node seals its files (a DEK was supplied at Open).
  // Reported on GET /_status as `at_rest_sealed`.
  bool at_rest_sealed() const { return at_rest_sealed_; }
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

  // Supplied by NetworkManager so the engine can know whether it is currently
  // isolated (no reachable peers). When isolated, local writes are staged in
  // the outbox for later replay instead of being broadcast (which would fail).
  void SetReachabilityProvider(std::function<bool()> provider) {
    reachability_provider_ = std::move(provider);
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
  //
  // Capacity-gated: bytes past transit_budget_bytes are refused with
  // ResourceExhausted rather than silently overfilling the store (a full
  // holder simply doesn't hold; gossip still converges the document, only
  // the transit fast-path is lost). All-or-nothing per document: the full
  // size is checked before the first chunk is stored, so a partial hold can
  // never strand unclaimed intents that pin the checkpoint gate.
  //
  // Documents bigger than transit_chunk_bytes are striped: one envelope +
  // one intent per chunk, each chunk named by TransitChunkKeyHash(doc hash,
  // index). `only_chunks` restricts the hold to the caller's assigned chunk
  // indexes (the network layer's deterministic holder selection); null
  // means hold every chunk.
  Status HoldForOfflineOwner(const std::string& owner_node, const std::string& collection,
                              const std::string& key, const std::string& encoded_doc,
                              const std::vector<uint32_t>* only_chunks = nullptr,
                              const std::string& message_id = std::string());

  // Envelopes this node is holding for `owner_node` -- what a kTransitQuery
  // from that peer is answered with.
  std::vector<TransitEnvelope> PendingTransitFor(const std::string& owner_node);

  // True when the transit log met present-but-invalid bytes at open (bad CRC
  // or implausible record length) rather than a clean EOF or torn tail. The
  // corrupt tail is dropped and rewritten on open; this flag (also on
  // GET /_transit as `load_corrupt`) tells operators corruption happened.
  bool TransitLoadCorrupt() const;

  // Called on the returning owner: applies a held document and appends
  // TRANSIT_CLAIMED to its own ledger.
  Status ApplyClaimedTransit(const std::string& collection, const std::string& key,
                              const std::string& encoded_doc);

  // Called on the holder when the owner reports having applied the bytes.
  // Appends TRANSIT_CLAIMED locally; the envelope itself is only released
  // once a quorum-verified checkpoint covers the pair (ledger/checkpoint.h).
  Status RecordRemoteClaim(const std::string& owner_node, const std::string& key_hash);

  // -- outbox (stage-anywhere, sync-on-reconnect flow) -----------------------
  // Drains all staged writes from the outbox, applying them as local writes
  // and broadcasting them to peers. Returns the number of entries replayed.
  // Intended to be called on reconnect or periodically.
  size_t FlushOutbox();

  // Returns the number of entries currently staged in the outbox.
  size_t OutboxSize() const;

  // Returns the total bytes staged in the outbox.
  uint64_t OutboxBytesHeld() const;

  // Waits for signed merge receipts from peers for the local write
  // identified by `message_id`. Returns when `durability` distinct
  // receipts have arrived (counting self as 1), or when `timeout_ms`
  // elapses. Returns the receipts received (may be fewer than requested
  // on timeout). This is a blocking call with zero residual state -- the
  // waiter is the HTTP request handler, and the map entry is erased on
  // completion or timeout.
  struct DurabilityReport {
    uint32_t requested = 0;
    uint32_t achieved = 0;
    bool timed_out = false;
    std::vector<std::string> replicas;  // node_ids that acknowledged
  };
  StatusOr<DurabilityReport> WaitForDurability(const std::string& message_id,
                                                uint32_t durability, uint32_t timeout_ms);

  // -- ledger ---------------------------------------------------------------
  WriteAheadLog::LedgerTip LedgerTip() const { return storage_->LedgerTip(); }
  WriteAheadLog::VerifyResult VerifyLedger();
  StorageEngine::VerifyReport VerifyEverything();
  StatusOr<std::vector<WalRecord>> LedgerEntries(lsn_t from, lsn_t to) {
    return storage_->LedgerEntries(from, to);
  }

  // TRANSIT_INTENT entries naming this node as the owner. Used by
  // ClaimPendingTransit to discover holders directly instead of polling all
  // peers. Returns (intent_lsn, key_hash, holder_node, chunk_index, chunk_total,
  // doc_size_bytes) sorted by intent_lsn.
  struct TransitIntent {
    lsn_t intent_lsn = kInvalidLsn;
    std::string key_hash;
    std::string holder_node;
    uint32_t chunk_index = 0;
    uint32_t chunk_total = 1;
    uint64_t doc_size_bytes = 0;
  };
  std::vector<TransitIntent> TransitIntentsForSelf();

  // Signs the current ledger chain tip (entry_id + entry_hash) with this
  // node's persistent Ed25519 identity key, binding the tip to a node_id a
  // peer already trusts. Per-entry signatures (storage/wal.h) make the same
  // guarantee for individual entries; this remains the cheap whole-history
  // attestation a peer checks first.
  std::string SignLedgerTip() const;
  // Signs an arbitrary (entry_id, entry_hash) pair -- used to attest a
  // truncated delta's tip, which may lag the node's current tip (C-6).
  std::string SignTipFor(lsn_t entry_id, const std::string& entry_hash) const;
  static std::string LedgerTipMessage(lsn_t entry_id, const std::string& entry_hash);
  bool VerifyPeerTip(const std::string& node_id, lsn_t entry_id, const std::string& entry_hash,
                      const std::string& signature) const;

  // Raw (still-encoded, still tombstone-carrying) document bytes for a key --
  // what the gossip/broadcast layer ships, as opposed to GetDocument()'s
  // materialised, tombstone-free JsonValue.
  StatusOr<std::string> GetRawEncoded(const std::string& collection, const std::string& key);

  StorageEngine::QuotaStatus Quota() const { return storage_->Quota(); }
  HybridLogicalClock& clock() { return *clock_; }

  // Runs the catalog's retention_days for `collection` now (explicit,
  // operator-driven -- there is no background scheduler). Only ts_rollup
  // honors retention; other engines report InvalidArgument. Returns chunks
  // dropped.
  StatusOr<size_t> RunRetention(const std::string& collection);

  // Sets a collection's ACL locally AND appends a signed kAcl ledger record
  // so the ACL replicates (H-1 fix). Peers apply it via ApplyRemoteAcl.
  Status SetCollectionAcl(const std::string& collection, const CollectionAcl& acl);
  // Applies a kAcl envelope received from `origin_node_id` (live gossip or
  // peer sync). Verifies the owner attestation against the handshake-proven
  // key and LWW-merges by updated_ms. With check_sig=false (boot replay,
  // where the chain's own integrity is the guarantee) only structure + LWW
  // apply.
  Status ApplyRemoteAcl(const std::string& origin_node_id, const std::string& collection,
                        const std::string& envelope_json, bool check_sig = true);

 private:
  NodeEngine() = default;
  Status WriteThrough(const std::string& collection, const std::string& key,
                       const std::string& encoded_doc, bool notify_hook);
  // HLC durability (C-5): the clock's state is checkpointed to
  // <data_dir>/hlc_clock so a restart never re-issues used timestamps.
  void LoadClockState();
  void PersistClockIfDue();

  Options options_;
  // Whether files are sealed. Kept as a bool (not the DEK) so the raw key
  // is not retained in memory longer than Open() needs it.
  bool at_rest_sealed_ = false;
  std::unique_ptr<StorageEngine> storage_;
  std::unique_ptr<HybridLogicalClock> clock_;
  std::unique_ptr<NodeIdentity> identity_;
  std::unique_ptr<TransitStore> transit_;
  std::unique_ptr<OutboxStore> outbox_;
  std::unique_ptr<ChangeFeed> changes_;
  std::unique_ptr<ReceiptTracker> receipt_tracker_;
  std::function<void(const std::string&, const std::string&, const std::string&)> on_local_write_;
  std::function<std::string(const std::string&)> resolve_public_key_;
  std::function<bool()> reachability_provider_;
  uint64_t hlc_saved_physical_ = 0;
  uint64_t hlc_save_counter_ = 0;
  // Digest cache (H-4 fix): (collection, key) -> last-computed digest entry.
  // LocalDigest used to full-scan, re-decode and re-hash every document per
  // peer per round. Hits skip the decode+hash; misses compute once and
  // populate. Invalidated (not merely updated) on every write path, so a
  // stale entry is impossible: the only mutations are PutDocument,
  // MergeRemote and DeleteDocument (via PutDocument), all of which note here,
  // and boot replay runs before anything is served. Cardinality follows the
  // data itself (one small entry per stored key); tombstones drop their entry
  // and are recomputed from the stored row on the next miss.
  mutable std::mutex digest_mu_;
  std::unordered_map<std::string, DigestEntryOut> digest_cache_;
  static std::string DigestCacheKey(const std::string& collection, const std::string& key);
  void NoteDigest(const std::string& collection, const std::string& key,
                  const std::string& encoded_doc);
};

}  // namespace desentry
