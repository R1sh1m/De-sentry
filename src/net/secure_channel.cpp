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

Status SendPlain(dsn_socket_t sockfd, const WireMessage& msg) {
  return WriteFrame(sockfd, EncodeMessage(msg));
}

StatusOr<WireMessage> RecvPlain(dsn_socket_t sockfd, size_t max_len) {
  auto bytes_or = ReadFrame(sockfd, max_len);
  if (!bytes_or.ok()) return bytes_or.status();
  return DecodeMessage(bytes_or.value());
}

constexpr size_t kMaxHandshakeFrame = 4096;

bool ConstantTimeEqual(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned diff = 0;
  for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned>(a[i] ^ b[i]);
  return diff == 0;
}

Status VerifyHello(const HelloPayload& hello, const std::string& cluster_secret) {
  if (NodeIdentity::DeriveNodeId(hello.ed25519_pubkey) != hello.node_id) {
    return Status::AuthError("node_id does not match SHA-256(public_key)");
  }
  if (!NodeIdentity::Verify(hello.ed25519_pubkey, hello.x25519_ephemeral_pubkey, hello.signature)) {
    return Status::AuthError("HELLO signature verification failed");
  }
  // Cluster membership: when a secret is configured, the peer must prove
  // current possession with HMAC(secret, ephemeral_pubkey). The tag is over
  // a fresh ephemeral each connection, so it is not replayable across
  // connections; without a configured secret the mesh stays open (mixed
  // rollout: configure the secret on every node to close it).
  if (!cluster_secret.empty()) {
    if (hello.membership_tag.empty()) {
      return Status::AuthError("peer offered no membership tag; cluster requires one");
    }
    const std::string expect =
        crypto::HmacSha256(cluster_secret, MembershipHelloMessage(hello.x25519_ephemeral_pubkey));
    if (!ConstantTimeEqual(expect, hello.membership_tag)) {
      return Status::AuthError("membership tag mismatch; not our cluster");
    }
  }
  return Status::OK();
}

}  // namespace

