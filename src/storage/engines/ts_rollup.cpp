#include "desentry/storage/engines/ts_rollup.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

#include "desentry/common/byte_buffer.h"
#include "desentry/common/json.h"
#include "desentry/common/logger.h"
#include "desentry/common/platform.h"
#include "desentry/storage/document_codec.h"
#include "desentry/storage/engines/columnar_lite.h"

namespace desentry {

namespace {

int64_t AlignDown(int64_t value, int64_t modulus) {
  if (modulus <= 0) return value;
  int64_t r = value % modulus;
  if (r < 0) r += modulus;
  return value - r;
}

const JsonValue* FindNumeric(const JsonValue& doc, std::initializer_list<const char*> names) {
  if (!doc.is_object()) return nullptr;
  for (const char* name : names) {
    const JsonValue* v = doc.Find(name);
    if (v != nullptr && v->is_number()) return v;
  }
  return nullptr;
}

}  // namespace

int64_t TsRollupBackend::ExtractTimestampMs(const std::string& encoded_doc) {
  CrdtValue doc;
  try {
    doc = DecodeDocument(encoded_doc);
  } catch (const std::exception&) {
    return 0;
  }
  JsonValue json = doc.ToJson();
  const JsonValue* ts = FindNumeric(json, {"ts", "timestamp", "time"});
  if (ts != nullptr) return ts->AsInt();
  // Documented fallback: the document's own HLC physical time. A collection
  // whose points carry no explicit timestamp still buckets sensibly by when
  // it was written, rather than collapsing into a single chunk at epoch 0.
  return static_cast<int64_t>(doc.MaxTimestamp().physical_ms);
}

std::string TsRollupBackend::ExtractSeries(const std::string& key, const std::string& encoded_doc) {
  try {
    JsonValue json = DecodeDocument(encoded_doc).ToJson();
    if (json.is_object()) {
      const JsonValue* s = json.Find("series");
      if (s != nullptr && s->is_string() && !s->AsString().empty()) return s->AsString();
    }
  } catch (const std::exception&) {
    // fall through to the key-prefix rule
  }
  auto colon = key.find(':');
  return colon == std::string::npos ? key : key.substr(0, colon);
}

bool TsRollupBackend::ExtractValue(const std::string& encoded_doc, double* out) {
  try {
    JsonValue json = DecodeDocument(encoded_doc).ToJson();
    const JsonValue* v = FindNumeric(json, {"value", "v", "reading"});
    if (v == nullptr) return false;
    *out = v->AsDouble();
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

// ---------------------------------------------------------------------------
// Rollup computation and codec
// ---------------------------------------------------------------------------

std::map<std::string, std::vector<TsRollupBucket>> TsRollupBackend::ComputeRollups(
    const std::vector<EngineRow>& rows) {
  // series -> bucket_start -> accumulator
  std::map<std::string, std::map<int64_t, TsRollupBucket>> acc;
  for (const auto& [key, doc] : rows) {
    double value = 0;
    if (!ExtractValue(doc, &value)) continue;  // non-numeric points are storable but not aggregatable
    const std::string series = ExtractSeries(key, doc);
    const int64_t bucket = AlignDown(ExtractTimestampMs(doc), kTsRollupBucketMs);
    auto& b = acc[series][bucket];
    if (b.count == 0) {
      b.bucket_start_ms = bucket;
      b.min = b.max = b.first = b.last = value;
      b.sum = value;
      b.count = 1;
    } else {
      b.min = std::min(b.min, value);
      b.max = std::max(b.max, value);
      b.sum += value;
      b.last = value;
      ++b.count;
    }
  }

  std::map<std::string, std::vector<TsRollupBucket>> out;
  for (const auto& [series, buckets] : acc) {
    std::vector<TsRollupBucket>& list = out[series];
    list.reserve(buckets.size());
    for (const auto& [start, bucket] : buckets) {
      (void)start;
      list.push_back(bucket);
    }
  }
  return out;
}

std::string TsRollupBackend::EncodeRollups(
    const std::map<std::string, std::vector<TsRollupBucket>>& rollups) {
  ByteWriter w;
  w.U32(static_cast<uint32_t>(rollups.size()));
  for (const auto& [series, buckets] : rollups) {
    w.Bytes(series);
    w.U32(static_cast<uint32_t>(buckets.size()));
    for (const TsRollupBucket& b : buckets) {
      w.I64(b.bucket_start_ms);
      w.U64(b.count);
      w.F64(b.min);
      w.F64(b.max);
      w.F64(b.sum);
      w.F64(b.first);
      w.F64(b.last);
    }
  }
  return w.TakeString();
}

StatusOr<std::map<std::string, std::vector<TsRollupBucket>>> TsRollupBackend::DecodeRollups(
    const std::string& bytes) {
  std::map<std::string, std::vector<TsRollupBucket>> out;
  try {
    ByteReader r(bytes);
    uint32_t series_count = r.U32();
    for (uint32_t i = 0; i < series_count; ++i) {
      std::string series = r.Bytes();
      uint32_t bucket_count = r.U32();
      std::vector<TsRollupBucket>& list = out[series];
      list.reserve(bucket_count);
      for (uint32_t j = 0; j < bucket_count; ++j) {
        TsRollupBucket b;
        b.bucket_start_ms = r.I64();
        b.count = r.U64();
        b.min = r.F64();
        b.max = r.F64();
        b.sum = r.F64();
        b.first = r.F64();
        b.last = r.F64();
        list.push_back(b);
      }
    }
  } catch (const std::exception& e) {
    return Status::Corruption(std::string("ts_rollup: malformed rollup block: ") + e.what());
  }
  return out;
}

std::string TsRollupBackend::EncodeChunk(const std::vector<EngineRow>& sorted_rows) {
  // A chunk is [rollups][points]. The rollup block is deliberately first and
  // length-prefixed, so an aggregate query reads the front of the page chain
  // and stops -- it never pulls the point column into memory at all.
  std::string rollups = EncodeRollups(ComputeRollups(sorted_rows));
  std::string points = ColumnarLiteBackend::EncodeSegment(sorted_rows);
  ByteWriter w;
  w.U32(kTsChunkMagic);
  w.U32(static_cast<uint32_t>(rollups.size()));
  w.U32(static_cast<uint32_t>(points.size()));
  w.RawBytes(rollups);
  w.RawBytes(points);
  return w.TakeString();
}

Status TsRollupBackend::SplitChunk(const std::string& blob, std::string* rollup_block,
                                    std::string* segment_block) {
  constexpr size_t kHeaderBytes = 12;
  if (blob.size() < kHeaderBytes) return Status::Corruption("ts_rollup: chunk truncated");
  ByteReader r(blob);
  if (r.U32() != kTsChunkMagic) return Status::Corruption("ts_rollup: chunk bad magic");
  uint32_t rollup_len = r.U32();
  uint32_t points_len = r.U32();
  if (kHeaderBytes + static_cast<size_t>(rollup_len) + points_len != blob.size()) {
    return Status::Corruption("ts_rollup: chunk block lengths do not span the blob");
  }
  if (rollup_block != nullptr) *rollup_block = blob.substr(kHeaderBytes, rollup_len);
  if (segment_block != nullptr) *segment_block = blob.substr(kHeaderBytes + rollup_len, points_len);
  return Status::OK();
}

// ---------------------------------------------------------------------------
// Backend
// ---------------------------------------------------------------------------

Status TsRollupBackend::Open(const std::string& data_dir, uint64_t quota_mb) {
  dir_ = data_dir + "/ts_rollup";
  if (!MakeDirs(dir_)) return Status::IOError("cannot create backend directory: " + dir_);
  manifest_path_ = dir_ + "/chunks.json";

  auto store_or = SegmentStore::Open(dir_ + "/ts.dsf");
  if (!store_or.ok()) return store_or.status();
  store_ = std::move(store_or.value());

  SetQuotaBytes(quota_mb * 1024ull * 1024ull);
  SetBytesUsed(store_->BytesOnDisk());
  return LoadManifest();
}

Status TsRollupBackend::LoadManifest() {
  std::ifstream f(manifest_path_);
  if (!f.is_open()) return Status::OK();
  std::ostringstream ss;
  ss << f.rdbuf();
  if (ss.str().empty()) return Status::OK();
  JsonValue root;
  try {
    root = JsonValue::Parse(ss.str());
  } catch (const std::exception& e) {
    return Status::Corruption(std::string("ts_rollup manifest parse error: ") + e.what());
  }
  if (!root.is_array()) return Status::OK();

  std::lock_guard<std::mutex> lock(mu_);
  for (const JsonValue& entry : root.AsArray()) {
    TsChunkMeta meta;
    meta.collection = entry.Get("collection").AsString();
    meta.first_page_id = static_cast<page_id_t>(entry.Get("first_page_id").AsInt());
    meta.chunk_start_ms = entry.Get("chunk_start_ms").AsInt();
    meta.min_ts_ms = entry.Get("min_ts_ms").AsInt();
    meta.max_ts_ms = entry.Get("max_ts_ms").AsInt();
    meta.row_count = static_cast<uint64_t>(entry.Get("row_count").AsInt());
    meta.encoded_bytes = static_cast<uint64_t>(entry.Get("encoded_bytes").AsInt());
    meta.sequence = static_cast<uint64_t>(entry.Get("sequence").AsInt());
    next_sequence_ = std::max(next_sequence_, meta.sequence + 1);
    collections_[meta.collection].chunks.push_back(std::move(meta));
  }

  for (auto& [name, state] : collections_) {
    std::sort(state.chunks.begin(), state.chunks.end(),
              [](const TsChunkMeta& a, const TsChunkMeta& b) { return a.sequence > b.sequence; });
    for (size_t i = 0; i < state.chunks.size(); ++i) {
      auto blob = store_->ReadBlob(state.chunks[i].first_page_id);
      if (!blob.ok()) return blob.status();
      std::string segment;
      Status st = SplitChunk(blob.value(), nullptr, &segment);
      if (!st.ok()) return st;
      auto keys = ColumnarLiteBackend::DecodeSegmentKeys(segment);
      if (!keys.ok()) return keys.status();
      for (const std::string& key : keys.value()) state.key_to_chunk.emplace(key, i);
    }
  }
  return Status::OK();
}

Status TsRollupBackend::SaveManifest() {
  JsonValue::Array arr;
  for (const auto& [name, state] : collections_) {
    for (const TsChunkMeta& meta : state.chunks) {
      JsonValue::Object o;
      o.emplace_back("collection", JsonValue(meta.collection));
      o.emplace_back("first_page_id", JsonValue(static_cast<int64_t>(meta.first_page_id)));
      o.emplace_back("chunk_start_ms", JsonValue(meta.chunk_start_ms));
      o.emplace_back("min_ts_ms", JsonValue(meta.min_ts_ms));
      o.emplace_back("max_ts_ms", JsonValue(meta.max_ts_ms));
      o.emplace_back("row_count", JsonValue(static_cast<int64_t>(meta.row_count)));
      o.emplace_back("encoded_bytes", JsonValue(static_cast<int64_t>(meta.encoded_bytes)));
      o.emplace_back("sequence", JsonValue(static_cast<int64_t>(meta.sequence)));
      arr.emplace_back(std::move(o));
    }
  }
  const std::string tmp = manifest_path_ + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc | std::ios::binary);
    if (!f.is_open()) return Status::IOError("ts_rollup: cannot write " + tmp);
    f << JsonValue(std::move(arr)).Dump();
    f.flush();
    if (!f.good()) return Status::IOError("ts_rollup: manifest write failed");
  }
  std::remove(manifest_path_.c_str());
  if (std::rename(tmp.c_str(), manifest_path_.c_str()) != 0) {
    return Status::IOError("ts_rollup: cannot commit " + manifest_path_);
  }
  return Status::OK();
}

Status TsRollupBackend::Put(const std::string& collection, const std::string& key,
                             const std::string& encoded_doc) {
  uint64_t old_cost = 0;
  {
    auto existing = Get(collection, key);
    if (existing.ok()) old_cost = RecordCost(key, existing.value());
  }
  Status charged = Charge(old_cost, RecordCost(key, encoded_doc));
  if (!charged.ok()) return charged;

  const int64_t chunk_start = AlignDown(ExtractTimestampMs(encoded_doc), kTsChunkMillis);

  std::lock_guard<std::mutex> lock(mu_);
  CollectionState& state = collections_[collection];

  // A point belonging to a different chunk than the open one seals the open
  // chunk first. Out-of-order arrival within the *same* chunk is free; a
  // point older than an already-sealed chunk starts a new chunk of its own
  // (correct, at the cost of a second chunk covering that hour -- readers
  // merge overlapping chunks newest-wins, so no data is lost or shadowed).
  if (state.open_active && state.open_chunk_start_ms != chunk_start) {
    Status st = SealLocked(collection, &state);
    if (!st.ok()) return st;
  }
  if (!state.open_active) {
    state.open_chunk_start_ms = chunk_start;
    state.open_active = true;
  }
  state.open_rows[key] = encoded_doc;

  if (state.open_rows.size() >= kTsMaxOpenRows) return SealLocked(collection, &state);
  return Status::OK();
}

Status TsRollupBackend::SealLocked(const std::string& collection, CollectionState* state) {
  if (state->open_rows.empty()) {
    state->open_active = false;
    return Status::OK();
  }

  std::vector<EngineRow> rows;
  rows.reserve(state->open_rows.size());
  int64_t min_ts = std::numeric_limits<int64_t>::max();
  int64_t max_ts = std::numeric_limits<int64_t>::min();
  for (const auto& [key, doc] : state->open_rows) {
    rows.emplace_back(key, doc);
    const int64_t ts = ExtractTimestampMs(doc);
    min_ts = std::min(min_ts, ts);
    max_ts = std::max(max_ts, ts);
  }

  std::string blob = EncodeChunk(rows);
  auto page_or = store_->AppendBlob(blob);
  if (!page_or.ok()) return page_or.status();

  TsChunkMeta meta;
  meta.collection = collection;
  meta.first_page_id = page_or.value();
  meta.chunk_start_ms = state->open_chunk_start_ms;
  meta.min_ts_ms = min_ts;
  meta.max_ts_ms = max_ts;
  meta.row_count = rows.size();
  meta.encoded_bytes = blob.size();
  meta.sequence = next_sequence_++;

  state->chunks.insert(state->chunks.begin(), std::move(meta));
  for (auto& entry : state->key_to_chunk) ++entry.second;
  for (const auto& [key, doc] : state->open_rows) {
    (void)doc;
    state->key_to_chunk[key] = 0;
  }
  state->open_rows.clear();
  state->open_active = false;

  SetBytesUsed(store_->BytesOnDisk());
  return SaveManifest();
}

Status TsRollupBackend::SealOpenChunk(const std::string& collection) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = collections_.find(collection);
  if (it == collections_.end()) return Status::OK();
  return SealLocked(collection, &it->second);
}

