// The supervisor's REST surface, registered on top of the ordinary node
// routes only when NodeConfig::supervisor is true.
//
// Everything here is control plane: discovering hardware, tracking node
// lifecycle, recording key custody, forcing a housekeeping pass. Nothing here
// reads or writes replicated data, and nothing here is reachable from the
// LAN -- a supervisor's API binds to loopback, enforced by
// NodeConfig::Validate().

#include <algorithm>

#include "desentry/common/hex.h"
#include "desentry/common/logger.h"
#include "desentry/common/platform.h"
#include "desentry/supervisor/supervisor.h"

namespace desentry {

namespace {

HttpResponse JsonOk(const JsonValue& v, int status = 200) {
  return HttpResponse::Json(status, v.Dump());
}

HttpResponse JsonError(int status, const std::string& message) {
  JsonValue::Object obj;
  obj.emplace_back("error", JsonValue(message));
  return HttpResponse::Json(status, JsonValue(std::move(obj)).Dump());
}

HttpResponse StatusError(const Status& st) {
  int code = 500;
  switch (st.code()) {
    case StatusCode::kNotFound: code = 404; break;
    case StatusCode::kInvalidArgument: code = 400; break;
    case StatusCode::kAlreadyExists: code = 409; break;
    case StatusCode::kAuthError: code = 403; break;
    default: break;
  }
  return JsonError(code, st.message());
}

NodeLifecycleState ParseStateOrDiscovered(const std::string& name) {
  if (name == "allocated") return NodeLifecycleState::kAllocated;
  if (name == "provisioned") return NodeLifecycleState::kProvisioned;
  if (name == "running") return NodeLifecycleState::kRunning;
  if (name == "degraded") return NodeLifecycleState::kDegraded;
  if (name == "reclaimed") return NodeLifecycleState::kReclaimed;
  return NodeLifecycleState::kDiscovered;
}

}  // namespace

void RegisterSupervisorRoutes(HttpServer* server, Supervisor* supervisor, NodeEngine* engine,
                               NetworkManager* network) {
  // -- hardware discovery ----------------------------------------------------
  server->Get("/_supervisor/scan", [](const HttpRequest& req) -> HttpResponse {
    std::vector<std::string> roots;
    auto root_it = req.query.find("root");
    if (root_it != req.query.end() && !root_it->second.empty()) roots.push_back(root_it->second);

    const std::vector<DataDirCandidate> candidates =
        roots.empty() ? HardwareScanner::ScanDefaults() : HardwareScanner::ScanRoots(roots);

    JsonValue::Array arr;
    for (const DataDirCandidate& candidate : candidates) arr.emplace_back(candidate.ToJson());
    JsonValue::Object obj;
    obj.emplace_back("count", JsonValue(static_cast<int64_t>(arr.size())));
    obj.emplace_back("candidates", JsonValue(std::move(arr)));
    return JsonOk(JsonValue(std::move(obj)));
  });

  server->Get("/_supervisor/mounts", [](const HttpRequest&) -> HttpResponse {
    JsonValue::Array arr;
    for (const MountPoint& mount : HardwareScanner::ListMounts()) arr.emplace_back(mount.ToJson());
    return JsonOk(JsonValue(std::move(arr)));
  });

  server->Get("/_supervisor/inspect", [](const HttpRequest& req) -> HttpResponse {
    auto path_it = req.query.find("path");
    if (path_it == req.query.end() || path_it->second.empty()) {
      return JsonError(400, "query parameter 'path' is required");
    }
    return JsonOk(HardwareScanner::Inspect(path_it->second).ToJson());
  });

  // -- managed node registry --------------------------------------------------
  server->Get("/_supervisor/nodes", [supervisor](const HttpRequest&) -> HttpResponse {
    JsonValue::Array arr;
    for (const ManagedNode& node : supervisor->Nodes()) arr.emplace_back(node.ToJson());
    return JsonOk(JsonValue(std::move(arr)));
  });

  server->Put("/_supervisor/nodes/:node_id", [supervisor](const HttpRequest& req) -> HttpResponse {
    JsonValue body;
    try {
      body = JsonValue::Parse(req.body.empty() ? "{}" : req.body);
    } catch (const std::exception& e) {
      return JsonError(400, std::string("invalid JSON body: ") + e.what());
    }
    ManagedNode node = ManagedNode::FromJson(body);
    node.node_id = req.params.at("node_id");
    Status st = supervisor->UpsertNode(node);
    if (!st.ok()) return StatusError(st);
    return JsonOk(node.ToJson());
  });

  server->Post("/_supervisor/nodes/:node_id/transition",
               [supervisor](const HttpRequest& req) -> HttpResponse {
                 JsonValue body;
                 try {
                   body = JsonValue::Parse(req.body.empty() ? "{}" : req.body);
                 } catch (const std::exception& e) {
                   return JsonError(400, std::string("invalid JSON body: ") + e.what());
                 }
                 const JsonValue* to = body.Find("state");
                 if (to == nullptr || !to->is_string()) {
                   return JsonError(400, "body must be {\"state\": \"...\"}");
                 }
                 Status st = supervisor->TransitionNode(req.params.at("node_id"),
                                                         ParseStateOrDiscovered(to->AsString()));
                 if (!st.ok()) return StatusError(st);
                 auto node = supervisor->GetNode(req.params.at("node_id"));
                 if (!node.ok()) return StatusError(node.status());
                 return JsonOk(node.value().ToJson());
               });

  // Key custody. The app calls this once the user has actually saved or
  // printed the recovery key, and the wizard refuses to finish until it has.
  // The key never appears in this request -- only the fact of the export.
  server->Post("/_supervisor/nodes/:node_id/recovery-key-exported",
               [supervisor](const HttpRequest& req) -> HttpResponse {
                 Status st = supervisor->RecordRecoveryKeyExport(req.params.at("node_id"));
                 if (!st.ok()) return StatusError(st);
                 return JsonOk(JsonValue(true));
               });

  server->Del("/_supervisor/nodes/:node_id", [supervisor](const HttpRequest& req) -> HttpResponse {
    Status st = supervisor->RemoveNode(req.params.at("node_id"));
    if (!st.ok()) return StatusError(st);
    JsonValue::Object obj;
    obj.emplace_back("ok", JsonValue(true));
    return JsonOk(JsonValue(std::move(obj)));
  });

  // -- portable node manifests ------------------------------------------------
  server->Post("/_supervisor/manifest", [](const HttpRequest& req) -> HttpResponse {
    JsonValue body;
    try {
      body = JsonValue::Parse(req.body.empty() ? "{}" : req.body);
    } catch (const std::exception& e) {
      return JsonError(400, std::string("invalid JSON body: ") + e.what());
    }
    const JsonValue* dir = body.Find("dir");
    const JsonValue* manifest = body.Find("manifest");
    if (dir == nullptr || !dir->is_string() || manifest == nullptr) {
      return JsonError(400, "body must be {\"dir\": \"...\", \"manifest\": {...}}");
    }
    Status st = HardwareScanner::WriteManifest(dir->AsString(), *manifest);
    if (!st.ok()) return StatusError(st);
    JsonValue::Object obj;
    obj.emplace_back("ok", JsonValue(true));
    return JsonOk(JsonValue(std::move(obj)));
  });

  // -- housekeeping -----------------------------------------------------------
  server->Post("/_supervisor/pass", [supervisor](const HttpRequest&) -> HttpResponse {
    const Supervisor::PassReport report = supervisor->RunOnce();
    JsonValue::Object obj;
    obj.emplace_back("peers_marked_degraded",
                     JsonValue(static_cast<int64_t>(report.peers_marked_degraded)));
    obj.emplace_back("transit_expired", JsonValue(static_cast<int64_t>(report.transit_expired)));
    obj.emplace_back("nodes_over_quota", JsonValue(static_cast<int64_t>(report.nodes_over_quota)));
    obj.emplace_back("checkpoint_attempted", JsonValue(report.checkpoint_attempted));
    obj.emplace_back("checkpoint_proceeded", JsonValue(report.checkpoint_proceeded));
    obj.emplace_back("checkpoint_reason", JsonValue(report.checkpoint_reason));
    obj.emplace_back("entries_pruned", JsonValue(static_cast<int64_t>(report.entries_pruned)));
    return JsonOk(JsonValue(std::move(obj)));
  });

  // A single call the app's sidebar refreshes from: everything needed to draw
  // the Device -> Directory/USB -> Node tree without a fan-out of requests.
  server->Get("/_supervisor/topology", [supervisor, engine, network](const HttpRequest&) -> HttpResponse {
    JsonValue::Array mounts;
    for (const MountPoint& mount : HardwareScanner::ListMounts()) mounts.emplace_back(mount.ToJson());

    JsonValue::Array nodes;
    for (const ManagedNode& node : supervisor->Nodes()) nodes.emplace_back(node.ToJson());

    const lsn_t network_max = network->peers().NetworkMaxLedgerEntryId();
    JsonValue::Array peers;
    for (const PeerInfo& peer : network->peers().Ranked()) {
      JsonValue::Object p;
      p.emplace_back("node_id", JsonValue(peer.node_id));
      p.emplace_back("host", JsonValue(peer.host));
      p.emplace_back("hostname", JsonValue(peer.hostname));
      p.emplace_back("api_port", JsonValue(static_cast<int64_t>(peer.api_port)));
      p.emplace_back("p2p_port", JsonValue(static_cast<int64_t>(peer.p2p_port)));
      p.emplace_back("state", JsonValue(NodeLifecycleStateName(peer.state)));
      p.emplace_back("supervisor", JsonValue(peer.is_supervisor));
      p.emplace_back("score", JsonValue(peer.fitness.Score(network_max)));
      p.emplace_back("ledger_entry_id",
                     JsonValue(static_cast<int64_t>(peer.fitness.ledger_freshness_entry_id)));
      peers.emplace_back(std::move(p));
    }

    JsonValue::Object obj;
    obj.emplace_back("supervisor_node_id", JsonValue(engine->identity().node_id()));
    obj.emplace_back("mounts", JsonValue(std::move(mounts)));
    obj.emplace_back("managed_nodes", JsonValue(std::move(nodes)));
    obj.emplace_back("lan_peers", JsonValue(std::move(peers)));
    obj.emplace_back("replication_factor",
                     JsonValue(static_cast<int64_t>(engine->replication_factor())));
    return JsonOk(JsonValue(std::move(obj)));
  });

  // QR-code pairing payload: what another device scans to join this mesh.
  // It carries addresses and a public key, never a secret -- pairing tells a
  // peer where to dial and whom to expect, and the secure-channel handshake
  // is what actually authenticates.
  server->Get("/_supervisor/pairing", [engine, network](const HttpRequest&) -> HttpResponse {
    JsonValue::Array bootstrap;
    for (const PeerInfo& peer : network->peers().Ranked()) {
      if (peer.p2p_port == 0 || peer.is_supervisor) continue;
      bootstrap.emplace_back(JsonValue(peer.host + ":" + std::to_string(peer.p2p_port)));
    }
    JsonValue::Object obj;
    obj.emplace_back("version", JsonValue(static_cast<int64_t>(2)));
    obj.emplace_back("node_id", JsonValue(engine->identity().node_id()));
    obj.emplace_back("public_key_hex", JsonValue(HexEncode(engine->identity().public_key())));
    obj.emplace_back("discovery_port",
                     JsonValue(static_cast<int64_t>(network->config().discovery_port)));
    obj.emplace_back("bootstrap_peers", JsonValue(std::move(bootstrap)));
    return JsonOk(JsonValue(std::move(obj)));
  });
}

}  // namespace desentry
