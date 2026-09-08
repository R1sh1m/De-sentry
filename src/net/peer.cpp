#include "desentry/net/peer.h"

#include <algorithm>
#include <cmath>

namespace desentry {

const char* NodeLifecycleStateName(NodeLifecycleState state) {
  switch (state) {
    case NodeLifecycleState::kDiscovered: return "discovered";
    case NodeLifecycleState::kAllocated: return "allocated";
    case NodeLifecycleState::kProvisioned: return "provisioned";
    case NodeLifecycleState::kRunning: return "running";
    case NodeLifecycleState::kDegraded: return "degraded";
    case NodeLifecycleState::kReclaimed: return "reclaimed";
  }
  return "unknown";
}

bool ValidNodeTransition(NodeLifecycleState from, NodeLifecycleState to) {
  if (from == to) return true;  // idempotent re-assertion of the current state
  switch (from) {
    case NodeLifecycleState::kDiscovered:
      return to == NodeLifecycleState::kAllocated || to == NodeLifecycleState::kReclaimed;
    case NodeLifecycleState::kAllocated:
      return to == NodeLifecycleState::kProvisioned || to == NodeLifecycleState::kReclaimed;
    case NodeLifecycleState::kProvisioned:
      return to == NodeLifecycleState::kRunning || to == NodeLifecycleState::kDegraded ||
             to == NodeLifecycleState::kReclaimed;
    case NodeLifecycleState::kRunning:
      // A running node that stops answering is degraded, not reclaimed:
      // reclaiming is a deliberate decommission, never an inference from a
      // missed health check.
      return to == NodeLifecycleState::kDegraded || to == NodeLifecycleState::kReclaimed;
    case NodeLifecycleState::kDegraded:
      return to == NodeLifecycleState::kRunning || to == NodeLifecycleState::kReclaimed;
    case NodeLifecycleState::kReclaimed:
      // Terminal. A reclaimed node's data has already been re-replicated
      // elsewhere; letting it come back as "running" would resurrect a
      // replica the placement layer no longer counts on.
      return false;
  }
  return false;
}

double PeerFitness::Score(lsn_t network_max_entry_id) const {
  // Reliability dominates (0.45): an unreachable peer's other numbers are
  // not measurements of anything useful.
  const double reliability = std::max(0.0, std::min(1.0, success_rate));

  // Latency, mapped through 1/(1+ms/50) so 0ms -> 1.0, 50ms -> 0.5,
  // 200ms -> 0.2. A hyperbola rather than a linear scale because the
  // difference between 1ms and 20ms barely matters on a LAN, while the
  // difference between 200ms and 2s matters enormously.
  const double latency_term = probes == 0 ? 0.5 : 1.0 / (1.0 + std::max(0.0, latency_ms) / 50.0);

  // Ledger freshness relative to the freshest peer we know of. A peer that
  // is far behind is a poor replica target: it would have to catch up before
  // it could serve anything.
  double freshness = 0.5;
  if (network_max_entry_id > 0 && ledger_freshness_entry_id >= 0) {
    freshness = static_cast<double>(ledger_freshness_entry_id) / static_cast<double>(network_max_entry_id);
    freshness = std::max(0.0, std::min(1.0, freshness));
  }

  // Free capacity, saturating at 1GiB -- past that, more headroom does not
  // make a peer a meaningfully better target. Weighted least because it is
  // the one figure the peer asserts about itself with no way for us to check.
  const double capacity = std::min(1.0, static_cast<double>(free_quota_mb) / 1024.0);

  return 0.45 * reliability + 0.25 * latency_term + 0.20 * freshness + 0.10 * capacity;
}

void PeerTable::Upsert(const PeerInfo& info) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = peers_.find(info.node_id);
  if (it == peers_.end()) {
    peers_[info.node_id] = info;
    return;
  }
  // Keep whichever info is freshest; don't clobber a known pubkey with an
  // empty one from a discovery broadcast that hasn't handshaked yet, and
  // never let a discovery packet reset accumulated fitness history.
  PeerInfo& existing = it->second;
  existing.host = info.host;
  existing.p2p_port = info.p2p_port;
  existing.last_seen_ms = info.last_seen_ms;
  if (!info.ed25519_pubkey.empty()) existing.ed25519_pubkey = info.ed25519_pubkey;
  if (info.api_port != 0) existing.api_port = info.api_port;
  if (!info.hostname.empty()) existing.hostname = info.hostname;
  if (info.is_supervisor) existing.is_supervisor = true;
  if (info.state != NodeLifecycleState::kDiscovered &&
      ValidNodeTransition(existing.state, info.state)) {
    existing.state = info.state;
  }
}

