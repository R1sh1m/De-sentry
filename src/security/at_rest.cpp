// At-rest encryption primitives. See the header for the format contracts and
// the fail-closed discipline. Every function here is Status-only: OpenSSL's
// throwing helpers (RandomBytes, HkdfSha256) are called inside try/catch and
// converted, so no exception ever crosses into engine code (AGENTS.md).

#include "desentry/security/at_rest.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>

#include "desentry/common/byte_buffer.h"
#include "desentry/security/crypto.h"

namespace desentry::at_rest {

namespace {

constexpr size_t kDekLen = 32;

// Crockford base32 alphabet (no I, L, O, U) -- matches recovery.rs and the
// sidecar's encode(), so what the app shows is what the engine accepts.
// A single table lookup rather than hand-enumerated values: the values are
// the alphabet positions by construction and cannot drift.
int CrockfordValue(char c) {
  if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  if (c == 'O') return 0;  // transcription tolerance for 0
  if (c == 'I' || c == 'L') return 1;  // transcription tolerance for 1
  static const char* kAlpha = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
  const char* p = std::strchr(kAlpha, c);
  if (p == nullptr) return -1;  // includes 'U', which Crockford excludes
  return static_cast<int>(p - kAlpha);
}

StatusOr<std::string> RandomNonce() {
  try {
    return crypto::RandomBytes(kPageNonceLen);
  } catch (const std::exception& e) {
    return Status::Internal(std::string("at-rest: RNG failure: ") + e.what());
  }
}

void PageAad(const std::string& file_tag, page_id_t page_id, std::string* out) {
  out->clear();
  out->append(file_tag);
  const uint32_t id = static_cast<uint32_t>(page_id);
  out->push_back(static_cast<char>((id >> 24) & 0xFF));
  out->push_back(static_cast<char>((id >> 16) & 0xFF));
  out->push_back(static_cast<char>((id >> 8) & 0xFF));
  out->push_back(static_cast<char>(id & 0xFF));
}

}  // namespace

StatusOr<std::string> DecodeRecoveryKey(const std::string& text) {
  uint32_t bits = 0;
  int bit_count = 0;
  std::string out;
  for (char raw : text) {
    if (raw == '-' || raw == ' ' || raw == '\t' || raw == '\n' || raw == '\r' || raw == '_') {
      continue;
    }
    const int v = CrockfordValue(raw);
    if (v < 0 || v > 31) {
      return Status::Corruption("unlock key contains a character that is not part of a key");
    }
    bits = (bits << 5) | static_cast<uint32_t>(v);
    bit_count += 5;
    if (bit_count >= 8) {
      bit_count -= 8;
      out.push_back(static_cast<char>((bits >> bit_count) & 0xFF));
    }
  }
  if (out.size() != kDekLen) {
    return Status::Corruption("unlock key is the wrong length (want 32 bytes)");
  }
  return out;
}

StatusOr<std::string> FileSubkey(const std::string& dek, const std::string& file_tag) {
  if (dek.size() != kDekLen) {
    return Status::InvalidArgument("at-rest: DEK must be 32 bytes");
  }
  try {
    return crypto::HkdfSha256(dek, file_tag, "desentry-at-rest-v1", kDekLen);
  } catch (const std::exception& e) {
    return Status::Internal(std::string("at-rest: subkey derivation failed: ") + e.what());
  }
}

StatusOr<std::string> SealRecord(const std::string& subkey, const std::string& plaintext,
                                 const std::string& aad) {
  if (subkey.size() != kDekLen) {
    return Status::InvalidArgument("at-rest: subkey must be 32 bytes");
  }
  auto nonce_or = RandomNonce();
  if (!nonce_or.ok()) return nonce_or.status();
  const std::string nonce = nonce_or.value();
  auto sealed_or = crypto::AesGcmSeal(subkey, nonce, plaintext, aad);
  if (!sealed_or.ok()) return sealed_or.status();
  std::string out(kSealedRecordMagic, 4);
  out += nonce;
  out += sealed_or.value();
  return out;
}

StatusOr<std::string> OpenRecord(const std::string& subkey, const std::string& sealed,
                                 const std::string& aad) {
  if (!LooksSealedRecord(sealed)) {
    return Status::Corruption("at-rest: record is not sealed");
  }
  if (subkey.size() != kDekLen) {
    return Status::InvalidArgument("at-rest: subkey must be 32 bytes");
  }
  const std::string nonce = sealed.substr(4, kPageNonceLen);
  const std::string ct_tag = sealed.substr(4 + kPageNonceLen);
  std::string plaintext;
  if (!crypto::AesGcmOpen(subkey, nonce, ct_tag, aad, &plaintext)) {
    return Status::Corruption(
        "at-rest: record authentication failed (wrong key or tampered file)");
  }
  return plaintext;
}

bool LooksSealedRecord(const std::string& bytes) {
  return bytes.size() >= 4 && std::memcmp(bytes.data(), kSealedRecordMagic, 4) == 0;
}

StatusOr<std::string> SealFile(const std::string& subkey, const std::string& plaintext,
                               const std::string& aad) {
  if (subkey.size() != kDekLen) {
    return Status::InvalidArgument("at-rest: subkey must be 32 bytes");
  }
  auto nonce_or = RandomNonce();
  if (!nonce_or.ok()) return nonce_or.status();
  const std::string nonce = nonce_or.value();
  auto sealed_or = crypto::AesGcmSeal(subkey, nonce, plaintext, aad);
  if (!sealed_or.ok()) return sealed_or.status();
  std::string out(kSealedFileMagic, 4);
  out += nonce;
  out += sealed_or.value();
  return out;
}

StatusOr<std::string> OpenFile(const std::string& subkey, const std::string& sealed,
                               const std::string& aad) {
  if (!LooksSealedFile(sealed)) {
    return Status::Corruption("at-rest: file is not sealed");
  }
  if (subkey.size() != kDekLen) {
    return Status::InvalidArgument("at-rest: subkey must be 32 bytes");
  }
  const std::string nonce = sealed.substr(4, kPageNonceLen);
  const std::string ct_tag = sealed.substr(4 + kPageNonceLen);
  std::string plaintext;
  if (!crypto::AesGcmOpen(subkey, nonce, ct_tag, aad, &plaintext)) {
    return Status::Corruption("at-rest: file authentication failed (wrong key or tampered file)");
  }
  return plaintext;
}

bool LooksSealedFile(const std::string& bytes) {
  return bytes.size() >= 4 && std::memcmp(bytes.data(), kSealedFileMagic, 4) == 0;
}

Status SealPage(const std::string& subkey, page_id_t page_id, const std::string& file_tag,
                const char* plaintext_page, char* out_sealed) {
  if (subkey.size() != kDekLen) {
    return Status::InvalidArgument("at-rest: subkey must be 32 bytes");
  }
  auto nonce_or = RandomNonce();
  if (!nonce_or.ok()) return nonce_or.status();
  const std::string nonce = nonce_or.value();
  std::string aad;
  PageAad(file_tag, page_id, &aad);
  auto sealed_or = crypto::AesGcmSeal(
      subkey, nonce, std::string(plaintext_page, kPageSize), aad);
  if (!sealed_or.ok()) return sealed_or.status();
  const std::string& ct_tag = sealed_or.value();
  // ct is exactly kPageSize bytes (GCM preserves length) + 16B tag.
  if (ct_tag.size() != kPageSize + crypto::kAesGcmTagLen) {
    return Status::Internal("at-rest: unexpected sealed page length");
  }
  std::memcpy(out_sealed, nonce.data(), kPageNonceLen);
  std::memcpy(out_sealed + kPageNonceLen, ct_tag.data(), ct_tag.size());
  return Status::OK();
}

Status OpenPage(const std::string& subkey, page_id_t page_id, const std::string& file_tag,
                const char* sealed_page, char* out_plaintext) {
  if (subkey.size() != kDekLen) {
    return Status::InvalidArgument("at-rest: subkey must be 32 bytes");
  }
  const std::string nonce(sealed_page, kPageNonceLen);
  const std::string ct_tag(sealed_page + kPageNonceLen, kPageSize + crypto::kAesGcmTagLen);
  std::string aad;
  PageAad(file_tag, page_id, &aad);
  std::string plaintext;
  if (!crypto::AesGcmOpen(subkey, nonce, ct_tag, aad, &plaintext) ||
      plaintext.size() != kPageSize) {
    return Status::Corruption("at-rest: page authentication failed for page " +
                              std::to_string(page_id) + " (wrong key, swapped or tampered page)");
  }
  std::memcpy(out_plaintext, plaintext.data(), kPageSize);
  Zeroize(plaintext);
  return Status::OK();
}

StatusOr<std::string> ReadSealedJsonFile(const std::string& path, const std::string& dek,
                                         const std::string& aad) {
  if (!dek.empty() && dek.size() != 32) {
    return Status::InvalidArgument("at-rest: DEK must be 32 bytes");
  }
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return Status::NotFound(std::string("no file at ") + path);
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string text = ss.str();
  if (text.empty()) return text;
  const bool sealed = LooksSealedFile(text);
  if (sealed && dek.empty()) {
    return Status::Corruption("at-rest: sealed file " + path + " opened without encryption");
  }
  if (!sealed && !dek.empty()) {
    return Status::Corruption("at-rest: plaintext file " + path +
                              " opened with encryption enabled; migrate it with "
                              "`desentryd --re-encrypt`");
  }
  if (!sealed) return text;
  auto subkey_or = FileSubkey(dek, aad);
  if (!subkey_or.ok()) return subkey_or.status();
  return OpenFile(subkey_or.value(), text, aad);
}

Status WriteSealedJsonFile(const std::string& path, const std::string& content,
                           const std::string& dek, const std::string& aad) {
  if (!dek.empty() && dek.size() != 32) {
    return Status::InvalidArgument("at-rest: DEK must be 32 bytes");
  }
  std::string out = content;
  if (!dek.empty()) {
    auto subkey_or = FileSubkey(dek, aad);
    if (!subkey_or.ok()) return subkey_or.status();
    auto sealed_or = SealFile(subkey_or.value(), content, aad);
    if (!sealed_or.ok()) return sealed_or.status();
    out = sealed_or.value();
  }
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc | std::ios::binary);
    if (!f.is_open()) return Status::IOError("cannot write " + tmp);
    f << out;
    f.flush();
    if (!f.good()) return Status::IOError("write failed: " + tmp);
  }
  std::remove(path.c_str());  // Windows rename() refuses to clobber
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    return Status::IOError("cannot commit " + path);
  }
  return Status::OK();
}