StatusOr<std::vector<EngineRow>> TsRollupBackend::LoadChunkRows(const TsChunkMeta& meta) const {
  auto blob = store_->ReadBlob(meta.first_page_id);
  if (!blob.ok()) return blob.status();
  std::string segment;
  Status st = SplitChunk(blob.value(), nullptr, &segment);
  if (!st.ok()) return st;
  return ColumnarLiteBackend::DecodeSegment(segment);
}

StatusOr<std::map<std::string, std::vector<TsRollupBucket>>> TsRollupBackend::LoadChunkRollups(
    const TsChunkMeta& meta) const {
  auto blob = store_->ReadBlob(meta.first_page_id);
  if (!blob.ok()) return blob.status();
  std::string rollups;
  Status st = SplitChunk(blob.value(), &rollups, nullptr);
  if (!st.ok()) return st;
  return DecodeRollups(rollups);
}

StatusOr<std::string> TsRollupBackend::Get(const std::string& collection, const std::string& key) {
  std::lock_guard<std::mutex> lock(mu_);
  auto coll_it = collections_.find(collection);
  if (coll_it == collections_.end()) return Status::NotFound("no such collection: " + collection);
  CollectionState& state = coll_it->second;

  auto open_it = state.open_rows.find(key);
  if (open_it != state.open_rows.end()) return open_it->second;

  auto idx_it = state.key_to_chunk.find(key);
  if (idx_it == state.key_to_chunk.end()) return Status::NotFound("no such key: " + key);
  if (idx_it->second >= state.chunks.size()) {
    return Status::Corruption("ts_rollup: key index points past the chunk list");
  }
  auto rows = LoadChunkRows(state.chunks[idx_it->second]);
  if (!rows.ok()) return rows.status();
  for (const auto& [k, doc] : rows.value()) {
    if (k == key) return doc;
  }
  return Status::Corruption("ts_rollup: key indexed to a chunk that does not contain it");
}

