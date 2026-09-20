#pragma once
// At-rest encryption primitives: the layer that makes `encrypt_at_rest`
// actually enforced rather than a warning in main.cpp.
//
// Everything here is AES-256-GCM over the OpenSSL EVP API (via crypto.h --
// this file never touches libcrypto directly), with per-file subkeys derived
// from the node's 32-byte DEK by HKDF-SHA256. Three shapes cover every file
// the engine writes:
//
//   * **Pages** (`*.dsf` via DiskManager): a 4096-byte page is sealed to
//     `[12B random nonce][4096B ciphertext][16B tag]` (4124 bytes on disk).
//     The nonce is random per write, so no per-page version state is needed
//     and a crash can never cause nonce reuse. AAD binds file_tag + page_id,
//     so a page swapped across files or within a file fails authentication.
//   * **Log records** (WAL, transit.log, outbox.log, cross-engine index):
//     the record payload is sealed to `"DSWE" + nonce + ct + tag`. CRC and
//     length framing are unchanged and cover the sealed bytes. AAD binds the
//     log kind ("wal", "transit", ...).
//   * **Whole-file JSON** (catalog.json, roots.json, engine manifests,
//     identity.key): the file content is sealed to `"DSNJ" + nonce + ct +
//     tag`. AAD binds the file kind ("catalog", "roots", ...).
//
// Fail-closed discipline (the load-bearing rule of this file): a sealed
// record/file opened without a key, a plaintext record/file opened WITH a
// key, and any authentication failure all return Corruption -- never silent
// plaintext, never a zeroed page pretending to be data. Callers distinguish
// a torn tail (short read: benign crash, handled by each format's existing
// torn-tail path BEFORE unsealing) from present-but-bad bytes.
//
// The DEK arrives from the sidecar as Crockford-base32 text on stdin (the
// same encoding recovery keys use); DecodeRecoveryKey parses it with the
// same transcription tolerance as the app. The DEK never touches disk.
//
// Known limits, stated not glossed: page rollback to an older sealed image
// passes authentication (GCM detects tampering, not age) -- crash
// consistency stays the WAL's job, and WAL replay heals a rolled-back page.
// Sealed files are ~0.7% (pages) to ~28B/record larger; quota accounting
// keeps its existing slack rather than pretending exactness.

#include <cstdint>
#include <string>

#include "desentry/common/status.h"
#include "desentry/storage/page.h"

namespace desentry::at_rest {

// On-disk sealed page stride (4096B plaintext + 12B nonce + 16B tag).
constexpr size_t kPageNonceLen = 12;
constexpr size_t kPageTagLen = 16;
constexpr size_t kSealedPageSize = kPageSize + kPageNonceLen + kPageTagLen;

// Magic prefixing every sealed log-record payload ("DSWE" = DeSentry W-Encrypted).
inline constexpr char kSealedRecordMagic[4] = {'D', 'S', 'W', 'E'};
// Magic prefixing every sealed whole-file blob ("DSNJ" = DeSentry JSON/sealed).
inline constexpr char kSealedFileMagic[4] = {'D', 'S', 'N', 'J'};
// Magic of the per-.dsf sidecar header naming the file's random tag.
inline constexpr char kPageHeaderMagic[8] = {'D', 'S', 'N', 'E', 'N', 'C', '1', '\0'};

// Parses Crockford-base32 recovery-key text (case/space/dash-insensitive,
// O->0, I/L->1) into the raw 32-byte DEK. Anything else is Corruption --
// this is where a mistyped stdin key becomes a clean fatal, not a boot with
// the wrong key.
StatusOr<std::string> DecodeRecoveryKey(const std::string& text);

// HKDF-SHA256(DEK, salt=file_tag, info="desentry-at-rest-v1") -> 32B subkey.
// Never throws: OpenSSL failures become Status::Internal.
StatusOr<std::string> FileSubkey(const std::string& dek, const std::string& file_tag);

// Seals a log-record payload: "DSWE" + random nonce + ct + tag. AAD binds
// the log kind. Never throws.
StatusOr<std::string> SealRecord(const std::string& subkey, const std::string& plaintext,
                                 const std::string& aad);
// Opens a sealed record payload. Corruption on bad magic or auth failure.
// Never throws.
StatusOr<std::string> OpenRecord(const std::string& subkey, const std::string& sealed,
                                 const std::string& aad);
// True when `bytes` starts with the sealed-record magic (caller must still
// have at least the magic; short reads are the format's torn-tail path).
bool LooksSealedRecord(const std::string& bytes);

// Whole-file seal: "DSNJ" + random nonce + ct + tag. Same Status-only rules.
StatusOr<std::string> SealFile(const std::string& subkey, const std::string& plaintext,
                               const std::string& aad);
StatusOr<std::string> OpenFile(const std::string& subkey, const std::string& sealed,
                               const std::string& aad);
bool LooksSealedFile(const std::string& bytes);

// Seals one 4096-byte page into a 4124-byte image with a fresh random nonce.
// AAD binds file_tag + page_id. Never throws; Corruption is impossible here
// (sealing cannot fail except on OpenSSL/environment errors -> Internal).
Status SealPage(const std::string& subkey, page_id_t page_id, const std::string& file_tag,
                const char* plaintext_page, char* out_sealed);
// Opens one sealed page image. Corruption on auth failure (wrong key or
// tampered/swapped page) -- never a zeroed page masquerading as data.
Status OpenPage(const std::string& subkey, page_id_t page_id, const std::string& file_tag,
                const char* sealed_page, char* out_plaintext);

// Peeks the first frame of a [u32 len][body][u32 crc] append log to decide
// sealed vs plaintext before the full read. Missing/empty/torn file: a new
// log in the current mode (OK, *sealed=false). Sealed-but-no-key or
// plaintext-but-key: Corruption naming the migrate tool. `record_cap` bounds
// the peek read (each log's own cap); an implausible first length is
// Corruption, never a guess.
Status DetectLogMode(const std::string& path, uint32_t record_cap, bool have_key,
                     const std::string& log_name, bool* sealed);

// Whole-file JSON persistence with transparent sealing (catalog.json,
// roots.json, engine manifests). Read: missing file -> NotFound (callers
// treat as fresh); empty file -> empty text; sealed content without a key,
// plaintext content with a key, or authentication failure -> Corruption.
// Write: sealed when dek is set, via tmp+rename exactly like the previous
// plaintext paths (a crash mid-save never leaves a truncated file).
StatusOr<std::string> ReadSealedJsonFile(const std::string& path, const std::string& dek,
                                         const std::string& aad);
Status WriteSealedJsonFile(const std::string& path, const std::string& content,
                           const std::string& dek, const std::string& aad);

// Per-.dsf sidecar header (`<db>.enc`): non-secret JSON naming the file's
// random tag. Shared by DiskManager (which reads/writes it at open) and the
// offline --re-encrypt tool (which writes it during migration), so the shape
// is defined once: {"magic":"DSNENC1","file_tag_hex":"..."}.
Status WritePageHeaderFile(const std::string& db_file, const std::string& tag_hex);
StatusOr<std::string> ReadPageHeaderFile(const std::string& db_file);
inline std::string PageHeaderPath(const std::string& db_file) { return db_file + ".enc"; }

// Best-effort memory hygiene for key material held in std::strings.
inline void Zeroize(std::string& secret) {
  volatile char* p = secret.empty() ? nullptr : &secret[0];
  for (size_t i = 0; p != nullptr && i < secret.size(); ++i) p[i] = 0;
}

}  // namespace desentry::at_rest
