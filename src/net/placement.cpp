#include "desentry/net/placement.h"

#include <algorithm>
#include <cstring>
#include <set>

#include "desentry/common/logger.h"
#include "desentry/common/platform.h"
#include "desentry/security/crypto.h"

namespace desentry {

uint64_t ConsistentHashRing::HashToRing(const std::string& value) {
  // The first 8 bytes of SHA-256, big-endian. SHA-256 rather than a fast
  // non-cryptographic hash because the ring position must be identical on
  // every platform and compiler -- a hash whose output depends on word size
  // or a seed would put the same key in different places on different nodes,
  // which is the one failure this structure exists to rule out.
  const std::string digest = crypto::Sha256(value);
  uint64_t out = 0;
  for (size_t i = 0; i < 8 && i < digest.size(); ++i) {
    out = (out << 8) | static_cast<uint8_t>(digest[i]);
  }
  return out;
}

void ConsistentHashRing::AddNode(const std::string& node_id, size_t virtual_nodes) {
  if (node_id.empty() || virtual_nodes == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  if (nodes_.count(node_id) != 0) return;
  nodes_[node_id] = virtual_nodes;
  for (size_t i = 0; i < virtual_nodes; ++i) {
    const uint64_t position = HashToRing(node_id + "#" + std::to_string(i));
    // A collision means two virtual nodes want the same position. Keeping
    // the lexicographically smaller node_id makes the resolution
    // deterministic across peers, which matters more than which one wins.
    auto it = ring_.find(position);
    if (it != ring_.end() && it->second <= node_id) continue;
    ring_[position] = node_id;
  }
}

void ConsistentHashRing::RemoveNode(const std::string& node_id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = nodes_.find(node_id);
  if (it == nodes_.end()) return;
  const size_t virtual_nodes = it->second;
  nodes_.erase(it);
  for (size_t i = 0; i < virtual_nodes; ++i) {
    const uint64_t position = HashToRing(node_id + "#" + std::to_string(i));
    auto slot = ring_.find(position);
    if (slot != ring_.end() && slot->second == node_id) ring_.erase(slot);
  }
}

void ConsistentHashRing::Clear() {
  std::lock_guard<std::mutex> lock(mu_);
  ring_.clear();
  nodes_.clear();
}

std::vector<std::string> ConsistentHashRing::Replicas(const std::string& key, size_t count) const {
  std::vector<std::string> out;
  if (count == 0) return out;
  std::lock_guard<std::mutex> lock(mu_);
  if (ring_.empty()) return out;

  const uint64_t position = HashToRing(key);
  auto it = ring_.lower_bound(position);
  if (it == ring_.end()) it = ring_.begin();  // wrap: the ring is a circle

  std::set<std::string> seen;
  const size_t max_steps = ring_.size();
  for (size_t step = 0; step < max_steps && out.size() < count; ++step) {
    if (seen.insert(it->second).second) out.push_back(it->second);
    ++it;
    if (it == ring_.end()) it = ring_.begin();
  }
  return out;
}

size_t ConsistentHashRing::NodeCount() const {
  std::lock_guard<std::mutex> lock(mu_);
  return nodes_.size();
}

size_t ConsistentHashRing::VirtualNodeCount() const {
  std::lock_guard<std::mutex> lock(mu_);
  return ring_.size();
}

bool ConsistentHashRing::Contains(const std::string& node_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  return nodes_.count(node_id) != 0;
}

bool PlacementPlan::Includes(const std::string& node_id) const {
  return std::find(replicas.begin(), replicas.end(), node_id) != replicas.end();
}

std::string PlacementHashInput(const std::string& collection, const std::string& key,
                                const std::string& shard_key_value) {
  if (!shard_key_value.empty()) {
    // Sharding by a document field co-locates everything that shares it
    // (all rows for one device, one tenant, one user), which is what makes a
    // range query on that field a single-replica read.
    return collection + "|shard|" + shard_key_value;
  }
  return collection + "/" + key;
}

void PlacementPolicy::Rebuild(const PeerTable& peers) {
  ring_.Clear();
  ring_with_absent_.Clear();
  skipped_.clear();

  const int64_t now = options_.now_ms != 0 ? options_.now_ms : NowMs();

  // The local node is always a candidate: it is reachable by definition, and
  // excluding it would make a single-node (airplane-mode) deployment have
  // nowhere to place anything.
  if (!local_node_id_.empty()) {
    ring_.AddNode(local_node_id_);
    ring_with_absent_.AddNode(local_node_id_);
  }

  for (const PeerInfo& peer : peers.List()) {
    if (peer.node_id.empty()) continue;
    // Bootstrap placeholders carry a synthetic id until a handshake reveals
    // the real one; placing data on "bootstrap#127.0.0.1:7801" would create
    // a replica set naming a node that does not exist.
    if (peer.node_id.rfind("bootstrap#", 0) == 0) {
      skipped_.push_back(peer.node_id + " (not yet handshaked)");
      continue;
    }
    if (peer.is_supervisor) {
      // Supervisors are app-local control-plane processes bound to loopback.
      // They are never on the data hot path, by design -- placing a replica
      // on one would make the control plane a data dependency.
      skipped_.push_back(peer.node_id + " (supervisor)");
      continue;
    }
    if (peer.state == NodeLifecycleState::kReclaimed) {
      skipped_.push_back(peer.node_id + " (reclaimed)");
      continue;
    }
    if (peer.state == NodeLifecycleState::kDegraded && !options_.include_degraded) {
      skipped_.push_back(peer.node_id + " (degraded)");
      ring_with_absent_.AddNode(peer.node_id);  // away, not gone: still owed writes
      continue;
    }
    if (peer.last_seen_ms != 0 && now - peer.last_seen_ms > options_.stale_after_ms) {
      skipped_.push_back(peer.node_id + " (stale)");
      ring_with_absent_.AddNode(peer.node_id);
      continue;
    }
    ring_.AddNode(peer.node_id);
    ring_with_absent_.AddNode(peer.node_id);
  }
}

PlacementPlan PlacementPolicy::Place(const std::string& collection, const std::string& key,
                                      const std::string& shard_key_value) const {
  PlacementPlan plan;
  plan.key = PlacementHashInput(collection, key, shard_key_value);
  plan.requested_rf = options_.replication_factor;
  plan.skipped = skipped_;
  plan.replicas = ring_.Replicas(plan.key, options_.replication_factor);
  plan.under_replicated = plan.replicas.size() < options_.replication_factor;

  // Who would have held this key if everyone were up. The difference between
  // the two rings is exactly the set of owners that are owed these bytes --
  // computed here because the live ring no longer contains them, so nothing
  // downstream could work it out on its own.
  if (ring_with_absent_.NodeCount() > ring_.NodeCount()) {
    for (const std::string& node : ring_with_absent_.Replicas(plan.key, options_.replication_factor)) {
      if (std::find(plan.replicas.begin(), plan.replicas.end(), node) == plan.replicas.end()) {
        plan.displaced_owners.push_back(node);
      }
    }
  }
  return plan;
}

std::map<std::string, std::vector<std::string>> PlacementPolicy::GroupByPrimary(
    const std::string& collection, const std::vector<std::string>& keys) const {
  std::map<std::string, std::vector<std::string>> grouped;
  for (const std::string& key : keys) {
    PlacementPlan plan = Place(collection, key);
    if (plan.replicas.empty()) continue;
    grouped[plan.replicas.front()].push_back(key);
  }
  return grouped;
}

}  // namespace desentry
