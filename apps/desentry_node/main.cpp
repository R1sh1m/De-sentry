// desentryd -- the peer daemon. Every instance of this binary is a
// complete database server *and* a P2P client; there is no separate
// "server" build. See docs/ARCHITECTURE.md for the full design.

#include <csignal>
#include <cstdio>
#include <iostream>

#include "desentry/api/http_server.h"
#include "desentry/api/routes.h"
#include "desentry/common/config.h"
#include "desentry/common/logger.h"
#include "desentry/engine/node_engine.h"
#include "desentry/net/network_manager.h"

namespace {
volatile std::sig_atomic_t g_shutdown = 0;
void OnSignal(int) { g_shutdown = 1; }
}  // namespace

int main(int argc, char** argv) {
  std::string config_path = "config/node.json";
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::printf(
          "usage: desentryd [--config path/to/node.json]\n\n"
          "Every desentryd process is a full peer: it serves a local REST API\n"
          "for applications, and participates as an equal in the P2P replication\n"
          "mesh. See config/node.example.json for every configurable field.\n");
      return 0;
    }
  }

  desentry::NodeConfig config = desentry::NodeConfig::LoadFromFile(config_path);

  desentry::NodeEngine::Options engine_opts;
  engine_opts.data_dir = config.data_dir;
  engine_opts.buffer_pool_pages = config.buffer_pool_pages;

  auto engine_or = desentry::NodeEngine::Open(engine_opts);
  if (!engine_or.ok()) {
    std::fprintf(stderr, "fatal: failed to open node engine: %s\n", engine_or.status().ToString().c_str());
    return 1;
  }
  auto engine = std::move(engine_or.value());

  desentry::NetworkManager network(engine.get(), config);
  desentry::Status net_st = network.Start();
  if (!net_st.ok()) {
    std::fprintf(stderr, "fatal: failed to start network layer: %s\n", net_st.ToString().c_str());
    return 1;
  }

  desentry::HttpServer api(config.api_bind_addr, config.api_port);
  desentry::RegisterRoutes(&api, engine.get(), &network);
  if (!api.Start()) {
    std::fprintf(stderr, "fatal: failed to start API server on %s:%u\n", config.api_bind_addr.c_str(), config.api_port);
    return 1;
  }

  DSN_LOG_INFO("main", "de-sentry node is up. node_id=" << engine->identity().node_id()
                        << " api=http://" << config.api_bind_addr << ":" << config.api_port
                        << " p2p=" << config.p2p_bind_addr << ":" << config.p2p_port);
  if (!config.node_name.empty()) {
    DSN_LOG_INFO("main", "node_name=" << config.node_name);
  }

  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
  while (!g_shutdown) {
    struct timespec ts{0, 200 * 1000 * 1000};
    nanosleep(&ts, nullptr);
  }

  DSN_LOG_INFO("main", "shutting down...");
  api.Stop();
  network.Stop();
  engine->storage().Checkpoint();
  DSN_LOG_INFO("main", "checkpointed and stopped cleanly.");
  return 0;
}
