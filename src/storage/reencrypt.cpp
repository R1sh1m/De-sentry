// Offline plaintext -> sealed migration. See the header for the contract
// (stopped node, idempotent skips, fail-closed aborts, vendored refusal).

#include "desentry/storage/reencrypt.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>

#include "desentry/common/crc32.h"
#include "desentry/common/json.h"
#include "desentry/common/logger.h"
#include "desentry/common/platform.h"
#include "desentry/security/at_rest.h"
#include "desentry/security/crypto.h"

namespace desentry {

namespace {

// Mirrors WriteAheadLog's kMaxRecordBytes (wal.cpp): bounds a hostile/corrupt
// length field while rewriting. The small logs use 1MiB, like their readers.
constexpr uint32_t kWalRecordCap = 256u << 20;
constexpr uint32_t kSmallLogCap = 1u << 20;

bool HasSuffix(const std::string& path, const std::string& suffix) {
  return path.size() >= suffix.size() &&
         path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string ReadWholeFile(const std::string& path, bool* missing) {
  *missing = false;
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    *missing = true;
    return std::string();
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Writes `content` to `path` via tmp + rename. Crash-safe like every other
// writer in this tree; the original is replaced only on success.
Status WriteAtomic(const std::string& path, const std::string& content) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc | std::ios::binary);
    if (!f.is_open()) return Status::IOError("cannot write " + tmp);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    f.flush();
    if (!f.good()) return Status::IOError("write failed: " + tmp);
  }
  std::remove(path.c_str());  // Windows rename() refuses to clobber
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    return Status::IOError("cannot commit " + path);
  }
  return Status::OK();
}

