#include "desentry/net/wire_protocol.h"

#include <cstring>

#include "desentry/common/byte_buffer.h"

namespace desentry {

// ---------------------------------------------------------------------------
// Payload encodings
// ---------------------------------------------------------------------------

std::string HelloPayload::Encode() const {
  ByteWriter w;
  w.Bytes(node_id);
  w.Bytes(ed25519_pubkey);
  w.Bytes(x25519_ephemeral_pubkey);
  w.Bytes(signature);
  w.U16(p2p_port);
  return w.TakeString();
}
HelloPayload HelloPayload::Decode(const std::string& bytes) {
  ByteReader r(bytes);
  HelloPayload h;
  h.node_id = r.Bytes();
  h.ed25519_pubkey = r.Bytes();
  h.x25519_ephemeral_pubkey = r.Bytes();
  h.signature = r.Bytes();
  h.p2p_port = r.U16();
  return h;
}

std::string DigestPayload::Encode() const {
  ByteWriter w;
  w.Bytes(collection);
  w.U32(static_cast<uint32_t>(entries.size()));
  for (auto& e : entries) {
    w.Bytes(e.key);
    w.Bytes(e.top_ts_encoded);
    w.Bytes(e.content_hash);
  }
  return w.TakeString();
}
DigestPayload DigestPayload::Decode(const std::string& bytes) {
  ByteReader r(bytes);
  DigestPayload d;
  d.collection = r.Bytes();
  uint32_t n = r.U32();
  d.entries.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    DigestEntry e;
    e.key = r.Bytes();
    e.top_ts_encoded = r.Bytes();
    e.content_hash = r.Bytes();
    d.entries.push_back(std::move(e));
  }
  return d;
}

std::string DeltaResponsePayload::Encode() const {
  ByteWriter w;
  w.Bytes(collection);
  w.U32(static_cast<uint32_t>(pushed.size()));
  for (auto& d : pushed) { w.Bytes(d.key); w.Bytes(d.encoded_doc); }
  w.U32(static_cast<uint32_t>(wanted_keys.size()));
  for (auto& k : wanted_keys) w.Bytes(k);
  return w.TakeString();
}
DeltaResponsePayload DeltaResponsePayload::Decode(const std::string& bytes) {
  ByteReader r(bytes);
  DeltaResponsePayload d;
  d.collection = r.Bytes();
  uint32_t n = r.U32();
  d.pushed.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    DocEntry e;
    e.key = r.Bytes();
    e.encoded_doc = r.Bytes();
    d.pushed.push_back(std::move(e));
  }
  uint32_t m = r.U32();
  d.wanted_keys.reserve(m);
  for (uint32_t i = 0; i < m; ++i) d.wanted_keys.push_back(r.Bytes());
  return d;
}

std::string OpBroadcastPayload::Encode() const {
  ByteWriter w;
  w.Bytes(collection);
  w.U32(static_cast<uint32_t>(docs.size()));
  for (auto& d : docs) { w.Bytes(d.key); w.Bytes(d.encoded_doc); }
  w.Bytes(message_id);
  w.U8(ttl);
  return w.TakeString();
}
OpBroadcastPayload OpBroadcastPayload::Decode(const std::string& bytes) {
  ByteReader r(bytes);
  OpBroadcastPayload o;
  o.collection = r.Bytes();
  uint32_t n = r.U32();
  o.docs.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    DocEntry e;
    e.key = r.Bytes();
    e.encoded_doc = r.Bytes();
    o.docs.push_back(std::move(e));
  }
  // message_id and ttl were added in v2. A v1 peer's broadcast simply ends
  // here, so the fields are read only if the buffer still has bytes -- the
  // mesh stays mixed-version-safe rather than throwing on an old peer.
  if (r.remaining() > 0) {
    o.message_id = r.Bytes();
    if (r.remaining() > 0) o.ttl = r.U8();
  }
  return o;
}

// -- v2 payloads -----------------------------------------------------------

namespace {

// Magic prefixing v2 payloads whose repeated elements grew new trailing
// fields. The WAL uses the same trick (kWalRecordMagicV2): an old payload
// starts with a count/flag field that can never equal the magic in practice,
// so the decoder can branch to the v1 parse instead of misreading old
// entries as new-tailed ones. Old decoders on new payloads throw (caught by
// every caller), which is the graceful direction for a best-effort path.
constexpr uint32_t kTransitResponseMagicV2 = 0x44545232;  // "DTR2"
constexpr uint32_t kLedgerDeltaMagicV2 = 0x444C4432;      // "DLD2"

bool HasMagic(const std::string& bytes, uint32_t magic) {
  if (bytes.size() < 4) return false;
  uint32_t v = 0;
  std::memcpy(&v, bytes.data(), 4);
  return v == magic;
}

}  // namespace

