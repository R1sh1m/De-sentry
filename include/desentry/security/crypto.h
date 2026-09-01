#pragma once
// Thin, deliberately narrow wrappers around OpenSSL's EVP API -- this file
// is the *only* place in the codebase that touches libcrypto directly.
// Everything above it (net/identity.h, net/secure_channel.h) works in
// terms of these functions, never raw OpenSSL handles, which keeps the
// actual cryptographic code auditable in one small file (see
// ARCHITECTURE.md §7 for the protocol these primitives compose into).
//
// Primitives provided:
//   * Ed25519 keypair generation, signing, verification  -- node identity
//   * X25519 keypair generation, ECDH                     -- session key agreement
//   * SHA-256                                              -- node_id derivation
//   * HKDF-SHA256                                          -- session key derivation
//   * AES-256-GCM seal/open                                -- authenticated wire encryption
//
// No third-party crypto library (e.g. libsodium) is used: this build's
// sandbox has OpenSSL's dev headers available but not libsodium's, and
// OpenSSL's EVP layer has supported Ed25519/X25519 natively since 1.1.1,
// so nothing here is a downgrade.

#include <array>
#include <string>

namespace desentry::crypto {

constexpr size_t kEd25519PublicKeyLen = 32;
constexpr size_t kEd25519PrivateKeyLen = 32;  // seed length (raw)
constexpr size_t kEd25519SignatureLen = 64;
constexpr size_t kX25519KeyLen = 32;
constexpr size_t kSha256Len = 32;
constexpr size_t kAesGcmKeyLen = 32;   // AES-256
constexpr size_t kAesGcmNonceLen = 12;
constexpr size_t kAesGcmTagLen = 16;

struct Ed25519KeyPair {
  std::string private_key;  // 32-byte raw seed
  std::string public_key;   // 32-byte raw public key
};

struct X25519KeyPair {
  std::string private_key;  // 32-byte raw
  std::string public_key;   // 32-byte raw
};

// Throws std::runtime_error on any OpenSSL failure -- these are all
// programmer/environment errors (bad key length, corrupt key material),
// not expected-failure control flow, hence exceptions rather than Status.
Ed25519KeyPair GenerateEd25519();
std::string Ed25519Sign(const std::string& private_key_seed, const std::string& message);
bool Ed25519Verify(const std::string& public_key, const std::string& message, const std::string& signature);

X25519KeyPair GenerateX25519();
// Computes the shared secret from our private key and their public key.
std::string X25519Ecdh(const std::string& our_private_key, const std::string& their_public_key);

std::string Sha256(const std::string& data);

// Derives `out_len` bytes from `secret` (+ optional salt/info) via HKDF-SHA256.
std::string HkdfSha256(const std::string& secret, const std::string& salt, const std::string& info,
                        size_t out_len);

std::string RandomBytes(size_t n);

// AES-256-GCM authenticated encryption. `nonce` must be kAesGcmNonceLen
// bytes and must never repeat under the same key (see net/secure_channel.h
// for how the per-direction counter guarantees this). Returns
// ciphertext || 16-byte tag. AesGcmOpen returns false (no plaintext) if
// authentication fails -- callers must treat that as a dropped/attacked
// message, never as "empty message".
std::string AesGcmSeal(const std::string& key, const std::string& nonce, const std::string& plaintext,
                        const std::string& aad);
bool AesGcmOpen(const std::string& key, const std::string& nonce, const std::string& ciphertext_and_tag,
                 const std::string& aad, std::string* out_plaintext);

}  // namespace desentry::crypto
