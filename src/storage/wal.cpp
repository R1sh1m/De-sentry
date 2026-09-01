#include "desentry/storage/wal.h"

#include <cstring>

#include "desentry/common/crc32.h"
#include "desentry/common/logger.h"
#include "desentry/security/crypto.h"

namespace desentry {

namespace {

void PutU32(std::string* out, uint32_t v) {
  char buf[4];
  std::memcpy(buf, &v, 4);
  out->append(buf, 4);
}
void PutU64(std::string* out, uint64_t v) {
  char buf[8];
  std::memcpy(buf, &v, 8);
  out->append(buf, 8);
}
uint32_t GetU32(const char* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint64_t GetU64(const char* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

std::string GenesisHash() { return std::string(kWalHashLen, '\0'); }

// Minimum possible body size: the fixed-width fields plus zero-length
// variable fields, plus the two 32-byte hash-chain fields, plus the 4-byte
// trailing CRC. Anything shorter than this can't be a well-formed record.
constexpr size_t kMinBodyLen = 8 /*lsn*/ + 1 /*type*/ + 4 /*coll_len*/ + 4 /*key_len*/ + 4 /*doc_len*/ +
                                kWalHashLen /*prev_hash*/ + kWalHashLen /*entry_hash*/ + 4 /*crc*/;

}  // namespace

std::string WriteAheadLog::BuildContent(lsn_t lsn, WalRecordType type, const std::string& collection,
                                         const std::string& key, const std::string& document_bytes) {
  std::string content;
  PutU64(&content, static_cast<uint64_t>(lsn));
  content.push_back(static_cast<char>(type));
  PutU32(&content, static_cast<uint32_t>(collection.size()));
  content += collection;
  PutU32(&content, static_cast<uint32_t>(key.size()));
  content += key;
  PutU32(&content, static_cast<uint32_t>(document_bytes.size()));
  content += document_bytes;
  return content;
}

StatusOr<std::unique_ptr<WriteAheadLog>> WriteAheadLog::Open(const std::string& wal_file) {
  {
    std::ifstream probe(wal_file, std::ios::binary);
    if (!probe.is_open()) {
      std::ofstream create(wal_file, std::ios::binary);
      if (!create.is_open()) return Status::IOError("cannot create WAL file: " + wal_file);
    }
  }
  std::fstream file(wal_file, std::ios::in | std::ios::out | std::ios::binary | std::ios::ate);
  if (!file.is_open()) return Status::IOError("cannot open WAL file: " + wal_file);

  // Construct with placeholder chain state, then run a full parse (via the
  // same ReadAll() logic used for recovery) to establish both the next LSN
  // and the current chain tip -- one scan, one source of truth for both,
  // rather than two divergent parsing paths.
  std::unique_ptr<WriteAheadLog> wal(new WriteAheadLog(std::move(file), wal_file, 0, GenesisHash()));
  auto records_or = wal->ReadAll();
  if (!records_or.ok()) return records_or.status();
  auto& records = records_or.value();
  if (!records.empty()) {
    wal->next_lsn_ = records.back().lsn + 1;
    wal->tip_hash_ = records.back().entry_hash;
  }
  return wal;
}

WriteAheadLog::~WriteAheadLog() {
  if (file_.is_open()) {
    file_.flush();
    file_.close();
  }
}

StatusOr<lsn_t> WriteAheadLog::Append(WalRecordType type, const std::string& collection,
                                       const std::string& key, const std::string& document_bytes) {
  std::lock_guard<std::mutex> lock(mu_);
  lsn_t lsn = next_lsn_++;

  std::string content = BuildContent(lsn, type, collection, key, document_bytes);
  const std::string& prev_hash = tip_hash_;
  std::string entry_hash = crypto::Sha256(content + prev_hash);

  std::string body = content;
  body += prev_hash;
  body += entry_hash;
  uint32_t crc = Crc32(body.data(), body.size());
  PutU32(&body, crc);

  std::string frame;
  PutU32(&frame, static_cast<uint32_t>(body.size()));
  frame += body;

  file_.clear();
  file_.seekp(0, std::ios::end);
  file_.write(frame.data(), static_cast<std::streamsize>(frame.size()));
  if (!file_.good()) {
    return Status::IOError("WAL append failed");
  }
  file_.flush();  // durability point: caller is acked only after this returns.
  tip_hash_ = std::move(entry_hash);
  return lsn;
}

StatusOr<std::vector<WalRecord>> WriteAheadLog::ReadAll() {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<WalRecord> records;
  file_.clear();
  file_.seekg(0);
  while (true) {
    char len_buf[4];
    file_.read(len_buf, 4);
    if (file_.gcount() < 4) break;
    uint32_t body_len = GetU32(len_buf);
    std::string body(body_len, '\0');
    file_.read(body.data(), static_cast<std::streamsize>(body_len));
    if (static_cast<uint32_t>(file_.gcount()) < body_len) {
      DSN_LOG_WARN("wal", "torn record tail detected, stopping replay");
      break;
    }
    if (body_len < kMinBodyLen) {
      DSN_LOG_WARN("wal", "malformed record (too short), stopping replay");
      break;
    }
    uint32_t stored_crc = GetU32(body.data() + body_len - 4);
    uint32_t actual_crc = Crc32(body.data(), body_len - 4);
    if (stored_crc != actual_crc) {
      DSN_LOG_WARN("wal", "CRC mismatch, stopping replay (crash-torn record)");
      break;
    }
    size_t off = 0;
    WalRecord rec;
    rec.lsn = static_cast<lsn_t>(GetU64(body.data() + off)); off += 8;
    rec.type = static_cast<WalRecordType>(body[off]); off += 1;
    uint32_t coll_len = GetU32(body.data() + off); off += 4;
    rec.collection = body.substr(off, coll_len); off += coll_len;
    uint32_t key_len = GetU32(body.data() + off); off += 4;
    rec.key = body.substr(off, key_len); off += key_len;
    uint32_t doc_len = GetU32(body.data() + off); off += 4;
    rec.document_bytes = body.substr(off, doc_len); off += doc_len;
    rec.prev_hash = body.substr(off, kWalHashLen); off += kWalHashLen;
    rec.entry_hash = body.substr(off, kWalHashLen); off += kWalHashLen;
    records.push_back(std::move(rec));
  }
  file_.clear();
  return records;
}

WriteAheadLog::LedgerTip WriteAheadLog::Tip() const {
  std::lock_guard<std::mutex> lock(mu_);
  return LedgerTip{next_lsn_ - 1, tip_hash_};
}

WriteAheadLog::VerifyResult WriteAheadLog::VerifyChain() {
  VerifyResult result;
  auto records_or = ReadAll();
  if (!records_or.ok()) {
    result.ok = false;
    result.reason = records_or.status().message();
    return result;
  }
  std::string expected_prev = GenesisHash();
  for (auto& rec : records_or.value()) {
    if (rec.prev_hash != expected_prev) {
      result.ok = false;
      result.failed_at_entry_id = rec.lsn;
      result.reason = "chain break: prev_hash does not match the preceding entry's hash";
      return result;
    }
    std::string content = BuildContent(rec.lsn, rec.type, rec.collection, rec.key, rec.document_bytes);
    std::string recomputed = crypto::Sha256(content + rec.prev_hash);
    if (recomputed != rec.entry_hash) {
      result.ok = false;
      result.failed_at_entry_id = rec.lsn;
      result.reason = "entry_hash mismatch: record content does not match its recorded hash";
      return result;
    }
    expected_prev = rec.entry_hash;
    result.entries_checked++;
  }
  result.failed_at_entry_id = kInvalidLsn;
  return result;
}

}  // namespace desentry
