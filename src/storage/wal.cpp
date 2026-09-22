#include "desentry/storage/wal.h"

#include <cstring>
#include <set>

#include "desentry/common/byte_buffer.h"
#include "desentry/common/crc32.h"
#include "desentry/common/hex.h"
#include "desentry/common/logger.h"
#include "desentry/common/platform.h"
#include "desentry/security/at_rest.h"
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

// Best-effort high-water-mark update (C-6): records (tip id, tip hash) in
// <wal>.hwm so a future Open can detect truncation. Never fails the write:
// a stale mark is merely a weaker check, while a failed append is data loss.
void WriteHwmBestEffort(const std::string& wal_file, lsn_t tip_id, const std::string& tip_hash) {
  const std::string hwm_path = wal_file + ".hwm";
  const std::string tmp = hwm_path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return;
    char buf[8 + 32];
    int64_t id = tip_id;
    std::memcpy(buf, &id, 8);
    std::memcpy(buf + 8, tip_hash.data(), 32);
    out.write(buf, sizeof(buf));
    out.flush();
    if (!out.good()) return;
  }
  std::string sync_err;
  SyncFileByPath(tmp, &sync_err);
  // remove() first: std::rename overwrites atomically on POSIX but fails on
  // Windows when the destination exists, which silently pinned the mark
  // stale (and hid real truncation logic bugs behind a best-effort write).
  std::remove(hwm_path.c_str());
  if (std::rename(tmp.c_str(), hwm_path.c_str()) != 0) return;
  SyncDirForFile(hwm_path, &sync_err);
}

// Minimum plausible v1 body: fixed fields, zero-length variable fields, two
// hash-chain fields, trailing CRC.
[[maybe_unused]] constexpr size_t kMinBodyLenV1 = 8 /*lsn*/ + 1 /*type*/ + 4 + 4 + 4 + kWalHashLen + kWalHashLen + 4;
// Minimum plausible v2 body: magic + the above shape plus the new fields.
[[maybe_unused]] constexpr size_t kMinBodyLenV2 = 4 /*magic*/ + 8 + 1 + 4 + 4 + 4 + 4 /*key_hash len*/ + 8 + 4 +
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
    case WalRecordType::kAbort: return "ABORT";
    case WalRecordType::kAcl: return "ACL";
  }
  return "UNKNOWN";
}

std::string LedgerKeyHash(const std::string& collection, const std::string& key) {
  // The 0x00 separator matters: without it, ("ab", "c") and ("a", "bc")
  // would hash identically, and a peer could be misled about which
  // collection an entry belongs to.
  //
  // Honest limit (M-1): this is an unsalted SHA-256, deliberately shared so
  // every peer computes the same hash without a key exchange. Anyone seeing
  // a key_hash -- including non-readers, who legitimately receive hashes for
  // convergence -- can test guesses offline. High-entropy keys (UUIDs,
  // random ids) are safe; guessable keyspaces (user ids, emails, invoice
  // numbers, users/<name>) are recoverable by dictionary attack. The
  // "names bytes without disclosing them" property holds only for
  // high-entropy keys. Per-collection HMAC keys shared among readers would
  // close this but need a key-distribution story that does not exist yet;
  // until then, treat key names in low-entropy spaces as visible to the mesh
  // and choose keying accordingly.
  std::string material = collection;
  material.push_back('\0');
  material += key;
  return crypto::Sha256(material);
}

std::string TransitChunkKeyHash(const std::string& doc_key_hash, uint32_t chunk_index) {
  // Same separator discipline as LedgerKeyHash: the index is fixed-width
  // big-endian so ("hash", 1) and ("hash\x01", ...) can never collide, and
  // chunk 0 of a striped document hashes differently from the whole-doc
  // hash, so a chunked intent can never match a whole-document claim.
  std::string material = doc_key_hash;
  material.push_back('\0');
  for (int shift = 24; shift >= 0; shift -= 8) {
    material.push_back(static_cast<char>((chunk_index >> shift) & 0xFF));
  }
  return crypto::Sha256(material);
}