std::string TransitQueryPayload::Encode() const {
  ByteWriter w;
  w.U64(offset);
  return w.TakeString();
}
TransitQueryPayload TransitQueryPayload::Decode(const std::string& bytes) {
  ByteReader r(bytes);
  TransitQueryPayload p;
  // An empty query is a zero offset: old peers send kTransitQuery with no
  // payload at all, and they must keep working.
  if (!bytes.empty()) p.offset = r.U64();
  return p;
}

std::string TransitResponsePayload::Encode() const {
  ByteWriter w;
  w.U32(kTransitResponseMagicV2);
  w.U32(static_cast<uint32_t>(entries.size()));
  for (const TransitEntry& e : entries) {
    w.Bytes(e.collection);
    w.Bytes(e.key);
    w.Bytes(e.key_hash);
    w.Bytes(e.encoded_doc);
    w.I64(e.intent_lsn);
    w.Bytes(e.holder_node);
    w.U64(e.doc_size_bytes);
    w.U32(e.chunk_index);
    w.U32(e.chunk_total);
  }
  w.U8(truncated ? 1 : 0);
  w.U64(next_offset);
  return w.TakeString();
}
TransitResponsePayload TransitResponsePayload::Decode(const std::string& bytes) {
  // v1 payloads (no magic) predate striping: entries end at holder_node,
  // whole-document defaults apply, and there is no resume cursor. The v1
  // branch is the exact old parse, kept so old holders stay readable.
  if (!HasMagic(bytes, kTransitResponseMagicV2)) {
    ByteReader r(bytes);
    TransitResponsePayload p;
    uint32_t n = r.U32();
    p.entries.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
      TransitEntry e;
      e.collection = r.Bytes();
      e.key = r.Bytes();
      e.key_hash = r.Bytes();
      e.encoded_doc = r.Bytes();
      e.intent_lsn = r.I64();
      e.holder_node = r.Bytes();
      p.entries.push_back(std::move(e));
    }
    if (r.remaining() > 0) p.truncated = r.U8() != 0;
    return p;
  }
  ByteReader r(bytes);
  (void)r.U32();  // magic
  TransitResponsePayload p;
  uint32_t n = r.U32();
  p.entries.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    TransitEntry e;
    e.collection = r.Bytes();
    e.key = r.Bytes();
    e.key_hash = r.Bytes();
    e.encoded_doc = r.Bytes();
    e.intent_lsn = r.I64();
    e.holder_node = r.Bytes();
    e.doc_size_bytes = r.U64();
    e.chunk_index = r.U32();
    e.chunk_total = r.U32();
    if (e.chunk_total == 0) e.chunk_total = 1;
    p.entries.push_back(std::move(e));
  }
  if (r.remaining() > 0) p.truncated = r.U8() != 0;
  if (r.remaining() > 0) p.next_offset = r.U64();
  return p;
}

std::string TransitClaimPayload::Encode() const {
  ByteWriter w;
  w.Bytes(claimer_node);
  w.U32(static_cast<uint32_t>(key_hashes.size()));
  for (const std::string& h : key_hashes) w.Bytes(h);
  return w.TakeString();
}
TransitClaimPayload TransitClaimPayload::Decode(const std::string& bytes) {
  ByteReader r(bytes);
  TransitClaimPayload p;
  p.claimer_node = r.Bytes();
  uint32_t n = r.U32();
  p.key_hashes.reserve(n);
  for (uint32_t i = 0; i < n; ++i) p.key_hashes.push_back(r.Bytes());
  return p;
}

std::string TransitHeldPayload::Encode() const {
  ByteWriter w;
  w.Bytes(message_id);
  w.Bytes(key_hash);
  w.Bytes(holder_node);
  w.I64(intent_lsn);
  w.Bytes(signature);
  return w.TakeString();
}
TransitHeldPayload TransitHeldPayload::Decode(const std::string& bytes) {
  ByteReader r(bytes);
  TransitHeldPayload p;
  p.message_id = r.Bytes();
  p.key_hash = r.Bytes();
  p.holder_node = r.Bytes();
  p.intent_lsn = r.I64();
  p.signature = r.Bytes();
  return p;
}

std::string MergeReceipt::Encode() const {
  ByteWriter w;
  w.Bytes(message_id);
  w.Bytes(key_hash);
  w.Bytes(applier_node);
  w.I64(applied_lsn);
  w.Bytes(signature);
  return w.TakeString();
}
MergeReceipt MergeReceipt::Decode(const std::string& bytes) {
  ByteReader r(bytes);
  MergeReceipt m;
  m.message_id = r.Bytes();
  m.key_hash = r.Bytes();
  m.applier_node = r.Bytes();
  m.applied_lsn = r.I64();
  m.signature = r.Bytes();
  return m;
}

