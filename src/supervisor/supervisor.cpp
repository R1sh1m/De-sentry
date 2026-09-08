#include "desentry/supervisor/supervisor.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "desentry/common/hex.h"
#include "desentry/common/logger.h"
#include "desentry/common/platform.h"
#include "desentry/ledger/checkpoint.h"

namespace desentry {

namespace {

NodeLifecycleState ParseState(const std::string& name) {
  if (name == "allocated") return NodeLifecycleState::kAllocated;
  if (name == "provisioned") return NodeLifecycleState::kProvisioned;
  if (name == "running") return NodeLifecycleState::kRunning;
  if (name == "degraded") return NodeLifecycleState::kDegraded;
  if (name == "reclaimed") return NodeLifecycleState::kReclaimed;
  return NodeLifecycleState::kDiscovered;
}

}  // namespace

JsonValue ManagedNode::ToJson() const {
  JsonValue::Object o;
  o.emplace_back("node_id", JsonValue(node_id));
  o.emplace_back("node_name", JsonValue(node_name));
  o.emplace_back("data_dir", JsonValue(data_dir));
  o.emplace_back("api_port", JsonValue(static_cast<int64_t>(api_port)));
  o.emplace_back("p2p_port", JsonValue(static_cast<int64_t>(p2p_port)));
  o.emplace_back("state", JsonValue(NodeLifecycleStateName(state)));
  o.emplace_back("removable", JsonValue(removable));
  o.emplace_back("encrypted", JsonValue(encrypted));
  o.emplace_back("recovery_key_exported", JsonValue(recovery_key_exported));
  o.emplace_back("recovery_key_exported_ms", JsonValue(recovery_key_exported_ms));
  o.emplace_back("keychain_ref", JsonValue(keychain_ref));
  o.emplace_back("created_ms", JsonValue(created_ms));
  o.emplace_back("last_seen_ms", JsonValue(last_seen_ms));
  o.emplace_back("last_error", JsonValue(last_error));
  return JsonValue(std::move(o));
}

ManagedNode ManagedNode::FromJson(const JsonValue& v) {
  ManagedNode node;
  if (!v.is_object()) return node;
  node.node_id = v.Get("node_id").AsString();
  node.node_name = v.Get("node_name").AsString();
  node.data_dir = v.Get("data_dir").AsString();
  node.api_port = static_cast<uint16_t>(v.Get("api_port").AsInt());
  node.p2p_port = static_cast<uint16_t>(v.Get("p2p_port").AsInt());
  node.state = ParseState(v.Get("state").AsString());
  const JsonValue* removable = v.Find("removable");
  if (removable && removable->is_bool()) node.removable = removable->AsBool();
  const JsonValue* encrypted = v.Find("encrypted");
  if (encrypted && encrypted->is_bool()) node.encrypted = encrypted->AsBool();
  const JsonValue* exported = v.Find("recovery_key_exported");
  if (exported && exported->is_bool()) node.recovery_key_exported = exported->AsBool();
  node.recovery_key_exported_ms = v.Get("recovery_key_exported_ms").AsInt();
  node.keychain_ref = v.Get("keychain_ref").AsString();
  node.created_ms = v.Get("created_ms").AsInt();
  node.last_seen_ms = v.Get("last_seen_ms").AsInt();
  node.last_error = v.Get("last_error").AsString();
  return node;
}

Supervisor::Supervisor(NodeEngine* engine, NetworkManager* network, const Options& options)
    : engine_(engine), network_(network), options_(options) {}

Supervisor::~Supervisor() { Stop(); }

Status Supervisor::Start() {
  if (!engine_->is_supervisor()) {
    // A hard refusal rather than a warning. A data node quietly running
    // supervisor duties would be exactly the coordinator this architecture
    // does not have, and it would prune its own ledger without a quorum.
    return Status::InvalidArgument(
        "Supervisor::Start() called on a node that is not configured as a supervisor "
        "(set \"supervisor\": true in node.json)");
  }
  Status st = LoadRegistry();
  if (!st.ok()) return st;
  running_ = true;
  thread_ = std::thread(&Supervisor::Loop, this);
  DSN_LOG_INFO("supervisor", "supervisor started (interval " << options_.interval_ms << "ms, "
                                                              << nodes_.size() << " managed node(s))");
  return Status::OK();
}

void Supervisor::Stop() {
  if (!running_) return;
  running_ = false;
  if (thread_.joinable()) thread_.join();
  SaveRegistry();
}

void Supervisor::Loop() {
  while (running_) {
    RunOnce();
    for (uint32_t waited = 0; waited < options_.interval_ms && running_; waited += 200) SleepMs(200);
  }
}

Supervisor::PassReport Supervisor::RunOnce() {
  PassReport report;
  report.peers_marked_degraded = MarkStalePeersDegraded();

  auto expired = engine_->transit().ExpireAsOf(NowMs());
  if (expired.ok()) report.transit_expired = expired.value();

  report.nodes_over_quota = EnforceQuota();

  const int64_t now = MonotonicMs();
  if (now - last_checkpoint_ms_ >= options_.checkpoint_interval_ms) {
    last_checkpoint_ms_ = now;
    AttemptCheckpoint(&report);
  }

  // Refresh the registry's liveness view from the peer table, so the app's
  // node list reflects what the mesh actually sees rather than what was true
  // when a node was created.
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (ManagedNode& node : nodes_) {
      PeerInfo peer;
      if (!network_->peers().Get(node.node_id, &peer)) continue;
      node.last_seen_ms = peer.last_seen_ms;
      if (ValidNodeTransition(node.state, peer.state)) node.state = peer.state;
    }
  }
  SaveRegistry();
  return report;
}

