#include "desentry/security/crypto.h"

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include <memory>
#include <stdexcept>
#include <vector>

namespace desentry::crypto {

namespace {

[[noreturn]] void ThrowOpenSsl(const std::string& where) {
  throw std::runtime_error("OpenSSL error in " + where);
}

using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using CtxPtr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using CipherCtxPtr = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

PkeyPtr GenRawKeyPair(int nid, std::string* out_priv, std::string* out_pub, size_t key_len) {
  CtxPtr ctx(EVP_PKEY_CTX_new_id(nid, nullptr), &EVP_PKEY_CTX_free);
  if (!ctx || EVP_PKEY_keygen_init(ctx.get()) <= 0) ThrowOpenSsl("keygen_init");
  EVP_PKEY* raw = nullptr;
  if (EVP_PKEY_keygen(ctx.get(), &raw) <= 0) ThrowOpenSsl("keygen");
  PkeyPtr pkey(raw, &EVP_PKEY_free);

  size_t priv_len = key_len;
  out_priv->resize(priv_len);
  if (EVP_PKEY_get_raw_private_key(pkey.get(), reinterpret_cast<unsigned char*>(out_priv->data()), &priv_len) <= 0) {
    ThrowOpenSsl("get_raw_private_key");
  }
  out_priv->resize(priv_len);

  size_t pub_len = key_len;
  out_pub->resize(pub_len);
  if (EVP_PKEY_get_raw_public_key(pkey.get(), reinterpret_cast<unsigned char*>(out_pub->data()), &pub_len) <= 0) {
    ThrowOpenSsl("get_raw_public_key");
  }
  out_pub->resize(pub_len);
  return pkey;
}

}  // namespace

Ed25519KeyPair GenerateEd25519() {
  Ed25519KeyPair kp;
  GenRawKeyPair(EVP_PKEY_ED25519, &kp.private_key, &kp.public_key, kEd25519PublicKeyLen);
  return kp;
}

std::string Ed25519Sign(const std::string& private_key_seed, const std::string& message) {
  PkeyPtr pkey(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                             reinterpret_cast<const unsigned char*>(private_key_seed.data()),
                                             private_key_seed.size()),
               &EVP_PKEY_free);
  if (!pkey) ThrowOpenSsl("new_raw_private_key(ed25519)");

  MdCtxPtr mdctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!mdctx) ThrowOpenSsl("MD_CTX_new");
  if (EVP_DigestSignInit(mdctx.get(), nullptr, nullptr, nullptr, pkey.get()) <= 0) ThrowOpenSsl("DigestSignInit");

  size_t sig_len = 0;
  if (EVP_DigestSign(mdctx.get(), nullptr, &sig_len, reinterpret_cast<const unsigned char*>(message.data()),
                      message.size()) <= 0) {
    ThrowOpenSsl("DigestSign(size probe)");
  }
  std::string sig(sig_len, '\0');
  if (EVP_DigestSign(mdctx.get(), reinterpret_cast<unsigned char*>(sig.data()), &sig_len,
                      reinterpret_cast<const unsigned char*>(message.data()), message.size()) <= 0) {
    ThrowOpenSsl("DigestSign");
  }
  sig.resize(sig_len);
  return sig;
}

bool Ed25519Verify(const std::string& public_key, const std::string& message, const std::string& signature) {
  if (public_key.size() != kEd25519PublicKeyLen) return false;
  PkeyPtr pkey(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                            reinterpret_cast<const unsigned char*>(public_key.data()),
                                            public_key.size()),
               &EVP_PKEY_free);
  if (!pkey) return false;

  MdCtxPtr mdctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!mdctx) return false;
  if (EVP_DigestVerifyInit(mdctx.get(), nullptr, nullptr, nullptr, pkey.get()) <= 0) return false;

  int rc = EVP_DigestVerify(mdctx.get(), reinterpret_cast<const unsigned char*>(signature.data()), signature.size(),
                             reinterpret_cast<const unsigned char*>(message.data()), message.size());
  return rc == 1;
}