std::vector<EngineRow> TsRollupBackend::Scan(const std::string& collection,
                                               const std::string& start_key, size_t limit) {
  std::lock_guard<std::mutex> lock(mu_);
  auto coll_it = collections_.find(collection);
  if (coll_it == collections_.end()) return {};
  CollectionState& state = coll_it->second;

  std::map<std::string, std::string> merged;
  for (size_t i = state.chunks.size(); i-- > 0;) {
    auto rows = LoadChunkRows(state.chunks[i]);
    if (!rows.ok()) {
      DSN_LOG_ERROR("ts_rollup", "scan skipped unreadable chunk " << state.chunks[i].sequence);
      continue;
    }
    for (auto& [key, doc] : rows.value()) merged[key] = std::move(doc);
  }
  for (const auto& [key, doc] : state.open_rows) merged[key] = doc;

  std::vector<EngineRow> out;
  for (auto it = merged.lower_bound(start_key); it != merged.end(); ++it) {
    out.emplace_back(it->first, it->second);
    if (limit != 0 && out.size() >= limit) break;
  }
  return out;
}

std::vector<EngineRow> TsRollupBackend::RangeQuery(const std::string& collection,
                                                     const std::string& series, int64_t from_ms,
                                                     int64_t to_ms, size_t limit) {
  std::lock_guard<std::mutex> lock(mu_);
  auto coll_it = collections_.find(collection);
  if (coll_it == collections_.end()) return {};
  CollectionState& state = coll_it->second;

  std::map<std::string, std::string> merged;
  for (size_t i = state.chunks.size(); i-- > 0;) {
    const TsChunkMeta& meta = state.chunks[i];
    // Chunk-level pruning: this is the whole reason chunks record their
    // min/max timestamps. A week-long collection answers a one-hour query by
    // touching one chunk.
    if (meta.max_ts_ms < from_ms || meta.min_ts_ms > to_ms) continue;
    auto rows = LoadChunkRows(meta);
    if (!rows.ok()) continue;
    for (auto& [key, doc] : rows.value()) merged[key] = std::move(doc);
  }
  for (const auto& [key, doc] : state.open_rows) merged[key] = doc;

  std::vector<EngineRow> out;
  for (const auto& [key, doc] : merged) {
    const int64_t ts = ExtractTimestampMs(doc);
    if (ts < from_ms || ts > to_ms) continue;
    if (!series.empty() && ExtractSeries(key, doc) != series) continue;
    out.emplace_back(key, doc);
    if (limit != 0 && out.size() >= limit) break;
  }
  return out;
}