size_t Supervisor::MarkStalePeersDegraded() {
  const int64_t now = NowMs();
  const int64_t stale_ms = static_cast<int64_t>(network_->config().gossip_interval_ms) * 3 + 5000;
  size_t marked = 0;
  for (const PeerInfo& peer : network_->peers().StalerThan(stale_ms, now)) {
    if (peer.state != NodeLifecycleState::kRunning) continue;
    network_->peers().SetState(peer.node_id, NodeLifecycleState::kDegraded);
    ++marked;
    DSN_LOG_WARN("supervisor", "peer " << peer.node_id << " has not been seen for "
                                        << (now - peer.last_seen_ms) << "ms; marking degraded");
  }
  if (marked > 0) network_->RebuildPlacement();
  return marked;
}

size_t Supervisor::EnforceQuota() {
  // The supervisor's own quota is the only one it can measure directly.
  // Peers report theirs via /_brain, which the gossip layer folds into their
  // fitness; a peer over budget therefore sinks in the ranking on its own.
  // What the supervisor adds is the explicit degraded transition, so the app
  // can warn *before* writes start failing rather than after.
  size_t over = 0;
  const StorageEngine::QuotaStatus quota = engine_->Quota();
  if (quota.limit_bytes > 0 && quota.used_fraction >= options_.quota_degraded_fraction) {
    ++over;
    DSN_LOG_WARN("supervisor", "this node is at " << static_cast<int>(quota.used_fraction * 100)
                                                   << "% of its quota");
  }

  const lsn_t network_max = network_->peers().NetworkMaxLedgerEntryId();
  for (const PeerInfo& peer : network_->peers().List()) {
    if (peer.fitness.probes == 0) continue;
    // A peer reporting zero free quota while otherwise healthy is out of
    // space, not unreachable -- a distinction that matters because the fix
    // is different. The report must actually exist: an unreported figure is
    // also zero, and "no data" must never read as "no space" (it would mark
    // every healthy peer -- unlimited-quota nodes included -- degraded).
    if (peer.fitness.quota_reported && peer.fitness.free_quota_mb == 0 &&
        peer.fitness.success_rate > 0.5) {
      ++over;
      network_->peers().SetState(peer.node_id, NodeLifecycleState::kDegraded);
      DSN_LOG_WARN("supervisor", "peer " << peer.node_id << " reports no free quota (fitness "
                                          << peer.fitness.Score(network_max) << "); marking degraded");
    }
  }
  if (over > 0) network_->RebuildPlacement();
  return over;
}

void Supervisor::AttemptCheckpoint(PassReport* report) {
  report->checkpoint_attempted = true;

  const auto tip = engine_->LedgerTip();
  if (tip.entry_id < 0) {
    report->checkpoint_reason = "ledger is empty";
    return;
  }

  ReplicaTip local;
  local.node_id = engine_->identity().node_id();
  local.entry_id = tip.entry_id;
  local.entry_hash = tip.entry_hash;
  local.signature = engine_->SignLedgerTip();
  local.self_verified = engine_->VerifyLedger().ok;
  local.signature_valid = true;  // we produced it

  auto outcome_or = RunCheckpoint(engine_->storage().wal(), &engine_->transit(), local,
                                   network_->CollectReplicaTips(), engine_->replication_factor(),
                                   engine_->clock().Now());
  if (!outcome_or.ok()) {
    report->checkpoint_reason = outcome_or.status().message();
    return;
  }
  report->checkpoint_proceeded = outcome_or.value().decision.proceed;
  report->checkpoint_reason = outcome_or.value().decision.reason;
  report->entries_pruned = outcome_or.value().entries_pruned;
}

// ---------------------------------------------------------------------------
// Managed node registry
// ---------------------------------------------------------------------------