uint32_t GetU32(const char* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

void PutU32(std::string* out, uint32_t v) {
  char buf[4];
  std::memcpy(buf, &v, 4);
  out->append(buf, 4);
}

// Seals one length-framed log in full. `crc_inside` selects the WAL layout
// ([len][payload+crc]) vs the transit/outbox/index layout
// ([len][body][crc]). Torn tails are copied verbatim (the sealed readers
// treat short reads as benign crashes, exactly as before). An already-sealed
// first payload means a previous run sealed this file: skip it whole
// (idempotent reruns). Sealed bytes deeper in a plaintext log, or any
// undecodable framing, abort the run.
// Returns the number of records sealed via `sealed`.
Status SealFramedLog(const std::string& path, const std::string& subkey, const std::string& aad,
                     uint32_t record_cap, bool crc_inside, size_t* sealed) {
  *sealed = 0;
  bool missing = false;
  const std::string src = ReadWholeFile(path, &missing);
  if (missing || src.empty()) return Status::OK();  // nothing to migrate
  size_t off = 0;
  const size_t n = src.size();
  bool first = true;
  std::string out;
  while (off < n) {
    if (n - off < 4) {
      out.append(src, off, n - off);  // torn length: verbatim
      break;
    }
    const uint32_t body_len = GetU32(src.data() + off);
    if (body_len < 8 || body_len > record_cap) {
      return Status::Corruption("re-encrypt: " + path + " has an implausible record length " +
                                std::to_string(body_len) + "; file is not a clean plaintext log");
    }
    if (n - off - 4 < body_len) {
      out.append(src, off, n - off);  // torn body: verbatim
      break;
    }
    std::string payload;
    if (crc_inside) {
      if (body_len < 4) {
        return Status::Corruption("re-encrypt: " + path + " has a truncated record");
      }
      payload.assign(src, off + 4, body_len - 4);
      const uint32_t stored_crc = GetU32(src.data() + off + 4 + body_len - 4);
      if (stored_crc != Crc32(payload.data(), payload.size())) {
        return Status::Corruption("re-encrypt: " + path +
                                  " fails CRC before migration; repair or restore first");
      }
    } else {
      if (n - off - 4 < body_len + 4) {
        out.append(src, off, n - off);  // torn CRC: verbatim
        break;
      }
      payload.assign(src, off + 4, body_len);
      const uint32_t stored_crc = GetU32(src.data() + off + 4 + body_len);
      if (stored_crc != Crc32(payload.data(), payload.size())) {
        return Status::Corruption("re-encrypt: " + path +
                                  " fails CRC before migration; repair or restore first");
      }
    }
    if (first && at_rest::LooksSealedRecord(payload)) {
      return Status::OK();  // already sealed by a previous run: skip whole
    }
    if (at_rest::LooksSealedRecord(payload)) {
      return Status::Corruption("re-encrypt: " + path + " mixes sealed and plaintext records");
    }
    auto sealed_or = at_rest::SealRecord(subkey, payload, aad);
    if (!sealed_or.ok()) return sealed_or.status();
    const std::string& sealed_payload = sealed_or.value();
    if (crc_inside) {
      std::string frame_body = sealed_payload;
      PutU32(&frame_body, Crc32(sealed_payload.data(), sealed_payload.size()));
      PutU32(&out, static_cast<uint32_t>(frame_body.size()));
      out += frame_body;
      off += 4 + body_len;
    } else {
      PutU32(&out, static_cast<uint32_t>(sealed_payload.size()));
      out += sealed_payload;
      PutU32(&out, Crc32(sealed_payload.data(), sealed_payload.size()));
      off += 4 + body_len + 4;
    }
    first = false;
    ++(*sealed);
  }
  if (*sealed == 0) return Status::OK();  // empty or torn-only: nothing changed
  return WriteAtomic(path, out);
}

// Seals one whole-file JSON blob. Already-sealed files are skipped
// (idempotent); undecodable content aborts.
Status SealJsonFile(const std::string& path, const std::string& subkey, const std::string& aad,
                    bool* changed) {
  *changed = false;
  bool missing = false;
  const std::string text = ReadWholeFile(path, &missing);
  if (missing || text.empty()) return Status::OK();
  if (at_rest::LooksSealedFile(text)) return Status::OK();  // previous run: skip
  try {
    JsonValue root = JsonValue::Parse(text);
    (void)root;
  } catch (const std::exception& e) {
    return Status::Corruption("re-encrypt: " + path + " is not decodable JSON: " + e.what());
  }
  auto sealed_or = at_rest::SealFile(subkey, text, aad);
  if (!sealed_or.ok()) return sealed_or.status();
  Status st = WriteAtomic(path, sealed_or.value());
  if (!st.ok()) return st;
  *changed = true;
  return Status::OK();
}

// Seals one paged `.dsf` file (4096B pages -> 4124B sealed pages) and writes
// its `.enc` sidecar header. An existing header means a previous run sealed
// it: skipped. A torn final page aborts (run the node once unencrypted to
// heal, or restore).
Status SealDsfFile(const std::string& path, const std::string& dek, bool* changed) {
  *changed = false;
  auto tag_or = at_rest::ReadPageHeaderFile(path);
  if (tag_or.ok()) return Status::OK();  // previous run: skip
  if (tag_or.status().code() != StatusCode::kNotFound) return tag_or.status();
  bool missing = false;
  const std::string src = ReadWholeFile(path, &missing);
  if (missing || src.empty()) return Status::OK();
  if (src.size() % kPageSize != 0) {
    return Status::Corruption("re-encrypt: " + path + " size " + std::to_string(src.size()) +
                              " is not a multiple of 4096 (torn?)");
  }
  std::string raw_tag;
  try {
    raw_tag = crypto::RandomBytes(16);
  } catch (const std::exception& e) {
    return Status::Internal(std::string("re-encrypt: RNG failure: ") + e.what());
  }
  static const char* kHex = "0123456789abcdef";
  std::string tag_hex;
  for (unsigned char c : raw_tag) {
    tag_hex.push_back(kHex[c >> 4]);
    tag_hex.push_back(kHex[c & 0xF]);
  }
  auto subkey_or = at_rest::FileSubkey(dek, tag_hex);
  if (!subkey_or.ok()) return subkey_or.status();
  const std::string& subkey = subkey_or.value();
  const size_t pages = src.size() / kPageSize;
  std::string out;
  out.resize(pages * at_rest::kSealedPageSize);
  for (size_t i = 0; i < pages; ++i) {
    Status st = at_rest::SealPage(subkey, static_cast<page_id_t>(i), tag_hex,
                                  src.data() + i * kPageSize,
                                  out.data() + i * at_rest::kSealedPageSize);
    if (!st.ok()) return st;
  }
  at_rest::Zeroize(raw_tag);
  Status st = WriteAtomic(path, out);
  if (!st.ok()) return st;
  st = at_rest::WritePageHeaderFile(path, tag_hex);
  if (!st.ok()) return st;
  *changed = true;
  return Status::OK();
}

const char* ManifestAad(const std::string& filename) {
  if (filename == "segments.json") return "columnar-manifest";
  if (filename == "chunks.json") return "ts-manifest";
  if (filename == "index.json") return "vector-manifest";
  if (filename == "graph.json") return "graph-manifest";
  if (filename == "roots.json") return "roots";
  return nullptr;  // not a sealed manifest
}

}  // namespace

