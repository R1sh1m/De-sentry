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
  // -- liveness ---------------------------------------------------------------
  kHeartbeat = 13,  // graded-failure-detector heartbeat: my tip + load + servability
  // -- receipts ---------------------------------------------------------------
  kMergeReceipt = 14,  // signed merge receipt from a peer (carried in kPong)
  // -- transit held-ack -------------------------------------------------------
  kTransitHeld = 15,  // holder confirms it has stored bytes for an offline owner
  // -- handshake key confirmation ---------------------------------------------
  kConfirm = 16,  // transcript signature proving identity binding (H-5 fix)
};

const char* MessageTypeName(MessageType type);

struct WireMessage {
  MessageType type;
  std::string payload;
};

// -- Handshake payload -------------------------------------------------
// Key-confirmation payload: signature over the full handshake transcript
// (H-5 fix). See secure_channel.h for the protocol.
struct ConfirmPayload {
  std::string transcript_signature;  // Ed25519 over TranscriptMessage
  std::string Encode() const;
  static ConfirmPayload Decode(const std::string& bytes);
};

// Domain-separated handshake transcript: client hello bytes first, then
// server hello bytes (raw encoded HELLO payloads, fixed order both sides).
std::string TranscriptMessage(const std::string& client_hello, const std::string& server_hello);

struct HelloPayload {
  std::string node_id;
  std::string ed25519_pubkey;
  std::string x25519_ephemeral_pubkey;
  std::string signature;   // Ed25519 signature over x25519_ephemeral_pubkey
  uint16_t p2p_port = 0;   // the sender's own listen port, so the receiver can dial back
  // Cluster-membership tag (trailing, tolerant): HMAC(cluster_secret,
  // x25519_ephemeral_pubkey). Empty on open-mesh peers. See
  // secure_channel.h VerifyHello for the enforcement rule.
  std::string membership_tag;  // 32 raw bytes when present
  std::string Encode() const;
  static HelloPayload Decode(const std::string& bytes);
};

// Domain-separated membership messages (cluster secret never leaves the HMAC).
// Tag over the ephemeral key proves current possession of the secret without
// a challenge round; beacons bind the advertised identity the same way.
std::string MembershipHelloMessage(const std::string& ephemeral_pubkey);
std::string MembershipBeaconMessage(const std::string& node_id, const std::string& pubkey,
                                    uint16_t p2p_port);

