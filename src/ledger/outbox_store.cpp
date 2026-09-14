// Outbox store implementation: an append-log with an in-memory map, in the
// same shape as the transit store. See the header for why envelopes live
// here rather than in a storage backend.

#include "desentry/ledger/outbox_store.h"

#include <algorithm>
#include <fstream>

#include "desentry/common/byte_buffer.h"
#include "desentry/common/crc32.h"
#include "desentry/common/logger.h"
#include "desentry/common/platform.h"

namespace desentry {

namespace {

constexpr uint32_t kOutboxMagic = 0x44534E4F;  // "DSNO"
constexpr uint32_t kOutboxVersion = 1;
constexpr uint32_t kOutboxRecordCap = 1u << 20;  // 1MiB

constexpr uint8_t kRecordPut = 1;
constexpr uint8_t kRecordDrop = 2;

}  // namespace

StatusOr<std::unique_ptr<OutboxStore>> OutboxStore::Open(const std::string& data_dir,
                                                          std::string local_node_id) {
  if (local_node_id.empty()) {
    return Status::InvalidArgument("outbox store opened with an empty node id");
  }
  if (!MakeDirs(data_dir)) return Status::IOError("cannot create directory: " + data_dir);
  std::unique_ptr<OutboxStore> store(
      new OutboxStore(data_dir + "/outbox.log", std::move(local_node_id)));
  Status st = store->Load();
  if (!st.ok()) return st;
  return store;
}

OutboxStore::~OutboxStore() {
  std::lock_guard<std::mutex> lock(mu_);
  if (file_ && file_->is_open()) {
    file_->flush();
    file_->close();
  }
}

std::string OutboxStore::MapKey(const std::string& collection, const std::string& key_hash) {
  std::string key = collection;
  key.push_back('\0');
  key += key_hash;
  return key;
}

std::string OutboxStore::EncodeEntry(const OutboxEntry& entry) {
  ByteWriter w;
  w.U32(kOutboxMagic);
  w.U32(kOutboxVersion);
  w.U8(kRecordPut);
  w.Bytes(entry.collection);
  w.Bytes(entry.key);
  w.Bytes(entry.key_hash);
  w.Bytes(entry.encoded_doc);
  // HLCTimestamp: physical_ms, logical, node_id
  w.U64(entry.hlc.physical_ms);
  w.U32(entry.hlc.logical);
  w.Bytes(entry.hlc.node_id);
  w.I64(entry.created_ms);
  return w.TakeString();
}

namespace {

std::string EncodeDropBody(const std::string& collection, const std::string& key_hash) {
  ByteWriter w;
  w.U32(kOutboxMagic);
  w.U32(kOutboxVersion);
  w.U8(kRecordDrop);
  w.Bytes(collection);
  w.Bytes(key_hash);
  return w.TakeString();
}

}  // namespace

StatusOr<OutboxEntry> OutboxStore::Decode(const std::string& body) {
  try {
    ByteReader r(body);
    if (r.U32() != kOutboxMagic) return Status::Corruption("outbox record is not an entry");
    if (r.U32() != kOutboxVersion) {
      return Status::Corruption("outbox record has an unsupported version");
    }
    const uint8_t kind = r.U8();
    if (kind == kRecordDrop) {
      return Status::NotFound("outbox entry was replayed");
    }
    if (kind != kRecordPut) return Status::Corruption("outbox record has an unknown kind");
    OutboxEntry entry;
    entry.collection = r.Bytes();
    entry.key = r.Bytes();
    entry.key_hash = r.Bytes();
    entry.encoded_doc = r.Bytes();
    entry.hlc.physical_ms = r.U64();
    entry.hlc.logical = r.U32();
    entry.hlc.node_id = r.Bytes();
    entry.created_ms = r.I64();
    if (r.remaining() != 0) return Status::Corruption("outbox record has trailing bytes");
    if (entry.collection.empty() || entry.key_hash.size() != kWalHashLen) {
      return Status::Corruption("outbox record has invalid entry fields");
    }
    return entry;
  } catch (const std::exception& e) {
    return Status::Corruption(std::string("outbox record is malformed: ") + e.what());
  }
}

Status OutboxStore::Load() {
  {
    std::ifstream probe(path_, std::ios::binary);
    if (!probe.is_open()) {
      std::ofstream create(path_, std::ios::binary);
      if (!create.is_open()) return Status::IOError("cannot create outbox log: " + path_);
    }
  }
  file_ = std::make_unique<std::fstream>(path_, std::ios::in | std::ios::out | std::ios::binary);
  if (!file_->is_open()) return Status::IOError("cannot open outbox log: " + path_);

  std::lock_guard<std::mutex> lock(mu_);
  file_->clear();
  file_->seekg(0);
  for (;;) {
    char len_buf[4];
    file_->read(len_buf, 4);
    if (file_->gcount() < 4) break;  // clean end-of-file
    uint32_t body_len = 0;
    std::memcpy(&body_len, len_buf, 4);
    if (body_len == 0 || body_len > kOutboxRecordCap) {
      DSN_LOG_WARN("outbox", "implausible record length, stopping load");
      break;
    }
    std::string body(body_len, '\0');
    file_->read(body.data(), static_cast<std::streamsize>(body_len));
    if (static_cast<uint32_t>(file_->gcount()) < body_len) break;  // torn tail
    char crc_buf[4];
    file_->read(crc_buf, 4);
    if (file_->gcount() < 4) break;
    uint32_t stored_crc = 0;
    std::memcpy(&stored_crc, crc_buf, 4);
    if (stored_crc != Crc32(body.data(), body.size())) {
      DSN_LOG_WARN("outbox", "CRC mismatch, stopping load at a torn record");
      break;
    }
    auto entry_or = Decode(body);
    if (!entry_or.ok()) {
      DSN_LOG_WARN("outbox", "skipping undecodable record: " << entry_or.status().message());
      continue;
    }
    const OutboxEntry& entry = entry_or.value();
    entries_[MapKey(entry.collection, entry.key_hash)] = entry;
  }
  file_->clear();
  return Status::OK();
}

Status OutboxStore::AppendRecord(const std::string& body) {
  uint32_t body_len = static_cast<uint32_t>(body.size());
  const uint32_t crc = Crc32(body.data(), body.size());

  std::string frame;
  frame.resize(4);
  std::memcpy(frame.data(), &body_len, 4);
  frame += body;
  const size_t off = frame.size();
  frame.resize(off + 4);
  std::memcpy(frame.data() + off, &crc, 4);

  file_->clear();
  file_->seekp(0, std::ios::end);
  file_->write(frame.data(), static_cast<std::streamsize>(frame.size()));
  if (!file_->good()) return Status::IOError("outbox log append failed");
  file_->flush();
  return Status::OK();
}

Status OutboxStore::Put(const OutboxEntry& entry) {
  if (entry.collection.empty()) {
    return Status::InvalidArgument("outbox put requires a collection");
  }
  if (entry.key_hash.size() != kWalHashLen) {
    return Status::InvalidArgument("outbox put requires a 32-byte key_hash");
  }
  if (entry.collection == kOutboxCollection) {
    return Status::InvalidArgument("outbox put: cannot write to the outbox collection itself");
  }

  OutboxEntry stored = entry;
  stored.created_ms = NowMs();

  std::lock_guard<std::mutex> lock(mu_);
  Status st = AppendRecord(EncodeEntry(stored));
  if (!st.ok()) return st;
  entries_[MapKey(stored.collection, stored.key_hash)] = std::move(stored);
  return Status::OK();
}

std::vector<OutboxEntry> OutboxStore::DrainAll() {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<OutboxEntry> out;
  out.reserve(entries_.size());
  for (const auto& [map_key, entry] : entries_) {
    (void)map_key;
    out.push_back(entry);
  }
  // Sort by created_ms ascending (oldest first) for replay order
  std::sort(out.begin(), out.end(), [](const OutboxEntry& a, const OutboxEntry& b) {
    return a.created_ms < b.created_ms;
  });
  if (!out.empty()) {
    // Rewrite the log with just tombstones for all entries we're draining
    for (const auto& entry : out) {
      Status st = AppendRecord(EncodeDropBody(entry.collection, entry.key_hash));
      if (!st.ok()) {
        DSN_LOG_WARN("outbox", "failed to append drop record during drain: " << st.message());
      }
    }
    entries_.clear();
    Status st = CompactLocked();
    if (!st.ok()) {
      DSN_LOG_WARN("outbox", "failed to compact after drain: " << st.message());
    }
  }
  return out;
}

size_t OutboxStore::Size() {
  std::lock_guard<std::mutex> lock(mu_);
  return entries_.size();
}

uint64_t OutboxStore::BytesHeld() {
  std::lock_guard<std::mutex> lock(mu_);
  uint64_t total = 0;
  for (const auto& [map_key, entry] : entries_) {
    (void)map_key;
    total += entry.encoded_doc.size();
  }
  return total;
}

Status OutboxStore::CompactLocked() {
  // Rewrite the log with live rows only, over a sibling file, so a crash
  // mid-compaction leaves the original intact rather than a half-built log.
  const std::string tmp = path_ + ".compact";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return Status::IOError("cannot write " + tmp);
    for (const auto& [map_key, entry] : entries_) {
      (void)map_key;
      const std::string body = EncodeEntry(entry);
      const uint32_t body_len = static_cast<uint32_t>(body.size());
      const uint32_t crc = Crc32(body.data(), body.size());
      out.write(reinterpret_cast<const char*>(&body_len), 4);
      out.write(body.data(), static_cast<std::streamsize>(body.size()));
      out.write(reinterpret_cast<const char*>(&crc), 4);
    }
    out.flush();
    if (!out.good()) return Status::IOError("outbox log compaction write failed");
  }
  if (file_ && file_->is_open()) file_->close();
  std::remove(path_.c_str());
  if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
    return Status::IOError("cannot commit compacted outbox log: " + path_);
  }
  file_ = std::make_unique<std::fstream>(path_, std::ios::in | std::ios::out | std::ios::binary);
  if (!file_->is_open()) return Status::IOError("cannot reopen outbox log: " + path_);
  return Status::OK();
}

}  // namespace desentry