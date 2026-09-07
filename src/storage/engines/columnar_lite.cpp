#include "desentry/storage/engines/columnar_lite.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

#include "desentry/common/byte_buffer.h"
#include "desentry/common/json.h"
#include "desentry/common/logger.h"
#include "desentry/common/platform.h"

namespace desentry {

namespace {

// -- varint / zigzag ------------------------------------------------------
void PutVarint(std::string* out, uint64_t v) {
  while (v >= 0x80) {
    out->push_back(static_cast<char>((v & 0x7F) | 0x80));
    v >>= 7;
  }
  out->push_back(static_cast<char>(v));
}

bool GetVarint(const std::string& in, size_t* pos, uint64_t* out) {
  uint64_t result = 0;
  int shift = 0;
  while (*pos < in.size()) {
    uint8_t byte = static_cast<uint8_t>(in[(*pos)++]);
    result |= static_cast<uint64_t>(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      *out = result;
      return true;
    }
    shift += 7;
    if (shift > 63) return false;
  }
  return false;
}

uint64_t ZigZag(int64_t v) { return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63); }
int64_t UnZigZag(uint64_t v) { return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1); }

size_t SharedPrefix(const std::string& a, const std::string& b) {
  size_t n = std::min(a.size(), b.size());
  size_t i = 0;
  while (i < n && a[i] == b[i]) ++i;
  return i;
}

// -- byte-run RLE ---------------------------------------------------------
// Format is a sequence of blocks:
//   [varint: (count << 1) | 1][byte]        -- a run of `count` identical bytes
//   [varint: (count << 1) | 0][count bytes] -- `count` literal bytes
// Runs shorter than kColumnarMinRunLength are folded into a literal block so
// the encoding never inflates data that does not repeat.
std::string RleEncode(const std::string& in) {
  std::string out;
  out.reserve(in.size() / 2 + 8);
  size_t i = 0;
  std::string literal;
  auto flush_literal = [&]() {
    if (literal.empty()) return;
    PutVarint(&out, static_cast<uint64_t>(literal.size()) << 1);
    out += literal;
    literal.clear();
  };
  while (i < in.size()) {
    size_t run = 1;
    while (i + run < in.size() && in[i + run] == in[i]) ++run;
    if (run >= kColumnarMinRunLength) {
      flush_literal();
      PutVarint(&out, (static_cast<uint64_t>(run) << 1) | 1u);
      out.push_back(in[i]);
    } else {
      literal.append(in, i, run);
    }
    i += run;
  }
  flush_literal();
  return out;
}

bool RleDecode(const std::string& in, size_t expected_len, std::string* out) {
  out->clear();
  out->reserve(expected_len);
  size_t pos = 0;
  while (pos < in.size()) {
    uint64_t header = 0;
    if (!GetVarint(in, &pos, &header)) return false;
    uint64_t count = header >> 1;
    if (count == 0) return false;
    if (out->size() + count > expected_len) return false;
    if ((header & 1u) != 0) {
      if (pos >= in.size()) return false;
      out->append(static_cast<size_t>(count), in[pos]);
      ++pos;
    } else {
      if (pos + count > in.size()) return false;
      out->append(in, pos, static_cast<size_t>(count));
      pos += static_cast<size_t>(count);
    }
  }
  return out->size() == expected_len;
}

}  // namespace

// ---------------------------------------------------------------------------
// Column codec
// ---------------------------------------------------------------------------
//
// Segment layout:
//   u32 magic | u32 row_count
//   u32 keys_len | u32 lens_len | u32 payload_raw_len | u32 payload_enc_len
//   [keys column][lengths column][RLE payload column]
//
// The four length fields are the reason a reader can take the key column
// alone: its offset and size are known without decoding anything else.

std::string ColumnarLiteBackend::EncodeSegment(const std::vector<EngineRow>& sorted_rows) {
  std::string keys_col;
  std::string lens_col;
  std::string payload_raw;

  std::string prev_key;
  int64_t prev_len = 0;
  for (const auto& [key, doc] : sorted_rows) {
    size_t shared = SharedPrefix(prev_key, key);
    PutVarint(&keys_col, static_cast<uint64_t>(shared));
    PutVarint(&keys_col, static_cast<uint64_t>(key.size() - shared));
    keys_col.append(key, shared, key.size() - shared);
    prev_key = key;

    int64_t len = static_cast<int64_t>(doc.size());
    PutVarint(&lens_col, ZigZag(len - prev_len));
    prev_len = len;

    payload_raw += doc;
  }

  std::string payload_enc = RleEncode(payload_raw);

  ByteWriter w;
  w.U32(kColumnarSegmentMagic);
  w.U32(static_cast<uint32_t>(sorted_rows.size()));
  w.U32(static_cast<uint32_t>(keys_col.size()));
  w.U32(static_cast<uint32_t>(lens_col.size()));
  w.U32(static_cast<uint32_t>(payload_raw.size()));
  w.U32(static_cast<uint32_t>(payload_enc.size()));
  w.RawBytes(keys_col);
  w.RawBytes(lens_col);
  w.RawBytes(payload_enc);
  return w.TakeString();
}

