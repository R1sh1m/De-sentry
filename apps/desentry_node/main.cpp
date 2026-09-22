// desentryd -- the peer daemon. Every instance of this binary is a complete
// database server *and* a P2P client; there is no separate "server" build.
// A supervisor is the same binary with `"supervisor": true` in its config
// (see include/desentry/supervisor/supervisor.h for why that is a role, not
// a privilege). See docs/architecture-v2.md for the full v2 design.
//
// The desktop app is the only thing expected to launch this in normal use:
// it writes node.json, allocates ports, and supervises the process. Running
// it by hand still works and is what the cluster scripts and integration
// tests do.

#include <csignal>
#include <cstdio>
#include <iostream>
#include <string>

#include "desentry/api/http_server.h"
#include "desentry/api/routes.h"
#include "desentry/common/config.h"
#include "desentry/common/logger.h"
#include "desentry/common/platform.h"
#include "desentry/engine/node_engine.h"
#include "desentry/net/network_manager.h"
#include "desentry/security/at_rest.h"
#include "desentry/supervisor/supervisor.h"
#include "desentry/storage/reencrypt.h"

namespace {

volatile std::sig_atomic_t g_shutdown = 0;
void OnSignal(int) { g_shutdown = 1; }

void PrintUsage() {
  std::printf(
      "usage: desentryd [--config path/to/node.json] [--data-dir DIR] [--api-port N]\n"
      "                 [--p2p-port N] [--supervisor] [--log-level debug|info|warn|error]\n"
      "                 [--read-unlock-stdin] [--re-encrypt]\n"
      "\n"
      "Every desentryd process is a full peer: it serves a local REST API for\n"
      "applications and participates as an equal in the P2P replication mesh.\n"
      "A process started with --supervisor additionally runs the app-local\n"
      "control plane (hardware discovery, node lifecycle, quorum-gated ledger\n"
      "checkpointing); it binds its API to loopback only, is never elected,\n"
      "and never holds replicated data.\n"
      "\n"
      "--read-unlock-stdin reads one line (the node's Crockford unlock key)\n"
      "from stdin and uses it as the at-rest data-encryption key. The desktop\n"
      "app passes this flag whenever it has a key to hand over; without it\n"
      "stdin is never touched, so running by hand in a terminal never blocks.\n"
      "\n"
      "--re-encrypt seals every plaintext data file under the configured\n"
      "data directory with the stdin key and exits (the node must be\n"
      "STOPPED first). Already-sealed files are skipped, so reruns finish\n"
      "the job. There is no decrypt direction: restore from backup.\n"
      "\n"
      "See config/node.example.json for every configurable field.\n");
}

// Reads one line from stdin (the sidecar writes the unlock key followed by a
// newline, then closes the pipe). Only called when --read-unlock-stdin was
// passed, so a hand-run in a terminal never blocks on input.
std::string ReadUnlockKeyFromStdin() {
  std::string line;
  if (!std::getline(std::cin, line)) return std::string();
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
  return line;
}

desentry::LogLevel ParseLogLevel(const std::string& name) {
  if (name == "debug") return desentry::LogLevel::kDebug;
  if (name == "warn") return desentry::LogLevel::kWarn;
  if (name == "error") return desentry::LogLevel::kError;
  return desentry::LogLevel::kInfo;
}

}  // namespace