std::string LedgerDigestPayload::Encode() const {
  ByteWriter w;
  w.I64(tip_entry_id);
  w.Bytes(tip_entry_hash);
  w.Bytes(tip_signature);
  w.I64(from_entry_id);
  w.U32(static_cast<uint32_t>(entry_hashes.size()));
  for (const std::string& h : entry_hashes) w.Bytes(h);
  return w.TakeString();
}
LedgerDigestPayload LedgerDigestPayload::Decode(const std::string& bytes) {
  ByteReader r(bytes);
  LedgerDigestPayload p;
  p.tip_entry_id = r.I64();
  p.tip_entry_hash = r.Bytes();
  p.tip_signature = r.Bytes();
  p.from_entry_id = r.I64();
  uint32_t n = r.U32();
  p.entry_hashes.reserve(n);
  for (uint32_t i = 0; i < n; ++i) p.entry_hashes.push_back(r.Bytes());
  return p;
}

std::string LedgerDeltaPayload::Encode() const {
  ByteWriter w;
  w.U32(kLedgerDeltaMagicV2);
  w.U8(hashes_only ? 1 : 0);
  w.U32(static_cast<uint32_t>(entries.size()));
  for (const LedgerEntrySummary& e : entries) {
    w.I64(e.entry_id);
    w.U8(e.operation);
    w.Bytes(e.key_hash);
    w.Bytes(e.entry_hash);
    w.Bytes(e.prev_hash);
    w.Bytes(e.origin_node_id);
    w.Bytes(e.origin_signature);
    w.I64(e.hlc_physical_ms);
    w.U32(e.hlc_logical);
    w.Bytes(e.collection);
    w.Bytes(e.key);
    w.Bytes(e.transit_holder);
    w.U64(e.transit_size_bytes);
    w.U32(e.transit_chunk_index);
    w.U32(e.transit_chunk_total);
  }
  return w.TakeString();
}

// Reads one v1 (pre-routing-tail) entry. Shared by the v1 branch below.
namespace {
LedgerEntrySummary DecodeLedgerEntryV1(ByteReader* r) {
  LedgerEntrySummary e;
  e.entry_id = r->I64();
  e.operation = r->U8();
  e.key_hash = r->Bytes();
  e.entry_hash = r->Bytes();
  e.prev_hash = r->Bytes();
  e.origin_node_id = r->Bytes();
  e.origin_signature = r->Bytes();
  e.hlc_physical_ms = r->I64();
  e.hlc_logical = r->U32();
  e.collection = r->Bytes();
  e.key = r->Bytes();
  return e;
}
}  // namespace

LedgerDeltaPayload LedgerDeltaPayload::Decode(const std::string& bytes) {
  // v1 deltas (no magic) predate the transit routing tail: entries end at
  // key, whole-document defaults apply. Without the version branch, a v1
  // entry followed by more entries would misparse as a tailed v2 entry --
  // trailing-field tolerance is only sound at message end, never inside a
  // repeated element.
  if (!HasMagic(bytes, kLedgerDeltaMagicV2)) {
    ByteReader r(bytes);
    LedgerDeltaPayload p;
    p.hashes_only = r.U8() != 0;
    uint32_t n = r.U32();
    p.entries.reserve(n);
    for (uint32_t i = 0; i < n; ++i) p.entries.push_back(DecodeLedgerEntryV1(&r));
    return p;
  }
  ByteReader r(bytes);
  (void)r.U32();  // magic
  LedgerDeltaPayload p;
  p.hashes_only = r.U8() != 0;
  uint32_t n = r.U32();
  p.entries.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    LedgerEntrySummary e = DecodeLedgerEntryV1(&r);
    e.transit_holder = r.Bytes();
    e.transit_size_bytes = r.U64();
    e.transit_chunk_index = r.U32();
    e.transit_chunk_total = r.U32();
    if (e.transit_chunk_total == 0) e.transit_chunk_total = 1;
    p.entries.push_back(std::move(e));
  }
  return p;
}