namespace {

struct SegmentHeader {
  uint32_t row_count = 0;
  uint32_t keys_len = 0;
  uint32_t lens_len = 0;
  uint32_t payload_raw_len = 0;
  uint32_t payload_enc_len = 0;
  size_t keys_off = 0;
  size_t lens_off = 0;
  size_t payload_off = 0;
};

Status ParseSegmentHeader(const std::string& blob, SegmentHeader* out) {
  constexpr size_t kHeaderBytes = 24;
  if (blob.size() < kHeaderBytes) return Status::Corruption("columnar segment truncated");
  ByteReader r(blob);
  if (r.U32() != kColumnarSegmentMagic) return Status::Corruption("columnar segment bad magic");
  out->row_count = r.U32();
  out->keys_len = r.U32();
  out->lens_len = r.U32();
  out->payload_raw_len = r.U32();
  out->payload_enc_len = r.U32();
  out->keys_off = kHeaderBytes;
  out->lens_off = out->keys_off + out->keys_len;
  out->payload_off = out->lens_off + out->lens_len;
  if (out->payload_off + out->payload_enc_len != blob.size()) {
    return Status::Corruption("columnar segment column lengths do not span the blob");
  }
  return Status::OK();
}

Status DecodeKeyColumn(const std::string& column, uint32_t row_count, std::vector<std::string>* out) {
  out->clear();
  out->reserve(row_count);
  size_t pos = 0;
  std::string prev;
  for (uint32_t i = 0; i < row_count; ++i) {
    uint64_t shared = 0, suffix_len = 0;
    if (!GetVarint(column, &pos, &shared) || !GetVarint(column, &pos, &suffix_len)) {
      return Status::Corruption("columnar key column truncated");
    }
    if (shared > prev.size() || pos + suffix_len > column.size()) {
      return Status::Corruption("columnar key column malformed");
    }
    std::string key = prev.substr(0, static_cast<size_t>(shared));
    key.append(column, pos, static_cast<size_t>(suffix_len));
    pos += static_cast<size_t>(suffix_len);
    prev = key;
    out->push_back(std::move(key));
  }
  return Status::OK();
}

}  // namespace

StatusOr<std::vector<std::string>> ColumnarLiteBackend::DecodeSegmentKeys(const std::string& blob) {
  SegmentHeader h;
  Status st = ParseSegmentHeader(blob, &h);
  if (!st.ok()) return st;
  std::vector<std::string> keys;
  // Only the key column is read: the payload column is never touched here.
  st = DecodeKeyColumn(blob.substr(h.keys_off, h.keys_len), h.row_count, &keys);
  if (!st.ok()) return st;
  return keys;
}

StatusOr<std::vector<EngineRow>> ColumnarLiteBackend::DecodeSegment(const std::string& blob) {
  SegmentHeader h;
  Status st = ParseSegmentHeader(blob, &h);
  if (!st.ok()) return st;

  std::vector<std::string> keys;
  st = DecodeKeyColumn(blob.substr(h.keys_off, h.keys_len), h.row_count, &keys);
  if (!st.ok()) return st;

  std::string lens_col = blob.substr(h.lens_off, h.lens_len);
  std::vector<size_t> lengths;
  lengths.reserve(h.row_count);
  size_t pos = 0;
  int64_t prev_len = 0;
  uint64_t total = 0;
  for (uint32_t i = 0; i < h.row_count; ++i) {
    uint64_t zz = 0;
    if (!GetVarint(lens_col, &pos, &zz)) return Status::Corruption("columnar length column truncated");
    int64_t len = prev_len + UnZigZag(zz);
    if (len < 0) return Status::Corruption("columnar length column produced a negative length");
    prev_len = len;
    lengths.push_back(static_cast<size_t>(len));
    total += static_cast<uint64_t>(len);
  }
  if (total != h.payload_raw_len) {
    return Status::Corruption("columnar length column disagrees with payload length");
  }

  std::string payload;
  if (!RleDecode(blob.substr(h.payload_off, h.payload_enc_len), h.payload_raw_len, &payload)) {
    return Status::Corruption("columnar payload column failed RLE decode");
  }

  std::vector<EngineRow> rows;
  rows.reserve(h.row_count);
  size_t offset = 0;
  for (uint32_t i = 0; i < h.row_count; ++i) {
    rows.emplace_back(keys[i], payload.substr(offset, lengths[i]));
    offset += lengths[i];
  }
  return rows;
}