std::vector<PeerInfo> PeerTable::List() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<PeerInfo> out;
  out.reserve(peers_.size());
  for (const auto& [id, info] : peers_) {
    (void)id;
    out.push_back(info);
  }
  return out;
}

size_t PeerTable::Size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return peers_.size();
}

bool PeerTable::Contains(const std::string& node_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  return peers_.count(node_id) != 0;
}

bool PeerTable::Get(const std::string& node_id, PeerInfo* out) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = peers_.find(node_id);
  if (it == peers_.end()) return false;
  *out = it->second;
  return true;
}

void PeerTable::RecordProbe(const std::string& node_id, double latency_ms, bool ok) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = peers_.find(node_id);
  if (it == peers_.end()) return;
  PeerFitness& fitness = it->second.fitness;
  const double alpha = kFitnessAlpha;
  fitness.success_rate = (1.0 - alpha) * fitness.success_rate + alpha * (ok ? 1.0 : 0.0);
  if (ok) {
    // A failed probe contributes nothing to the latency estimate: a timeout
    // is not a round-trip measurement, and folding one in would make a dead
    // peer look merely slow.
    fitness.latency_ms = fitness.probes == 0
                              ? latency_ms
                              : (1.0 - alpha) * fitness.latency_ms + alpha * latency_ms;
  }
  ++fitness.probes;
}

void PeerTable::RecordReport(const std::string& node_id, lsn_t ledger_entry_id,
                              uint64_t free_quota_mb) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = peers_.find(node_id);
  if (it == peers_.end()) return;
  it->second.fitness.ledger_freshness_entry_id = ledger_entry_id;
  it->second.fitness.free_quota_mb = free_quota_mb;
  it->second.fitness.quota_reported = true;
}

void PeerTable::RecordLedgerHeight(const std::string& node_id, lsn_t ledger_entry_id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = peers_.find(node_id);
  if (it == peers_.end()) return;
  it->second.fitness.ledger_freshness_entry_id = ledger_entry_id;
}

bool PeerTable::AdoptIdentity(const std::string& old_id, const std::string& real_id) {
  if (old_id.rfind("bootstrap#", 0) != 0 || real_id.empty() || old_id == real_id) return false;
  std::lock_guard<std::mutex> lock(mu_);
  auto it = peers_.find(old_id);
  if (it == peers_.end()) return false;
  auto existing = peers_.find(real_id);
  if (existing != peers_.end()) {
    // Both a placeholder and a real entry for one peer (e.g. bootstrap plus
    // a discovery packet): fold the placeholder's liveness into the real one
    // and drop the duplicate, keeping accumulated fitness either way.
    PeerInfo& keep = existing->second;
    const PeerInfo& drop = it->second;
    keep.last_seen_ms = std::max(keep.last_seen_ms, drop.last_seen_ms);
    if (keep.host.empty()) keep.host = drop.host;
    if (keep.p2p_port == 0) keep.p2p_port = drop.p2p_port;
    if (keep.ed25519_pubkey.empty()) keep.ed25519_pubkey = drop.ed25519_pubkey;
    peers_.erase(it);
    return true;
  }
  PeerInfo moved = std::move(it->second);
  peers_.erase(it);
  moved.node_id = real_id;
  peers_[real_id] = std::move(moved);
  return true;
}

void PeerTable::SetState(const std::string& node_id, NodeLifecycleState state) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = peers_.find(node_id);
  if (it == peers_.end()) return;
  if (ValidNodeTransition(it->second.state, state)) it->second.state = state;
}

lsn_t PeerTable::NetworkMaxLedgerEntryId() const {
  std::lock_guard<std::mutex> lock(mu_);
  lsn_t max_id = 0;
  for (const auto& [id, info] : peers_) {
    (void)id;
    max_id = std::max(max_id, info.fitness.ledger_freshness_entry_id);
  }
  return max_id;
}

std::vector<PeerInfo> PeerTable::Ranked() const {
  const lsn_t network_max = NetworkMaxLedgerEntryId();
  std::vector<PeerInfo> out = List();
  std::sort(out.begin(), out.end(), [network_max](const PeerInfo& a, const PeerInfo& b) {
    const double sa = a.fitness.Score(network_max);
    const double sb = b.fitness.Score(network_max);
    if (sa != sb) return sa > sb;
    return a.node_id < b.node_id;  // stable, and identical on every node
  });
  return out;
}

std::vector<PeerInfo> PeerTable::StalerThan(int64_t stale_ms, int64_t now_ms) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<PeerInfo> out;
  for (const auto& [id, info] : peers_) {
    (void)id;
    if (info.last_seen_ms != 0 && now_ms - info.last_seen_ms > stale_ms) out.push_back(info);
  }
  return out;
}

}  // namespace desentry
