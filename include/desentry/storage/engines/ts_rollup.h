#pragma once
// `ts_rollup` -- a from-scratch time-series backend: chunked time segments,
// precomputed downsampled rollups, and retention-based pruning.
//
// A sensor stream is the one workload where a general key/value engine is
// straightforwardly the wrong shape. Three properties dominate it, and each
// maps to a piece of this design:
//
//   * Writes are almost always **append-at-the-tail**. So rows land in the
//     chunk covering "now"; older chunks are immutable and sealed, and a
//     late-arriving point re-opens only the one chunk it belongs to.
//   * Reads are almost always **a time range plus an aggregate**, not a
//     point lookup. So every sealed chunk carries precomputed rollups
//     (count/min/max/sum/first/last per kTsRollupBucketMs bucket, per
//     series). A "min temperature per minute over last week" query reads
//     rollup blocks, never the points -- which is the entire reason this
//     backend exists rather than scanning `kv`.
//   * Old data is **dropped wholesale**, not deleted row by row. So
//     retention prunes whole chunks: one manifest edit, no tombstones, no
//     compaction storm.
//
// Timestamp extraction is explicit rather than magic: the point's time is
// the first present numeric field named `ts`, `timestamp` or `time` (epoch
// milliseconds), and failing that the document's own HLC physical time.
// Series identity is the field `series`, falling back to the key's prefix
// before the first ':'. Both fallbacks are documented rather than silent,
// and both are visible in the rollup output so a misconfigured collection
// looks wrong instead of looking empty.
//
// Licence note: this is written from scratch specifically because the
// obvious off-the-shelf answer (TDengine) is AGPL and InfluxDB/QuestDB are
// servers -- neither can ship inside a desktop app the way this product
// requires.

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

constexpr int64_t kTsChunkMillis = 3600LL * 1000;   // one hour per chunk
constexpr int64_t kTsRollupBucketMs = 60LL * 1000;  // one-minute downsample
constexpr size_t kTsMaxOpenRows = 8192;
constexpr uint32_t kTsChunkMagic = 0x44535401;  // "DST" + version 1

// One downsampled bucket for one series.
struct TsRollupBucket {
  int64_t bucket_start_ms = 0;
  uint64_t count = 0;
  double min = 0;
  double max = 0;
  double sum = 0;
  double first = 0;
  double last = 0;
};

struct TsChunkMeta {
  std::string collection;
  page_id_t first_page_id = kInvalidPageId;
  int64_t chunk_start_ms = 0;  // aligned to kTsChunkMillis
  int64_t min_ts_ms = 0;
  int64_t max_ts_ms = 0;
  uint64_t row_count = 0;
  uint64_t encoded_bytes = 0;
  uint64_t sequence = 0;
};

class TsRollupBackend : public BaseBackend {
 public:
  std::string Name() const override { return "ts_rollup"; }

  Status Open(const std::string& data_dir, uint64_t quota_mb) override;
  Status Put(const std::string& collection, const std::string& key,
              const std::string& encoded_doc) override;
  StatusOr<std::string> Get(const std::string& collection, const std::string& key) override;
  std::vector<EngineRow> Scan(const std::string& collection, const std::string& start_key,
                               size_t limit) override;
  Status Verify() override;
  Status Flush() override;
  std::vector<std::string> ListCollections() const override;

  // -- time-series surface -------------------------------------------------
  // Raw points in [from_ms, to_ms]. `series` empty means every series.
  std::vector<EngineRow> RangeQuery(const std::string& collection, const std::string& series,
                                     int64_t from_ms, int64_t to_ms, size_t limit);

  // Downsampled aggregates over the same range, read from the sealed
  // chunks' precomputed rollup blocks (and computed on the fly for the open
  // chunk only). `bucket_ms` is rounded up to a multiple of
  // kTsRollupBucketMs, since finer than the stored resolution would require
  // reading the points anyway -- the API says so rather than silently
  // degrading.
  std::map<std::string, std::vector<TsRollupBucket>> Rollups(const std::string& collection,
                                                               const std::string& series,
                                                               int64_t from_ms, int64_t to_ms,
                                                               int64_t bucket_ms);

  // Drops whole chunks whose newest point is older than `cutoff_ms`.
  // Returns the number of chunks removed.
  StatusOr<size_t> Prune(const std::string& collection, int64_t cutoff_ms);
  // Convenience wrapper honouring CollectionMeta::retention_days.
  StatusOr<size_t> ApplyRetention(const std::string& collection, uint32_t retention_days);

  Status SealOpenChunk(const std::string& collection);

  // Exposed for tests: the documented extraction rules above, as code.
  static int64_t ExtractTimestampMs(const std::string& encoded_doc);
  static std::string ExtractSeries(const std::string& key, const std::string& encoded_doc);
  static bool ExtractValue(const std::string& encoded_doc, double* out);

 private:
  struct CollectionState {
    std::vector<TsChunkMeta> chunks;                 // newest chunk first
    std::map<std::string, std::string> open_rows;    // the unsealed chunk
    int64_t open_chunk_start_ms = 0;
    bool open_active = false;
    std::unordered_map<std::string, size_t> key_to_chunk;
  };

  Status SealLocked(const std::string& collection, CollectionState* state);
  Status SaveManifest();
  Status LoadManifest();
  StatusOr<std::vector<EngineRow>> LoadChunkRows(const TsChunkMeta& meta) const;
  StatusOr<std::map<std::string, std::vector<TsRollupBucket>>> LoadChunkRollups(
      const TsChunkMeta& meta) const;

  static std::string EncodeChunk(const std::vector<EngineRow>& sorted_rows);
  static Status SplitChunk(const std::string& blob, std::string* rollup_block,
                            std::string* segment_block);
  static std::string EncodeRollups(const std::map<std::string, std::vector<TsRollupBucket>>& rollups);
  static StatusOr<std::map<std::string, std::vector<TsRollupBucket>>> DecodeRollups(
      const std::string& bytes);
  static std::map<std::string, std::vector<TsRollupBucket>> ComputeRollups(
      const std::vector<EngineRow>& rows);

  std::string dir_;
  std::string manifest_path_;
  std::unique_ptr<SegmentStore> store_;
  uint64_t next_sequence_ = 1;

  mutable std::mutex mu_;
  std::unordered_map<std::string, CollectionState> collections_;
};

}  // namespace desentry