// ---------------------------------------------------------------------------
// Backend
// ---------------------------------------------------------------------------

Status ColumnarLiteBackend::Open(const std::string& data_dir, uint64_t quota_mb) {
  dir_ = data_dir + "/columnar_lite";
  if (!MakeDirs(dir_)) return Status::IOError("cannot create backend directory: " + dir_);
  manifest_path_ = dir_ + "/segments.json";

  auto store_or = SegmentStore::Open(dir_ + "/columnar.dsf");
  if (!store_or.ok()) return store_or.status();
  store_ = std::move(store_or.value());

  SetQuotaBytes(quota_mb * 1024ull * 1024ull);
  SetBytesUsed(store_->BytesOnDisk());
  return LoadManifest();
}

Status ColumnarLiteBackend::LoadManifest() {
  std::ifstream f(manifest_path_);
  if (!f.is_open()) return Status::OK();
  std::ostringstream ss;
  ss << f.rdbuf();
  if (ss.str().empty()) return Status::OK();
  JsonValue root;
  try {
    root = JsonValue::Parse(ss.str());
  } catch (const std::exception& e) {
    return Status::Corruption(std::string("columnar manifest parse error: ") + e.what());
  }
  if (!root.is_array()) return Status::OK();

  std::lock_guard<std::mutex> lock(mu_);
  for (const JsonValue& entry : root.AsArray()) {
    ColumnarSegmentMeta meta;
    meta.collection = entry.Get("collection").AsString();
    meta.first_page_id = static_cast<page_id_t>(entry.Get("first_page_id").AsInt());
    meta.row_count = static_cast<uint64_t>(entry.Get("row_count").AsInt());
    meta.encoded_bytes = static_cast<uint64_t>(entry.Get("encoded_bytes").AsInt());
    meta.min_key = entry.Get("min_key").AsString();
    meta.max_key = entry.Get("max_key").AsString();
    meta.sequence = static_cast<uint64_t>(entry.Get("sequence").AsInt());
    next_sequence_ = std::max(next_sequence_, meta.sequence + 1);
    collections_[meta.collection].segments.push_back(std::move(meta));
  }

  // Rebuild the key -> newest-segment map by reading only each segment's key
  // column. This is the columnar payoff at startup: reopening a collection
  // with a gigabyte of payload reads only its keys.
  for (auto& [name, state] : collections_) {
    std::sort(state.segments.begin(), state.segments.end(),
              [](const ColumnarSegmentMeta& a, const ColumnarSegmentMeta& b) {
                return a.sequence > b.sequence;  // newest first
              });
    for (size_t i = 0; i < state.segments.size(); ++i) {
      auto blob = store_->ReadBlob(state.segments[i].first_page_id);
      if (!blob.ok()) {
        DSN_LOG_ERROR("columnar", "segment " << state.segments[i].sequence << " of " << name
                                              << " unreadable: " << blob.status().ToString());
        return blob.status();
      }
      auto keys = DecodeSegmentKeys(blob.value());
      if (!keys.ok()) return keys.status();
      for (const std::string& key : keys.value()) {
        state.key_to_segment.emplace(key, i);  // first (newest) wins
      }
    }
  }
  return Status::OK();
}

Status ColumnarLiteBackend::SaveManifest() {
  JsonValue::Array arr;
  for (const auto& [name, state] : collections_) {
    for (const ColumnarSegmentMeta& meta : state.segments) {
      JsonValue::Object o;
      o.emplace_back("collection", JsonValue(meta.collection));
      o.emplace_back("first_page_id", JsonValue(static_cast<int64_t>(meta.first_page_id)));
      o.emplace_back("row_count", JsonValue(static_cast<int64_t>(meta.row_count)));
      o.emplace_back("encoded_bytes", JsonValue(static_cast<int64_t>(meta.encoded_bytes)));
      o.emplace_back("min_key", JsonValue(meta.min_key));
      o.emplace_back("max_key", JsonValue(meta.max_key));
      o.emplace_back("sequence", JsonValue(static_cast<int64_t>(meta.sequence)));
      arr.emplace_back(std::move(o));
    }
  }
  const std::string tmp = manifest_path_ + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc | std::ios::binary);
    if (!f.is_open()) return Status::IOError("columnar: cannot write " + tmp);
    f << JsonValue(std::move(arr)).Dump();
    f.flush();
    if (!f.good()) return Status::IOError("columnar: manifest write failed");
  }
  std::remove(manifest_path_.c_str());
  if (std::rename(tmp.c_str(), manifest_path_.c_str()) != 0) {
    return Status::IOError("columnar: cannot commit " + manifest_path_);
  }
  return Status::OK();
}

