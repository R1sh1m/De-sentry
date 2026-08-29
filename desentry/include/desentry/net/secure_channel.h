#pragma once
// The authenticated-encryption handshake described in ARCHITECTURE.md §7.2:
// each side sends a signed ephemeral X25519 public key, both derive a
// shared secret via ECDH, and HKDF splits that into two independent
// AES-256-GCM keys (one per direction) so encryption and decryption never
// share a nonce space. Ephemeral keys mean every connection gets a fresh
// secret -- compromising a node's long-term identity key later does not
// let an attacker decrypt a session recorded earlier.

#include <cstdint>
#include <string>

#include "desentry/common/status.h"
#include "desentry/net/identity.h"
#include "desentry/net/wire_protocol.h"

namespace desentry {

struct SessionKeys {
  std::string send_key;
  std::string recv_key;
  uint64_t send_counter = 0;
  uint64_t recv_counter = 0;
};

struct HandshakeResult {
  SessionKeys keys;
  std::string peer_node_id;
  std::string peer_ed25519_pubkey;
  uint16_t peer_p2p_port = 0;
};

// Client side: we initiated the TCP connection.
StatusOr<HandshakeResult> ClientHandshake(int sockfd, const NodeIdentity& identity, uint16_t our_p2p_port);

// Server side: we accepted the TCP connection.
StatusOr<HandshakeResult> ServerHandshake(int sockfd, const NodeIdentity& identity, uint16_t our_p2p_port);

// Encrypts `msg` and writes it as one length-prefixed frame; advances
// keys->send_counter.
Status SendEncrypted(int sockfd, SessionKeys* keys, const WireMessage& msg);

// Reads one length-prefixed frame and decrypts it; advances
// keys->recv_counter. Returns a NetworkError (not a crash) if authentication
// fails -- the same treatment as any other malformed/hostile input.
StatusOr<WireMessage> RecvEncrypted(int sockfd, SessionKeys* keys, size_t max_len);

}  // namespace desentry
