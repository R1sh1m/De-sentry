#pragma once
// Transit store: bytes held for an offline owner (docs/architecture-v2.md Sec 5.2).
//
// When a write lands for a key whose placement targets an unreachable owner,
// a replica keeps the document bytes so a returning node can pull what it
// missed, and records TRANSIT_INTENT on the ledger so the fact that bytes
// are being held is itself part of the audited history. When the owner
// returns and applies the bytes it records TRANSIT_CLAIMED; once a
// quorum-verified checkpoint covers the pair, the envelope is released
// (ledger/checkpoint.h).
//
// Persistence is a small fsync-free append log (`transit.log` in the node's
// data directory) with an in-memory map over it -- the same shape as the
// cross-engine index (storage/router.h), for the same reason: envelopes are
// keyed by (owner, key_hash), a 97-byte composite the B+Tree's 64-byte key
// limit cannot hold, and they must never pollute user collections' backends,
// quotas or checksums. Later records supersede earlier ones for the same
// key; release and expiry are tombstones; a truncated tail from a crash
// costs the last record. Records are flushed per append (like the index);
// the ledger's fsync'd TRANSIT_INTENT is the durability point for the
// promise, and an intent naming bytes a crashed holder lost is discovered
// and reported by the returning owner rather than pretended away.

#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "desentry/common/status.h"
#include "desentry/storage/page.h"
#include "desentry/storage/wal.h"

namespace desentry {

// Logical name of the engine-internal transit collection. It never appears
// in collection listings (NodeEngine filters it) and local writes to it are
// refused; it exists so transit envelopes have a namespace in ledger records
// and replication guards.
inline constexpr const char* kTransitCollection = "_transit";

// One held document: the bytes plus the ledger coordinates that name them.
struct TransitEnvelope {
  std::string owner_node;    // node the bytes are being held for
  std::string collection;    // real collection of the held document
  std::string key;           // real key of the held document
  std::string key_hash;      // 32 raw bytes; LedgerKeyHash(collection, key)
  std::string encoded_doc;   // CRDT-encoded document bytes
  std::string holder_node;   // node holding the bytes (this node, normally)
  lsn_t intent_lsn = kInvalidLsn;  // ledger LSN of the matching TRANSIT_INTENT
  int64_t expires_ms = 0;          // wall-clock expiry; 0 == never expires
};

class TransitStore {
 public:
  // Opens (creating if absent) the transit log under `data_dir`. `ttl_seconds`
  // is the envelope lifetime stamped at Hold() time (0 == never expire).
  static StatusOr<std::unique_ptr<TransitStore>> Open(const std::string& data_dir,
                                                       uint32_t ttl_seconds,
                                                       std::string holder_node_id);
  ~TransitStore();

  // Stores an envelope, stamping expires_ms from the TTL. Upserts on
  // (owner, key_hash): re-holding the same document refreshes its bytes and
  // its expiry rather than duplicating it.
  Status Hold(const TransitEnvelope& envelope);

  // Merges an envelope received from another holder (the replication path
  // NodeEngine::MergeRemote serves for the transit collection). Stored as
  // sent -- holder and expiry are the sender's, not restamped -- so a second
  // holder's copy agrees with the first's about when the bytes lapse.
  Status MergeRemoteEnvelope(const TransitEnvelope& envelope);

  // Decodes one envelope from its stored/wire encoding.
  static StatusOr<TransitEnvelope> Decode(const std::string& bytes);

  // Live (unexpired) envelopes held for `owner_node`.
  std::vector<TransitEnvelope> PendingFor(const std::string& owner_node);

  // Direct lookup of one live envelope. NotFound when no live envelope with
  // this (owner, key_hash) exists -- used by RecordRemoteClaim() to recover
  // the real key a claim refers to.
  StatusOr<TransitEnvelope> Lookup(const std::string& owner_node, const std::string& key_hash);

  // Owners with at least one live envelope held for them.
  std::vector<std::string> Owners();

  // Number of live envelopes currently held.
  size_t Size();

  // Sum of encoded_doc bytes over live envelopes.
  uint64_t BytesHeld();

  uint32_t ttl_seconds() const { return ttl_seconds_; }

  // Drops every envelope with 0 < expires_ms <= now_ms. Returns the number
  // of envelopes expired.
  StatusOr<uint64_t> ExpireAsOf(int64_t now_ms);

  // Releases the envelopes naming any of `key_hashes` (raw 32-byte digests),
  // regardless of owner. Called by RunCheckpoint() once a quorum-verified
  // checkpoint covers the matching intent/claimed pairs. Returns the number
  // of envelopes released.
  StatusOr<size_t> Release(const std::vector<std::string>& key_hashes);

 private:
  TransitStore(std::string path, uint32_t ttl_seconds, std::string holder_node_id)
      : path_(std::move(path)),
        ttl_seconds_(ttl_seconds),
        holder_node_id_(std::move(holder_node_id)) {}

  static std::string MapKey(const std::string& owner_node, const std::string& key_hash);
  static std::string EncodeEnvelope(const TransitEnvelope& envelope);
  // Decodes one log body; NotFound for tombstones, Corruption for garbage.
  static StatusOr<TransitEnvelope> DecodeBody(const std::string& body, bool* is_tombstone,
                                              std::string* tomb_owner, std::string* tomb_key_hash);

  Status Load();
  Status AppendRecord(const std::string& body);
  Status CompactLocked();  // caller holds mu_; rewrites the log with live rows only

  std::string path_;
  uint32_t ttl_seconds_;
  std::string holder_node_id_;

  mutable std::mutex mu_;
  std::unordered_map<std::string, TransitEnvelope> entries_;
  std::unique_ptr<std::fstream> file_;
};

}  // namespace desentry