// -- Gossip payloads -----------------------------------------------------
struct DigestEntry {
  std::string key;
  std::string top_ts_encoded;  // HLCTimestamp::Encode() of the document's freshest field
  // First 8 bytes of SHA-256 over the stored document. The timestamp alone
  // cannot decide whether two copies agree: after a merge, a document's
  // freshest field can come from a write the *other* side already has, so two
  // peers holding genuinely different documents can report the same top
  // timestamp and each conclude there is nothing to exchange. That is a
  // permanent divergence, and this field is what breaks it.
  std::string content_hash;
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
  std::string encoded_doc;  // one chunk's bytes when chunk_total > 1
  int64_t intent_lsn = -1;
  std::string holder_node;
  // Striping (see TransitEnvelope): full document size plus this chunk's
  // position. Whole documents read chunk_total == 1.
  uint64_t doc_size_bytes = 0;
  uint32_t chunk_index = 0;
  uint32_t chunk_total = 1;
  // Content integrity (C-3 fix): SHA-256 of encoded_doc + holder's signature
  // over TransitAttestMessage(...). Trailing wire fields: old holders omit
  // them (claim path rejects entries without them), new holders always send.
  std::string content_hash;  // 32 raw bytes
  std::string holder_sig;
};
// "Are you holding anything for me?" The requester is the authenticated
// peer, so no owner field: a node can only ever ask for its own bytes.
// `offset` resumes a truncated listing (see TransitResponsePayload).
struct TransitQueryPayload {
  uint64_t offset = 0;  // envelopes to skip, in the holder's stable order
  std::string Encode() const;
  static TransitQueryPayload Decode(const std::string& bytes);
};
struct TransitResponsePayload {
  std::vector<TransitEntry> entries;
  bool truncated = false;  // more envelopes exist than fit in one response
  // Resume cursor: pass as the next query's offset to continue the listing
  // where this response stopped. Ordering is the holder's stable
  // intent_lsn order, so a cursor stays meaningful across calls.
  uint64_t next_offset = 0;
  // Wire versioning: Encode prefixes a magic ("DTR2"); Decode branches to
  // the v1 parse (no striping tail, no cursor) when it is absent, so old
  // holders stay readable. Trailing-field tolerance would be unsound here:
  // inside a repeated element a v1 entry followed by more entries is
  // indistinguishable from a tailed v2 entry.
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

// Holder confirms it has stored bytes for an offline owner (carried in
// kTransitHeld). Sent from holder to writer so the writer can count it
// toward its durability target when the owner is offline.
struct TransitHeldPayload {
  std::string message_id;       // the original broadcast's message_id
  std::string key_hash;         // 32 raw bytes (doc or chunk hash)
  std::string holder_node;      // node_id of the holder
  int64_t intent_lsn = -1;      // LSN of the TRANSIT_INTENT on holder's ledger
  std::string signature;        // Ed25519 over ("DSN-HELD-v1" || message_id || key_hash || intent_lsn)
  std::string Encode() const;
  static TransitHeldPayload Decode(const std::string& bytes);
};

// Signed merge receipt: a peer acknowledges applying a local write.
// Carried in the kPong response payload on the broadcast/fulfilment path
// (kPing/kPong for liveness still carries just the node_id string, so
// old peers and bootstrap adoption keep working). The two are
// distinguished by message type: HandleOpBroadcast gets kPong with a
// receipt; HandlePing gets kPong with a node_id.
struct MergeReceipt {
  std::string message_id;
  std::string key_hash;       // 32 raw bytes
  std::string applier_node;   // node_id of the peer that applied the merge
  int64_t applied_lsn = -1;   // LSN on the applier's ledger
  std::string signature;      // Ed25519 over ("DSN-RECEIPT-v1" || message_id || key_hash || applied_lsn)
  std::string Encode() const;
  static MergeReceipt Decode(const std::string& bytes);
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
  // Transit routing metadata, meaningful on TRANSIT_INTENT entries: which
  // holder keeps the bytes and which chunk this entry names. Carried for
  // every peer (reader or not) -- like key_hash, it names bytes without
  // disclosing them, and it is what lets a returning owner ask the right
  // holder directly instead of querying the whole mesh.
  std::string transit_holder;
  uint64_t transit_size_bytes = 0;
  uint32_t transit_chunk_index = 0;
  uint32_t transit_chunk_total = 1;
  // kAcl document bytes (the signed ACL envelope), carried for every peer
  // regardless of read access: ACL metadata names no user bytes, and without
  // it a peer could never learn a private collection's readers (H-1).
  // Empty on all other record types. Trailing wire field: absent on old
  // deltas, which simply carry no replicable ACLs.
  std::string acl_json;
};
struct LedgerDeltaPayload {
  std::vector<LedgerEntrySummary> entries;
  bool hashes_only = false;  // true when the requester is not a reader
  // Responder's signature over the delta tip (entries.back) under
  // NodeEngine::LedgerTipMessage -- verified in CollectReplicaTips against
  // the responder's handshake-proven key (C-6 fix). Empty on v1 deltas.
  std::string tip_signature;
  // Wire versioning, same scheme as TransitResponsePayload: Encode prefixes
  // a magic ("DLD2"); Decode branches to the v1 parse (entries end at key)
  // when it is absent.
  std::string Encode() const;
  static LedgerDeltaPayload Decode(const std::string& bytes);
};

// Liveness heartbeat for the graded failure detector (net/peer.h
// PeerSuspicion). Request and response share this shape: the requester sends
// its own heartbeat and the responder replies with its own, so one round
// trip updates both sides' liveness, ledger-height and load figures without
// a second exchange. Runs over the authenticated channel, so node_id is the
// handshake-proven identity, not a claim.
//
// Replaces kPing/kPong between current peers (kept for mixed-version
// safety: an old peer answers kHeartbeat with kError, and the prober falls
// back to kPing rather than marking it dead for speaking v1).
struct HeartbeatPayload {
  std::string node_id;  // sender's id; the responder's in the reply
  int64_t ledger_tip_entry_id = -1;
  uint64_t free_quota_mb = 0;
  // True when the sender enforces a quota (limit_bytes > 0). An unlimited
  // node reports free_quota_mb == 0 with quota_limited == false -- the same
  // honesty rule as PeerFitness::quota_reported.
  bool quota_limited = false;
  // Transit-store load: bytes held for offline owners, and the budget they
  // count against (0 == unbounded). Feeds holder-set sizing in the transit
  // flow; advisory, never authoritative.
  uint64_t transit_bytes_held = 0;
  uint64_t transit_budget_bytes = 0;
  // Self-assessed servability as a NodeLifecycleState byte (kRunning when
  // serving normally, kDegraded when over quota). Informational for the mesh
  // view; lifecycle transitions stay supervisor-driven, never inferred here.
  uint8_t lifecycle_state = 3;
  bool is_supervisor = false;
  std::string Encode() const;
  static HeartbeatPayload Decode(const std::string& bytes);
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
