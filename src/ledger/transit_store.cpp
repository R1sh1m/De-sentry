// Transit store implementation: an append-log with an in-memory map, in the
// same shape as the cross-engine index. See the header for why envelopes
// live here rather than in a storage backend.

#include "desentry/ledger/transit_store.h"

#include <cstring>
#include <fstream>
#include <set>

#include "desentry/common/byte_buffer.h"
#include "desentry/common/crc32.h"
#include "desentry/common/logger.h"
#include "desentry/common/platform.h"

namespace desentry {

namespace {

// On-disk framing: [u32 body_len][body][u32 crc32(body)], mirroring the
// cross-engine index. A truncated tail from a crash costs the last record.
constexpr uint32_t kTransitMagic = 0x44534E54;  // "DSNT"
constexpr uint32_t kTransitVersion = 1;
constexpr uint32_t kTransitRecordCap = 1u << 20;  // 1MiB: absurdly generous for one envelope

constexpr uint8_t kRecordHold = 1;
constexpr uint8_t kRecordDrop = 2;

}  // namespace

StatusOr<std::unique_ptr<TransitStore>> TransitStore::Open(const std::string& data_dir,
                                                           uint32_t ttl_seconds,
                                                           std::string holder_node_id) {
  if (holder_node_id.empty()) {
    return Status::InvalidArgument("transit store opened with an empty holder node id");
  }
  if (!MakeDirs(data_dir)) return Status::IOError("cannot create directory: " + data_dir);
  std::unique_ptr<TransitStore> store(
      new TransitStore(data_dir + "/transit.log", ttl_seconds, std::move(holder_node_id)));
  Status st = store->Load();
  if (!st.ok()) return st;
  return store;
}

TransitStore::~TransitStore() {
  std::lock_guard<std::mutex> lock(mu_);
  if (file_ && file_->is_open()) {
    file_->flush();
    file_->close();
  }
}

std::string TransitStore::MapKey(const std::string& owner_node, const std::string& key_hash) {
  std::string key = owner_node;
  key.push_back('\0');
  key += key_hash;
  return key;
}

std::string TransitStore::EncodeEnvelope(const TransitEnvelope& envelope) {
  ByteWriter w;
  w.U32(kTransitMagic);
  w.U32(kTransitVersion);
  w.U8(kRecordHold);
  w.Bytes(envelope.owner_node);
  w.Bytes(envelope.collection);
  w.Bytes(envelope.key);
  w.Bytes(envelope.key_hash);
  w.Bytes(envelope.encoded_doc);
  w.Bytes(envelope.holder_node);
  w.I64(envelope.intent_lsn);
  w.I64(envelope.expires_ms);
  return w.TakeString();
}

namespace {

// A drop record names (owner, key_hash); the envelope bytes are already gone.
std::string EncodeDropBody(const std::string& owner_node, const std::string& key_hash) {
  ByteWriter w;
  w.U32(kTransitMagic);
  w.U32(kTransitVersion);
  w.U8(kRecordDrop);
  w.Bytes(owner_node);
  w.Bytes(key_hash);
  return w.TakeString();
}

}  // namespace

StatusOr<TransitEnvelope> TransitStore::DecodeBody(const std::string& body, bool* is_tombstone,
                                                   std::string* tomb_owner,
                                                   std::string* tomb_key_hash) {
  try {
    ByteReader r(body);
    if (r.U32() != kTransitMagic) return Status::Corruption("transit record is not an envelope");
    if (r.U32() != kTransitVersion) {
      return Status::Corruption("transit record has an unsupported envelope version");
    }
    const uint8_t kind = r.U8();
    if (kind == kRecordDrop) {
      *is_tombstone = true;
      *tomb_owner = r.Bytes();
      *tomb_key_hash = r.Bytes();
      if (r.remaining() != 0) return Status::Corruption("transit tombstone has trailing bytes");
      if (tomb_owner->empty() || tomb_key_hash->size() != kWalHashLen) {
        return Status::Corruption("transit tombstone has invalid fields");
      }
      return TransitEnvelope{};
    }
    if (kind != kRecordHold) return Status::Corruption("transit record has an unknown kind");
    *is_tombstone = false;
    TransitEnvelope envelope;
    envelope.owner_node = r.Bytes();
    envelope.collection = r.Bytes();
    envelope.key = r.Bytes();
    envelope.key_hash = r.Bytes();
    envelope.encoded_doc = r.Bytes();
    envelope.holder_node = r.Bytes();
    envelope.intent_lsn = r.I64();
    envelope.expires_ms = r.I64();
    if (r.remaining() != 0) return Status::Corruption("transit record has trailing bytes");
    if (envelope.owner_node.empty() || envelope.key_hash.size() != kWalHashLen) {
      return Status::Corruption("transit record has invalid envelope fields");
    }
    return envelope;
  } catch (const std::exception& e) {
    return Status::Corruption(std::string("transit record is malformed: ") + e.what());
  }
}

StatusOr<TransitEnvelope> TransitStore::Decode(const std::string& bytes) {
  bool is_tombstone = false;
  std::string tomb_owner, tomb_key_hash;
  auto env_or = DecodeBody(bytes, &is_tombstone, &tomb_owner, &tomb_key_hash);
  if (!env_or.ok()) return env_or.status();
  if (is_tombstone) return Status::NotFound("transit envelope was released");
  return env_or.value();
}

Status TransitStore::Load() {
  {
    std::ifstream probe(path_, std::ios::binary);
    if (!probe.is_open()) {
      std::ofstream create(path_, std::ios::binary);
      if (!create.is_open()) return Status::IOError("cannot create transit log: " + path_);
    }
  }
  file_ = std::make_unique<std::fstream>(path_, std::ios::in | std::ios::out | std::ios::binary);
  if (!file_->is_open()) return Status::IOError("cannot open transit log: " + path_);

  std::lock_guard<std::mutex> lock(mu_);
  file_->clear();
  file_->seekg(0);
  for (;;) {
    char len_buf[4];
    file_->read(len_buf, 4);
    if (file_->gcount() < 4) break;  // clean end-of-file
    uint32_t body_len = 0;
    std::memcpy(&body_len, len_buf, 4);
    if (body_len == 0 || body_len > kTransitRecordCap) {
      DSN_LOG_WARN("transit", "implausible record length, stopping load");
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
      DSN_LOG_WARN("transit", "CRC mismatch, stopping load at a torn record");
      break;
    }
    bool is_tombstone = false;
    std::string tomb_owner, tomb_key_hash;
    auto env_or = DecodeBody(body, &is_tombstone, &tomb_owner, &tomb_key_hash);
    if (!env_or.ok()) {
      DSN_LOG_WARN("transit", "skipping undecodable record: " << env_or.status().message());
      continue;
    }
    if (is_tombstone) {
      entries_.erase(MapKey(tomb_owner, tomb_key_hash));
    } else {
      const TransitEnvelope& envelope = env_or.value();
      entries_[MapKey(envelope.owner_node, envelope.key_hash)] = envelope;
    }
  }
  file_->clear();
  return Status::OK();
}

Status TransitStore::AppendRecord(const std::string& body) {
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
  if (!file_->good()) return Status::IOError("transit log append failed");
  file_->flush();
  return Status::OK();
}

Status TransitStore::Hold(const TransitEnvelope& envelope) {
  if (envelope.owner_node.empty()) {
    return Status::InvalidArgument("transit hold requires an owner node");
  }
  if (envelope.key_hash.size() != kWalHashLen) {
    return Status::InvalidArgument("transit hold requires a 32-byte key_hash");
  }
  if (envelope.owner_node == holder_node_id_) {
    return Status::InvalidArgument("transit hold: an owner must be another node");
  }
  TransitEnvelope stored = envelope;
  stored.holder_node = holder_node_id_;
  stored.expires_ms = ttl_seconds_ == 0 ? 0 : NowMs() + static_cast<int64_t>(ttl_seconds_) * 1000;

  std::lock_guard<std::mutex> lock(mu_);
  Status st = AppendRecord(EncodeEnvelope(stored));
  if (!st.ok()) return st;
  entries_[MapKey(stored.owner_node, stored.key_hash)] = std::move(stored);
  return Status::OK();
}

Status TransitStore::MergeRemoteEnvelope(const TransitEnvelope& envelope) {
  if (envelope.owner_node.empty() || envelope.key_hash.size() != kWalHashLen) {
    return Status::InvalidArgument("transit merge requires an owner and a 32-byte key_hash");
  }

  std::lock_guard<std::mutex> lock(mu_);
  Status st = AppendRecord(EncodeEnvelope(envelope));
  if (!st.ok()) return st;
  entries_[MapKey(envelope.owner_node, envelope.key_hash)] = envelope;
  return Status::OK();
}

std::vector<TransitEnvelope> TransitStore::PendingFor(const std::string& owner_node) {
  const int64_t now = NowMs();
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<TransitEnvelope> out;
  for (const auto& [map_key, envelope] : entries_) {
    (void)map_key;
    if (envelope.owner_node != owner_node) continue;
    if (envelope.expires_ms != 0 && envelope.expires_ms <= now) continue;
    out.push_back(envelope);
  }
  return out;
}

StatusOr<TransitEnvelope> TransitStore::Lookup(const std::string& owner_node,
                                               const std::string& key_hash) {
  if (key_hash.size() != kWalHashLen) {
    return Status::InvalidArgument("transit lookup requires a 32-byte key_hash");
  }
  std::lock_guard<std::mutex> lock(mu_);
  auto it = entries_.find(MapKey(owner_node, key_hash));
  if (it == entries_.end()) return Status::NotFound("no transit envelope held for this key");
  if (it->second.expires_ms != 0 && it->second.expires_ms <= NowMs()) {
    return Status::NotFound("transit envelope has expired");
  }
  return it->second;
}

std::vector<std::string> TransitStore::Owners() {
  const int64_t now = NowMs();
  std::lock_guard<std::mutex> lock(mu_);
  std::set<std::string> owners;
  for (const auto& [map_key, envelope] : entries_) {
    (void)map_key;
    if (envelope.expires_ms != 0 && envelope.expires_ms <= now) continue;
    owners.insert(envelope.owner_node);
  }
  return std::vector<std::string>(owners.begin(), owners.end());
}

size_t TransitStore::Size() {
  const int64_t now = NowMs();
  std::lock_guard<std::mutex> lock(mu_);
  size_t count = 0;
  for (const auto& [map_key, envelope] : entries_) {
    (void)map_key;
    if (envelope.expires_ms != 0 && envelope.expires_ms <= now) continue;
    ++count;
  }
  return count;
}

uint64_t TransitStore::BytesHeld() {
  const int64_t now = NowMs();
  std::lock_guard<std::mutex> lock(mu_);
  uint64_t total = 0;
  for (const auto& [map_key, envelope] : entries_) {
    (void)map_key;
    if (envelope.expires_ms != 0 && envelope.expires_ms <= now) continue;
    total += envelope.encoded_doc.size();
  }
  return total;
}

Status TransitStore::CompactLocked() {
  // Rewrite the log with live rows only, over a sibling file, so a crash
  // mid-compaction leaves the original intact rather than a half-built log.
  const std::string tmp = path_ + ".compact";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return Status::IOError("cannot write " + tmp);
    for (const auto& [map_key, envelope] : entries_) {
      (void)map_key;
      const std::string body = EncodeEnvelope(envelope);
      const uint32_t body_len = static_cast<uint32_t>(body.size());
      const uint32_t crc = Crc32(body.data(), body.size());
      out.write(reinterpret_cast<const char*>(&body_len), 4);
      out.write(body.data(), static_cast<std::streamsize>(body.size()));
      out.write(reinterpret_cast<const char*>(&crc), 4);
    }
    out.flush();
    if (!out.good()) return Status::IOError("transit log compaction write failed");
  }
  if (file_ && file_->is_open()) file_->close();
  std::remove(path_.c_str());
  if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
    return Status::IOError("cannot commit compacted transit log: " + path_);
  }
  file_ = std::make_unique<std::fstream>(path_, std::ios::in | std::ios::out | std::ios::binary);
  if (!file_->is_open()) return Status::IOError("cannot reopen transit log: " + path_);
  return Status::OK();
}