Status WritePageHeaderFile(const std::string& db_file, const std::string& tag_hex) {
  const std::string header_path = PageHeaderPath(db_file);
  const std::string content =
      "{\"magic\":\"DSNENC1\",\"file_tag_hex\":\"" + tag_hex + "\"}";
  const std::string tmp = header_path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc | std::ios::binary);
    if (!f.is_open()) return Status::IOError("cannot write " + tmp);
    f << content;
    f.flush();
    if (!f.good()) return Status::IOError("page header write failed: " + tmp);
  }
  std::remove(header_path.c_str());  // Windows rename() refuses to clobber
  if (std::rename(tmp.c_str(), header_path.c_str()) != 0) {
    return Status::IOError("cannot commit " + header_path);
  }
  return Status::OK();
}

StatusOr<std::string> ReadPageHeaderFile(const std::string& db_file) {
  const std::string header_path = PageHeaderPath(db_file);
  std::ifstream f(header_path, std::ios::binary);
  if (!f.is_open()) return Status::NotFound(std::string("no page header at ") + header_path);
  std::ostringstream ss;
  ss << f.rdbuf();
  const std::string text = ss.str();
  // Minimal parse (no JSON dependency in this module): find "file_tag_hex":"...".
  const std::string key = "\"file_tag_hex\"";
  const size_t kpos = text.find(key);
  if (text.find("\"DSNENC1\"") == std::string::npos || kpos == std::string::npos) {
    return Status::Corruption("page header is missing its magic or file tag");
  }
  const size_t q1 = text.find('"', kpos + key.size());
  if (q1 == std::string::npos) return Status::Corruption("page header is missing its file tag");
  const size_t q2 = text.find('"', q1 + 1);
  if (q2 == std::string::npos) return Status::Corruption("page header is missing its file tag");
  const std::string tag = text.substr(q1 + 1, q2 - q1 - 1);
  if (tag.empty()) return Status::Corruption("page header is missing its file tag");
  return tag;
}

