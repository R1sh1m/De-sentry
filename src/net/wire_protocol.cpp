#include "desentry/net/wire_protocol.h"


#include <cerrno>
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

std::string TransitResponsePayload::Encode() const {
  ByteWriter w;
  w.U32(static_cast<uint32_t>(entries.size()));
  for (const TransitEntry& e : entries) {
    w.Bytes(e.collection);
    w.Bytes(e.key);
    w.Bytes(e.key_hash);
    w.Bytes(e.encoded_doc);
    w.I64(e.intent_lsn);
    w.Bytes(e.holder_node);
  }
  w.U8(truncated ? 1 : 0);
  return w.TakeString();
}
TransitResponsePayload TransitResponsePayload::Decode(const std::string& bytes) {
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
  }
  return w.TakeString();
}
LedgerDeltaPayload LedgerDeltaPayload::Decode(const std::string& bytes) {
  ByteReader r(bytes);
  LedgerDeltaPayload p;
  p.hashes_only = r.U8() != 0;
  uint32_t n = r.U32();
  p.entries.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    LedgerEntrySummary e;
    e.entry_id = r.I64();
    e.operation = r.U8();
    e.key_hash = r.Bytes();
    e.entry_hash = r.Bytes();
    e.prev_hash = r.Bytes();
    e.origin_node_id = r.Bytes();
    e.origin_signature = r.Bytes();
    e.hlc_physical_ms = r.I64();
    e.hlc_logical = r.U32();
    e.collection = r.Bytes();
    e.key = r.Bytes();
    p.entries.push_back(std::move(e));
  }
  return p;
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