std::map<std::string, std::vector<TsRollupBucket>> TsRollupBackend::Rollups(
    const std::string& collection, const std::string& series, int64_t from_ms, int64_t to_ms,
    int64_t bucket_ms) {
  if (bucket_ms < kTsRollupBucketMs) bucket_ms = kTsRollupBucketMs;
  bucket_ms = AlignDown(bucket_ms + kTsRollupBucketMs - 1, kTsRollupBucketMs);

  std::lock_guard<std::mutex> lock(mu_);
  auto coll_it = collections_.find(collection);
  if (coll_it == collections_.end()) return {};
  CollectionState& state = coll_it->second;

  // series -> coarse bucket start -> accumulator
  std::map<std::string, std::map<int64_t, TsRollupBucket>> acc;
  auto fold = [&](const std::string& s, const TsRollupBucket& fine) {
    if (fine.count == 0) return;
    if (fine.bucket_start_ms + kTsRollupBucketMs <= from_ms || fine.bucket_start_ms > to_ms) return;
    if (!series.empty() && s != series) return;
    const int64_t coarse = AlignDown(fine.bucket_start_ms, bucket_ms);
    auto& b = acc[s][coarse];
    if (b.count == 0) {
      b = fine;
      b.bucket_start_ms = coarse;
    } else {
      b.min = std::min(b.min, fine.min);
      b.max = std::max(b.max, fine.max);
      b.sum += fine.sum;
      b.last = fine.last;
      b.count += fine.count;
    }
  };

  for (size_t i = state.chunks.size(); i-- > 0;) {
    const TsChunkMeta& meta = state.chunks[i];
    if (meta.max_ts_ms < from_ms || meta.min_ts_ms > to_ms) continue;
    auto rollups = LoadChunkRollups(meta);  // reads the front of the chunk only
    if (!rollups.ok()) continue;
    for (const auto& [s, buckets] : rollups.value()) {
      for (const TsRollupBucket& b : buckets) fold(s, b);
    }
  }
  if (!state.open_rows.empty()) {
    std::vector<EngineRow> rows;
    rows.reserve(state.open_rows.size());
    for (const auto& [key, doc] : state.open_rows) rows.emplace_back(key, doc);
    for (const auto& [s, buckets] : ComputeRollups(rows)) {
      for (const TsRollupBucket& b : buckets) fold(s, b);
    }
  }

  std::map<std::string, std::vector<TsRollupBucket>> out;
  for (const auto& [s, buckets] : acc) {
    std::vector<TsRollupBucket>& list = out[s];
    list.reserve(buckets.size());
    for (const auto& [start, bucket] : buckets) {
      (void)start;
      list.push_back(bucket);
    }
  }
  return out;
}

