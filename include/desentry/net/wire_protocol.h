#pragma once
// P2P wire message types and framing.
//
// Transport model (deliberately simple -- see net/tcp_transport.h for the
// full rationale): every P2P exchange is a short-lived TCP connection that
// does one handshake, one request, one response, then closes. That keeps
// this file to "one plaintext-length-prefixed frame, or one AEAD-sealed
// frame" with no persistent multiplexed-stream state machine to get wrong.
//
// Message payloads are encoded with ByteWriter/ByteReader (see
// common/byte_buffer.h) -- the same style used for the on-disk document
// codec, so there is exactly one binary-encoding idiom in the codebase.

#include <cstdint>
#include <string>
#include <vector>

#include "desentry/common/status.h"

namespace desentry {

enum class MessageType : uint8_t {
  kHello = 1,          // handshake: identity + ephemeral X25519 key + signature
  kDigest = 2,          // gossip round 1: "here's what I have for this collection"
  kDeltaResponse = 3,   // gossip round 1 reply: documents pushed + keys wanted
  kOpBroadcast = 4,     // eager single-write push, or gossip round 2 (wanted-key fulfillment)
  kPing = 5,
  kPong = 6,
  kError = 7,
};

struct WireMessage {
  MessageType type;
  std::string payload;
};

// -- Handshake payload -------------------------------------------------
struct HelloPayload {
  std::string node_id;
  std::string ed25519_pubkey;
  std::string x25519_ephemeral_pubkey;
  std::string signature;   // Ed25519 signature over x25519_ephemeral_pubkey
  uint16_t p2p_port = 0;   // the sender's own listen port, so the receiver can dial back
  std::string Encode() const;
  static HelloPayload Decode(const std::string& bytes);
};

// -- Gossip payloads -----------------------------------------------------
struct DigestEntry {
  std::string key;
  std::string top_ts_encoded;  // HLCTimestamp::Encode() of the document's freshest field
};
struct DigestPayload {
  std::string collection;
  std::vector<DigestEntry> entries;
  std::string Encode() const;
  static DigestPayload Decode(const std::string& bytes);
};

struct DocEntry {
  std::string key;
  std::string encoded_doc;  // CrdtValue::Encode()
};
struct DeltaResponsePayload {
  std::string collection;
  std::vector<DocEntry> pushed;         // documents the responder thinks the requester needs
  std::vector<std::string> wanted_keys;  // keys the responder wants the requester to push back
  std::string Encode() const;
  static DeltaResponsePayload Decode(const std::string& bytes);
};

struct OpBroadcastPayload {
  std::string collection;
  std::vector<DocEntry> docs;
  std::string Encode() const;
  static OpBroadcastPayload Decode(const std::string& bytes);
};

// -- Framing over a raw fd ------------------------------------------------
// [4-byte big-endian length][payload bytes]. Used both for the plaintext
// handshake and, wrapping AEAD ciphertext, for every post-handshake
// message (see net/secure_channel.h).
Status WriteFrame(int sockfd, const std::string& bytes);
StatusOr<std::string> ReadFrame(int sockfd, size_t max_len);

std::string EncodeMessage(const WireMessage& msg);
StatusOr<WireMessage> DecodeMessage(const std::string& bytes);

}  // namespace desentry
