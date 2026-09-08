#pragma once
// Write-Ahead Log / hash-chained ledger (v2).
//
// v1's role stays intact: an append-only durability log, fsync'd before a
// write is acknowledged, whose records double as the replication stream --
// the durability log and the replication log are deliberately the same log.
// Records are **logical** ("apply this document mutation"), which is safe
// precisely because every mutation is a CRDT merge and CRDT merges are
// idempotent, so replaying a record twice during recovery is always correct.
//
// v2 adds four things, all in service of the offline-owner flow described in
// docs/architecture-v2.md Sec 5:
//
//   1. **Operation vocabulary.** PUT / DEL / TRANSIT_INTENT /
//      TRANSIT_CLAIMED / CHECKPOINT. The two transit ops are what let a node
//      that was offline when a write happened discover, on its return, that
//      bytes are being held for it -- without any peer having to remember
//      "who was down when".
//
//   2. **key_hash.** SHA-256(collection || 0x00 || key). The ledger is
//      designed to be shareable with peers that are *not* readers of a
//      private collection: they converge on the SET of entry hashes (which
//      is what makes the chain verifiable everywhere) while learning neither
//      the key nor the bytes. Keys stay in the record for the owning node's
//      own replay; the gossip filter (net/gossip.cpp) strips them, and
//      key_hash is what survives.
//
//   3. **HLC timestamp + origin signature.** Each entry carries the Hybrid
//      Logical Clock stamp of the write and an Ed25519 signature over the
//      entry content by the node that originated it. A SHA-256 chain proves
//      only internal self-consistency -- anyone can fabricate a different
//      but internally consistent chain. Per-entry signatures bind each entry
//      to a node_id (itself derived from the same public key), which is what
//      makes "node X attests to this exact history" checkable. v1 signed
//      only the chain tip; signing every entry closes the gap where a node
//      could disown an individual entry it had in fact written.
//
//   4. **Checkpoint + prune.** A CHECKPOINT entry marks a prefix as settled.
//      LedgerEntries() and Prune() use it to drop TRANSIT_INTENT /
//      TRANSIT_CLAIMED pairs below it -- but only after quorum verification
//      (ledger/checkpoint.h), never on a single node's say-so.
//
// Signing is injected as a callback rather than by including net/identity.h
// here: the storage layer must not depend on the network layer (the same
// layering rule NodeEngine's local-write hook follows).
//
// v1 -> v2 on-disk migration is explicit and audited, not silent: see
// WriteAheadLog::Open().

#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "desentry/common/status.h"
#include "desentry/crdt/hlc.h"
#include "desentry/storage/page.h"

namespace desentry {

enum class WalRecordType : uint8_t {
  kPut = 1,
  kDelete = 2,
  kCheckpoint = 3,
  // Bytes for a key whose owner was unreachable are being held by a replica.
  kTransitIntent = 4,
  // The owner came back, pulled those bytes, and applied them. Once a
  // CHECKPOINT covers a matching INTENT/CLAIMED pair, both can be pruned.
  kTransitClaimed = 5,
};

const char* WalRecordTypeName(WalRecordType type);

// Length of a hash-chain link (SHA-256 digest), in raw bytes.
constexpr size_t kWalHashLen = 32;

// Magic prefixing every v2 record body. A v1 body began with the record's
// 8-byte LSN, so this both identifies the format and makes a v1 file
// unmistakable on open.
constexpr uint32_t kWalRecordMagicV2 = 0x44535732;  // "DSW2"

struct WalRecord {
  lsn_t lsn = kInvalidLsn;
  WalRecordType type = WalRecordType::kPut;
  std::string collection;
  std::string key;
  std::string document_bytes;  // binary-encoded CRDT document; empty for DEL/CHECKPOINT
  std::string key_hash;        // 32 raw bytes; SHA-256(collection || 0x00 || key)
  HLCTimestamp hlc;            // when the originating node produced this entry
  std::string origin_node_id;  // hex node_id of the writer
  std::string origin_signature;  // Ed25519 over the canonical content; empty if unsigned
  std::string prev_hash;       // 32 raw bytes; chain link (zero bytes for genesis)
  std::string entry_hash;      // 32 raw bytes; SHA-256(content || prev_hash)

  bool IsTransit() const {
    return type == WalRecordType::kTransitIntent || type == WalRecordType::kTransitClaimed;
  }
};

// Computes the stable key hash used everywhere a key must be referenced
// without being disclosed. Exposed because the network layer computes it
// too (to match an incoming INTENT against a local key) and the two must
// agree exactly.
std::string LedgerKeyHash(const std::string& collection, const std::string& key);

class WriteAheadLog {
 public:
  // Signs an arbitrary message with the node's identity key. Injected so the
  // storage layer never includes net/identity.h.
  using Signer = std::function<std::string(const std::string& message)>;

  static StatusOr<std::unique_ptr<WriteAheadLog>> Open(const std::string& wal_file);
  ~WriteAheadLog();

  // Installs the origin identity used to stamp and sign subsequent entries.
  // Entries appended before this is called are stored unsigned and are
  // reported as such by VerifyChain() rather than being treated as valid.
  void SetOrigin(std::string node_id, Signer signer);

