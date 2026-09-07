#pragma once
// Node configuration. Loaded from a JSON file (config/node.example.json
// shows every field) with sane defaults so `desentryd` runs zero-config
// for local demo purposes.
//
// v2 note: users are never expected to hand-edit this file. The desktop
// app (app/) is the single point of access -- its Rust sidecar writes
// node.json, allocates ports, and spawns `desentryd --config <path>`. The
// file format stays human-readable anyway, because a config you can't read
// during an incident is a config you can't debug.

#include <cstdint>
#include <string>
#include <vector>

namespace desentry {

// How a node's byte budget is divided between its subsystems. Percentages
// of `quota_mb`, summing to 100. The AI sizing step (app/src-tauri/ai.rs)
// proposes a split per workload shape; the defaults here are the generic
// profile it starts from.
struct QuotaSplit {
  uint32_t db_pct = 60;            // user collections (router engines)
  uint32_t transit_store_pct = 15;  // bytes held on behalf of offline owners
  uint32_t cache_hash_pct = 10;     // engine-local caches / secondary indexes
  uint32_t ledger_pct = 10;         // WAL + ledger chain
  uint32_t net_buffers_pct = 5;     // in-flight gossip/broadcast buffers

  uint32_t Total() const {
    return db_pct + transit_store_pct + cache_hash_pct + ledger_pct + net_buffers_pct;
  }
  bool Valid() const { return Total() == 100; }

  // Absolute byte budget for one bucket given the node's total quota.
  uint64_t DbBytes(uint64_t quota_mb) const { return quota_mb * 1024ull * 1024ull * db_pct / 100; }
  uint64_t TransitBytes(uint64_t quota_mb) const { return quota_mb * 1024ull * 1024ull * transit_store_pct / 100; }
  uint64_t CacheBytes(uint64_t quota_mb) const { return quota_mb * 1024ull * 1024ull * cache_hash_pct / 100; }
  uint64_t LedgerBytes(uint64_t quota_mb) const { return quota_mb * 1024ull * 1024ull * ledger_pct / 100; }
  uint64_t NetBufferBytes(uint64_t quota_mb) const { return quota_mb * 1024ull * 1024ull * net_buffers_pct / 100; }
};

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

  // ---------------------------------------------------------------------
  // v2 fields
  // ---------------------------------------------------------------------

  // App-local *supervisor* node (docs/architecture-v2.md Sec 3). A supervisor
  // owns hardware discovery, fitness ranking, placement, key custody and
  // node lifecycle. It is deliberately NOT a coordinator: it is never
  // elected, never authoritative for data, and never on the data hot path.
  // Every supervisor is app-local and binds its API to loopback only --
  // enforced in NodeConfig::Validate(), not merely documented -- so a
  // supervisor cannot become a de-facto ROOT node reachable from the LAN
  // (see docs/comparison.md Sec 2 for why no coordinator exists here).
  bool supervisor = false;

  // Total on-disk byte budget for this node, in MiB. 0 == unlimited (the
  // v1 behaviour). Enforced in StorageEngine::PutRaw / EngineBackend::Put.
  uint64_t quota_mb = 0;
  QuotaSplit quota_split;

  // Storage router (storage/router.h). `engines` lists the backends this
  // node may bind collections to; `default_engine` is what an unbound
  // collection gets. Names: "kv", "columnar_lite", "ts_rollup",
  // "vector_hnsw_lite", "graph_adj", plus the optional vendored backends
  // "sqlite", "duckdb", "lmdb", "sqlite_vec" when compiled in.
  std::vector<std::string> engines{"kv"};
  std::string default_engine = "kv";

  // Replication factor used by the placement layer (net/placement.h).
  uint32_t replication_factor = 3;

  // Bytes held on behalf of an offline owner expire after this long
  // (ledger/transit_store.h). 0 == never expire.
  uint32_t transit_ttl_seconds = 7 * 24 * 3600;

  // Per-collection retention for time-series-shaped engines; 0 == keep all.
  uint32_t retention_days = 0;

  // At-rest encryption of the paged data file + transit store. The key
  // itself never lives in this file -- `keychain_ref` names an entry in the
  // OS keychain (Windows Credential Manager / macOS Keychain / Linux Secret
  // Service) that the app unlocks and passes over the sidecar's stdin.
  bool encrypt_at_rest = false;
  std::string keychain_ref;

  // Hard cap on concurrent per-peer worker threads for eager broadcast, so
  // a 50-node LAN cannot fan a single write out to 50 detached threads.
  uint32_t max_peer_threads = 8;

  // Token-bucket admission control on inbound P2P requests, per peer.
  // architecture.md Sec 8 called this out as required before any non-loopback
  // deployment; v2 implements it rather than leaving it a note.
  uint32_t peer_rate_limit_per_sec = 200;
  uint32_t peer_rate_burst = 400;

  // mDNS-style hostname advertisement alongside UDP broadcast, so the app's
  // sidebar can show "studio-imac.local" rather than a bare IP.
  bool mdns_enabled = true;
  std::string advertise_hostname;

  static NodeConfig LoadFromFile(const std::string& path);
  static NodeConfig Default() { return NodeConfig(); }

  // Serializes back to the same JSON shape LoadFromFile() reads. The app's
  // sidecar uses this to write node.json, so there is exactly one authority
  // on the file format rather than a Rust struct that drifts from the C++
  // one.
  std::string ToJson() const;

  // Rejects internally inconsistent configs (quota split that doesn't sum
  // to 100, a supervisor bound to a non-loopback address, an unknown
  // default_engine). Returns an empty string when the config is valid,
  // otherwise a human-readable reason.
  std::string Validate() const;
};

}  // namespace desentry
