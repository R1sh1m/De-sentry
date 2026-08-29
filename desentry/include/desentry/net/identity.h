#pragma once
// A peer's persistent cryptographic identity: an Ed25519 signing keypair,
// generated once on first boot and reused across restarts (data/identity.key,
// mode 0600). node_id = hex(SHA-256(public_key))[:32] -- derived, not
// chosen, so it can't be spoofed without the private key (ARCHITECTURE.md §7.1).
//
// This is deliberately just the long-lived *signing* identity. The
// per-connection X25519 key used for the secure-channel handshake
// (net/secure_channel.h) is generated fresh for every connection instead
// of being part of this persistent identity, which is what gives each
// connection forward secrecy: compromising a stored identity key later
// doesn't let an attacker decrypt a session recorded earlier.

#include <string>

#include "desentry/common/status.h"

namespace desentry {

class NodeIdentity {
 public:
  static StatusOr<NodeIdentity> LoadOrCreate(const std::string& key_file);

  const std::string& node_id() const { return node_id_; }
  const std::string& public_key() const { return public_key_; }

  std::string Sign(const std::string& message) const;
  static bool Verify(const std::string& public_key, const std::string& message, const std::string& signature);
  static std::string DeriveNodeId(const std::string& public_key);

 private:
  NodeIdentity() = default;

  std::string private_key_;
  std::string public_key_;
  std::string node_id_;
};

}  // namespace desentry