Status ReencryptDataDir(const std::string& data_dir, const std::string& dek,
                        std::string* summary) {
  if (dek.size() != 32) return Status::InvalidArgument("re-encrypt: DEK must be 32 bytes");
  // Refuse vendored engines up front: their files are outside the sealed
  // layer, and a half-migrated directory (built-ins sealed, sqlite
  // plaintext) would be worse than a clean refusal.
  {
    bool missing = false;
    const std::string conf = ReadWholeFile(data_dir + "/node.json", &missing);
    if (!missing && !conf.empty()) {
      try {
        JsonValue root = JsonValue::Parse(conf);
        const JsonValue* engines = root.Find("engines");
        if (engines && engines->is_array()) {
          for (const JsonValue& e : engines->AsArray()) {
            if (!e.is_string()) continue;
            const std::string& name = e.AsString();
            if (name == "sqlite" || name == "duckdb" || name == "lmdb" || name == "sqlite_vec") {
              return Status::InvalidArgument(
                  "re-encrypt: engine '" + name +
                  "' manages its own files outside the sealed layer; bind collections to a "
                  "built-in engine first");
            }
          }
        }
      } catch (const std::exception&) {
        // An unparseable node.json is the node's problem at boot, not the
        // migration's: proceed with file detection instead of refusing.
      }
    }
  }

  auto subkey_for = [&](const char* tag) -> StatusOr<std::string> {
    return at_rest::FileSubkey(dek, tag);
  };

  size_t files_sealed = 0;
  size_t files_skipped = 0;
  size_t records_sealed = 0;

  // Length-framed logs: (path, aad, cap, crc_inside).
  struct FramedLog {
    const char* rel;
    const char* aad;
    uint32_t cap;
    bool crc_inside;
  };
  const FramedLog logs[] = {
      {"desentry.wal", "wal", kWalRecordCap, true},
      {"transit.log", "transit", kSmallLogCap, false},
      {"outbox.log", "outbox", kSmallLogCap, false},
      {"engines/cross_engine_index.log", "index", kSmallLogCap, false},
  };
  for (const FramedLog& log : logs) {
    const std::string path = data_dir + "/" + log.rel;
    bool missing = false;
    const std::string head = ReadWholeFile(path, &missing);
    if (missing || head.empty()) continue;
    // Skip already-sealed files (idempotent reruns) without parsing.
    if (head.size() >= 8) {
      // Peek the first payload exactly like the readers do.
      uint32_t first_len = 0;
      std::memcpy(&first_len, head.data(), 4);
      if (first_len >= 8 && first_len <= log.cap && head.size() >= 4 + first_len) {
        const size_t pay_off = 4;
        const size_t pay_len =
            log.crc_inside ? first_len - 4 : std::min<size_t>(first_len, head.size() - 8);
        if (pay_len >= 4 &&
            at_rest::LooksSealedRecord(head.substr(pay_off, pay_len))) {
          ++files_skipped;
          continue;
        }
      }
    }
    auto subkey_or = subkey_for(log.aad);
    if (!subkey_or.ok()) return subkey_or.status();
    size_t sealed_count = 0;
    Status st = SealFramedLog(path, subkey_or.value(), log.aad, log.cap, log.crc_inside,
                              &sealed_count);
    if (!st.ok()) return st;
    if (sealed_count == 0) {
      ++files_skipped;
    } else {
      ++files_sealed;
      records_sealed += sealed_count;
    }
    DSN_LOG_INFO("re-encrypt", log.rel << ": " << sealed_count << " record(s) sealed");
  }

  // Whole-file JSON: catalog + every backend manifest/roots file.
  {
    auto subkey_or = subkey_for("catalog");
    if (!subkey_or.ok()) return subkey_or.status();
    bool changed = false;
    Status st = SealJsonFile(data_dir + "/catalog.json", subkey_or.value(), "catalog", &changed);
    if (!st.ok()) return st;
    if (changed) ++files_sealed;
  }
  {
    const std::string engines_dir = data_dir + "/engines";
    for (const std::string& backend : ListDir(engines_dir)) {
      const std::string bdir = engines_dir + "/" + backend;
      for (const std::string& name : ListDir(bdir)) {
        const char* aad = ManifestAad(name);
        if (aad == nullptr) continue;
        auto subkey_or = subkey_for(aad);
        if (!subkey_or.ok()) return subkey_or.status();
        bool changed = false;
        Status st = SealJsonFile(bdir + "/" + name, subkey_or.value(), aad, &changed);
        if (!st.ok()) return st;
        if (changed) ++files_sealed;
      }
      // Nested kv dirs (graph_adj embeds a kv backend with its own roots.json).
      for (const std::string& sub : ListDir(bdir)) {
        const std::string subdir = bdir + "/" + sub;
        for (const std::string& name : ListDir(subdir)) {
          const char* aad = ManifestAad(name);
          if (aad == nullptr) continue;
          auto subkey_or = subkey_for(aad);
          if (!subkey_or.ok()) return subkey_or.status();
          bool changed = false;
          Status st = SealJsonFile(subdir + "/" + name, subkey_or.value(), aad, &changed);
          if (!st.ok()) return st;
          if (changed) ++files_sealed;
        }
      }
    }
  }

  // Paged data files: every *.dsf under engines/ (recursive one level for
  // embedded stores like graph_adj/kv).
  {
    const std::string engines_dir = data_dir + "/engines";
    std::vector<std::string> dirs{engines_dir};
    for (const std::string& backend : ListDir(engines_dir)) dirs.push_back(engines_dir + "/" + backend);
    // One extra level for embedded stores.
    size_t base = dirs.size();
    for (size_t i = 1; i < base; ++i) {
      for (const std::string& sub : ListDir(dirs[i])) dirs.push_back(dirs[i] + "/" + sub);
    }
    for (const std::string& dir : dirs) {
      for (const std::string& name : ListDir(dir)) {
        if (!HasSuffix(name, ".dsf")) continue;
        bool changed = false;
        Status st = SealDsfFile(dir + "/" + name, dek, &changed);
        if (!st.ok()) return st;
        if (changed) ++files_sealed;
      }
    }
  }

  // identity.key: 64 raw bytes (32 priv + 32 pub).
  {
    bool missing = false;
    const std::string raw = ReadWholeFile(data_dir + "/identity.key", &missing);
    if (!missing && !raw.empty()) {
      if (at_rest::LooksSealedFile(raw)) {
        ++files_skipped;
      } else if (raw.size() != 64) {
        return Status::Corruption("re-encrypt: identity.key has unexpected size " +
                                  std::to_string(raw.size()));
      } else {
        auto subkey_or = subkey_for("identity");
        if (!subkey_or.ok()) return subkey_or.status();
        auto sealed_or = at_rest::SealFile(subkey_or.value(), raw, "identity");
        if (!sealed_or.ok()) return sealed_or.status();
        Status st = WriteAtomic(data_dir + "/identity.key", sealed_or.value());
        if (!st.ok()) return st;
        ++files_sealed;
      }
    }
  }

  if (summary != nullptr) {
    *summary = "sealed " + std::to_string(files_sealed) + " file(s) (" +
               std::to_string(records_sealed) + " log records); " +
               std::to_string(files_skipped) + " already sealed, skipped";
  }
  DSN_LOG_INFO("re-encrypt", "migration complete: " << files_sealed << " file(s) sealed, "
                                                    << files_skipped << " skipped");
  return Status::OK();
}

}  // namespace desentry
