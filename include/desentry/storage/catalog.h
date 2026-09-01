#pragma once
// System catalog: the metadata about every collection (its B+Tree root
// page, and -- for a "structured" collection -- the JSON-Schema-like
// validator the API layer enforces on write; see ARCHITECTURE.md §4.1 for
// why structured and unstructured collections are otherwise identical).
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

struct CollectionMeta {
  std::string name;
  page_id_t root_page_id = kInvalidPageId;
  bool has_schema = false;
  JsonValue schema;  // JSON-Schema-lite (see api/routes.h validator) -- only meaningful if has_schema
  uint64_t created_at_ms = 0;
};

class Catalog {
 public:
  static StatusOr<std::unique_ptr<Catalog>> Open(const std::string& catalog_file);

  bool HasCollection(const std::string& name) const;
  const CollectionMeta* Get(const std::string& name) const;
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

  Status Save();  // flush current state to disk

 private:
  explicit Catalog(std::string path) : path_(std::move(path)) {}
  Status LoadFromDisk();

  mutable std::mutex mu_;
  std::string path_;
  std::unordered_map<std::string, CollectionMeta> collections_;
};

}  // namespace desentry
