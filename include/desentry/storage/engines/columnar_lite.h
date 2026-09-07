#pragma once
// `columnar_lite` -- a from-scratch column-segmented backend for
// blob/columnar and analytical collections.
//
// Shape of the problem it solves: a collection that is written once and
// scanned repeatedly (event rows, exported tables, document blobs) pays
// twice under the `kv` backend -- once for the per-record slotted-page
// framing, and once again because a full scan touches every page even when
// only the keys are needed. A column-segmented layout fixes both.
//
// Physical model:
//   * Rows accumulate in an in-memory **open segment**.
//   * At kRowsPerSegment rows (or kBytesPerSegment bytes) the segment is
//     **sealed**: rows are sorted by key and encoded as three independent
//     columns, then written to the shared SegmentStore as one page chain.
//   * A sealed segment's header records each column's offset and length, so
//     a reader can pull the key column alone without materializing the
//     payload -- which is what makes reopening a large collection cheap and
//     is the actual point of a columnar layout, not a decoration on it.
//
// Column encodings (both are real, not nominal):
//   * keys        -- front coding: each key stores the length of the prefix
//                    it shares with its predecessor plus the remaining
//                    suffix. This is delta encoding over a sorted string
//                    column, and on typical keys ("evt:2026-09-06T10:00:01")
//                    it removes most of the column.
//   * doc lengths -- zigzag varint deltas against the previous length.
//   * payload     -- byte-run RLE. CRDT-encoded documents carry long runs of
//                    zero bytes in their fixed-width HLC fields, which is
//                    exactly what RLE is good at; runs shorter than
//                    kMinRunLength are emitted literally so the encoding
//                    never inflates incompressible data.
//
// Upsert semantics: a key may appear in more than one segment. The newest
// occurrence wins, exactly like an LSM tree's level ordering -- there is no
// in-place update of a sealed segment, and superseded rows are reclaimed by
// Compact(), never on the write path.

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "desentry/storage/engines/engine_common.h"
#include "desentry/storage/segment_store.h"

namespace desentry {

// Encoding parameters. Exposed so tests can seal segments deterministically
// rather than by writing thousands of rows.
constexpr size_t kColumnarRowsPerSegment = 2048;
constexpr size_t kColumnarBytesPerSegment = 4u << 20;  // 4MiB
constexpr size_t kColumnarMinRunLength = 4;
constexpr uint32_t kColumnarSegmentMagic = 0x44534301;  // "DSC" + version 1

// One sealed segment's metadata, persisted alongside the data file so a
// reopen does not have to scan the whole store to find its segments.
struct ColumnarSegmentMeta {
  std::string collection;
  page_id_t first_page_id = kInvalidPageId;
  uint64_t row_count = 0;
  uint64_t encoded_bytes = 0;
  std::string min_key;
  std::string max_key;
  uint64_t sequence = 0;  // monotonically increasing; higher wins on key collision
};

class ColumnarLiteBackend : public BaseBackend {
 public:
  std::string Name() const override { return "columnar_lite"; }

  Status Open(const std::string& data_dir, uint64_t quota_mb) override;
  Status Put(const std::string& collection, const std::string& key,
              const std::string& encoded_doc) override;
  StatusOr<std::string> Get(const std::string& collection, const std::string& key) override;
  std::vector<EngineRow> Scan(const std::string& collection, const std::string& start_key,
                               size_t limit) override;
  Status Verify() override;
  Status Flush() override;
  std::vector<std::string> ListCollections() const override;

  // -- columnar-specific surface (reached via StorageRouter::Backend) -----
  // Seals the open segment immediately. Called by Flush(), by the sealing
  // threshold, and by tests that want a deterministic sealed segment.
  Status SealOpenSegment(const std::string& collection);

  // Rewrites a collection's segments, dropping rows superseded by a newer
  // segment. Housekeeping only -- never on the write path.
  Status Compact(const std::string& collection);

  struct Stats {
    uint64_t sealed_segments = 0;
    uint64_t open_rows = 0;
    uint64_t logical_bytes = 0;   // sum of raw key+payload bytes
    uint64_t encoded_bytes = 0;   // sum of sealed segment sizes
  };
  Stats StatsFor(const std::string& collection) const;

  // Column codec, exposed for unit tests: encoding a row set and decoding it
  // must round-trip exactly, and DecodeKeys() must agree with Decode()'s key
  // column without reading the payload.
  static std::string EncodeSegment(const std::vector<EngineRow>& sorted_rows);
  static StatusOr<std::vector<EngineRow>> DecodeSegment(const std::string& blob);
  static StatusOr<std::vector<std::string>> DecodeSegmentKeys(const std::string& blob);

 private:
  struct CollectionState {
    // Newest-first ordering is what makes "first hit wins" correct.
    std::vector<ColumnarSegmentMeta> segments;
    std::map<std::string, std::string> open_rows;  // sorted; the unsealed segment
    uint64_t open_bytes = 0;
    // key -> index into `segments` holding the newest copy. Rows in
    // open_rows are newer than every sealed segment and are checked first.
    std::unordered_map<std::string, size_t> key_to_segment;
  };

  CollectionState& StateFor(const std::string& collection);  // caller holds mu_
  Status SealLocked(const std::string& collection, CollectionState* state);
  Status SaveManifest();
  Status LoadManifest();
  StatusOr<std::vector<EngineRow>> LoadSegment(const ColumnarSegmentMeta& meta) const;

  std::string dir_;
  std::string manifest_path_;
  std::unique_ptr<SegmentStore> store_;
  uint64_t next_sequence_ = 1;

  mutable std::mutex mu_;
  std::unordered_map<std::string, CollectionState> collections_;
};

}  // namespace desentry
