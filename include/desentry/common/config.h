#pragma once
// Node configuration. Loaded from a JSON file (config/node.example.json
// shows every field) with sane defaults so `desentryd` runs zero-config
// for local demo purposes.

#include <cstdint>
#include <string>
#include <vector>

namespace desentry {

struct NodeConfig {
  // Identity / storage paths.
  std::string data_dir = "./data";

  // Local application-facing REST API.
  std::string api_bind_addr = "127.0.0.1";
  uint16_t api_port = 7701;

  // P2P wire protocol (peer-to-peer TCP).
  std::string p2p_bind_addr = "0.0.0.0";
  uint16_t p2p_port = 7801;

  // UDP LAN discovery.
  bool discovery_enabled = true;
  uint16_t discovery_port = 7901;
  uint32_t discovery_interval_ms = 2000;

  // Static bootstrap peers "host:port" (P2P port), used in addition to /
  // instead of broadcast discovery (e.g. across L3 boundaries where UDP
  // broadcast doesn't reach).
  std::vector<std::string> bootstrap_peers;

  // Gossip anti-entropy interval.
  uint32_t gossip_interval_ms = 2000;

  // Buffer pool size, in 4KiB pages. 1024 pages == 4MiB, deliberately small
  // so the LRU replacer's eviction path is easy to exercise in the demo.
  uint32_t buffer_pool_pages = 1024;

  std::string node_name;  // human-friendly label for logs; not the identity.

  static NodeConfig LoadFromFile(const std::string& path);
  static NodeConfig Default() { return NodeConfig(); }
};

}  // namespace desentry
