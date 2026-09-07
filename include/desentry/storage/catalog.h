#pragma once
// System catalog: the metadata about every collection (its B+Tree root
// page, the router backend it is bound to, its access-control list, and --
// for a "structured" collection -- the JSON-Schema-like validator the API
// layer enforces on write; see ARCHITECTURE.md Sec 4.1 for why structured
// and unstructured collections are otherwise identical).
//
// Deliberately persisted as a small standalone JSON file rather than a
// bootstrapped "collection zero" inside the paged data file: catalog
// mutations are rare (schema changes, not per-document writes) and keeping
// it out of the hot path removes an entire class of chicken-and-egg
// bootstrap complexity from the B+Tree/buffer-pool code for very little
// cost. This mirrors how several production engines (e.g. RocksDB's
// MANIFEST/OPTIONS) keep control-plane metadata in separate small files
// from the data-plane storage.

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "desentry/common/json.h"
#include "desentry/common/status.h"
#include "desentry/storage/page.h"

namespace desentry {

// Per-collection access control (docs/architecture-v2.md Sec 6).
//
// Two things make this meaningful rather than decorative:
//   1. It is enforced at *both* boundaries -- the REST layer (routes.cpp
//      PUT/GET/DELETE) and the replication layer (gossip's byte filter). A
//      peer that is not a reader of a private collection still learns the
//      key *hashes* in the ledger (that is unavoidable: the ledger is the
//      convergence mechanism) but never receives document bytes.
//   2. `owner_node` is a node_id, which is derived from an Ed25519 public
//      key (net/identity.h), so "who owns this" is a cryptographic fact the
//      handshake already proves, not a self-asserted header.
//
// This is deliberately NOT a capability/ACL system with delegation or
// revocation lists. It is the smallest thing that answers "can this peer
// read these bytes?" honestly. Anything richer needs key-per-collection
// envelope encryption, which is called out as future work in
// architecture-v2.md rather than half-built here.
struct CollectionAcl {
  std::string owner_node;              // node_id of the creator; empty == unowned/public
  bool is_private = false;             // false: any peer may read; true: readers[] + owner only
  std::vector<std::string> readers;    // additional node_ids allowed to read
  std::string parent;                  // inherit from this collection when unset here

  bool AllowsReader(const std::string& node_id) const {
    if (!is_private) return true;
    if (!owner_node.empty() && node_id == owner_node) return true;
    for (const std::string& r : readers) {
      if (r == node_id) return true;
    }
    return false;
  }
};

struct CollectionMeta {
  std::string name;
  page_id_t root_page_id = kInvalidPageId;
  bool has_schema = false;
  JsonValue schema;  // JSON-Schema-lite (see api/routes.h validator) -- only meaningful if has_schema
  uint64_t created_at_ms = 0;

  // -- v2 --------------------------------------------------------------
  // Router backend this collection's bytes live in (storage/router.h).
  // Empty means "use the node's default_engine". Immutable after the first
  // write: changing it would strand existing rows in the old backend, so
  // Catalog::SetEngine() refuses once the collection is non-empty and the
  // app offers a copy-and-rebind flow instead.
  std::string engine;

  CollectionAcl acl;

  // Time-series retention in days; 0 == keep everything. Honoured by the
  // ts_rollup backend and by the ledger checkpoint/GC pass.
  uint32_t retention_days = 0;

  // Placement hints (net/placement.h). `shard_key` names the document field
  // hashed onto the consistent-hash ring; empty means hash the primary key.
  std::string shard_key;
  uint32_t replication_factor = 0;  // 0 == inherit the node's config value
  std::vector<std::string> secondary_indexes;
};

class Catalog {
 public:
  static StatusOr<std::unique_ptr<Catalog>> Open(const std::string& catalog_file);

  bool HasCollection(const std::string& name) const;
  const CollectionMeta* Get(const std::string& name) const;
  // Snapshot copy -- safe to hold while other threads mutate the catalog,
  // unlike Get()'s raw pointer into the map.
  bool GetCopy(const std::string& name, CollectionMeta* out) const;
  std::vector<std::string> ListCollections() const;

  // Registers a brand-new collection (caller has already created its
  // B+Tree and knows the root page id).
  Status CreateCollection(const std::string& name, page_id_t root_page_id);

  // Creates the entry if absent, or overwrites root_page_id if present.
  // Used during WAL-replay recovery, which always rebuilds a collection's
  // B+Tree into fresh pages rather than trusting a catalog-recorded root
  // page that may never have been durably flushed before a crash (see
  // storage_engine.h's class comment).
  Status UpsertRootPageId(const std::string& name, page_id_t root_page_id);
  Status SetSchema(const std::string& name, const JsonValue& schema);
  Status DropSchema(const std::string& name);

  // -- v2 ----------------------------------------------------------------
  Status SetEngine(const std::string& name, const std::string& engine);
  Status SetAcl(const std::string& name, const CollectionAcl& acl);
  Status SetRetentionDays(const std::string& name, uint32_t days);
  Status SetPlacement(const std::string& name, const std::string& shard_key, uint32_t replication_factor);

  // Resolves the effective ACL for `name`, walking `parent` links (bounded
  // to kMaxAclParentDepth so a cycle in hand-edited metadata can't hang the
  // read path -- it degrades to "deny" instead).
  CollectionAcl EffectiveAcl(const std::string& name) const;

  // Convenience predicates used by routes.cpp and by gossip's byte filter.
  // `node_id` is the *authenticated* peer id from the handshake, or this
  // node's own id for local API calls.
  bool CanRead(const std::string& collection, const std::string& node_id) const;
  bool CanWrite(const std::string& collection, const std::string& node_id) const;

  Status Save();  // flush current state to disk

  static constexpr int kMaxAclParentDepth = 16;

 private:
  explicit Catalog(std::string path) : path_(std::move(path)) {}
  Status LoadFromDisk();
  Status SaveLocked();  // caller already holds mu_

  mutable std::mutex mu_;
  std::string path_;
  std::unordered_map<std::string, CollectionMeta> collections_;
};

}  // namespace desentry
