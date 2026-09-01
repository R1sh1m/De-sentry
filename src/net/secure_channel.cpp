#include "desentry/net/secure_channel.h"

#include <cstring>

#include "desentry/security/crypto.h"

namespace desentry {

namespace {

std::string CounterNonce(uint64_t counter) {
  std::string nonce(crypto::kAesGcmNonceLen, '\0');
  for (int i = 0; i < 8; ++i) {
    nonce[7 - i] = static_cast<char>((counter >> (8 * i)) & 0xFF);
  }
  // remaining 4 bytes stay zero -- safe because each connection derives a
  // brand-new key from a fresh ephemeral ECDH, so (key, counter) never
  // repeats across connections even though counters always start at 0.
  return nonce;
}

std::string CanonicalSalt(const std::string& a, const std::string& b) {
  return (a < b) ? (a + b) : (b + a);
}

Status SendPlain(int sockfd, const WireMessage& msg) {
  return WriteFrame(sockfd, EncodeMessage(msg));
}

StatusOr<WireMessage> RecvPlain(int sockfd, size_t max_len) {
  auto bytes_or = ReadFrame(sockfd, max_len);
  if (!bytes_or.ok()) return bytes_or.status();
  return DecodeMessage(bytes_or.value());
}

constexpr size_t kMaxHandshakeFrame = 4096;

Status VerifyHello(const HelloPayload& hello) {
  if (NodeIdentity::DeriveNodeId(hello.ed25519_pubkey) != hello.node_id) {
    return Status::AuthError("node_id does not match SHA-256(public_key)");
  }
  if (!NodeIdentity::Verify(hello.ed25519_pubkey, hello.x25519_ephemeral_pubkey, hello.signature)) {
    return Status::AuthError("HELLO signature verification failed");
  }
  return Status::OK();
}

}  // namespace

StatusOr<HandshakeResult> ClientHandshake(int sockfd, const NodeIdentity& identity, uint16_t our_p2p_port) {
  auto eph = crypto::GenerateX25519();

  HelloPayload our_hello;
  our_hello.node_id = identity.node_id();
  our_hello.ed25519_pubkey = identity.public_key();
  our_hello.x25519_ephemeral_pubkey = eph.public_key;
  our_hello.signature = identity.Sign(eph.public_key);
  our_hello.p2p_port = our_p2p_port;

  Status st = SendPlain(sockfd, WireMessage{MessageType::kHello, our_hello.Encode()});
  if (!st.ok()) return st;

  auto reply_or = RecvPlain(sockfd, kMaxHandshakeFrame);
  if (!reply_or.ok()) return reply_or.status();
  if (reply_or.value().type != MessageType::kHello) return Status::AuthError("expected HELLO from server");

  HelloPayload peer_hello = HelloPayload::Decode(reply_or.value().payload);
  Status verify_st = VerifyHello(peer_hello);
  if (!verify_st.ok()) return verify_st;

  std::string shared = crypto::X25519Ecdh(eph.private_key, peer_hello.x25519_ephemeral_pubkey);
  std::string salt = CanonicalSalt(eph.public_key, peer_hello.x25519_ephemeral_pubkey);
  std::string c2s = crypto::HkdfSha256(shared, salt, "desentry-c2s", crypto::kAesGcmKeyLen);
  std::string s2c = crypto::HkdfSha256(shared, salt, "desentry-s2c", crypto::kAesGcmKeyLen);

  HandshakeResult result;
  result.keys.send_key = c2s;
  result.keys.recv_key = s2c;
  result.peer_node_id = peer_hello.node_id;
  result.peer_ed25519_pubkey = peer_hello.ed25519_pubkey;
  result.peer_p2p_port = peer_hello.p2p_port;
  return result;
}

StatusOr<HandshakeResult> ServerHandshake(int sockfd, const NodeIdentity& identity, uint16_t our_p2p_port) {
  auto req_or = RecvPlain(sockfd, kMaxHandshakeFrame);
  if (!req_or.ok()) return req_or.status();
  if (req_or.value().type != MessageType::kHello) return Status::AuthError("expected HELLO from client");

  HelloPayload peer_hello = HelloPayload::Decode(req_or.value().payload);
  Status verify_st = VerifyHello(peer_hello);
  if (!verify_st.ok()) return verify_st;

  auto eph = crypto::GenerateX25519();
  HelloPayload our_hello;
  our_hello.node_id = identity.node_id();
  our_hello.ed25519_pubkey = identity.public_key();
  our_hello.x25519_ephemeral_pubkey = eph.public_key;
  our_hello.signature = identity.Sign(eph.public_key);
  our_hello.p2p_port = our_p2p_port;

  Status st = SendPlain(sockfd, WireMessage{MessageType::kHello, our_hello.Encode()});
  if (!st.ok()) return st;

  std::string shared = crypto::X25519Ecdh(eph.private_key, peer_hello.x25519_ephemeral_pubkey);
  std::string salt = CanonicalSalt(peer_hello.x25519_ephemeral_pubkey, eph.public_key);
  std::string c2s = crypto::HkdfSha256(shared, salt, "desentry-c2s", crypto::kAesGcmKeyLen);
  std::string s2c = crypto::HkdfSha256(shared, salt, "desentry-s2c", crypto::kAesGcmKeyLen);

  HandshakeResult result;
  result.keys.send_key = s2c;  // server sends on the s2c key
  result.keys.recv_key = c2s;
  result.peer_node_id = peer_hello.node_id;
  result.peer_ed25519_pubkey = peer_hello.ed25519_pubkey;
  result.peer_p2p_port = peer_hello.p2p_port;
  return result;
}

Status SendEncrypted(int sockfd, SessionKeys* keys, const WireMessage& msg) {
  std::string plaintext = EncodeMessage(msg);
  std::string nonce = CounterNonce(keys->send_counter++);
  std::string sealed = crypto::AesGcmSeal(keys->send_key, nonce, plaintext, "");
  return WriteFrame(sockfd, sealed);
}

StatusOr<WireMessage> RecvEncrypted(int sockfd, SessionKeys* keys, size_t max_len) {
  auto sealed_or = ReadFrame(sockfd, max_len);
  if (!sealed_or.ok()) return sealed_or.status();
  std::string nonce = CounterNonce(keys->recv_counter++);
  std::string plaintext;
  if (!crypto::AesGcmOpen(keys->recv_key, nonce, sealed_or.value(), "", &plaintext)) {
    return Status::AuthError("AEAD authentication failed (corrupt or tampered message)");
  }
  return DecodeMessage(plaintext);
}

}  // namespace desentry
