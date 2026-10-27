#pragma once
// Optional self-hosted relay client for NAT traversal.
//
// The relay is a metadata-only broker: it never sees plaintext, never
// decodes frames, and is never on the data path when a direct P2P
// connection is available. It is purely a fallback for NAT traversal.
//
// Frame format (length-prefixed, big-endian):
//   [4 bytes: total payload length] [payload bytes]
//
// The relay server (desentry-relay) accepts WebSocket connections,
// registers node_ids, and forwards framed messages to registered
// recipients. It is intentionally dumb: no persistence, no auth,
// no state beyond the connection table.

#include <cstdint>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>

#include "desentry/common/status.h"

namespace desentry {

class RelayClient {
 public:
  // Encode a payload into a length-prefixed frame.
  // These static helpers are header-only so they can be unit-tested
  // regardless of whether DESENTRY_WITH_RELAY is enabled.
  static std::string EncodeFrame(const std::string& payload) {
    // 4-byte big-endian length + payload
    std::string frame;
    frame.reserve(4 + payload.size());
    uint32_t len = static_cast<uint32_t>(payload.size());
    frame.push_back(static_cast<char>((len >> 24) & 0xFF));
    frame.push_back(static_cast<char>((len >> 16) & 0xFF));
    frame.push_back(static_cast<char>((len >> 8) & 0xFF));
    frame.push_back(static_cast<char>(len & 0xFF));
    frame.append(payload);
    return frame;
  }

  // Decode a length-prefixed frame back to its payload.
  static StatusOr<std::string> DecodeFrame(const std::string& frame) {
    if (frame.size() < 4) {
      return Status::InvalidArgument("frame too short for length prefix");
    }
    uint32_t len = 0;
    len |= static_cast<uint32_t>(static_cast<unsigned char>(frame[0])) << 24;
    len |= static_cast<uint32_t>(static_cast<unsigned char>(frame[1])) << 16;
    len |= static_cast<uint32_t>(static_cast<unsigned char>(frame[2])) << 8;
    len |= static_cast<uint32_t>(static_cast<unsigned char>(frame[3]));

    if (frame.size() < 4 + len) {
      return Status::InvalidArgument("frame truncated: expected " +
                                      std::to_string(4 + len) + " bytes, got " +
                                      std::to_string(frame.size()));
    }
    return frame.substr(4, len);
  }

  // Construct a relay client. `relay_url` should be ws:// or wss://.
  // `node_id` is the node's identity used for registration.
  RelayClient(const std::string& relay_url, const std::string& node_id);
  ~RelayClient();

  // Connect to the relay server. Returns OK on success.
  Status Connect();

  // Disconnect from the relay server.
  void Disconnect();

  // Send an encrypted P2P message to a specific node via the relay.
  // The relay only sees the encrypted frame; it never decodes.
  Status RelaySend(const std::string& dest_node_id, const std::string& encrypted_payload);

  // Check if the relay is connected.
  bool connected() const { return connected_.load(); }

  // Get the relay URL.
  const std::string& relay_url() const { return relay_url_; }

  // Get the node ID.
  const std::string& node_id() const { return node_id_; }

 private:
  // Main relay loop: handles reconnection with exponential backoff.
  void RelayLoop();

  // Send a raw frame over the WebSocket connection.
  Status SendFrame(const std::string& frame);

  // Receive a frame from the WebSocket connection.
  StatusOr<std::string> RecvFrame();

  std::string relay_url_;
  std::string node_id_;

  std::atomic<bool> connected_{false};
  std::atomic<bool> running_{false};
  std::thread relay_thread_;

  // Mutex protecting the send queue.
  std::mutex send_queue_mu_;
  struct PendingMessage {
    std::string dest_node_id;
    std::string encrypted_payload;
  };
  std::queue<PendingMessage> send_queue_;
};

}  // namespace desentry