int main(int argc, char** argv) {
  desentry::NetInit();

  std::string config_path = "config/node.json";
  // Command-line overrides applied after the file is loaded. The app's
  // sidecar uses them so a node can be started with a different port without
  // rewriting its config -- useful when a port turns out to be taken.
  std::string override_data_dir;
  int override_api_port = 0;
  int override_p2p_port = 0;
  bool force_supervisor = false;
  bool read_unlock_stdin = false;
  bool re_encrypt = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else if (arg == "--data-dir" && i + 1 < argc) {
      override_data_dir = argv[++i];
    } else if (arg == "--api-port" && i + 1 < argc) {
      override_api_port = std::atoi(argv[++i]);
    } else if (arg == "--p2p-port" && i + 1 < argc) {
      override_p2p_port = std::atoi(argv[++i]);
    } else if (arg == "--supervisor") {
      force_supervisor = true;
    } else if (arg == "--read-unlock-stdin") {
      read_unlock_stdin = true;
    } else if (arg == "--re-encrypt") {
      re_encrypt = true;
    } else if (arg == "--log-level" && i + 1 < argc) {
      desentry::Logger::Instance().SetLevel(ParseLogLevel(argv[++i]));
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      PrintUsage();
      return 2;
    }
  }

  desentry::NodeConfig config = desentry::NodeConfig::LoadFromFile(config_path);
  if (!override_data_dir.empty()) config.data_dir = override_data_dir;
  if (override_api_port > 0) config.api_port = static_cast<uint16_t>(override_api_port);
  if (override_p2p_port > 0) config.p2p_port = static_cast<uint16_t>(override_p2p_port);
  if (force_supervisor) config.supervisor = true;

  // A supervisor's API must be loopback-only. Enforcing it here rather than
  // trusting the config file means a hand-edited or app-generated config can
  // never expose the control plane to the LAN.
  if (config.supervisor && config.api_bind_addr != "127.0.0.1" && config.api_bind_addr != "::1") {
    DSN_LOG_WARN("main", "supervisor api_bind_addr was '" << config.api_bind_addr
                                                           << "'; forcing 127.0.0.1");
    config.api_bind_addr = "127.0.0.1";
  }
  const std::string invalid = config.Validate();
  if (!invalid.empty()) {
    std::fprintf(stderr, "fatal: invalid configuration: %s\n", invalid.c_str());
    return 1;
  }

  // At-rest key handling. The unlock key arrives on stdin (one line, then
  // EOF) ONLY when --read-unlock-stdin is passed -- the sidecar passes it
  // whenever it has a key, and anything else leaves stdin alone so hand runs
  // never block. The key is Crockford text; the 32-byte DEK it decodes to is
  // what seals every data file, and it never touches disk.
  std::string dek;
  if (read_unlock_stdin || re_encrypt) {
    const std::string typed = ReadUnlockKeyFromStdin();
    if (!typed.empty()) {
      auto dek_or = desentry::at_rest::DecodeRecoveryKey(typed);
      if (!dek_or.ok()) {
        std::fprintf(stderr, "fatal: unlock key on stdin is not valid: %s\n",
                     dek_or.status().ToString().c_str());
        return 1;
      }
      dek = dek_or.value();
    }
  }

  if (re_encrypt) {
    // Offline migration: seal every plaintext file and exit. The node must
    // be stopped; the tool refuses sealed/mixed/torn input rather than
    // guessing, and skips already-sealed files so reruns finish the job.
    if (dek.empty()) {
      std::fprintf(stderr,
                   "fatal: --re-encrypt needs the node's unlock key on stdin "
                   "(pass --read-unlock-stdin or pipe the key)\n");
      return 1;
    }
    std::string summary;
    desentry::Status st =
        desentry::ReencryptDataDir(config.data_dir, dek, &summary);
    desentry::at_rest::Zeroize(dek);
    if (!st.ok()) {
      std::fprintf(stderr, "fatal: re-encrypt failed: %s\n", st.ToString().c_str());
      return 1;
    }
    std::printf("re-encrypt complete for %s: %s\n", config.data_dir.c_str(), summary.c_str());
    return 0;
  }

  // Fail closed: an encrypted node without its key must never boot into a
  // state that reads (or writes) plaintext. The sidecar surfaces this as the
  // unlock prompt; a hand run needs --read-unlock-stdin with the key piped.
  if (config.encrypt_at_rest && dek.empty()) {
    std::fprintf(stderr,
                 "fatal: encrypt_at_rest=true but no unlock key was provided (pass "
                 "--read-unlock-stdin and pipe the node's recovery key or passphrase-derived "
                 "key); refusing to boot unencrypted\n");
    return 1;
  }
  if (!config.encrypt_at_rest && !dek.empty()) {
    DSN_LOG_WARN("main", "an unlock key was provided but encrypt_at_rest=false; ignoring the key");
    desentry::at_rest::Zeroize(dek);
    dek.clear();
  }
  if (config.encrypt_at_rest) {
    DSN_LOG_INFO("main", "encrypt_at_rest=true: data files are sealed with the provided key");
  }

  desentry::NodeEngine::Options engine_opts;
  engine_opts.data_dir = config.data_dir;
  engine_opts.buffer_pool_pages = config.buffer_pool_pages;
  engine_opts.quota_mb = config.quota_mb;
  engine_opts.db_share_pct = config.quota_split.db_pct;
  engine_opts.engines = config.engines;
  engine_opts.default_engine = config.default_engine;
  engine_opts.transit_ttl_seconds = config.transit_ttl_seconds;
  engine_opts.transit_budget_bytes = config.quota_split.TransitBytes(config.quota_mb);
  engine_opts.transit_chunk_bytes = config.transit_chunk_bytes;
  engine_opts.replication_factor = config.replication_factor;
  engine_opts.supervisor = config.supervisor;
  engine_opts.dek = dek;

  auto engine_or = desentry::NodeEngine::Open(engine_opts);
  // The engine derived per-file subkeys at open; drop the raw key from this
  // scope so memory holds subkeys only.
  desentry::at_rest::Zeroize(engine_opts.dek);
  engine_opts.dek.clear();
  desentry::at_rest::Zeroize(dek);
  dek.clear();
  if (!engine_or.ok()) {
    std::fprintf(stderr, "fatal: failed to open node engine: %s\n",
                 engine_or.status().ToString().c_str());
    return 1;
  }
  auto engine = std::move(engine_or.value());

  desentry::NetworkManager network(engine.get(), config);
  const desentry::Status net_st = network.Start();
  if (!net_st.ok()) {
    std::fprintf(stderr, "fatal: failed to start network layer: %s\n", net_st.ToString().c_str());
    return 1;
  }

  std::unique_ptr<desentry::Supervisor> supervisor;
  if (config.supervisor) {
    desentry::Supervisor::Options supervisor_opts;
    supervisor_opts.registry_path = config.data_dir + "/managed_nodes.json";
    supervisor = std::make_unique<desentry::Supervisor>(engine.get(), &network, supervisor_opts);
    const desentry::Status sup_st = supervisor->Start();
    if (!sup_st.ok()) {
      std::fprintf(stderr, "fatal: failed to start supervisor: %s\n", sup_st.ToString().c_str());
      return 1;
    }
  }

  desentry::HttpServer api(config.api_bind_addr, config.api_port);
  // Per-boot bearer token (C-8): the desktop sidecar generates one boot and
  // passes it here; engine-only/dev use stays unauthenticated as before.
  if (const char* token = std::getenv("DESENTRY_API_TOKEN")) {
    if (*token != '\0') api.SetBearerToken(token);
  }
  desentry::RegisterRoutes(&api, engine.get(), &network);
  if (supervisor) {
    desentry::RegisterSupervisorRoutes(&api, supervisor.get(), engine.get(), &network);
  }
  if (!api.Start()) {
    std::fprintf(stderr, "fatal: failed to start API server on %s:%u\n", config.api_bind_addr.c_str(),
                 config.api_port);
    return 1;
  }

  DSN_LOG_INFO("main", "de-sentry node is up. node_id=" << engine->identity().node_id()
                                                         << " api=http://" << config.api_bind_addr
                                                         << ":" << config.api_port
                                                         << " p2p=" << config.p2p_bind_addr << ":"
                                                         << config.p2p_port
                                                         << (config.supervisor ? " [supervisor]" : ""));
  if (!config.node_name.empty()) DSN_LOG_INFO("main", "node_name=" << config.node_name);

  // A node that was offline may have bytes waiting for it on replicas. Asking
  // on startup -- rather than waiting for a gossip round to surface the
  // intents -- is what makes "close the laptop, reopen it, the data is there"
  // feel immediate. It is best-effort: peers that are not up yet are covered
  // by the periodic gossip path.
  if (!config.supervisor) {
    const desentry::NetworkManager::ClaimReport claimed = network.ClaimPendingTransit();
    if (claimed.documents_claimed > 0) {
      DSN_LOG_INFO("main", "claimed " << claimed.documents_claimed
                                       << " document(s) held while this node was offline");
    }
  }

  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
  while (g_shutdown == 0) desentry::SleepMs(200);

  DSN_LOG_INFO("main", "shutting down...");
  api.Stop();
  if (supervisor) supervisor->Stop();
  network.Stop();
  engine->storage().Checkpoint();
  DSN_LOG_INFO("main", "checkpointed and stopped cleanly.");
  return 0;
}
