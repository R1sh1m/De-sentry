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

#include "desentry/common/platform.h"
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
  // -- v2 -------------------------------------------------------------------
  kTransitQuery = 8,     // "are you holding anything for me?" (sender is authenticated)
  kTransitResponse = 9,  // the held envelopes, bytes included
  kTransitClaim = 10,    // "I applied these; you may release them after checkpoint"
  kLedgerDigest = 11,    // hash-only ledger convergence: my tip + recent entry hashes
  kLedgerDelta = 12,     // the entries the requester is missing, hashes-only if not a reader
};

const char* MessageTypeName(MessageType type);

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
  // Globally-unique id for this broadcast, so a peer that receives the same
  // push twice (directly and relayed) applies it once. CRDT merge is
  // idempotent, so a duplicate is never *incorrect* -- but at 50 nodes on
  // one LAN it is a real bandwidth and CPU cost, which is what dedup buys.
  std::string message_id;
  // Hops remaining. Eager broadcast in v1 was strictly one hop (see
  // node_engine.cpp on why merged remote writes are not rebroadcast); with
  // message-id dedup in place, a small bounded relay is now safe and cuts
  // convergence latency on a partially-connected mesh.
  uint8_t ttl = 1;
  std::string Encode() const;
  static OpBroadcastPayload Decode(const std::string& bytes);
};

// -- v2 payloads -----------------------------------------------------------

// A replica's answer to "are you holding anything for me?". `entries` carry
// the actual document bytes; the requester is the authenticated peer from
// the handshake, so a node cannot ask for another node's held bytes.
struct TransitEntry {
  std::string collection;
  std::string key;
  std::string key_hash;     // 32 raw bytes
  std::string encoded_doc;
  int64_t intent_lsn = -1;
  std::string holder_node;
};
struct TransitResponsePayload {
  std::vector<TransitEntry> entries;
  bool truncated = false;  // more envelopes exist than fit in one response
  std::string Encode() const;
  static TransitResponsePayload Decode(const std::string& bytes);
};

// "I applied these, you may release them once a checkpoint covers them."
struct TransitClaimPayload {
  std::vector<std::string> key_hashes;  // 32 raw bytes each
  std::string claimer_node;
  std::string Encode() const;
  static TransitClaimPayload Decode(const std::string& bytes);
};

// Hash-only ledger convergence. Peers exchange the SET of entry hashes they
// hold; the set is grow-only and merges by union, which is why every node
// converges on the same history without any node being authoritative. A peer
// that is not a reader of a private collection still participates fully here
// -- it learns hashes, never keys or bytes.
struct LedgerDigestPayload {
  int64_t tip_entry_id = -1;
  std::string tip_entry_hash;                // 32 raw bytes
  std::string tip_signature;                 // Ed25519 over "<id>:<hash>"
  int64_t from_entry_id = 0;                 // window the hashes below cover
  std::vector<std::string> entry_hashes;     // 32 raw bytes each, in LSN order
  std::string Encode() const;
  static LedgerDigestPayload Decode(const std::string& bytes);
};

struct LedgerEntrySummary {
  int64_t entry_id = -1;
  uint8_t operation = 0;      // WalRecordType
  std::string key_hash;       // 32 raw bytes -- always present
  std::string entry_hash;     // 32 raw bytes
  std::string prev_hash;      // 32 raw bytes
  std::string origin_node_id;
  std::string origin_signature;
  int64_t hlc_physical_ms = 0;
  uint32_t hlc_logical = 0;
  // Present only when the requesting peer is an authorised reader of the
  // collection. The gossip byte filter (net/gossip.cpp) is what strips them.
  std::string collection;
  std::string key;
};
struct LedgerDeltaPayload {
  std::vector<LedgerEntrySummary> entries;
  bool hashes_only = false;  // true when the requester is not a reader
  std::string Encode() const;
  static LedgerDeltaPayload Decode(const std::string& bytes);
};

// -- Framing over a raw fd ------------------------------------------------
// [4-byte big-endian length][payload bytes]. Used both for the plaintext
// handshake and, wrapping AEAD ciphertext, for every post-handshake
// message (see net/secure_channel.h).
Status WriteFrame(dsn_socket_t sockfd, const std::string& bytes);
StatusOr<std::string> ReadFrame(dsn_socket_t sockfd, size_t max_len);

std::string EncodeMessage(const WireMessage& msg);
StatusOr<WireMessage> DecodeMessage(const std::string& bytes);

}  // namespace desentry