std::string HeartbeatPayload::Encode() const {
  ByteWriter w;
  w.Bytes(node_id);
  w.I64(ledger_tip_entry_id);
  w.U64(free_quota_mb);
  w.U8(quota_limited ? 1 : 0);
  w.U64(transit_bytes_held);
  w.U64(transit_budget_bytes);
  w.U8(lifecycle_state);
  w.U8(is_supervisor ? 1 : 0);
  return w.TakeString();
}
HeartbeatPayload HeartbeatPayload::Decode(const std::string& bytes) {
  ByteReader r(bytes);
  HeartbeatPayload h;
  h.node_id = r.Bytes();
  h.ledger_tip_entry_id = r.I64();
  h.free_quota_mb = r.U64();
  // Fields after free_quota_mb postdate the first heartbeat version. An old
  // peer never sends kHeartbeat at all (it answers kError, and the prober
  // falls back to kPing), but a truncated or forwarded payload must still
  // decode rather than throw -- the same mixed-version rule as
  // OpBroadcastPayload above.
  if (r.remaining() > 0) h.quota_limited = r.U8() != 0;
  if (r.remaining() > 0) h.transit_bytes_held = r.U64();
  if (r.remaining() > 0) h.transit_budget_bytes = r.U64();
  if (r.remaining() > 0) h.lifecycle_state = r.U8();
  if (r.remaining() > 0) h.is_supervisor = r.U8() != 0;
  return h;
}

const char* MessageTypeName(MessageType type) {
  switch (type) {
    case MessageType::kHello: return "HELLO";
    case MessageType::kDigest: return "DIGEST";
    case MessageType::kDeltaResponse: return "DELTA_RESPONSE";
    case MessageType::kOpBroadcast: return "OP_BROADCAST";
    case MessageType::kPing: return "PING";
    case MessageType::kPong: return "PONG";
    case MessageType::kError: return "ERROR";
    case MessageType::kTransitQuery: return "TRANSIT_QUERY";
    case MessageType::kTransitResponse: return "TRANSIT_RESPONSE";
    case MessageType::kTransitClaim: return "TRANSIT_CLAIM";
    case MessageType::kLedgerDigest: return "LEDGER_DIGEST";
    case MessageType::kLedgerDelta: return "LEDGER_DELTA";
    case MessageType::kHeartbeat: return "HEARTBEAT";
    case MessageType::kMergeReceipt: return "MERGE_RECEIPT";
    case MessageType::kTransitHeld: return "TRANSIT_HELD";
  }
  return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Message envelope
// ---------------------------------------------------------------------------

std::string EncodeMessage(const WireMessage& msg) {
  ByteWriter w;
  w.U8(static_cast<uint8_t>(msg.type));
  w.RawBytes(msg.payload);
  return w.TakeString();
}

StatusOr<WireMessage> DecodeMessage(const std::string& bytes) {
  if (bytes.empty()) return Status::InvalidArgument("empty message");
  WireMessage msg;
  msg.type = static_cast<MessageType>(static_cast<uint8_t>(bytes[0]));
  msg.payload = bytes.substr(1);
  return msg;
}

// ---------------------------------------------------------------------------
// Raw framing over a socket fd
// ---------------------------------------------------------------------------

Status WriteFrame(dsn_socket_t sockfd, const std::string& bytes) {
  uint32_t len = htonl(static_cast<uint32_t>(bytes.size()));
  std::string frame(reinterpret_cast<char*>(&len), 4);
  frame += bytes;
  size_t sent = 0;
  while (sent < frame.size()) {
    dsn_iolen_t n = SocketSend(sockfd, frame.data() + sent, frame.size() - sent);
    if (n <= 0) {
      if (SocketRetryable()) continue;
      return Status::NetworkError("send failed: " + SocketErrorString());
    }
    sent += static_cast<size_t>(n);
  }
  return Status::OK();
}

StatusOr<std::string> ReadFrame(dsn_socket_t sockfd, size_t max_len) {
  uint32_t len_be = 0;
  size_t got = 0;
  char* len_ptr = reinterpret_cast<char*>(&len_be);
  while (got < 4) {
    dsn_iolen_t n = SocketRecv(sockfd, len_ptr + got, 4 - got);
    if (n == 0) return Status::NetworkError("connection closed while reading frame length");
    if (n < 0) {
      if (SocketRetryable()) continue;
      return Status::NetworkError("recv failed: " + SocketErrorString());
    }
    got += static_cast<size_t>(n);
  }
  uint32_t len = ntohl(len_be);
  if (len > max_len) return Status::InvalidArgument("frame exceeds max_len (" + std::to_string(len) + " > " + std::to_string(max_len) + ")");

  std::string body(len, '\0');
  size_t body_got = 0;
  while (body_got < len) {
    dsn_iolen_t n = SocketRecv(sockfd, body.data() + body_got, len - body_got);
    if (n == 0) return Status::NetworkError("connection closed while reading frame body");
    if (n < 0) {
      if (SocketRetryable()) continue;
      return Status::NetworkError("recv failed: " + SocketErrorString());
    }
    body_got += static_cast<size_t>(n);
  }
  return body;
}

}  // namespace desentry
