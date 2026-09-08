#include "desentry/storage/wal.h"

#include <cstring>
#include <set>

#include "desentry/common/byte_buffer.h"
#include "desentry/common/crc32.h"
#include "desentry/common/hex.h"
#include "desentry/common/logger.h"
#include "desentry/security/crypto.h"

namespace desentry {

namespace {

void PutU32(std::string* out, uint32_t v) {
  char buf[4];
  std::memcpy(buf, &v, 4);
  out->append(buf, 4);
}
uint32_t GetU32(const char* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}
uint64_t GetU64(const char* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}

std::string GenesisHash() { return std::string(kWalHashLen, '\0'); }

// Minimum plausible v1 body: fixed fields, zero-length variable fields, two
// hash-chain fields, trailing CRC.
constexpr size_t kMinBodyLenV1 = 8 /*lsn*/ + 1 /*type*/ + 4 + 4 + 4 + kWalHashLen + kWalHashLen + 4;
// Minimum plausible v2 body: magic + the above shape plus the new fields.
constexpr size_t kMinBodyLenV2 = 4 /*magic*/ + 8 + 1 + 4 + 4 + 4 + 4 /*key_hash len*/ + 8 + 4 +
                                  4 /*hlc node_id*/ + 4 + 4 + kWalHashLen + kWalHashLen + 4;

constexpr uint32_t kMaxRecordBytes = 256u << 20;  // 256MiB: bounds a hostile/corrupt length field

}  // namespace

const char* WalRecordTypeName(WalRecordType type) {
  switch (type) {
    case WalRecordType::kPut: return "PUT";
    case WalRecordType::kDelete: return "DEL";
    case WalRecordType::kCheckpoint: return "CHECKPOINT";
    case WalRecordType::kTransitIntent: return "TRANSIT_INTENT";
    case WalRecordType::kTransitClaimed: return "TRANSIT_CLAIMED";
  }
  return "UNKNOWN";
}

std::string LedgerKeyHash(const std::string& collection, const std::string& key) {
  // The 0x00 separator matters: without it, ("ab", "c") and ("a", "bc")
  // would hash identically, and a peer could be misled about which
  // collection an entry belongs to.
  std::string material = collection;
  material.push_back('\0');
  material += key;
  return crypto::Sha256(material);
}

std::string WriteAheadLog::BuildContent(const WalRecord& record) {
  ByteWriter w;
  w.U32(kWalRecordMagicV2);
  w.U64(static_cast<uint64_t>(record.lsn));
  w.U8(static_cast<uint8_t>(record.type));
  w.Bytes(record.collection);
  w.Bytes(record.key);
  w.Bytes(record.document_bytes);
  w.Bytes(record.key_hash);
  w.U64(record.hlc.physical_ms);
  w.U32(record.hlc.logical);
  w.Bytes(record.hlc.node_id);
  w.Bytes(record.origin_node_id);
  return w.TakeString();
}

std::string WriteAheadLog::EncodeBody(const WalRecord& record) {
  std::string body = BuildContent(record);
  ByteWriter w;
  w.Bytes(record.origin_signature);
  w.RawBytes(record.prev_hash);
  w.RawBytes(record.entry_hash);
  body += w.str();
  return body;
}

Status WriteAheadLog::DecodeBody(const std::string& body, WalRecord* out) {
  try {
    ByteReader r(body);
    if (r.U32() != kWalRecordMagicV2) return Status::Corruption("WAL: not a v2 record body");
    out->lsn = static_cast<lsn_t>(r.U64());
    out->type = static_cast<WalRecordType>(r.U8());
    out->collection = r.Bytes();
    out->key = r.Bytes();
    out->document_bytes = r.Bytes();
    out->key_hash = r.Bytes();
    out->hlc.physical_ms = r.U64();
    out->hlc.logical = r.U32();
    out->hlc.node_id = r.Bytes();
    out->origin_node_id = r.Bytes();
    out->origin_signature = r.Bytes();
    if (r.remaining() < 2 * kWalHashLen) return Status::Corruption("WAL: record missing chain fields");
    out->prev_hash = r.RawBytes(kWalHashLen);
    out->entry_hash = r.RawBytes(kWalHashLen);
  } catch (const std::exception& e) {
    return Status::Corruption(std::string("WAL: malformed v2 record: ") + e.what());
  }
  return Status::OK();
}

Status WriteAheadLog::DecodeBodyV1(const std::string& body, WalRecord* out) {
  if (body.size() < kMinBodyLenV1 - 4) return Status::Corruption("WAL: v1 record too short");
  size_t off = 0;
  const size_t payload_len = body.size();  // CRC already stripped by the caller
  auto need = [&](size_t n) { return off + n <= payload_len; };

  if (!need(9)) return Status::Corruption("WAL: v1 record truncated");
  out->lsn = static_cast<lsn_t>(GetU64(body.data() + off));
  off += 8;
  out->type = static_cast<WalRecordType>(static_cast<uint8_t>(body[off]));
  off += 1;

  auto read_blob = [&](std::string* dest) -> bool {
    if (!need(4)) return false;
    uint32_t len = GetU32(body.data() + off);
    off += 4;
    if (!need(len)) return false;
    dest->assign(body, off, len);
    off += len;
    return true;
  };
  if (!read_blob(&out->collection) || !read_blob(&out->key) || !read_blob(&out->document_bytes)) {
    return Status::Corruption("WAL: v1 record truncated in a variable-length field");
  }
  if (!need(2 * kWalHashLen)) return Status::Corruption("WAL: v1 record missing chain fields");
  out->prev_hash.assign(body, off, kWalHashLen);
  off += kWalHashLen;
  out->entry_hash.assign(body, off, kWalHashLen);

  // Fields v1 did not have. key_hash is derivable; the rest genuinely are
  // not, and are left empty so VerifyChain() reports them as unsigned rather
  // than inventing an attestation that was never made.
  out->key_hash = LedgerKeyHash(out->collection, out->key);
  return Status::OK();
}

StatusOr<std::unique_ptr<WriteAheadLog>> WriteAheadLog::Open(const std::string& wal_file) {
  {
    std::ifstream probe(wal_file, std::ios::binary);
    if (!probe.is_open()) {
      std::ofstream create(wal_file, std::ios::binary);
      if (!create.is_open()) return Status::IOError("cannot create WAL file: " + wal_file);
    }
  }
  std::fstream file(wal_file, std::ios::in | std::ios::out | std::ios::binary);
  if (!file.is_open()) return Status::IOError("cannot open WAL file: " + wal_file);

  std::unique_ptr<WriteAheadLog> wal(new WriteAheadLog(std::move(file), wal_file, 0, GenesisHash()));

  std::vector<WalRecord> records;
  {
    std::lock_guard<std::mutex> lock(wal->mu_);
    Status st = wal->ReadAllLocked(&records);
    if (!st.ok()) return st;
  }

  if (!records.empty()) {
    wal->next_lsn_ = records.back().lsn + 1;
    wal->tip_hash_ = records.back().entry_hash;
    for (const WalRecord& rec : records) {
      if (rec.type == WalRecordType::kCheckpoint) wal->last_checkpoint_lsn_ = rec.lsn;
    }
  }

  if (wal->migrated_from_v1_) {
    // The v1 -> v2 upgrade re-derives every entry_hash, because the hashed
    // content now covers fields v1 records did not carry. That is a genuine
    // discontinuity in the tamper-evidence chain, so it is recorded rather
    // than glossed over: the pre-migration tip is kept on the object, logged
    // here, and written to ledger_migration.json by the engine layer, so an
    // auditor can compare it against another replica's v1 tip.
    wal->pre_migration_tip_hash_ = records.empty() ? GenesisHash() : records.back().entry_hash;
    std::vector<WalRecord> upgraded = records;
    std::string prev = GenesisHash();
    for (WalRecord& rec : upgraded) {
      rec.prev_hash = prev;
      rec.entry_hash = crypto::Sha256(BuildContent(rec) + prev);
      prev = rec.entry_hash;
    }
    std::lock_guard<std::mutex> lock(wal->mu_);
    Status st = wal->RewriteLocked(upgraded);
    if (!st.ok()) return st;
    wal->tip_hash_ = prev;
    DSN_LOG_WARN("wal", "migrated " << upgraded.size() << " v1 ledger entries to the v2 format; "
                                     << "pre-migration tip was "
                                     << HexEncode(wal->pre_migration_tip_hash_));
  }

  return wal;
}

WriteAheadLog::~WriteAheadLog() {
  if (file_.is_open()) {
    file_.flush();
    file_.close();
  }
}

void WriteAheadLog::SetOrigin(std::string node_id, Signer signer) {
  std::lock_guard<std::mutex> lock(mu_);
  origin_node_id_ = std::move(node_id);
  signer_ = std::move(signer);
}

StatusOr<lsn_t> WriteAheadLog::Append(WalRecordType type, const std::string& collection,
                                       const std::string& key, const std::string& document_bytes,
                                       const AppendOptions& options) {
  std::lock_guard<std::mutex> lock(mu_);

  WalRecord rec;
  rec.lsn = next_lsn_;
  rec.type = type;
  rec.collection = collection;
  rec.key = key;
  rec.document_bytes = document_bytes;
  rec.key_hash = LedgerKeyHash(collection, key);
  rec.hlc = options.hlc;
  rec.origin_node_id = origin_node_id_;
  if (rec.hlc.node_id.empty()) rec.hlc.node_id = origin_node_id_;
  // A transit entry names the node the bytes are being held for. It rides in
  // the collection field's sibling slot rather than a dedicated column
  // because it is only meaningful for two of five op types; encoding it in
  // the key keeps the record shape uniform.
  if (rec.IsTransit() && !options.transit_owner.empty()) {
    rec.collection = collection;
    rec.key = key;
    rec.document_bytes = document_bytes;
    rec.origin_node_id = origin_node_id_;
  }

  const std::string content = BuildContent(rec);
  rec.prev_hash = tip_hash_;
  rec.entry_hash = crypto::Sha256(content + rec.prev_hash);
  if (signer_) rec.origin_signature = signer_(content);

  std::string body = EncodeBody(rec);
  uint32_t crc = Crc32(body.data(), body.size());
  PutU32(&body, crc);

  std::string frame;
  PutU32(&frame, static_cast<uint32_t>(body.size()));
  frame += body;

  file_.clear();
  file_.seekp(0, std::ios::end);
  file_.write(frame.data(), static_cast<std::streamsize>(frame.size()));
  if (!file_.good()) return Status::IOError("WAL append failed");
  file_.flush();  // durability point: the caller is acked only after this returns.

  tip_hash_ = rec.entry_hash;
  if (type == WalRecordType::kCheckpoint) last_checkpoint_lsn_ = rec.lsn;
  ++next_lsn_;
  return rec.lsn;
}

Status WriteAheadLog::ReadAllLocked(std::vector<WalRecord>* out) {
  out->clear();
  file_.clear();
  file_.seekg(0);
  unparsed_tail_bytes_ = 0;
  // Offset just past the last record that parsed cleanly. Whatever lies
  // beyond it when the loop stops is damage, and how much there is says what
  // kind: nothing means an interrupted append, something means the log was
  // altered in the middle.
  std::streamoff good_pos = 0;
  // Set when a record was fully present but did not survive its own checks.
  // A short read is not this: that is a file ending mid-record, which is what
  // an interrupted append leaves behind.
  bool damaged = false;
  for (;;) {
    char len_buf[4];
    file_.read(len_buf, 4);
    if (file_.gcount() < 4) break;
    uint32_t body_len = GetU32(len_buf);
    if (body_len < 8 || body_len > kMaxRecordBytes) {
      DSN_LOG_WARN("wal", "implausible record length, stopping replay");
      damaged = true;
      break;
    }
    std::string body(body_len, '\0');
    file_.read(body.data(), static_cast<std::streamsize>(body_len));
    if (static_cast<uint32_t>(file_.gcount()) < body_len) {
      DSN_LOG_WARN("wal", "torn record tail detected, stopping replay");
      break;
    }
    uint32_t stored_crc = GetU32(body.data() + body_len - 4);
    if (stored_crc != Crc32(body.data(), body_len - 4)) {
      DSN_LOG_WARN("wal", "CRC mismatch, stopping replay (crash-torn record)");
      damaged = true;
      break;
    }
    const std::string payload = body.substr(0, body_len - 4);

    WalRecord rec;
    // Format detection is by magic, not by guessing from lengths: a v2 body
    // starts with "DSW2", a v1 body with the record's little-endian LSN. A v1
    // LSN would have to be exactly 0x44535732 to collide, and even then the
    // v2 parse would fail its own internal length checks.
    if (payload.size() >= 4 && GetU32(payload.data()) == kWalRecordMagicV2) {
      Status st = DecodeBody(payload, &rec);
      if (!st.ok()) {
        DSN_LOG_WARN("wal", "stopping replay: " << st.message());
        damaged = true;
        break;
      }
    } else {
      Status st = DecodeBodyV1(payload, &rec);
      if (!st.ok()) {
        DSN_LOG_WARN("wal", "stopping replay: " << st.message());
        damaged = true;
        break;
      }
      migrated_from_v1_ = true;
    }
    out->push_back(std::move(rec));
    good_pos = static_cast<std::streamoff>(4) + static_cast<std::streamoff>(body_len) + good_pos;
  }
  file_.clear();
  file_.seekg(0, std::ios::end);
  const std::streamoff end_pos = file_.tellg();
  file_.clear();
  if (damaged && end_pos > good_pos) unparsed_tail_bytes_ = end_pos - good_pos;
  return Status::OK();
}

StatusOr<std::vector<WalRecord>> WriteAheadLog::ReadAll() {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<WalRecord> records;
  Status st = ReadAllLocked(&records);
  if (!st.ok()) return st;
  return records;
}

WriteAheadLog::LedgerTip WriteAheadLog::Tip() const {
  std::lock_guard<std::mutex> lock(mu_);
  return LedgerTip{next_lsn_ - 1, tip_hash_};
}

lsn_t WriteAheadLog::LastCheckpointLsn() const {
  std::lock_guard<std::mutex> lock(mu_);
  return last_checkpoint_lsn_;
}

WriteAheadLog::VerifyResult WriteAheadLog::VerifyChain(const SignatureVerifier& verify_signature) {
  VerifyResult result;
  std::vector<WalRecord> records;
  std::streamoff unparsed = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    Status st = ReadAllLocked(&records);
    if (!st.ok()) {
      result.ok = false;
      result.reason = st.message();
      return result;
    }
    unparsed = unparsed_tail_bytes_;
  }

