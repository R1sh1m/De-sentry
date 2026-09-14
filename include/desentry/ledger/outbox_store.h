// Outbox store: writes made while the node is isolated from the mesh.
// (docs/architecture-v2.md Sec 5.3 - Dropbox-like stage-anywhere ingest)
//
// When a node cannot reach any peer (no live P2P connections), writes to
// collections are accepted locally but cannot be broadcast or held as transit
// for offline owners. The outbox records these writes with their full CRDT
// bytes so they can be replayed when the mesh becomes reachable again.
//
// Persistence is an fsync-free append log (`outbox.log` in the node's data
// directory) with an in-memory map over it, keyed by (collection, key_hash).
// Later records supersede earlier ones for the same key; replay consumes and
// removes them. A truncated tail from a crash costs the last record.

#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "desentry/common/status.h"
#include "desentry/storage/wal.h"

namespace desentry {

// Logical name of the engine-internal outbox collection. It never appears in
// collection listings and local writes to it are refused.
inline constexpr const char* kOutboxCollection = "_outbox";

struct OutboxEntry {
  std::string collection;     // real collection of the document
  std::string key;            // real key of the document
  std::string key_hash;       // 32 raw bytes; LedgerKeyHash(collection, key)
  std::string encoded_doc;    // CRDT-encoded document bytes
  HLCTimestamp hlc;           // origin clock stamp
  int64_t created_ms = 0;     // wall-clock when the write was staged
};

class OutboxStore {
 public:
  // Opens (creating if absent) the outbox log under `data_dir`.
  static StatusOr<std::unique_ptr<OutboxStore>> Open(const std::string& data_dir,
                                                      std::string local_node_id);

  // Stores an entry for a write made while isolated. Upserts on
  // (collection, key_hash): a newer write for the same key replaces the old.
  Status Put(const OutboxEntry& entry);

  // Returns all live entries, oldest first (by created_ms), for replay.
  std::vector<OutboxEntry> DrainAll();

  // Returns the number of live entries.
  size_t Size();

  // Sum of encoded_doc bytes over live entries.
  uint64_t BytesHeld();

 private:
  OutboxStore(std::string path, std::string local_node_id)
      : path_(std::move(path)),
        local_node_id_(std::move(local_node_id)) {}

  static std::string MapKey(const std::string& collection, const std::string& key_hash);
  static std::string EncodeEntry(const OutboxEntry& entry);
  static StatusOr<OutboxEntry> Decode(const std::string& body);

  Status Load();
  Status AppendRecord(const std::string& body);
  Status CompactLocked();  // caller holds mu_; rewrites the log with live rows only

  std::string path_;
  std::string local_node_id_;

  mutable std::mutex mu_;
  std::unordered_map<std::string, OutboxEntry> entries_;
  std::unique_ptr<std::fstream> file_;
};

}  // namespace desentry