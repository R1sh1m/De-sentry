#pragma once
// Top-level network component: owns the TCP transport, UDP discovery, peer
// table and gossip engine, and is the single place that dispatches inbound
// P2P requests (kDigest / kOpBroadcast / kPing) against the NodeEngine.
// This is also what wires NodeEngine's local-write hook to eager broadcast
// (ARCHITECTURE.md §7.5) -- the only place those two layers touch, kept
// deliberately narrow (see node_engine.h's header comment on why the hook
// is a callback rather than a direct include).

#include <memory>
#include <string>

#include "desentry/common/config.h"
#include "desentry/common/status.h"
#include "desentry/engine/node_engine.h"
#include "desentry/net/gossip.h"
#include "desentry/net/peer.h"
#include "desentry/net/tcp_transport.h"
#include "desentry/net/udp_discovery.h"

namespace desentry {

class NetworkManager {
 public:
  NetworkManager(NodeEngine* engine, const NodeConfig& config) : engine_(engine), config_(config) {}

  Status Start();
  void Stop();

  PeerTable& peers() { return peer_table_; }
  TcpTransport* transport() { return transport_.get(); }

 private:
  WireMessage HandleRequest(const std::string& peer_node_id, const WireMessage& request);
  WireMessage HandleDigest(const DigestPayload& digest);
  WireMessage HandleOpBroadcast(const OpBroadcastPayload& broadcast);
  void BroadcastLocalWrite(const std::string& collection, const std::string& key, const std::string& encoded_doc);

  NodeEngine* engine_;
  NodeConfig config_;
  PeerTable peer_table_;
  std::unique_ptr<TcpTransport> transport_;
  std::unique_ptr<UdpDiscovery> discovery_;
  std::unique_ptr<GossipEngine> gossip_;
};

}  // namespace desentry