ColumnarLiteBackend::CollectionState& ColumnarLiteBackend::StateFor(const std::string& collection) {
  return collections_[collection];
}

Status ColumnarLiteBackend::Put(const std::string& collection, const std::string& key,
                                 const std::string& encoded_doc) {
  uint64_t old_cost = 0;
  {
    auto existing = Get(collection, key);
    if (existing.ok()) old_cost = RecordCost(key, existing.value());
  }
  Status charged = Charge(old_cost, RecordCost(key, encoded_doc));
  if (!charged.ok()) return charged;

  std::lock_guard<std::mutex> lock(mu_);
  CollectionState& state = StateFor(collection);
  auto it = state.open_rows.find(key);
  if (it != state.open_rows.end()) {
    state.open_bytes -= it->second.size() + key.size();
    it->second = encoded_doc;
  } else {
    state.open_rows.emplace(key, encoded_doc);
  }
  state.open_bytes += encoded_doc.size() + key.size();

  if (state.open_rows.size() >= kColumnarRowsPerSegment || state.open_bytes >= kColumnarBytesPerSegment) {
    return SealLocked(collection, &state);
  }
  return Status::OK();
}

Status ColumnarLiteBackend::SealLocked(const std::string& collection, CollectionState* state) {
  if (state->open_rows.empty()) return Status::OK();

  // std::map iterates in key order, which is exactly the sort the front
  // coder needs -- no separate sort pass.
  std::vector<EngineRow> rows;
  rows.reserve(state->open_rows.size());
  for (const auto& [key, doc] : state->open_rows) rows.emplace_back(key, doc);

  std::string blob = EncodeSegment(rows);
  auto page_or = store_->AppendBlob(blob);
  if (!page_or.ok()) return page_or.status();

  ColumnarSegmentMeta meta;
  meta.collection = collection;
  meta.first_page_id = page_or.value();
  meta.row_count = rows.size();
  meta.encoded_bytes = blob.size();
  meta.min_key = rows.front().first;
  meta.max_key = rows.back().first;
  meta.sequence = next_sequence_++;

  // Newest first, so index 0 is always the freshest segment.
  state->segments.insert(state->segments.begin(), std::move(meta));
  for (auto& [key, seg_idx] : state->key_to_segment) {
    (void)key;
    ++seg_idx;  // every existing segment shifted down by one
  }
  for (const auto& [key, doc] : state->open_rows) {
    (void)doc;
    state->key_to_segment[key] = 0;
  }
  state->open_rows.clear();
  state->open_bytes = 0;

  SetBytesUsed(store_->BytesOnDisk());
  return SaveManifest();
}

Status ColumnarLiteBackend::SealOpenSegment(const std::string& collection) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = collections_.find(collection);
  if (it == collections_.end()) return Status::OK();
  return SealLocked(collection, &it->second);
}

StatusOr<std::vector<EngineRow>> ColumnarLiteBackend::LoadSegment(const ColumnarSegmentMeta& meta) const {
  auto blob = store_->ReadBlob(meta.first_page_id);
  if (!blob.ok()) return blob.status();
  return DecodeSegment(blob.value());
}

StatusOr<std::string> ColumnarLiteBackend::Get(const std::string& collection, const std::string& key) {
  std::lock_guard<std::mutex> lock(mu_);
  auto coll_it = collections_.find(collection);
  if (coll_it == collections_.end()) return Status::NotFound("no such collection: " + collection);
  CollectionState& state = coll_it->second;

  // The unsealed segment is newer than every sealed one.
  auto open_it = state.open_rows.find(key);
  if (open_it != state.open_rows.end()) return open_it->second;

  auto idx_it = state.key_to_segment.find(key);
  if (idx_it == state.key_to_segment.end()) return Status::NotFound("no such key: " + key);
  if (idx_it->second >= state.segments.size()) {
    return Status::Corruption("columnar: key index points past the segment list");
  }
  auto rows = LoadSegment(state.segments[idx_it->second]);
  if (!rows.ok()) return rows.status();
  for (const auto& [k, doc] : rows.value()) {
    if (k == key) return doc;
  }
  return Status::Corruption("columnar: key indexed to a segment that does not contain it");
}