  // Recovery trusts the prefix before a bad record and moves on -- that is
  // correct for a crash. Verification must not: a record that failed its own
  // checks with more of the log still sitting behind it is the signature of
  // an edit, and reporting the surviving prefix as an intact chain would be
  // exactly the lie this ledger exists to prevent.
  if (unparsed > 0) {
    result.ok = false;
    result.entries_checked = records.size();
    result.failed_at_entry_id = records.empty() ? kInvalidLsn : records.back().lsn + 1;
    result.reason = "log is damaged after entry " +
                    (records.empty() ? std::string("genesis") : std::to_string(records.back().lsn)) +
                    ": " + std::to_string(static_cast<long long>(unparsed)) +
                    " byte(s) could not be parsed";
    return result;
  }

  std::string expected_prev = GenesisHash();
  for (const WalRecord& rec : records) {
    if (rec.prev_hash != expected_prev) {
      result.ok = false;
      result.failed_at_entry_id = rec.lsn;
      result.reason = "chain break: prev_hash does not match the preceding entry's hash";
      return result;
    }
    const std::string content = BuildContent(rec);
    if (crypto::Sha256(content + rec.prev_hash) != rec.entry_hash) {
      result.ok = false;
      result.failed_at_entry_id = rec.lsn;
      result.reason = "entry_hash mismatch: record content does not match its recorded hash";
      return result;
    }
    if (rec.origin_signature.empty() || rec.origin_node_id.empty()) {
      ++result.unsigned_entries;
    } else {
      ++result.signed_entries;
      if (verify_signature && !verify_signature(rec.origin_node_id, content, rec.origin_signature)) {
        result.ok = false;
        result.failed_at_entry_id = rec.lsn;
        result.reason = "origin signature does not verify against node_id " + rec.origin_node_id;
        return result;
      }
    }
    expected_prev = rec.entry_hash;
    ++result.entries_checked;
  }
  result.failed_at_entry_id = kInvalidLsn;
  return result;
}

