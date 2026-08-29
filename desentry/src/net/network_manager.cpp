#include "desentry/net/network_manager.h"

#include <chrono>
#include <thread>
#include <unordered_map>

#include "desentry/common/logger.h"

namespace desentry {

namespace {
int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool ParseHostPort(const std::string& s, std::string* host, uint16_t* port) {
  auto pos = s.rfind(':');
  if (pos == std::string::npos) return false;
  *host = s.substr(0, pos);
  try {
    *port = static_cast<uint16_t>(std::stoi(s.substr(pos + 1)));
  } catch (...) {
    return false;
  }
  return true;
}
}  // namespace

Status NetworkManager::Start() {
  transport_ = std::make_unique<TcpTransport>(&engine_->identity(), config_.p2p_port);
  Status listen_st = transport_->StartListening(
      config_.p2p_bind_addr, [this](const std::string& peer_id, const WireMessage& req) {
        return HandleRequest(peer_id, req);
      });
  if (!listen_st.ok()) return listen_st;

  if (config_.discovery_enabled) {
    discovery_ = std::make_unique<UdpDiscovery>(&engine_->identity(), config_.p2p_port, config_.discovery_port,
                                                 config_.discovery_interval_ms);
    discovery_->Start(&peer_table_);
  }

  for (auto& spec : config_.bootstrap_peers) {
    std::string host;
    uint16_t port;
    if (!ParseHostPort(spec, &host, &port)) {
      DSN_LOG_WARN("network", "ignoring malformed bootstrap peer spec: " << spec);
      continue;
    }
    // node_id is unknown until we actually handshake with this address, so
    // it's keyed synthetically for now; once UDP discovery or a gossip
    // round learns its real node_id, that becomes a second (harmless,
    // CRDT-merge-idempotent) table entry for the same physical peer -- a
    // known minor inefficiency documented here rather than papered over
    // with more bookkeeping than an MVP peer table needs.
    PeerInfo info;
    info.node_id = "bootstrap#" + spec;
    info.host = host;
    info.p2p_port = port;
    info.last_seen_ms = NowMs();
    peer_table_.Upsert(info);
    DSN_LOG_INFO("network", "added bootstrap peer " << spec);
  }

  gossip_ = std::make_unique<GossipEngine>(engine_, &peer_table_, transport_.get(), config_.gossip_interval_ms);
  gossip_->Start();

  engine_->SetLocalWriteHook([this](const std::string& collection, const std::string& key, const std::string& bytes) {
    BroadcastLocalWrite(collection, key, bytes);
  });

  DSN_LOG_INFO("network", "network manager started (node_id=" << engine_->identity().node_id() << ")");
  return Status::OK();
}

void NetworkManager::Stop() {
  if (gossip_) gossip_->Stop();
  if (discovery_) discovery_->Stop();
  if (transport_) transport_->Stop();
}

WireMessage NetworkManager::HandleRequest(const std::string& peer_node_id, const WireMessage& request) {
  // Any authenticated inbound contact is itself useful discovery
  // information (works even when UDP broadcast is unavailable, e.g. a
  // peer that dialed us via bootstrap_peers).
  switch (request.type) {
    case MessageType::kDigest:
      return HandleDigest(DigestPayload::Decode(request.payload));
    case MessageType::kOpBroadcast:
      return HandleOpBroadcast(OpBroadcastPayload::Decode(request.payload));
    case MessageType::kPing:
      return WireMessage{MessageType::kPong, peer_node_id};
    default:
      return WireMessage{MessageType::kError, "unrecognized message type"};
  }
}

WireMessage NetworkManager::HandleDigest(const DigestPayload& digest) {
  std::unordered_map<std::string, HLCTimestamp> remote;
  for (auto& e : digest.entries) remote[e.key] = HLCTimestamp::Decode(e.top_ts_encoded);

  DeltaResponsePayload response;
  response.collection = digest.collection;

  std::unordered_map<std::string, HLCTimestamp> local;
  for (auto& e : engine_->LocalDigest(digest.collection)) local[e.key] = e.top_ts;

  for (auto& [key, local_ts] : local) {
    auto it = remote.find(key);
    if (it == remote.end() || local_ts > it->second) {
      auto raw_or = engine_->GetRawEncoded(digest.collection, key);
      if (raw_or.ok()) response.pushed.push_back(DocEntry{key, raw_or.value()});
    }
  }
  for (auto& [key, remote_ts] : remote) {
    auto it = local.find(key);
    if (it == local.end() || remote_ts > it->second) {
      response.wanted_keys.push_back(key);
    }
  }

  return WireMessage{MessageType::kDeltaResponse, response.Encode()};
}

WireMessage NetworkManager::HandleOpBroadcast(const OpBroadcastPayload& broadcast) {
  for (auto& doc : broadcast.docs) {
    engine_->MergeRemote(broadcast.collection, doc.key, doc.encoded_doc);
  }
  return WireMessage{MessageType::kPong, ""};
}

void NetworkManager::BroadcastLocalWrite(const std::string& collection, const std::string& key,
                                          const std::string& encoded_doc) {
  OpBroadcastPayload payload;
  payload.collection = collection;
  payload.docs.push_back(DocEntry{key, encoded_doc});
  WireMessage msg{MessageType::kOpBroadcast, payload.Encode()};

  // Fire-and-forget, one thread per peer per write: acceptable at MVP/demo
  // write rates (this is exactly the eager-broadcast path, not the
  // durability path -- the WAL write already happened before this hook
  // runs). A bounded worker pool is the natural upgrade once write volume
  // makes unbounded thread creation here a real cost, not a demo one.
  for (auto& peer : peer_table_.List()) {
    std::string host = peer.host;
    uint16_t port = peer.p2p_port;
    TcpTransport* transport = transport_.get();
    std::thread([transport, host, port, msg]() {
      transport->SendRequest(host, port, msg);
    }).detach();
  }
}

}  // namespace desentry
