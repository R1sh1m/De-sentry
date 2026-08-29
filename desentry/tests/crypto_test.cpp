// Crypto primitive test suite: Ed25519 sign/verify, X25519 ECDH agreement,
// SHA-256, HKDF, and AES-256-GCM including tamper/AAD-mismatch rejection --
// the properties the whole secure-channel handshake (net/secure_channel.h)
// depends on.

#include <cassert>
#include <iostream>

#include "desentry/security/crypto.h"

using namespace desentry::crypto;

int main() {
  auto kp = GenerateEd25519();
  assert(kp.private_key.size() == kEd25519PrivateKeyLen);
  assert(kp.public_key.size() == kEd25519PublicKeyLen);
  std::string msg = "de-sentry replicated write payload";
  std::string sig = Ed25519Sign(kp.private_key, msg);
  assert(sig.size() == kEd25519SignatureLen);
  assert(Ed25519Verify(kp.public_key, msg, sig));
  assert(!Ed25519Verify(kp.public_key, "tampered payload", sig));
  auto kp2 = GenerateEd25519();
  assert(!Ed25519Verify(kp2.public_key, msg, sig));
  std::cout << "[crypto_test] Ed25519 sign/verify: PASS" << std::endl;

  auto a = GenerateX25519();
  auto b = GenerateX25519();
  std::string secretA = X25519Ecdh(a.private_key, b.public_key);
  std::string secretB = X25519Ecdh(b.private_key, a.public_key);
  assert(secretA == secretB && secretA.size() == kX25519KeyLen);
  std::cout << "[crypto_test] X25519 ECDH shared-secret agreement: PASS" << std::endl;

  auto h1 = Sha256("hello");
  auto h2 = Sha256("hello");
  auto h3 = Sha256("hellO");
  assert(h1 == h2 && h1 != h3 && h1.size() == 32);
  std::cout << "[crypto_test] SHA-256: PASS" << std::endl;

  auto k1 = HkdfSha256(secretA, "salt", "desentry-session", 32);
  auto k2 = HkdfSha256(secretB, "salt", "desentry-session", 32);
  assert(k1 == k2 && k1.size() == 32);
  std::cout << "[crypto_test] HKDF-SHA256: PASS" << std::endl;

  std::string nonce = RandomBytes(kAesGcmNonceLen);
  std::string plaintext = R"({"op":"put","key":"u1"})";
  std::string aad = "header-metadata";
  std::string sealed = AesGcmSeal(k1, nonce, plaintext, aad);
  std::string opened;
  assert(AesGcmOpen(k1, nonce, sealed, aad, &opened) && opened == plaintext);
  std::cout << "[crypto_test] AES-256-GCM round-trip: PASS" << std::endl;

  std::string tampered = sealed;
  tampered[0] ^= 0x01;
  std::string opened2;
  assert(!AesGcmOpen(k1, nonce, tampered, aad, &opened2));
  std::string opened3;
  assert(!AesGcmOpen(k1, nonce, sealed, "wrong-aad", &opened3));
  std::cout << "[crypto_test] AES-GCM tamper + AAD-mismatch rejection: PASS" << std::endl;

  std::cout << "[crypto_test] ALL CRYPTO TESTS PASSED" << std::endl;
  return 0;
}