X25519KeyPair GenerateX25519() {
  X25519KeyPair kp;
  GenRawKeyPair(EVP_PKEY_X25519, &kp.private_key, &kp.public_key, kX25519KeyLen);
  return kp;
}

std::string X25519Ecdh(const std::string& our_private_key, const std::string& their_public_key) {
  PkeyPtr ours(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr,
                                             reinterpret_cast<const unsigned char*>(our_private_key.data()),
                                             our_private_key.size()),
               &EVP_PKEY_free);
  PkeyPtr theirs(EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr,
                                              reinterpret_cast<const unsigned char*>(their_public_key.data()),
                                              their_public_key.size()),
                 &EVP_PKEY_free);
  if (!ours || !theirs) ThrowOpenSsl("X25519Ecdh: key construction");

  CtxPtr dctx(EVP_PKEY_CTX_new(ours.get(), nullptr), &EVP_PKEY_CTX_free);
  if (!dctx || EVP_PKEY_derive_init(dctx.get()) <= 0) ThrowOpenSsl("derive_init");
  if (EVP_PKEY_derive_set_peer(dctx.get(), theirs.get()) <= 0) ThrowOpenSsl("derive_set_peer");

  size_t len = 0;
  if (EVP_PKEY_derive(dctx.get(), nullptr, &len) <= 0) ThrowOpenSsl("derive(size probe)");
  std::string secret(len, '\0');
  if (EVP_PKEY_derive(dctx.get(), reinterpret_cast<unsigned char*>(secret.data()), &len) <= 0) {
    ThrowOpenSsl("derive");
  }
  secret.resize(len);
  return secret;
}

std::string Sha256(const std::string& data) {
  std::string out(kSha256Len, '\0');
  unsigned int len = 0;
  if (EVP_Digest(data.data(), data.size(), reinterpret_cast<unsigned char*>(out.data()), &len, EVP_sha256(),
                  nullptr) <= 0) {
    ThrowOpenSsl("EVP_Digest(sha256)");
  }
  out.resize(len);
  return out;
}

std::string HkdfSha256(const std::string& secret, const std::string& salt, const std::string& info,
                        size_t out_len) {
  CtxPtr kctx(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), &EVP_PKEY_CTX_free);
  if (!kctx || EVP_PKEY_derive_init(kctx.get()) <= 0) ThrowOpenSsl("hkdf derive_init");
  if (EVP_PKEY_CTX_set_hkdf_md(kctx.get(), EVP_sha256()) <= 0) ThrowOpenSsl("hkdf set_md");
  if (!salt.empty() &&
      EVP_PKEY_CTX_set1_hkdf_salt(kctx.get(), reinterpret_cast<const unsigned char*>(salt.data()), static_cast<int>(salt.size())) <= 0) {
    ThrowOpenSsl("hkdf set_salt");
  }
  if (EVP_PKEY_CTX_set1_hkdf_key(kctx.get(), reinterpret_cast<const unsigned char*>(secret.data()), static_cast<int>(secret.size())) <= 0) {
    ThrowOpenSsl("hkdf set_key");
  }
  if (!info.empty() &&
      EVP_PKEY_CTX_add1_hkdf_info(kctx.get(), reinterpret_cast<const unsigned char*>(info.data()), static_cast<int>(info.size())) <= 0) {
    ThrowOpenSsl("hkdf add_info");
  }
  std::string out(out_len, '\0');
  size_t len = out_len;
  if (EVP_PKEY_derive(kctx.get(), reinterpret_cast<unsigned char*>(out.data()), &len) <= 0) {
    ThrowOpenSsl("hkdf derive");
  }
  out.resize(len);
  return out;
}

std::string RandomBytes(size_t n) {
  std::string out(n, '\0');
  if (RAND_bytes(reinterpret_cast<unsigned char*>(out.data()), static_cast<int>(n)) <= 0) {
    ThrowOpenSsl("RAND_bytes");
  }
  return out;
}