StatusOr<HandshakeResult> ClientHandshake(dsn_socket_t sockfd, const NodeIdentity& identity,
                                           uint16_t our_p2p_port,
                                           const std::string& cluster_secret) {
  auto eph = crypto::GenerateX25519();

  HelloPayload our_hello;
  our_hello.node_id = identity.node_id();
  our_hello.ed25519_pubkey = identity.public_key();
  our_hello.x25519_ephemeral_pubkey = eph.public_key;
  our_hello.signature = identity.Sign(eph.public_key);
  our_hello.p2p_port = our_p2p_port;
  if (!cluster_secret.empty()) {
    our_hello.membership_tag =
        crypto::HmacSha256(cluster_secret, MembershipHelloMessage(eph.public_key));
  }

  Status st = SendPlain(sockfd, WireMessage{MessageType::kHello, our_hello.Encode()});
  if (!st.ok()) return st;

  auto reply_or = RecvPlain(sockfd, kMaxHandshakeFrame);
  if (!reply_or.ok()) return reply_or.status();
  if (reply_or.value().type != MessageType::kHello) return Status::AuthError("expected HELLO from server");

  HelloPayload peer_hello = HelloPayload::Decode(reply_or.value().payload);
  Status verify_st = VerifyHello(peer_hello, cluster_secret);
  if (!verify_st.ok()) return verify_st;

  // Key confirmation (H-5 fix): both sides sign the full transcript
  // (client hello bytes, then server hello bytes -- fixed order), binding
  // identities, ephemeral keys and ports into one attestation. A replayed
  // HELLO verifies on its own but its replayer cannot produce this
  // signature (it has neither long-term key), so the replay authenticates
  // nothing and registers nothing. Re-encoding the decoded structs is
  // byte-exact (payloads round-trip untouched, Encode always writes every
  // field), so both sides sign identical bytes.
  const std::string transcript =
      TranscriptMessage(EncodeMessage(WireMessage{MessageType::kHello, our_hello.Encode()}),
                        EncodeMessage(WireMessage{MessageType::kHello, peer_hello.Encode()}));
  ConfirmPayload our_confirm;
  our_confirm.transcript_signature = identity.Sign(transcript);
  st = SendPlain(sockfd, WireMessage{MessageType::kConfirm, our_confirm.Encode()});
  if (!st.ok()) return st;

  auto confirm_or = RecvPlain(sockfd, kMaxHandshakeFrame);
  if (!confirm_or.ok()) return confirm_or.status();
  if (confirm_or.value().type != MessageType::kConfirm) {
    return Status::AuthError("expected CONFIRM from server (old peer without H-5 support?)");
  }
  ConfirmPayload peer_confirm = ConfirmPayload::Decode(confirm_or.value().payload);
  if (!NodeIdentity::Verify(peer_hello.ed25519_pubkey, transcript,
                            peer_confirm.transcript_signature)) {
    return Status::AuthError("CONFIRM signature verification failed");
  }

  std::string shared = crypto::X25519Ecdh(eph.private_key, peer_hello.x25519_ephemeral_pubkey);
  // KDF binds identities as well as ephemeral keys (H-5 fix): session keys
  // from a recording are useless without the peer identities, and two pairs
  // can never derive the same keys for different identities.
  std::string salt = crypto::Sha256(CanonicalSalt(eph.public_key, peer_hello.x25519_ephemeral_pubkey) +
                                    "desentry-kdf-v2" + identity.node_id() + peer_hello.node_id);
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

StatusOr<HandshakeResult> ServerHandshake(dsn_socket_t sockfd, const NodeIdentity& identity,
                                           uint16_t our_p2p_port,
                                           const std::string& cluster_secret) {
  auto req_or = RecvPlain(sockfd, kMaxHandshakeFrame);
  if (!req_or.ok()) return req_or.status();
  if (req_or.value().type != MessageType::kHello) return Status::AuthError("expected HELLO from client");

  HelloPayload peer_hello = HelloPayload::Decode(req_or.value().payload);
  Status verify_st = VerifyHello(peer_hello, cluster_secret);
  if (!verify_st.ok()) return verify_st;

  auto eph = crypto::GenerateX25519();
  HelloPayload our_hello;
  our_hello.node_id = identity.node_id();
  our_hello.ed25519_pubkey = identity.public_key();
  our_hello.x25519_ephemeral_pubkey = eph.public_key;
  our_hello.signature = identity.Sign(eph.public_key);
  our_hello.p2p_port = our_p2p_port;
  if (!cluster_secret.empty()) {
    our_hello.membership_tag =
        crypto::HmacSha256(cluster_secret, MembershipHelloMessage(eph.public_key));
  }

  const std::string our_hello_raw =
      EncodeMessage(WireMessage{MessageType::kHello, our_hello.Encode()});
  Status st = SendPlain(sockfd, WireMessage{MessageType::kHello, our_hello.Encode()});
  if (!st.ok()) return st;

  // Key confirmation (H-5 fix, server side): verify the client's transcript
  // signature before sending our own. An attacker replaying a recorded HELLO
  // fails here -- the confirm requires the peer's long-term key -- so the
  // handshake never completes and nothing is registered.
  auto confirm_req_or = RecvPlain(sockfd, kMaxHandshakeFrame);
  if (!confirm_req_or.ok()) return confirm_req_or.status();
  if (confirm_req_or.value().type != MessageType::kConfirm) {
    return Status::AuthError("expected CONFIRM from client (old peer without H-5 support?)");
  }
  const std::string server_transcript = TranscriptMessage(
      EncodeMessage(WireMessage{MessageType::kHello, peer_hello.Encode()}), our_hello_raw);
  ConfirmPayload client_confirm = ConfirmPayload::Decode(confirm_req_or.value().payload);
  if (!NodeIdentity::Verify(peer_hello.ed25519_pubkey, server_transcript,
                            client_confirm.transcript_signature)) {
    return Status::AuthError("CONFIRM signature verification failed");
  }

  ConfirmPayload our_confirm;
  our_confirm.transcript_signature = identity.Sign(server_transcript);
  st = SendPlain(sockfd, WireMessage{MessageType::kConfirm, our_confirm.Encode()});
  if (!st.ok()) return st;

  std::string shared = crypto::X25519Ecdh(eph.private_key, peer_hello.x25519_ephemeral_pubkey);
  // KDF binds identities as well as ephemeral keys (H-5 fix): the client and
  // server roles order the ids identically on both sides.
  std::string salt = crypto::Sha256(CanonicalSalt(peer_hello.x25519_ephemeral_pubkey, eph.public_key) +
                                    "desentry-kdf-v2" + peer_hello.node_id + identity.node_id());
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

Status SendEncrypted(dsn_socket_t sockfd, SessionKeys* keys, const WireMessage& msg) {
  std::string plaintext = EncodeMessage(msg);
  std::string nonce = CounterNonce(keys->send_counter++);
  auto sealed_or = crypto::AesGcmSeal(keys->send_key, nonce, plaintext, "");
  if (!sealed_or.ok()) return sealed_or.status();
  return WriteFrame(sockfd, sealed_or.value());
}

StatusOr<WireMessage> RecvEncrypted(dsn_socket_t sockfd, SessionKeys* keys, size_t max_len) {
  auto sealed_or = ReadFrame(sockfd, max_len);
  if (!sealed_or.ok()) return sealed_or.status();
  // Advance the nonce only on successful authentication (H-6 fix): one
  // injected frame must not desynchronise every subsequent legitimate frame.
  // A failed open is fatal to this connection (caller closes it).
  std::string nonce = CounterNonce(keys->recv_counter);
  std::string plaintext;
  if (!crypto::AesGcmOpen(keys->recv_key, nonce, sealed_or.value(), "", &plaintext)) {
    return Status::AuthError("AEAD authentication failed (corrupt or tampered message)");
  }
  keys->recv_counter++;
  return DecodeMessage(plaintext);
}

}  // namespace desentry