std::vector<EngineRow> ColumnarLiteBackend::Scan(const std::string& collection,
                                                   const std::string& start_key, size_t limit) {
  std::lock_guard<std::mutex> lock(mu_);
  auto coll_it = collections_.find(collection);
  if (coll_it == collections_.end()) return {};
  CollectionState& state = coll_it->second;

  // Merge the unsealed rows with every sealed segment, newest wins. A
  // std::map keyed by row key both de-duplicates and produces the ordered
  // output Scan() promises.
  std::map<std::string, std::string> merged;
  for (size_t i = state.segments.size(); i-- > 0;) {  // oldest first, so newer overwrites
    auto rows = LoadSegment(state.segments[i]);
    if (!rows.ok()) {
      DSN_LOG_ERROR("columnar", "scan skipped unreadable segment " << state.segments[i].sequence
                                                                    << ": " << rows.status().ToString());
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

Status ColumnarLiteBackend::Compact(const std::string& collection) {
  std::lock_guard<std::mutex> lock(mu_);
  auto coll_it = collections_.find(collection);
  if (coll_it == collections_.end()) return Status::OK();
  CollectionState& state = coll_it->second;
  if (state.segments.size() < 2) return Status::OK();

  std::map<std::string, std::string> live;
  for (size_t i = state.segments.size(); i-- > 0;) {
    auto rows = LoadSegment(state.segments[i]);
    if (!rows.ok()) return rows.status();
    for (auto& [key, doc] : rows.value()) live[key] = std::move(doc);
  }

  std::vector<EngineRow> rows;
  rows.reserve(live.size());
  for (const auto& [key, doc] : live) rows.emplace_back(key, doc);
  if (rows.empty()) {
    state.segments.clear();
    state.key_to_segment.clear();
    return SaveManifest();
  }

  std::string blob = EncodeSegment(rows);
  auto page_or = store_->AppendBlob(blob);
  if (!page_or.ok()) return page_or.status();

  ColumnarSegmentMeta meta;
  meta.collection = collection;
  meta.first_page_id = page_or.value();
  meta.row_count = rows.size();
  meta.encoded_bytes = blob.size();
  meta.min_key = rows.front().first;
  meta.max_key = rows.back().first;
  meta.sequence = next_sequence_++;

  // The superseded segments' pages become unreachable rather than being
  // returned to a free list -- the same explicit "vacuum later" trade-off
  // the slotted-page layer makes, recorded in STATUS.md's known limits
  // rather than quietly assumed.
  state.segments.clear();
  state.segments.push_back(std::move(meta));
  state.key_to_segment.clear();
  for (const auto& [key, doc] : rows) {
    (void)doc;
    state.key_to_segment[key] = 0;
  }
  SetBytesUsed(store_->BytesOnDisk());
  return SaveManifest();
}

ColumnarLiteBackend::Stats ColumnarLiteBackend::StatsFor(const std::string& collection) const {
  Stats stats;
  std::lock_guard<std::mutex> lock(mu_);
  auto it = collections_.find(collection);
  if (it == collections_.end()) return stats;
  stats.sealed_segments = it->second.segments.size();
  stats.open_rows = it->second.open_rows.size();
  stats.logical_bytes = it->second.open_bytes;
  for (const ColumnarSegmentMeta& meta : it->second.segments) {
    stats.encoded_bytes += meta.encoded_bytes;
  }
  return stats;
}

Status ColumnarLiteBackend::Verify() {
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& [name, state] : collections_) {
    for (const ColumnarSegmentMeta& meta : state.segments) {
      Status st = store_->VerifyBlob(meta.first_page_id);
      if (!st.ok()) {
        return Status::Corruption("columnar: segment " + std::to_string(meta.sequence) + " of " + name +
                                   ": " + st.message());
      }
      auto blob = store_->ReadBlob(meta.first_page_id);
      if (!blob.ok()) return blob.status();
      auto keys = DecodeSegmentKeys(blob.value());
      if (!keys.ok()) return keys.status();
      if (keys.value().size() != meta.row_count) {
        return Status::Corruption("columnar: segment " + std::to_string(meta.sequence) +
                                   " row count disagrees with its manifest entry");
      }
    }
  }
  return Status::OK();
}

Status ColumnarLiteBackend::Flush() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& [name, state] : collections_) {
      Status st = SealLocked(name, &state);
      if (!st.ok()) return st;
    }
  }
  return store_->Flush();
}

std::vector<std::string> ColumnarLiteBackend::ListCollections() const {
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