std::string AesGcmSeal(const std::string& key, const std::string& nonce, const std::string& plaintext,
                        const std::string& aad) {
  if (key.size() != kAesGcmKeyLen || nonce.size() != kAesGcmNonceLen) {
    throw std::runtime_error("AesGcmSeal: bad key/nonce length");
  }
  CipherCtxPtr ctx(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
  if (!ctx) ThrowOpenSsl("CIPHER_CTX_new");

  if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) <= 0) ThrowOpenSsl("EncryptInit(1)");
  if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kAesGcmNonceLen), nullptr) <= 0) {
    ThrowOpenSsl("set ivlen");
  }
  if (EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, reinterpret_cast<const unsigned char*>(key.data()),
                          reinterpret_cast<const unsigned char*>(nonce.data())) <= 0) {
    ThrowOpenSsl("EncryptInit(2)");
  }

  int len = 0;
  if (!aad.empty()) {
    if (EVP_EncryptUpdate(ctx.get(), nullptr, &len, reinterpret_cast<const unsigned char*>(aad.data()),
                           static_cast<int>(aad.size())) <= 0) {
      ThrowOpenSsl("EncryptUpdate(aad)");
    }
  }

  std::string ciphertext(plaintext.size(), '\0');
  int out_len = 0;
  if (EVP_EncryptUpdate(ctx.get(), reinterpret_cast<unsigned char*>(ciphertext.data()), &out_len,
                         reinterpret_cast<const unsigned char*>(plaintext.data()), static_cast<int>(plaintext.size())) <= 0) {
    ThrowOpenSsl("EncryptUpdate(pt)");
  }
  int final_len = 0;
  if (EVP_EncryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char*>(ciphertext.data()) + out_len, &final_len) <= 0) {
    ThrowOpenSsl("EncryptFinal");
  }
  ciphertext.resize(static_cast<size_t>(out_len + final_len));

  std::string tag(kAesGcmTagLen, '\0');
  if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(kAesGcmTagLen), tag.data()) <= 0) {
    ThrowOpenSsl("get tag");
  }
  return ciphertext + tag;
}

bool AesGcmOpen(const std::string& key, const std::string& nonce, const std::string& ciphertext_and_tag,
                 const std::string& aad, std::string* out_plaintext) {
  if (key.size() != kAesGcmKeyLen || nonce.size() != kAesGcmNonceLen) return false;
  if (ciphertext_and_tag.size() < kAesGcmTagLen) return false;

  size_t ct_len = ciphertext_and_tag.size() - kAesGcmTagLen;
  const char* tag_ptr = ciphertext_and_tag.data() + ct_len;

  CipherCtxPtr ctx(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
  if (!ctx) return false;

  if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) <= 0) return false;
  if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kAesGcmNonceLen), nullptr) <= 0) return false;
  if (EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, reinterpret_cast<const unsigned char*>(key.data()),
                          reinterpret_cast<const unsigned char*>(nonce.data())) <= 0) {
    return false;
  }

  int len = 0;
  if (!aad.empty()) {
    if (EVP_DecryptUpdate(ctx.get(), nullptr, &len, reinterpret_cast<const unsigned char*>(aad.data()),
                           static_cast<int>(aad.size())) <= 0) {
      return false;
    }
  }

  std::string plaintext(ct_len, '\0');
  int out_len = 0;
  if (ct_len > 0) {
    if (EVP_DecryptUpdate(ctx.get(), reinterpret_cast<unsigned char*>(plaintext.data()), &out_len,
                           reinterpret_cast<const unsigned char*>(ciphertext_and_tag.data()),
                           static_cast<int>(ct_len)) <= 0) {
      return false;
    }
  }

  if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(kAesGcmTagLen),
                           const_cast<char*>(tag_ptr)) <= 0) {
    return false;
  }

  int final_len = 0;
  int ok = EVP_DecryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char*>(plaintext.data()) + out_len, &final_len);
  if (ok <= 0) return false;  // tag mismatch: authentication failed

  plaintext.resize(static_cast<size_t>(out_len + final_len));
  *out_plaintext = std::move(plaintext);
  return true;
}

}  // namespace desentry::crypto