Status DetectLogMode(const std::string& path, uint32_t record_cap, bool have_key,
                     const std::string& log_name, bool* sealed) {
  *sealed = false;
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return Status::OK();  // created empty by the caller: new log
  char len_buf[4];
  in.read(len_buf, 4);
  if (in.gcount() < 4) return Status::OK();  // empty file: new log
  uint32_t body_len = 0;
  std::memcpy(&body_len, len_buf, 4);
  if (body_len == 0 || body_len > record_cap) {
    return Status::Corruption(log_name + ": implausible first record length -- not a " +
                              log_name + " file");
  }
  const size_t peek = body_len < 64 ? body_len : 64;
  std::string head(peek, '\0');
  in.read(head.data(), static_cast<std::streamsize>(peek));
  if (static_cast<size_t>(in.gcount()) < peek) return Status::OK();  // torn tail: mode unknown
  *sealed = head.size() >= 4 && LooksSealedRecord(head);
  if (*sealed && !have_key) {
    return Status::Corruption("at-rest: sealed " + log_name + " " + path +
                              " opened without encryption (missing unlock key?)");
  }
  if (!*sealed && have_key) {
    return Status::Corruption("at-rest: plaintext " + log_name + " " + path +
                              " opened with encryption enabled; migrate it with "
                              "`desentryd --re-encrypt`");
  }
  return Status::OK();
}

}  // namespace desentry::at_rest
