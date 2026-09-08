#include "desentry/common/config.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "desentry/common/json.h"
#include "desentry/common/logger.h"

namespace desentry {

namespace {

// The engine names storage/router.h knows how to construct. Kept here (not
// only in the router) so a typo in node.json is caught at config-load time
// with a clear message, rather than at first write with "no such engine".
bool IsKnownEngine(const std::string& name) {
  return name == "kv" || name == "columnar_lite" || name == "ts_rollup" ||
         name == "vector_hnsw_lite" || name == "graph_adj" || name == "sqlite" ||
         name == "duckdb" || name == "lmdb" || name == "sqlite_vec";
}

bool IsLoopback(const std::string& addr) {
  return addr == "127.0.0.1" || addr == "localhost" || addr == "::1";
}

}  // namespace

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
  auto uint64_field = [&](const char* key, uint64_t* out) {
    const JsonValue* v = root.Find(key);
    if (v && v->is_number()) *out = static_cast<uint64_t>(v->AsInt());
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

  // -- v2 fields -----------------------------------------------------------
  bool_field("supervisor", &cfg.supervisor);
  uint64_field("quota_mb", &cfg.quota_mb);
  str_field("default_engine", &cfg.default_engine);
  uint32_field("replication_factor", &cfg.replication_factor);
  uint32_field("transit_ttl_seconds", &cfg.transit_ttl_seconds);
  uint32_field("retention_days", &cfg.retention_days);
  bool_field("encrypt_at_rest", &cfg.encrypt_at_rest);
  str_field("keychain_ref", &cfg.keychain_ref);
  uint32_field("max_peer_threads", &cfg.max_peer_threads);
  uint32_field("peer_rate_limit_per_sec", &cfg.peer_rate_limit_per_sec);
  uint32_field("peer_rate_burst", &cfg.peer_rate_burst);
  bool_field("mdns_enabled", &cfg.mdns_enabled);
  str_field("advertise_hostname", &cfg.advertise_hostname);

  const JsonValue* engines = root.Find("engines");
  if (engines && engines->is_array()) {
    cfg.engines.clear();
    for (auto& e : engines->AsArray()) {
      if (e.is_string()) cfg.engines.push_back(e.AsString());
    }
    if (cfg.engines.empty()) cfg.engines.push_back("kv");
  }

  const JsonValue* split = root.Find("quota_split");
  if (split && split->is_object()) {
    auto pct = [&](const char* key, uint32_t* out) {
      const JsonValue* v = split->Find(key);
      if (v && v->is_number()) *out = static_cast<uint32_t>(v->AsInt());
    };
    pct("db", &cfg.quota_split.db_pct);
    pct("transit_store", &cfg.quota_split.transit_store_pct);
    pct("cache_hash", &cfg.quota_split.cache_hash_pct);
    pct("ledger", &cfg.quota_split.ledger_pct);
    pct("net_buffers", &cfg.quota_split.net_buffers_pct);
  }

  std::string problem = cfg.Validate();
  if (!problem.empty()) {
    // Loud, but not fatal: a node that refuses to boot because one field is
    // off is worse for an offline-first product than one that boots with a
    // corrected value and says so. The app surfaces this line in the node's
    // log pane.
    DSN_LOG_ERROR("config", "invalid config in " << path << ": " << problem);
  }
  return cfg;
}

std::string NodeConfig::Validate() const {
  if (!quota_split.Valid()) {
    return "quota_split percentages sum to " + std::to_string(quota_split.Total()) + ", expected 100";
  }
  if (supervisor && !IsLoopback(api_bind_addr)) {
    return "supervisor nodes must bind api_bind_addr to loopback (got " + api_bind_addr + ")";
  }
  if (!IsKnownEngine(default_engine)) {
    return "unknown default_engine: " + default_engine;
  }
  for (const std::string& e : engines) {
    if (!IsKnownEngine(e)) return "unknown engine in engines[]: " + e;
  }
  // A default the node never loads leaves every unbound collection
  // unroutable, and the failure would surface at first write rather than at
  // boot. IsKnownEngine() above only says the name exists; this says this
  // node actually has it.
  if (!engines.empty() &&
      std::find(engines.begin(), engines.end(), default_engine) == engines.end()) {
    return "default_engine \"" + default_engine + "\" is not listed in engines[]";
  }
  if (replication_factor == 0) return "replication_factor must be >= 1";
  return std::string();
}

std::string NodeConfig::ToJson() const {
  JsonValue::Object o;
  o.emplace_back("node_name", JsonValue(node_name));
  o.emplace_back("data_dir", JsonValue(data_dir));
  o.emplace_back("api_bind_addr", JsonValue(api_bind_addr));
  o.emplace_back("api_port", JsonValue(static_cast<int64_t>(api_port)));
  o.emplace_back("p2p_bind_addr", JsonValue(p2p_bind_addr));
  o.emplace_back("p2p_port", JsonValue(static_cast<int64_t>(p2p_port)));
  o.emplace_back("discovery_enabled", JsonValue(discovery_enabled));
  o.emplace_back("discovery_port", JsonValue(static_cast<int64_t>(discovery_port)));
  o.emplace_back("discovery_interval_ms", JsonValue(static_cast<int64_t>(discovery_interval_ms)));

  JsonValue::Array peers;
  for (const std::string& p : bootstrap_peers) peers.emplace_back(JsonValue(p));
  o.emplace_back("bootstrap_peers", JsonValue(std::move(peers)));

  o.emplace_back("gossip_interval_ms", JsonValue(static_cast<int64_t>(gossip_interval_ms)));
  o.emplace_back("buffer_pool_pages", JsonValue(static_cast<int64_t>(buffer_pool_pages)));
  o.emplace_back("supervisor", JsonValue(supervisor));
  o.emplace_back("quota_mb", JsonValue(static_cast<int64_t>(quota_mb)));

  JsonValue::Object split;
  split.emplace_back("db", JsonValue(static_cast<int64_t>(quota_split.db_pct)));
  split.emplace_back("transit_store", JsonValue(static_cast<int64_t>(quota_split.transit_store_pct)));
  split.emplace_back("cache_hash", JsonValue(static_cast<int64_t>(quota_split.cache_hash_pct)));
  split.emplace_back("ledger", JsonValue(static_cast<int64_t>(quota_split.ledger_pct)));
  split.emplace_back("net_buffers", JsonValue(static_cast<int64_t>(quota_split.net_buffers_pct)));
  o.emplace_back("quota_split", JsonValue(std::move(split)));

  JsonValue::Array eng;
  for (const std::string& e : engines) eng.emplace_back(JsonValue(e));
  o.emplace_back("engines", JsonValue(std::move(eng)));
  o.emplace_back("default_engine", JsonValue(default_engine));
  o.emplace_back("replication_factor", JsonValue(static_cast<int64_t>(replication_factor)));
  o.emplace_back("transit_ttl_seconds", JsonValue(static_cast<int64_t>(transit_ttl_seconds)));
  o.emplace_back("retention_days", JsonValue(static_cast<int64_t>(retention_days)));
  o.emplace_back("encrypt_at_rest", JsonValue(encrypt_at_rest));
  o.emplace_back("keychain_ref", JsonValue(keychain_ref));
  o.emplace_back("max_peer_threads", JsonValue(static_cast<int64_t>(max_peer_threads)));
  o.emplace_back("peer_rate_limit_per_sec", JsonValue(static_cast<int64_t>(peer_rate_limit_per_sec)));
  o.emplace_back("peer_rate_burst", JsonValue(static_cast<int64_t>(peer_rate_burst)));
  o.emplace_back("mdns_enabled", JsonValue(mdns_enabled));
  o.emplace_back("advertise_hostname", JsonValue(advertise_hostname));
  return JsonValue(std::move(o)).Dump();
}

}  // namespace desentry