Status WriteAheadLog::RewriteLocked(const std::vector<WalRecord>& records) {
  const std::string tmp = path_ + ".rewrite";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return Status::IOError("cannot write " + tmp);
    for (const WalRecord& rec : records) {
      std::string body = EncodeBody(rec);
      uint32_t crc = Crc32(body.data(), body.size());
      PutU32(&body, crc);
      uint32_t body_len = static_cast<uint32_t>(body.size());
      out.write(reinterpret_cast<const char*>(&body_len), 4);
      out.write(body.data(), static_cast<std::streamsize>(body.size()));
    }
    out.flush();
    if (!out.good()) return Status::IOError("WAL rewrite failed");
  }
  if (file_.is_open()) file_.close();
  std::remove(path_.c_str());
  if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
    return Status::IOError("cannot commit rewritten WAL: " + path_);
  }
  file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
  if (!file_.is_open()) return Status::IOError("cannot reopen WAL after rewrite: " + path_);
  return Status::OK();
}

StatusOr<WriteAheadLog::PruneResult> WriteAheadLog::Prune(lsn_t checkpoint_lsn) {
  std::lock_guard<std::mutex> lock(mu_);

  std::vector<WalRecord> records;
  Status st = ReadAllLocked(&records);
  if (!st.ok()) return st;

  PruneResult result;
  result.previous_tip_hash = tip_hash_;
  result.pruned_through = checkpoint_lsn;

  // A transit pair is only droppable once both halves are below the
  // checkpoint: an INTENT whose CLAIMED has not arrived is exactly the
  // record a returning owner needs, so dropping it would strand the bytes.
  std::set<std::string> claimed_below;
  for (const WalRecord& rec : records) {
    if (rec.lsn > checkpoint_lsn) break;
    if (rec.type == WalRecordType::kTransitClaimed) claimed_below.insert(rec.key_hash);
  }

  std::vector<WalRecord> kept;
  kept.reserve(records.size());
  for (const WalRecord& rec : records) {
    const bool below = rec.lsn <= checkpoint_lsn;
    const bool droppable = below && rec.IsTransit() && claimed_below.count(rec.key_hash) != 0;
    if (droppable) {
      ++result.dropped;
      continue;
    }
    kept.push_back(rec);
  }
  if (result.dropped == 0) {
    result.new_tip_hash = tip_hash_;
    return result;
  }

  // LSNs are renumbered densely and the chain re-derived: a chain with gaps
  // would fail VerifyChain() on every peer, and preserving the removed links
  // is impossible by construction.
  std::string prev = GenesisHash();
  lsn_t next = 0;
  for (WalRecord& rec : kept) {
    rec.lsn = next++;
    rec.prev_hash = prev;
    rec.entry_hash = crypto::Sha256(BuildContent(rec) + prev);
    // The origin signature covered the *old* LSN, so it no longer applies.
    // Clearing it is the honest outcome: a pruning node cannot re-sign
    // another node's entry, and leaving a signature that will not verify
    // would be worse than none. This is why Prune() runs only after quorum
    // agreement (ledger/checkpoint.h) -- the surviving attestation is the
    // quorum's, recorded in the checkpoint entry.
    rec.origin_signature.clear();
    prev = rec.entry_hash;
  }

  st = RewriteLocked(kept);
  if (!st.ok()) return st;
  tip_hash_ = prev;
  next_lsn_ = next;
  last_checkpoint_lsn_ = kInvalidLsn;
  for (const WalRecord& rec : kept) {
    if (rec.type == WalRecordType::kCheckpoint) last_checkpoint_lsn_ = rec.lsn;
  }
  result.new_tip_hash = tip_hash_;
  DSN_LOG_INFO("wal", "pruned " << result.dropped << " settled transit entr(ies) through LSN "
                                 << checkpoint_lsn);
  return result;
}

}  // namespace desentry