std::vector<ManagedNode> Supervisor::Nodes() const {
  std::lock_guard<std::mutex> lock(mu_);
  return nodes_;
}

StatusOr<ManagedNode> Supervisor::GetNode(const std::string& node_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  for (const ManagedNode& node : nodes_) {
    if (node.node_id == node_id) return node;
  }
  return Status::NotFound("no managed node with id " + node_id);
}

Status Supervisor::UpsertNode(const ManagedNode& node) {
  if (node.node_id.empty()) return Status::InvalidArgument("a managed node needs a node_id");
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = std::find_if(nodes_.begin(), nodes_.end(),
                           [&](const ManagedNode& n) { return n.node_id == node.node_id; });
    if (it == nodes_.end()) {
      ManagedNode fresh = node;
      if (fresh.created_ms == 0) fresh.created_ms = NowMs();
      nodes_.push_back(std::move(fresh));
    } else {
      ManagedNode updated = node;
      updated.created_ms = it->created_ms;
      // An update must not silently rewind the lifecycle. Rejecting the
      // transition and keeping the current state is safer than accepting a
      // caller's stale view.
      if (!ValidNodeTransition(it->state, updated.state)) updated.state = it->state;
      // Key custody is monotonic: once a recovery key has been exported,
      // nothing may un-record that.
      if (it->recovery_key_exported) {
        updated.recovery_key_exported = true;
        updated.recovery_key_exported_ms = it->recovery_key_exported_ms;
      }
      *it = std::move(updated);
    }
  }
  return SaveRegistry();
}

Status Supervisor::TransitionNode(const std::string& node_id, NodeLifecycleState to) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = std::find_if(nodes_.begin(), nodes_.end(),
                           [&](const ManagedNode& n) { return n.node_id == node_id; });
    if (it == nodes_.end()) return Status::NotFound("no managed node with id " + node_id);
    if (!ValidNodeTransition(it->state, to)) {
      return Status::InvalidArgument(std::string("invalid lifecycle transition ") +
                                      NodeLifecycleStateName(it->state) + " -> " +
                                      NodeLifecycleStateName(to));
    }
    it->state = to;
  }
  network_->peers().SetState(node_id, to);
  network_->RebuildPlacement();
  return SaveRegistry();
}

Status Supervisor::RecordRecoveryKeyExport(const std::string& node_id) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = std::find_if(nodes_.begin(), nodes_.end(),
                           [&](const ManagedNode& n) { return n.node_id == node_id; });
    if (it == nodes_.end()) return Status::NotFound("no managed node with id " + node_id);
    // Only that it happened, and when. There is no escrow: the key itself
    // never touches disk here or anywhere else.
    it->recovery_key_exported = true;
    it->recovery_key_exported_ms = NowMs();
  }
  return SaveRegistry();
}

Status Supervisor::RemoveNode(const std::string& node_id) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = std::remove_if(nodes_.begin(), nodes_.end(),
                             [&](const ManagedNode& n) { return n.node_id == node_id; });
    if (it == nodes_.end()) return Status::NotFound("no managed node with id " + node_id);
    nodes_.erase(it, nodes_.end());
  }
  return SaveRegistry();
}

Status Supervisor::SaveRegistry() {
  if (options_.registry_path.empty()) return Status::OK();
  JsonValue::Array arr;
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (const ManagedNode& node : nodes_) arr.emplace_back(node.ToJson());
  }
  const std::string tmp = options_.registry_path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc | std::ios::binary);
    if (!f.is_open()) return Status::IOError("cannot write " + tmp);
    f << JsonValue(std::move(arr)).Dump();
    f.flush();
    if (!f.good()) return Status::IOError("supervisor registry write failed");
  }
  std::remove(options_.registry_path.c_str());
  if (std::rename(tmp.c_str(), options_.registry_path.c_str()) != 0) {
    return Status::IOError("cannot commit " + options_.registry_path);
  }
  return Status::OK();
}

Status Supervisor::LoadRegistry() {
  if (options_.registry_path.empty()) return Status::OK();
  std::ifstream f(options_.registry_path);
  if (!f.is_open()) return Status::OK();
  std::ostringstream ss;
  ss << f.rdbuf();
  if (ss.str().empty()) return Status::OK();
  JsonValue root;
  try {
    root = JsonValue::Parse(ss.str());
  } catch (const std::exception& e) {
    return Status::Corruption(std::string("supervisor registry parse error: ") + e.what());
  }
  if (!root.is_array()) return Status::OK();
  std::lock_guard<std::mutex> lock(mu_);
  nodes_.clear();
  for (const JsonValue& entry : root.AsArray()) nodes_.push_back(ManagedNode::FromJson(entry));
  return Status::OK();
}

}  // namespace desentry
