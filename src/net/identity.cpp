#include "desentry/net/identity.h"


#include <fstream>
#include <sstream>

#include "desentry/common/logger.h"
#include "desentry/security/at_rest.h"
#include "desentry/security/crypto.h"

namespace desentry {

namespace {
std::string ToHex(const std::string& bytes) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char c : bytes) {
    out.push_back(kHex[c >> 4]);
    out.push_back(kHex[c & 0xF]);
  }
  return out;
}
}  // namespace

std::string NodeIdentity::DeriveNodeId(const std::string& public_key) {
  return ToHex(crypto::Sha256(public_key)).substr(0, 32);
}

StatusOr<NodeIdentity> NodeIdentity::LoadOrCreate(const std::string& key_file,
                                                    const std::string& dek) {
  if (!dek.empty() && dek.size() != 32) {
    return Status::InvalidArgument("at-rest: DEK must be 32 bytes");
  }
  NodeIdentity id;

  std::ifstream in(key_file, std::ios::binary);
  if (in.is_open()) {
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string raw = ss.str();
    if (!raw.empty()) {
      // Sealed identities open here (fail closed without the key); a
      // plaintext identity file opened with a key is refused rather than
      // silently adopted -- migrate with `desentryd --re-encrypt`.
      if (at_rest::LooksSealedFile(raw)) {
        if (dek.empty()) {
          return Status::Corruption("at-rest: sealed identity " + key_file +
                                    " opened without encryption (missing unlock key?)");
        }
        auto subkey_or = at_rest::FileSubkey(dek, "identity");
        if (!subkey_or.ok()) return subkey_or.status();
        auto open_or = at_rest::OpenFile(subkey_or.value(), raw, "identity");
        if (!open_or.ok()) return open_or.status();
        raw = open_or.value();
      } else if (!dek.empty()) {
        return Status::Corruption("at-rest: plaintext identity " + key_file +
                                  " opened with encryption enabled; migrate it with "
                                  "`desentryd --re-encrypt`");
      }
    }
    if (raw.size() == crypto::kEd25519PrivateKeyLen + crypto::kEd25519PublicKeyLen) {
      id.private_key_ = raw.substr(0, crypto::kEd25519PrivateKeyLen);
      id.public_key_ = raw.substr(crypto::kEd25519PrivateKeyLen);
      id.node_id_ = DeriveNodeId(id.public_key_);
      at_rest::Zeroize(raw);
      DSN_LOG_INFO("identity", "loaded existing identity, node_id=" << id.node_id_);
      return id;
    }
    at_rest::Zeroize(raw);
    // An empty file is a fresh node (created by the probe open); anything
    // else malformed regenerates exactly as before -- EXCEPT under
    // encryption, where regenerating over an undecryptable file would
    // orphan the node's data. Fail closed instead.
    std::ifstream probe(key_file, std::ios::binary | std::ios::ate);
    const bool empty = probe.is_open() && probe.tellg() == 0;
    if (!dek.empty() && !empty) {
      return Status::Corruption("at-rest: identity file " + key_file +
                                " is present but unreadable with this key");
    }
    DSN_LOG_WARN("identity", "identity file " << key_file << " is malformed, regenerating");
  }

  auto kp = crypto::GenerateEd25519();
  id.private_key_ = kp.private_key;
  id.public_key_ = kp.public_key;
  id.node_id_ = DeriveNodeId(id.public_key_);

  std::string content = id.private_key_ + id.public_key_;
  if (!dek.empty()) {
    auto subkey_or = at_rest::FileSubkey(dek, "identity");
    if (!subkey_or.ok()) return subkey_or.status();
    auto sealed_or = at_rest::SealFile(subkey_or.value(), content, "identity");
    at_rest::Zeroize(content);
    if (!sealed_or.ok()) return sealed_or.status();
    content = sealed_or.value();
  }
  std::ofstream out(key_file, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    at_rest::Zeroize(content);
    return Status::IOError("cannot write identity file: " + key_file);
  }
  out << content;
  out.close();
  at_rest::Zeroize(content);
  RestrictToOwner(key_file);  // chmod 0600 on POSIX; owner-only DACL on Windows

  DSN_LOG_INFO("identity", "generated new identity, node_id=" << id.node_id_
                                                               << (dek.empty() ? "" : " [sealed]"));
  return id;
}

std::string NodeIdentity::Sign(const std::string& message) const {
  return crypto::Ed25519Sign(private_key_, message);
}

bool NodeIdentity::Verify(const std::string& public_key, const std::string& message, const std::string& signature) {
  return crypto::Ed25519Verify(public_key, message, signature);
}

}  // namespace desentry