StatusOr<size_t> TsRollupBackend::Prune(const std::string& collection, int64_t cutoff_ms) {
  std::lock_guard<std::mutex> lock(mu_);
  auto coll_it = collections_.find(collection);
  if (coll_it == collections_.end()) return static_cast<size_t>(0);
  CollectionState& state = coll_it->second;

  std::vector<TsChunkMeta> kept;
  std::vector<TsChunkMeta> dropped;
  for (TsChunkMeta& meta : state.chunks) {
    if (meta.max_ts_ms < cutoff_ms) {
      dropped.push_back(meta);
    } else {
      kept.push_back(meta);
    }
  }
  if (dropped.empty()) return static_cast<size_t>(0);

  // Retention drops whole chunks: one manifest edit, no per-row tombstones.
  // The dropped chunks' pages become unreachable (the same documented
  // "vacuum later" trade-off as everywhere else in this engine).
  state.chunks = std::move(kept);
  state.key_to_chunk.clear();
  for (size_t i = 0; i < state.chunks.size(); ++i) {
    auto blob = store_->ReadBlob(state.chunks[i].first_page_id);
    if (!blob.ok()) return blob.status();
    std::string segment;
    Status st = SplitChunk(blob.value(), nullptr, &segment);
    if (!st.ok()) return st;
    auto keys = ColumnarLiteBackend::DecodeSegmentKeys(segment);
    if (!keys.ok()) return keys.status();
    for (const std::string& key : keys.value()) state.key_to_chunk.emplace(key, i);
  }
  Status st = SaveManifest();
  if (!st.ok()) return st;
  DSN_LOG_INFO("ts_rollup", "retention dropped " << dropped.size() << " chunk(s) from " << collection);
  return dropped.size();
}

