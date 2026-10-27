#include "desentry/net/relay_client.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#include "desentry/common/logger.h"

namespace desentry {

// ---------------------------------------------------------------------------
// RelayClient implementation
// ---------------------------------------------------------------------------

RelayClient::RelayClient(const std::string& relay_url, const std::string& node_id)
    : relay_url_(relay_url), node_id_(node_id) {}

RelayClient::~RelayClient() { Disconnect(); }

Status RelayClient::Connect() {
  if (connected_.load()) return Status::OK();

  // Validate URL scheme.
  if (relay_url_.empty()) {
    return Status::InvalidArgument("relay URL is empty");
  }
  if (relay_url_.substr(0, 5) != "ws://" && relay_url_.substr(0, 6) != "wss://") {
    return Status::InvalidArgument("relay URL must use ws:// or wss:// scheme");
  }

  DSN_LOG_INFO("relay", "connecting to relay at " << relay_url_ << " as " << node_id_);

  running_ = true;
  relay_thread_ = std::thread(&RelayClient::RelayLoop, this);

  // Wait briefly for the relay thread to establish connection.
  // In a real implementation this would block on a condition variable.
  // For now we just mark connected after the thread starts.
  connected_ = true;
  DSN_LOG_INFO("relay", "relay client connected (node_id=" << node_id_ << ")");
  return Status::OK();
}

void RelayClient::Disconnect() {
  if (!running_.exchange(false)) return;
  connected_ = false;
  if (relay_thread_.joinable()) relay_thread_.join();
  DSN_LOG_INFO("relay", "relay client disconnected");
}

Status RelayClient::RelaySend(const std::string& dest_node_id,
                                 const std::string& encrypted_payload) {
  if (!connected_.load()) {
    return Status::NetworkError("relay not connected");
  }

  // Queue the message for sending. In a real implementation this would
  // be sent over the WebSocket connection.
  {
    std::lock_guard<std::mutex> lock(send_queue_mu_);
    send_queue_.push({dest_node_id, encrypted_payload});
  }

  DSN_LOG_DEBUG("relay", "queued message to " << dest_node_id << " via relay");
  return Status::OK();
}

void RelayClient::RelayLoop() {
  // Exponential backoff for reconnection.
  constexpr int64_t kInitialBackoffMs = 1000;
  constexpr int64_t kMaxBackoffMs = 30000;
  int64_t backoff_ms = kInitialBackoffMs;

  while (running_.load()) {
    // In a real implementation, this would:
    // 1. Establish WebSocket connection to relay_url_
    // 2. Send registration frame with node_id_
    // 3. Enter receive loop, forwarding messages to NetworkManager
    // 4. On disconnect, sleep for backoff_ms and retry

    // Drain pending messages (simulated).
    {
      std::lock_guard<std::mutex> lock(send_queue_mu_);
      while (!send_queue_.empty()) {
        auto msg = std::move(send_queue_.front());
        send_queue_.pop();
        // In a real implementation, send the frame over WebSocket.
        // For now, just log.
        DSN_LOG_DEBUG("relay", "sending to " << msg.dest_node_id << " via relay");
      }
    }

    // Sleep with backoff.
    int64_t sleep_ms = std::min(backoff_ms, kMaxBackoffMs);
    // Simple sleep loop that checks running_ periodically.
    for (int64_t elapsed = 0; elapsed < sleep_ms && running_.load(); elapsed += 100) {
      // Sleep in small increments to allow quick shutdown.
      // In a real implementation, use a condition variable.
    }

    // Increase backoff.
    backoff_ms = std::min(backoff_ms * 2, kMaxBackoffMs);
  }
}

Status RelayClient::SendFrame(const std::string& frame) {
  // Placeholder: in a real implementation, send over WebSocket.
  return Status::OK();
}

StatusOr<std::string> RelayClient::RecvFrame() {
  // Placeholder: in a real implementation, receive from WebSocket.
  return Status::NetworkError("not connected");
}

}  // namespace desentry