StatusOr<uint64_t> TransitStore::ExpireAsOf(int64_t now_ms) {
  std::lock_guard<std::mutex> lock(mu_);
  uint64_t expired = 0;
  for (auto it = entries_.begin(); it != entries_.end();) {
    const TransitEnvelope& envelope = it->second;
    if (envelope.expires_ms == 0 || envelope.expires_ms > now_ms) {
      ++it;
      continue;
    }
    Status st = AppendRecord(EncodeDropBody(envelope.owner_node, envelope.key_hash));
    if (!st.ok()) return st;
    it = entries_.erase(it);
    ++expired;
  }
  if (expired > 0) {
    DSN_LOG_INFO("transit", "expired " << expired << " held envelope(s)");
    Status st = CompactLocked();
    if (!st.ok()) return st;
  }
  return expired;
}

StatusOr<size_t> TransitStore::Release(const std::vector<std::string>& key_hashes) {
  if (key_hashes.empty()) return size_t{0};
  const std::set<std::string> wanted(key_hashes.begin(), key_hashes.end());
  std::lock_guard<std::mutex> lock(mu_);
  size_t released = 0;
  for (auto it = entries_.begin(); it != entries_.end();) {
    const TransitEnvelope& envelope = it->second;
    if (wanted.count(envelope.key_hash) == 0) {
      ++it;
      continue;
    }
    Status st = AppendRecord(EncodeDropBody(envelope.owner_node, envelope.key_hash));
    if (!st.ok()) return st;
    it = entries_.erase(it);
    ++released;
  }
  if (released > 0) {
    DSN_LOG_INFO("transit", "released " << released << " settled envelope(s) after checkpoint");
    Status st = CompactLocked();
    if (!st.ok()) return st;
  }
  return released;
}

}  // namespace desentry