void WriteAheadLog::WriteHwmIfDueLocked(lsn_t tip_id, const std::string& tip_hash) {
  // A trailing mark is sound: truncation *below* the mark is still caught,
  // and crash-torn tails above it replay benignly. The mark must only never
  // run AHEAD of the durable tip, which throttling preserves.
  const int64_t now = MonotonicMs();
  if (tip_id < hwm_saved_lsn_ + 64 && now < hwm_saved_ms_ + 1000) return;
  WriteHwmBestEffort(path_, tip_id, tip_hash);
  hwm_saved_lsn_ = tip_id;
  hwm_saved_ms_ = now;
}

std::string WriteAheadLog::BuildContentLegacy(const WalRecord& record) {
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

std::string WriteAheadLog::BuildContent(const WalRecord& record) {
  // Position-bound content (C-6 fix): v3 magic plus prev_hash inside the
  // signed bytes, so a signature cannot be transplanted onto a re-chained
  // history.
  std::string legacy = BuildContentLegacy(record);
  // Swap the v2 magic for v3 (first 4 bytes, little-endian on all supported
  // hosts -- both magics are written with the same host-order U32, so a
  // byte-swap keeps them comparable).
  std::string content = legacy;
  std::memcpy(content.data(), &kWalRecordMagicV3, 4);
  content += record.prev_hash;
  return content;
}

std::string WriteAheadLog::EncodeBody(const WalRecord& record) {
  // The content's own magic (v2 vs v3) records which layout this record
  // uses, so decoding knows where the signature starts (v3 content ends
  // with 32 bytes of prev_hash).
  std::string body = record.position_bound ? BuildContent(record) : BuildContentLegacy(record);
  ByteWriter w;
  w.Bytes(record.origin_signature);
  w.RawBytes(record.prev_hash);
  w.RawBytes(record.entry_hash);
  body += w.str();
  // Unsigned envelope extension for transit intents (see WalRecord): holder
  // routing metadata outside the signed content, so old readers verify the
  // chain exactly as before and ignore the tail. Non-transit records carry
  // no tail -- their shape is byte-identical to before this change.
  if (record.type == WalRecordType::kTransitIntent) {
    ByteWriter tail;
    tail.Bytes(record.transit_holder);
    tail.U64(record.transit_size_bytes);
    tail.U32(record.transit_chunk_index);
    tail.U32(record.transit_chunk_total);
    body += tail.TakeString();
  }
  return body;
}

Status WriteAheadLog::DecodeBody(const std::string& body, WalRecord* out) {
  try {
    ByteReader r(body);
    const uint32_t magic = r.U32();
    if (magic != kWalRecordMagicV2 && magic != kWalRecordMagicV3) {
      return Status::Corruption("WAL: not a v2/v3 record body");
    }
    out->position_bound = (magic == kWalRecordMagicV3);
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
    // v3 content ends with the chain's prev_hash inside the signed bytes.
    std::string content_prev;
    if (out->position_bound) content_prev = r.RawBytes(kWalHashLen);
    out->origin_signature = r.Bytes();
    if (r.remaining() < 2 * kWalHashLen) return Status::Corruption("WAL: record missing chain fields");
    out->prev_hash = r.RawBytes(kWalHashLen);
    if (out->position_bound && out->prev_hash != content_prev) {
      return Status::Corruption("WAL: v3 content prev_hash does not match chain prev_hash");
    }
    out->entry_hash = r.RawBytes(kWalHashLen);
    // Unsigned transit tail, if present. Records written before the tail
    // existed simply have no bytes left: the defaults (unknown holder,
    // whole document) apply. A partial tail is corruption, not an old
    // record -- the tail is written atomically with the record.
    if (r.remaining() > 0) {
      if (out->type != WalRecordType::kTransitIntent) {
        return Status::Corruption("WAL: non-transit record has trailing bytes");
      }
      out->transit_holder = r.Bytes();
      out->transit_size_bytes = r.U64();
      out->transit_chunk_index = r.U32();
      out->transit_chunk_total = r.U32();
      if (out->transit_chunk_total == 0) {
        return Status::Corruption("WAL: transit intent has zero chunk total");
      }
      if (out->transit_chunk_index >= out->transit_chunk_total) {
        return Status::Corruption("WAL: transit intent chunk index out of range");
      }
    }
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

Status WriteAheadLog::DetectMode(const std::string& wal_file, bool have_key, bool* sealed) {
  *sealed = false;
  std::ifstream in(wal_file, std::ios::binary);
  if (!in.is_open()) return Status::OK();  // created empty below: new log in the current mode
  char len_buf[4];
  in.read(len_buf, 4);
  if (in.gcount() < 4) return Status::OK();  // empty file: new log
  const uint32_t body_len = GetU32(len_buf);
  if (body_len < 8 || body_len > kMaxRecordBytes) {
    return Status::Corruption("WAL: implausible first record length -- file is not a ledger");
  }
  std::string head(std::min<size_t>(body_len, 64), '\0');
  in.read(head.data(), static_cast<std::streamsize>(head.size()));
  if (static_cast<size_t>(in.gcount()) < head.size()) return Status::OK();  // torn tail: mode unknown
  // The sealed magic sits at the payload start; CRC occupies the last 4
  // bytes, so require at least magic + CRC before calling it sealed.
  *sealed = head.size() >= 8 && at_rest::LooksSealedRecord(head);
  if (*sealed && !have_key) {
    return Status::Corruption("at-rest: sealed ledger " + wal_file +
                              " opened without encryption (missing unlock key?)");
  }
  if (!*sealed && have_key) {
    return Status::Corruption("at-rest: plaintext ledger " + wal_file +
                              " opened with encryption enabled; migrate it with "
                              "`desentryd --re-encrypt`");
  }
  return Status::OK();
}

StatusOr<std::unique_ptr<WriteAheadLog>> WriteAheadLog::Open(const std::string& wal_file,
                                                             const std::string& dek) {
  {
    std::ifstream probe(wal_file, std::ios::binary);
    if (!probe.is_open()) {
      std::ofstream create(wal_file, std::ios::binary);
      if (!create.is_open()) return Status::IOError("cannot create WAL file: " + wal_file);
      create.flush();
      create.close();
      // Ensure the new file's directory entry is durable (C-1).
      std::string dir_err;
      SyncDirForFile(wal_file, &dir_err);
    }
  }
  bool sealed = false;
  Status mode_st = DetectMode(wal_file, !dek.empty(), &sealed);
  if (!mode_st.ok()) return mode_st;
  if (!dek.empty() && dek.size() != 32) {
    return Status::InvalidArgument("at-rest: DEK must be 32 bytes");
  }
  std::fstream file(wal_file, std::ios::in | std::ios::out | std::ios::binary);
  if (!file.is_open()) return Status::IOError("cannot open WAL file: " + wal_file);

  std::unique_ptr<WriteAheadLog> wal(new WriteAheadLog(std::move(file), wal_file, 0, GenesisHash()));
  if (!dek.empty()) {
    auto subkey_or = at_rest::FileSubkey(dek, "wal");
    if (!subkey_or.ok()) return subkey_or.status();
    wal->subkey_ = subkey_or.value();
  }

  std::vector<WalRecord> records;
  {
    std::lock_guard<std::mutex> lock(wal->mu_);
    Status st = wal->ReadAllLocked(&records);
    if (!st.ok()) return st;
  }

  if (!dek.empty() && records.empty() && wal->last_read_corrupt_) {
    // Wrong-key fail-closed: a sealed log whose very first record does not
    // authenticate recovered nothing. Booting with an empty ledger here
    // would fork history (new writes reuse LSNs) and strand every peer's
    // convergence -- refuse instead. (A sealed log with a merely torn tail
    // reads clean with last_read_corrupt_ false, so crash recovery still
    // opens; later-record corruption keeps the established VerifyChain
    // semantics and is reported, not hidden.)
    return Status::Corruption("at-rest: sealed ledger " + wal_file +
                              " did not authenticate (wrong unlock key or tampered file)");
  }

  if (!records.empty()) {
    wal->next_lsn_ = records.back().lsn + 1;
    wal->tip_hash_ = records.back().entry_hash;
    for (const WalRecord& rec : records) {
      if (rec.type == WalRecordType::kCheckpoint) wal->last_checkpoint_lsn_ = rec.lsn;
    }
  }

  // Truncation detection (C-6/M-6): a signed-by-locality high-water mark
  // (<wal>.hwm, id + hash) records the tallest tip this file ever held. A
  // file shorter than its mark lost records -- power loss with C-1 syncs
  // cannot produce this shape, so it is tampering or a copying error, and
  // booting on it would reuse LSNs. Not a defense against disk-level
  // attackers (who can rewrite the mark too); it catches truncation, which
  // VerifyChain's prefix-blessing otherwise misses.
  {
    const std::string hwm_path = wal_file + ".hwm";
    std::ifstream hwm(hwm_path, std::ios::binary);
    if (hwm.is_open()) {
      char buf[8 + 32];
      hwm.read(buf, sizeof(buf));
      if (hwm.gcount() == static_cast<std::streamsize>(sizeof(buf))) {
        int64_t hwm_id = 0;
        std::memcpy(&hwm_id, buf, 8);
        const std::string hwm_hash(buf + 8, 32);
        const int64_t tip_id = records.empty() ? kInvalidLsn : records.back().lsn;
        const std::string tip_hash =
            records.empty() ? GenesisHash() : records.back().entry_hash;
        if (tip_id < hwm_id || (tip_id == hwm_id && tip_hash != hwm_hash)) {
          return Status::Corruption("ledger file is shorter than its high-water mark: " +
                                    std::to_string(tip_id + 1) + " entries present, mark at " +
                                    std::to_string(hwm_id + 1) + " -- refusing to boot on a " +
                                    "truncated ledger (would reuse LSNs)");
        }
      }
    }
  }

  // Fail closed at boot on mid-file corruption (M-6 fix): ReadAllLocked's
  // torn-tail tolerance is for crash recovery, but bytes present-but-bad
  // deeper in the file must refuse to serve, not boot on a silently
  // truncated prefix.
  {
    WriteAheadLog::VerifyResult vr = wal->VerifyChain();
    if (!vr.ok) {
      return Status::Corruption("ledger failed boot verification at entry " +
                                std::to_string(vr.failed_at_entry_id) + ": " + vr.reason);
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
      rec.position_bound = false;
      // Legacy formula: migrated records predate position binding and verify
      // via the legacy path (their origin signatures, if any, cover legacy
      // content, which cannot be re-signed here).
      rec.entry_hash = crypto::Sha256(BuildContentLegacy(rec) + prev);
      prev = rec.entry_hash;
    }
    std::lock_guard<std::mutex> lock(wal->mu_);
    Status st = wal->RewriteLocked(upgraded);
    if (!st.ok()) return st;
    wal->tip_hash_ = prev;
    // Same HWM rule as Prune: the re-derived tip replaces the mark.
    WriteHwmBestEffort(wal_file, wal->next_lsn_ - 1, wal->tip_hash_);
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
  rec.position_bound = true;  // all new appends bind chain position (C-6)
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
  // Routing metadata for intents: who holds the bytes and which chunk this
  // is. Stamped only on TRANSIT_INTENT; claims match on key_hash alone, so
  // they carry none. Serialized as the unsigned tail (EncodeBody), outside
  // the signed content -- see WalRecord.
  if (type == WalRecordType::kTransitIntent) {
    rec.transit_holder = options.transit_holder;
    rec.transit_size_bytes = options.transit_size_bytes;
    rec.transit_chunk_index = options.transit_chunk_index;
    rec.transit_chunk_total = options.transit_chunk_total;
  }

  // prev_hash is assigned BEFORE content is built: it is inside the signed
  // bytes (C-6), so entry_hash is over the bound content alone.
  rec.prev_hash = tip_hash_;
  const std::string content = BuildContent(rec);
  rec.entry_hash = crypto::Sha256(content);
  if (signer_) rec.origin_signature = signer_(content);

  std::string payload = EncodeBody(rec);
  Status seal_st = SealPayload(payload, &payload);
  if (!seal_st.ok()) return seal_st;
  std::string body = std::move(payload);
  uint32_t crc = Crc32(body.data(), body.size());
  PutU32(&body, crc);

  std::string frame;
  PutU32(&frame, static_cast<uint32_t>(body.size()));
  frame += body;

  file_.clear();
  file_.seekp(0, std::ios::end);
  const int64_t frame_off = static_cast<int64_t>(file_.tellp());
  file_.write(frame.data(), static_cast<std::streamsize>(frame.size()));
  if (!file_.good()) return Status::IOError("WAL append failed");
  file_.flush();
  // Durability point (C-1 fix): flush() only reaches the OS page cache.
  // SyncFileByPath issues fdatasync / F_FULLFSYNC / FlushFileBuffers so an
  // acked write survives power loss. Group-commit batching can coalesce
  // concurrent syncs here later; correctness requires the sync before ack.
  std::string sync_err;
  if (!SyncFileByPath(path_, &sync_err)) {
    return Status::IOError("WAL sync failed: " + sync_err);
  }

  tip_hash_ = rec.entry_hash;
  if (type == WalRecordType::kCheckpoint) last_checkpoint_lsn_ = rec.lsn;
  if (rec.lsn >= 0) {
    if (static_cast<size_t>(rec.lsn) >= lsn_offsets_.size()) {
      lsn_offsets_.resize(static_cast<size_t>(rec.lsn) + 1, -1);
    }
    lsn_offsets_[static_cast<size_t>(rec.lsn)] = frame_off;
  }
  ++next_lsn_;
  WriteHwmIfDueLocked(rec.lsn, rec.entry_hash);
  return rec.lsn;
}

Status WriteAheadLog::ReadAllLocked(std::vector<WalRecord>* out) {
  out->clear();
  // A replay can end three ways: a clean end-of-file, a short (torn) tail
  // record from a crash mid-append -- both benign -- or bytes that are fully
  // present but fail length, CRC or decode checks. A crash can only tear the
  // last write, so the third case is never a torn tail: it is tampering or
  // bit-rot, and VerifyChain() must fail on it rather than verify the prefix
  // as if the missing record had never existed.
  bool corrupt = false;
  // LSN monotonicity (M-5 fix): sealed records authenticate under one AAD
  // ("wal") with no position bound in, so a duplicated or reordered record
  // still decrypts. The hash chain catches that at VerifyChain -- and now
  // here too, at read time: a record whose LSN goes backwards (duplicate or
  // earlier record spliced in) is present-but-invalid, i.e. corruption, not
  // a torn tail. Forward gaps are legitimate (prune drops transit pairs but
  // preserves survivor LSNs), so only backwards steps fail.
  lsn_t expected_lsn = 0;
  file_.clear();
  file_.seekg(0);
  for (;;) {
    char len_buf[4];
    file_.read(len_buf, 4);
    if (file_.gcount() < 4) break;  // clean end-of-file
    uint32_t body_len = GetU32(len_buf);
    if (body_len < 8 || body_len > kMaxRecordBytes) {
      DSN_LOG_WARN("wal", "implausible record length, stopping replay");
      corrupt = true;
      break;
    }
    const int64_t frame_off = static_cast<int64_t>(file_.tellg()) - 4;
    std::string body(body_len, '\0');
    file_.read(body.data(), static_cast<std::streamsize>(body_len));
    if (static_cast<uint32_t>(file_.gcount()) < body_len) {
      DSN_LOG_WARN("wal", "torn record tail detected, stopping replay");
      break;  // short read: the file really ends here (crash mid-append)
    }
    std::string payload = body.substr(0, body_len - 4);
    uint32_t stored_crc = GetU32(body.data() + body_len - 4);
    uint32_t computed_crc = Crc32(payload.data(), payload.size());
    if (stored_crc != computed_crc) {
      DSN_LOG_WARN("wal", "stopping replay: CRC mismatch on record (stored="
                               << stored_crc << ", computed=" << computed_crc << ")");
      corrupt = true;
      break;
    }
    // At-rest layer: sealed payloads open here; the torn-tail-vs-corruption
    // split above is unchanged (short reads still break benignly before any
    // cryptographic check runs).
    Status unseal_st = UnsealPayload(payload, &payload, &corrupt);
    if (!unseal_st.ok()) {
      DSN_LOG_WARN("wal", "stopping replay: " << unseal_st.message());
      corrupt = true;
      break;
    }

    WalRecord rec;
    // Format detection is by magic, not by guessing from lengths: a v2 body
    // starts with "DSW2", a v3 (position-bound) body with "DSW3", a v1 body
    // with the record's little-endian LSN. A v1 LSN would have to be exactly
    // 0x44535732/0x44535733 to collide, and even then the v2/v3 parse would
    // fail its own internal length checks.
    if (payload.size() >= 4 && (GetU32(payload.data()) == kWalRecordMagicV2 ||
                                GetU32(payload.data()) == kWalRecordMagicV3)) {
      Status st = DecodeBody(payload, &rec);
      if (!st.ok()) {
        DSN_LOG_WARN("wal", "stopping replay: " << st.message());
        corrupt = true;
        break;
      }
    } else {
      Status st = DecodeBodyV1(payload, &rec);
      if (!st.ok()) {
        DSN_LOG_WARN("wal", "stopping replay: " << st.message());
        corrupt = true;
        break;
      }
      migrated_from_v1_ = true;
    }
    if (rec.lsn >= 0 && rec.lsn < expected_lsn) {
      DSN_LOG_WARN("wal", "LSN went backwards (" << rec.lsn << " after " << (expected_lsn - 1)
                                                << "): duplicated or reordered record, stopping replay");
      corrupt = true;
      break;
    }
    if (rec.lsn >= expected_lsn) expected_lsn = rec.lsn + 1;
    // Maintain the LSN -> frame-offset index (H-2): bounded reads seek here
    // instead of re-reading the whole file. Sized for dense LSNs with -1
    // for pruned gaps.
    if (rec.lsn >= 0) {
      if (static_cast<size_t>(rec.lsn) >= lsn_offsets_.size()) {
        lsn_offsets_.resize(static_cast<size_t>(rec.lsn) + 1, -1);
      }
      lsn_offsets_[static_cast<size_t>(rec.lsn)] = frame_off;
    }
    out->push_back(std::move(rec));
  }
  file_.clear();
  last_read_corrupt_ = corrupt;
  return Status::OK();
}

StatusOr<std::vector<WalRecord>> WriteAheadLog::ReadRange(lsn_t from, lsn_t to) {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<WalRecord> out;
  if (to < 0 || next_lsn_ <= 0) return out;
  if (from < 0) from = 0;
  if (from > to) return out;
  // Seek to the first present frame at or after `from` (prune gaps are -1).
  int64_t start_off = -1;
  for (lsn_t lsn = from; lsn <= to && lsn < static_cast<lsn_t>(lsn_offsets_.size()); ++lsn) {
    if (lsn >= 0 && lsn_offsets_[static_cast<size_t>(lsn)] >= 0) {
      start_off = lsn_offsets_[static_cast<size_t>(lsn)];
      break;
    }
  }
  if (start_off < 0) {
    // No indexed frame in range (empty log, or range fully pruned): fall
    // back to a bounded forward scan rather than failing.
    file_.clear();
    file_.seekg(0);
  } else {
    file_.clear();
    file_.seekg(start_off);
  }
  for (;;) {
    char len_buf[4];
    file_.read(len_buf, 4);
    if (file_.gcount() < 4) break;
    uint32_t body_len = GetU32(len_buf);
    if (body_len < 8 || body_len > kMaxRecordBytes) break;
    std::string body(body_len, '\0');
    file_.read(body.data(), static_cast<std::streamsize>(body_len));
    if (static_cast<uint32_t>(file_.gcount()) < body_len) break;
    std::string payload = body.substr(0, body_len - 4);
    if (GetU32(body.data() + body_len - 4) != Crc32(payload.data(), payload.size())) break;
    bool corrupt = false;
    Status unseal_st = UnsealPayload(payload, &payload, &corrupt);
    if (!unseal_st.ok() || corrupt) break;
    WalRecord rec;
    bool parsed = false;
    if (payload.size() >= 4 && (GetU32(payload.data()) == kWalRecordMagicV2 ||
                                GetU32(payload.data()) == kWalRecordMagicV3)) {
      if (DecodeBody(payload, &rec).ok()) parsed = true;
    } else {
      if (DecodeBodyV1(payload, &rec).ok()) parsed = true;
    }
    if (!parsed) break;
    if (rec.lsn > to) break;
    if (rec.lsn >= from) out.push_back(std::move(rec));
  }
  file_.clear();
  return out;
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
  {
    std::lock_guard<std::mutex> lock(mu_);
    Status st = ReadAllLocked(&records);
    if (!st.ok()) {
      result.ok = false;
      result.reason = st.message();
      return result;
    }
  }

  std::string expected_prev = GenesisHash();
  if (last_read_corrupt_) {
    // The replay stopped on bytes that are present but invalid -- a hole in
    // the middle of the file, which a crash-torn tail cannot produce. The
    // surviving prefix may be internally consistent, but verifying it as
    // "the ledger" would bless history with a record cut out of it.
    result.ok = false;
    result.entries_checked = records.size();
    result.failed_at_entry_id = records.empty() ? 0 : records.back().lsn + 1;
    result.reason = "ledger file contains a corrupt record: replay stopped before end-of-file";
    return result;
  }
  for (const WalRecord& rec : records) {
    if (rec.prev_hash != expected_prev) {
      result.ok = false;
      result.failed_at_entry_id = rec.lsn;
      result.reason = "chain break: prev_hash does not match the preceding entry's hash";
      return result;
    }
    // Position-bound records verify under the new layout; pre-fix records
    // verify under the legacy layout (content without prev_hash). Anything
    // matching neither is tampering or corruption.
    const std::string content = BuildContent(rec);
    const std::string legacy = BuildContentLegacy(rec);
    const bool bound_ok = (crypto::Sha256(content) == rec.entry_hash);
    const bool legacy_ok =
        !bound_ok && (crypto::Sha256(legacy + rec.prev_hash) == rec.entry_hash);
    if (!bound_ok && !legacy_ok) {
      result.ok = false;
      result.failed_at_entry_id = rec.lsn;
      result.reason = "entry_hash mismatch: record content does not match its recorded hash";
      return result;
    }
    if (rec.origin_signature.empty() || rec.origin_node_id.empty()) {
      ++result.unsigned_entries;
    } else {
      ++result.signed_entries;
      if (verify_signature) {
        const bool sig_ok = verify_signature(rec.origin_node_id, content, rec.origin_signature) ||
                            (!bound_ok && verify_signature(rec.origin_node_id, legacy,
                                                           rec.origin_signature));
        if (!sig_ok) {
          result.ok = false;
          result.failed_at_entry_id = rec.lsn;
          result.reason = "origin signature does not verify against node_id " + rec.origin_node_id;
          return result;
        }
      }
    }
    expected_prev = rec.entry_hash;
    ++result.entries_checked;
  }
  result.failed_at_entry_id = kInvalidLsn;
  return result;
}

Status WriteAheadLog::SealPayload(const std::string& payload, std::string* out) const {
  if (subkey_.empty()) {
    *out = payload;
    return Status::OK();
  }
  auto sealed_or = at_rest::SealRecord(subkey_, payload, "wal");
  if (!sealed_or.ok()) return sealed_or.status();
  *out = sealed_or.value();
  return Status::OK();
}

Status WriteAheadLog::UnsealPayload(const std::string& payload, std::string* out,
                                    bool* corrupt) const {
  const bool sealed = at_rest::LooksSealedRecord(payload);
  if (sealed && subkey_.empty()) {
    // Sealed bytes with no key: DetectMode fail-closes at Open for a sealed
    // first record; a sealed record deeper in a plaintext log is tampering.
    *corrupt = true;
    return Status::Corruption("WAL: sealed record without an unlock key");
  }
  if (!sealed && !subkey_.empty()) {
    // Plaintext record in a sealed log: fail closed rather than mixing modes.
    *corrupt = true;
    return Status::Corruption("WAL: plaintext record in a sealed ledger");
  }
  if (!sealed) {
    *out = payload;
    return Status::OK();
  }
  auto open_or = at_rest::OpenRecord(subkey_, payload, "wal");
  if (!open_or.ok()) {
    *corrupt = true;
    return open_or.status();
  }
  *out = open_or.value();
  return Status::OK();
}

Status WriteAheadLog::RewriteLocked(const std::vector<WalRecord>& records) {
  const std::string tmp = path_ + ".rewrite";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return Status::IOError("cannot write " + tmp);
    for (const WalRecord& rec : records) {
      std::string payload = EncodeBody(rec);
      Status seal_st = SealPayload(payload, &payload);
      if (!seal_st.ok()) return seal_st;
      std::string body = std::move(payload);
      uint32_t crc = Crc32(body.data(), body.size());
      PutU32(&body, crc);
      uint32_t body_len = static_cast<uint32_t>(body.size());
      out.write(reinterpret_cast<const char*>(&body_len), 4);
      out.write(body.data(), static_cast<std::streamsize>(body.size()));
    }
    out.flush();
    if (!out.good()) return Status::IOError("WAL rewrite failed");
  }
  { std::string sync_err; SyncFileByPath(tmp, &sync_err); }
  if (file_.is_open()) file_.close();
  std::remove(path_.c_str());
  if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
    return Status::IOError("cannot commit rewritten WAL: " + path_);
  }
  { std::string dir_err; SyncDirForFile(path_, &dir_err); }
  file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
  if (!file_.is_open()) return Status::IOError("cannot reopen WAL after rewrite: " + path_);
  // Rebuild the offset index synchronously (H-2): every offset changed, and
  // a ReadRange between rewrite and the next full read would otherwise seek
  // stale positions. Rewrite is rare (prune/migrate); one pass is cheap.
  // NOTE: no HWM write here -- re-chaining changes the tip hash, and the new
  // tip is only known by the caller (Prune/migrate) after it updates
  // tip_hash_/next_lsn_. Writing the pre-update tip here pinned a stale mark
  // that the next Open read as truncation. Callers refresh the mark.
  lsn_offsets_.clear();
  std::vector<WalRecord> reindexed;
  Status reindex_st = ReadAllLocked(&reindexed);
  if (!reindex_st.ok()) return reindex_st;
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

  // Preserve the original LSNs so a checkpoint remains a meaningful cursor,
  // while rebuilding the hash chain from the first surviving record.
  // NOTE: a pruned chain verifies structurally (links + hashes) but its
  // entries verify only as unsigned/legacy -- this is precisely why Prune()
  // runs only after quorum agreement, and why position-bound signatures on
  // the survivors cannot be preserved (see below). A node presenting a
  // pruned chain as complete history is making a claim only the quorum
  // attestation backs.
  std::string prev = GenesisHash();
  for (WalRecord& rec : kept) {
    rec.prev_hash = prev;
    // Each survivor keeps the layout it was written with (v2 stays legacy,
    // v3 stays bound) so it keeps verifying under the matching path.
    rec.entry_hash = rec.position_bound ? crypto::Sha256(BuildContent(rec))
                                        : crypto::Sha256(BuildContentLegacy(rec) + prev);
    // The origin signature covered the pre-prune chain links, which no
    // longer exist. Clearing it is the honest outcome: a pruning node
    // cannot re-sign another node's entry, and leaving a signature that
    // will not verify would be worse than none. This is why Prune() runs
    // only after quorum agreement (ledger/checkpoint.h) -- the surviving
    // attestation is the quorum's, recorded in the checkpoint entry.
    rec.origin_signature.clear();
    prev = rec.entry_hash;
  }

  st = RewriteLocked(kept);
  if (!st.ok()) return st;
  tip_hash_ = prev;
  next_lsn_ = kept.empty() ? 0 : kept.back().lsn + 1;
  // Refresh the truncation mark to the re-chained tip (C-6): the pre-prune
  // tip hash no longer exists in this file, so the old mark would read as
  // truncation on the next Open.
  WriteHwmBestEffort(path_, next_lsn_ - 1, tip_hash_);
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
