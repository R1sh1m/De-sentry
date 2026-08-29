#include "desentry/common/config.h"

#include <fstream>
#include <sstream>

#include "desentry/common/json.h"
#include "desentry/common/logger.h"

namespace desentry {

NodeConfig NodeConfig::LoadFromFile(const std::string& path) {
  NodeConfig cfg;
  std::ifstream f(path);
  if (!f.is_open()) {
    DSN_LOG_WARN("config", "no config file at " << path << ", using defaults");
    return cfg;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  JsonValue root;
  try {
    root = JsonValue::Parse(ss.str());
  } catch (const std::exception& e) {
    DSN_LOG_ERROR("config", "failed to parse " << path << ": " << e.what() << " -- using defaults");
    return cfg;
  }
  if (!root.is_object()) return cfg;

  auto str_field = [&](const char* key, std::string* out) {
    const JsonValue* v = root.Find(key);
    if (v && v->is_string()) *out = v->AsString();
  };
  auto uint16_field = [&](const char* key, uint16_t* out) {
    const JsonValue* v = root.Find(key);
    if (v && v->is_number()) *out = static_cast<uint16_t>(v->AsInt());
  };
  auto uint32_field = [&](const char* key, uint32_t* out) {
    const JsonValue* v = root.Find(key);
    if (v && v->is_number()) *out = static_cast<uint32_t>(v->AsInt());
  };
  auto bool_field = [&](const char* key, bool* out) {
    const JsonValue* v = root.Find(key);
    if (v && v->is_bool()) *out = v->AsBool();
  };

  str_field("data_dir", &cfg.data_dir);
  str_field("api_bind_addr", &cfg.api_bind_addr);
  uint16_field("api_port", &cfg.api_port);
  str_field("p2p_bind_addr", &cfg.p2p_bind_addr);
  uint16_field("p2p_port", &cfg.p2p_port);
  bool_field("discovery_enabled", &cfg.discovery_enabled);
  uint16_field("discovery_port", &cfg.discovery_port);
  uint32_field("discovery_interval_ms", &cfg.discovery_interval_ms);
  uint32_field("gossip_interval_ms", &cfg.gossip_interval_ms);
  uint32_field("buffer_pool_pages", &cfg.buffer_pool_pages);
  str_field("node_name", &cfg.node_name);

  const JsonValue* peers = root.Find("bootstrap_peers");
  if (peers && peers->is_array()) {
    for (auto& p : peers->AsArray()) {
      if (p.is_string()) cfg.bootstrap_peers.push_back(p.AsString());
    }
  }
  return cfg;
}

}  // namespace desentry