  struct AppendOptions {
    HLCTimestamp hlc;
    // Transit ops name the node the bytes are being held for; ignored for
    // PUT/DEL/CHECKPOINT.
    std::string transit_owner;
  };

  // Appends a record and fsyncs before returning -- this is the durability
  // point. Extends the hash chain and, if an origin is installed, signs the
  // entry. Returns the assigned LSN.
  StatusOr<lsn_t> Append(WalRecordType type, const std::string& collection, const std::string& key,
                          const std::string& document_bytes, const AppendOptions& options = {});

  // Reads every well-formed record in the log in order, for startup recovery
  // and for ledger introspection. A record failing its CRC (a torn write
  // from a crash mid-append) ends the read there -- everything before it is
  // trusted, per standard WAL semantics.
  StatusOr<std::vector<WalRecord>> ReadAll();

  lsn_t LastLsn() const { return next_lsn_ - 1; }

  struct LedgerTip {
    lsn_t entry_id = kInvalidLsn;  // LastLsn(); -1 == "no entries yet"
    std::string entry_hash;        // 32 raw bytes; all-zero genesis hash if empty
  };
  // Current chain tip -- O(1), just the cached running hash, no disk I/O.
  LedgerTip Tip() const;

  // LSN of the newest CHECKPOINT entry, or kInvalidLsn if there is none.
  lsn_t LastCheckpointLsn() const;

  struct VerifyResult {
    bool ok = true;
    uint64_t entries_checked = 0;
    uint64_t signed_entries = 0;
    uint64_t unsigned_entries = 0;
    lsn_t failed_at_entry_id = kInvalidLsn;
    std::string reason;
  };
  // Replays the entire chain from genesis, recomputing and checking every
  // entry_hash and prev_hash link. O(log size); a full re-verification, not a
  // "trust the tip" shortcut.
  //
  // `verify_signature` is supplied by the caller (NodeEngine, which knows how
  // to map a node_id to a public key via the peer table) so this layer stays
  // free of crypto-identity concerns. When null, signatures are counted but
  // not checked, and the result says so via signed_entries/unsigned_entries
  // rather than implying a stronger guarantee than was actually tested.
  using SignatureVerifier = std::function<bool(const std::string& node_id, const std::string& message,
                                                const std::string& signature)>;
  VerifyResult VerifyChain(const SignatureVerifier& verify_signature = nullptr);

  // Rewrites the log, dropping every TRANSIT_INTENT / TRANSIT_CLAIMED entry
  // whose LSN is at or below `checkpoint_lsn` and whose key_hash has a
  // matching CLAIMED. PUT/DEL/CHECKPOINT entries are never dropped -- they
  // are the history the chain attests to.
  //
  // Pruning necessarily re-derives the chain over the surviving entries (the
  // removed links cannot be preserved), so the pre-prune tip is returned for
  // the caller to record. ledger/checkpoint.h is the only production caller
  // and it refuses to prune without quorum agreement first.
  struct PruneResult {
    uint64_t dropped = 0;
    lsn_t pruned_through = kInvalidLsn;
    std::string previous_tip_hash;
    std::string new_tip_hash;
  };
  StatusOr<PruneResult> Prune(lsn_t checkpoint_lsn);

  // True if this log was migrated from the v1 on-disk format when opened.
  bool migrated_from_v1() const { return migrated_from_v1_; }
  const std::string& pre_migration_tip_hash() const { return pre_migration_tip_hash_; }

 private:
  WriteAheadLog(std::fstream file, std::string path, lsn_t next_lsn, std::string tip_hash)
      : file_(std::move(file)), path_(std::move(path)), next_lsn_(next_lsn), tip_hash_(std::move(tip_hash)) {}

  // Canonical byte layout hashed into entry_hash and signed by the origin:
  // everything about the record except the chain/signature fields
  // themselves. Shared by Append() (to produce them) and VerifyChain() (to
  // recompute and check them).
  static std::string BuildContent(const WalRecord& record);
  static std::string EncodeBody(const WalRecord& record);
  static Status DecodeBody(const std::string& body, WalRecord* out);
  // v1 body parser, used only by the one-time migration on Open().
  static Status DecodeBodyV1(const std::string& body, WalRecord* out);

  Status ReadAllLocked(std::vector<WalRecord>* out);
  Status RewriteLocked(const std::vector<WalRecord>& records);

  std::fstream file_;
  std::string path_;
  mutable std::mutex mu_;
  lsn_t next_lsn_;
  std::string tip_hash_;  // 32 raw bytes; running chain tip, updated on every Append()
  lsn_t last_checkpoint_lsn_ = kInvalidLsn;

  std::string origin_node_id_;
  Signer signer_;

  bool migrated_from_v1_ = false;
  std::string pre_migration_tip_hash_;

  // Bytes left unparsed after the last well-formed record, set by
  // ReadAllLocked(). A crash mid-append leaves a torn record at the very end
  // of the file and nothing after it; bytes that survive *past* the point
  // where parsing gave up mean the damage is in the middle of the log, which
  // is corruption or tampering rather than an interrupted write. Recovery
  // treats both the same way (trust the prefix); VerifyChain() must not.
  std::streamoff unparsed_tail_bytes_ = 0;
};

}  // namespace desentry
