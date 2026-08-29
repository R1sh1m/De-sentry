#pragma once
// Write-Ahead Log: append-only durability log, fsync'd before a write is
// acknowledged to the caller, exactly like every production RDBMS's redo
// log. Deliberately **logical** rather than physical: a record captures
// "apply this document mutation to this collection", not a raw before/after
// page image. Two things make logical logging the right call here rather
// than a liability:
//   1. Every mutation in this engine is a CRDT merge, and CRDT merges are
//      idempotent by construction -- replaying a logical record twice
//      during recovery is always safe, so we get correctness "for free"
//      without needing careful physical-redo LSN bookkeeping per page.
//   2. The exact same record shape is what gets shipped over the wire to
//      other peers (see net/wire_protocol.h) -- the durability log and the
//      replication log are the same log, which is a deliberate, load-bearing
//      simplification for the MVP.
//
// Hash-chained audit ledger: every record also carries a SHA-256
// `entry_hash` computed over its own content plus the previous record's
// `entry_hash` (`prev_hash`) -- a Git-commit-style chain: entry N's hash
// covers entry N-1's hash, so altering, reordering, or deleting any past
// record breaks the chain from that point forward, detectable by
// VerifyChain() without a second copy to diff against. The genesis record
// chains from 32 zero bytes. This turns the WAL into a tamper-evident audit
// trail of every mutation this node has ever made, on top of its existing
// durability/replication role -- see ARCHITECTURE.md's ledger section for
// how NodeEngine layers an Ed25519 signature over the chain tip so a peer
// can verify *who* attests to it, not just that it's internally consistent.

#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "desentry/common/status.h"
#include "desentry/storage/page.h"

namespace desentry {

enum class WalRecordType : uint8_t {
  kPut = 1,
  kDelete = 2,
  kCheckpoint = 3,
};

// Length of a hash-chain link (SHA-256 digest), in raw bytes.
constexpr size_t kWalHashLen = 32;

struct WalRecord {
  lsn_t lsn = kInvalidLsn;
  WalRecordType type = WalRecordType::kPut;
  std::string collection;
  std::string key;
  std::string document_bytes;  // binary-encoded CRDT document (see document_codec.h); empty for kDelete/kCheckpoint
  std::string prev_hash;       // 32 raw bytes; chain link to the preceding entry (zero bytes for genesis)
  std::string entry_hash;      // 32 raw bytes; SHA-256(content || prev_hash)
};

class WriteAheadLog {
 public:
  static StatusOr<std::unique_ptr<WriteAheadLog>> Open(const std::string& wal_file);
  ~WriteAheadLog();

  // Appends a record and fsyncs before returning -- this is the durability
  // point. Returns the assigned LSN. Also extends the hash chain: the new
  // record's prev_hash is the current chain tip, and its entry_hash becomes
  // the new tip.
  StatusOr<lsn_t> Append(WalRecordType type, const std::string& collection,
                          const std::string& key, const std::string& document_bytes);

  // Reads every well-formed record in the log in order, for startup
  // recovery and for ledger introspection (GET /_ledger/entries). A record
  // that fails its CRC check (torn write from a crash mid-append) ends
  // replay at that point -- everything before it is trusted, per standard
  // WAL semantics.
  StatusOr<std::vector<WalRecord>> ReadAll();

  lsn_t LastLsn() const { return next_lsn_ - 1; }

  struct LedgerTip {
    lsn_t entry_id = kInvalidLsn;     // LastLsn(); -1 ("no entries yet")
    std::string entry_hash;           // 32 raw bytes; all-zero genesis hash if empty
  };
  // Current chain tip -- O(1), just the cached running hash, no disk I/O.
  LedgerTip Tip() const;

  struct VerifyResult {
    bool ok = true;
    uint64_t entries_checked = 0;
    lsn_t failed_at_entry_id = kInvalidLsn;
    std::string reason;
  };
  // Replays the entire chain from genesis, recomputing and checking every
  // entry_hash and every prev_hash link. O(log size); this is a full,
  // honest re-verification, not a cached "trust the tip" shortcut -- the
  // same guarantee ARCHITECTURE.md's ledger section documents for peer
  // audits.
  VerifyResult VerifyChain();

 private:
  explicit WriteAheadLog(std::fstream file, std::string path, lsn_t next_lsn, std::string tip_hash)
      : file_(std::move(file)), path_(std::move(path)), next_lsn_(next_lsn), tip_hash_(std::move(tip_hash)) {}

  // Canonical byte layout hashed into entry_hash (everything about the
  // record except the hash-chain fields themselves) -- shared by Append()
  // (to produce the hash) and VerifyChain() (to recompute and check it).
  static std::string BuildContent(lsn_t lsn, WalRecordType type, const std::string& collection,
                                   const std::string& key, const std::string& document_bytes);

  std::fstream file_;
  std::string path_;
  mutable std::mutex mu_;
  lsn_t next_lsn_;
  std::string tip_hash_;  // 32 raw bytes; running chain tip, updated on every Append()
};

}  // namespace desentry
