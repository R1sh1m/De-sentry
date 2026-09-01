#include "desentry/net/wire_protocol.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

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
  return o;
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

Status WriteFrame(int sockfd, const std::string& bytes) {
  uint32_t len = htonl(static_cast<uint32_t>(bytes.size()));
  std::string frame(reinterpret_cast<char*>(&len), 4);
  frame += bytes;
  size_t sent = 0;
  while (sent < frame.size()) {
    ssize_t n = ::send(sockfd, frame.data() + sent, frame.size() - sent, 0);
    if (n <= 0) {
      if (errno == EINTR) continue;
      return Status::NetworkError(std::string("send failed: ") + std::strerror(errno));
    }
    sent += static_cast<size_t>(n);
  }
  return Status::OK();
}

StatusOr<std::string> ReadFrame(int sockfd, size_t max_len) {
  uint32_t len_be = 0;
  size_t got = 0;
  char* len_ptr = reinterpret_cast<char*>(&len_be);
  while (got < 4) {
    ssize_t n = ::recv(sockfd, len_ptr + got, 4 - got, 0);
    if (n == 0) return Status::NetworkError("connection closed while reading frame length");
    if (n < 0) {
      if (errno == EINTR) continue;
      return Status::NetworkError(std::string("recv failed: ") + std::strerror(errno));
    }
    got += static_cast<size_t>(n);
  }
  uint32_t len = ntohl(len_be);
  if (len > max_len) return Status::InvalidArgument("frame exceeds max_len (" + std::to_string(len) + " > " + std::to_string(max_len) + ")");

  std::string body(len, '\0');
  size_t body_got = 0;
  while (body_got < len) {
    ssize_t n = ::recv(sockfd, body.data() + body_got, len - body_got, 0);
    if (n == 0) return Status::NetworkError("connection closed while reading frame body");
    if (n < 0) {
      if (errno == EINTR) continue;
      return Status::NetworkError(std::string("recv failed: ") + std::strerror(errno));
    }
    body_got += static_cast<size_t>(n);
  }
  return body;
}

}  // namespace desentry