StatusOr<size_t> TsRollupBackend::ApplyRetention(const std::string& collection, uint32_t retention_days) {
  if (retention_days == 0) return static_cast<size_t>(0);
  const int64_t cutoff = NowMs() - static_cast<int64_t>(retention_days) * 24 * 3600 * 1000;
  return Prune(collection, cutoff);
}

Status TsRollupBackend::Verify() {
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& [name, state] : collections_) {
    for (const TsChunkMeta& meta : state.chunks) {
      Status st = store_->VerifyBlob(meta.first_page_id);
      if (!st.ok()) {
        return Status::Corruption("ts_rollup: chunk " + std::to_string(meta.sequence) + " of " + name +
                                   ": " + st.message());
      }
      auto blob = store_->ReadBlob(meta.first_page_id);
      if (!blob.ok()) return blob.status();
      std::string rollups, segment;
      st = SplitChunk(blob.value(), &rollups, &segment);
      if (!st.ok()) return st;
      auto decoded = DecodeRollups(rollups);
      if (!decoded.ok()) return decoded.status();
      auto keys = ColumnarLiteBackend::DecodeSegmentKeys(segment);
      if (!keys.ok()) return keys.status();
      if (keys.value().size() != meta.row_count) {
        return Status::Corruption("ts_rollup: chunk " + std::to_string(meta.sequence) +
                                   " row count disagrees with its manifest entry");
      }
    }
  }
  return Status::OK();
}

Status TsRollupBackend::Flush() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& [name, state] : collections_) {
      Status st = SealLocked(name, &state);
      if (!st.ok()) return st;
    }
  }
  return store_->Flush();
}

std::vector<std::string> TsRollupBackend::ListCollections() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::string> names;
  names.reserve(collections_.size());
  for (const auto& [name, state] : collections_) {
    (void)state;
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

}  // namespace desentry
