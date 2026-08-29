#include "desentry/net/identity.h"

#include <sys/stat.h>

#include <fstream>
#include <sstream>

#include "desentry/common/logger.h"
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

StatusOr<NodeIdentity> NodeIdentity::LoadOrCreate(const std::string& key_file) {
  NodeIdentity id;

  std::ifstream in(key_file, std::ios::binary);
  if (in.is_open()) {
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string raw = ss.str();
    if (raw.size() == crypto::kEd25519PrivateKeyLen + crypto::kEd25519PublicKeyLen) {
      id.private_key_ = raw.substr(0, crypto::kEd25519PrivateKeyLen);
      id.public_key_ = raw.substr(crypto::kEd25519PrivateKeyLen);
      id.node_id_ = DeriveNodeId(id.public_key_);
      DSN_LOG_INFO("identity", "loaded existing identity, node_id=" << id.node_id_);
      return id;
    }
    DSN_LOG_WARN("identity", "identity file " << key_file << " is malformed, regenerating");
  }

  auto kp = crypto::GenerateEd25519();
  id.private_key_ = kp.private_key;
  id.public_key_ = kp.public_key;
  id.node_id_ = DeriveNodeId(id.public_key_);

  std::ofstream out(key_file, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    return Status::IOError("cannot write identity file: " + key_file);
  }
  out << id.private_key_ << id.public_key_;
  out.close();
  ::chmod(key_file.c_str(), 0600);

  DSN_LOG_INFO("identity", "generated new identity, node_id=" << id.node_id_);
  return id;
}

std::string NodeIdentity::Sign(const std::string& message) const {
  return crypto::Ed25519Sign(private_key_, message);
}

bool NodeIdentity::Verify(const std::string& public_key, const std::string& message, const std::string& signature) {
  return crypto::Ed25519Verify(public_key, message, signature);
}

}  // namespace desentry
