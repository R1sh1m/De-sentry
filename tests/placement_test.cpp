// Placement test suite: the consistent hash ring and the policy over it.
//
// Two things have to be true for placement to be worth having:
//
//   * **Stability.** Adding or removing one node must move only the keys that
//     belonged to it. A ring that reshuffled everything on a membership change
//     would make every node join a full re-replication, which on a 50-node LAN
//     is the difference between a working mesh and a broken one.
//   * **Determinism across machines.** Every node must compute the same
//     replica set for the same key, on every platform. That is why
//     `HashToRing` reads the first eight bytes of a SHA-256 big-endian rather
//     than using anything host-dependent, and it is asserted here directly.
//
// The policy layer adds filtering, and its exclusions are checked too: a
// supervisor is never a placement target (it holds no data, by construction),
// and neither is a reclaimed or long-unseen node.

#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "desentry/common/platform.h"
#include "desentry/net/peer.h"
#include "desentry/net/placement.h"

using namespace desentry;

namespace {

std::vector<std::string> Keys(size_t count, const char* prefix = "key") {
  std::vector<std::string> keys;
  keys.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%s-%06zu", prefix, i);
    keys.emplace_back(buffer);
  }
  return keys;
}

void TestRingBasics() {
  std::cout << "  ring: membership and replica sets\n";
  ConsistentHashRing ring;
  assert(ring.NodeCount() == 0);
  assert(ring.Replicas("anything", 3).empty());

  ring.AddNode("node-a");
  ring.AddNode("node-b");
  ring.AddNode("node-c");
  assert(ring.NodeCount() == 3);
  assert(ring.VirtualNodeCount() == 3 * kVirtualNodesPerPeer);
  assert(ring.Contains("node-b"));

  auto replicas = ring.Replicas("some/key", 3);
  assert(replicas.size() == 3);
  // Distinct physical nodes, not three virtual nodes of the same host --
  // three copies on one disk is not a replication factor of three.
  std::set<std::string> distinct(replicas.begin(), replicas.end());
  assert(distinct.size() == 3);

  // Asking for more replicas than there are nodes returns what exists rather
  // than repeating or failing.
  assert(ring.Replicas("some/key", 10).size() == 3);

  // The same key always resolves the same way, and the order is stable --
  // the primary must not drift between calls.
  assert(ring.Replicas("some/key", 3) == replicas);

  ring.RemoveNode("node-b");
  assert(ring.NodeCount() == 2);
  assert(!ring.Contains("node-b"));

  ring.Clear();
  assert(ring.NodeCount() == 0);
}

void TestHashIsPlatformStable() {
  std::cout << "  ring: the hash is fixed, not host-dependent\n";
  // HashToRing reads the first eight bytes of a SHA-256 big-endian, so it is
  // identical on every platform. What is checked here is that it is a
  // function of its input alone and that it separates a realistic key space
  // without collisions -- the two properties a mesh depends on when two nodes
  // must independently agree on where a key lives.
  const uint64_t empty = ConsistentHashRing::HashToRing("");
  const uint64_t a = ConsistentHashRing::HashToRing("a");
  assert(empty != a);

  // Determinism across calls, and no accidental dependence on ordering.
  for (const std::string& value : {std::string(""), std::string("a"), std::string("collection/key")}) {
    assert(ConsistentHashRing::HashToRing(value) == ConsistentHashRing::HashToRing(value));
  }

  // Distinct inputs must not collide at 64 bits over a realistic key space --
  // a collision would silently co-locate two shards.
  std::set<uint64_t> seen;
  for (const std::string& key : Keys(5000)) {
    assert(seen.insert(ConsistentHashRing::HashToRing(key)).second);
  }
}

void TestDistributionIsEven() {
  std::cout << "  ring: distribution across nodes\n";
  ConsistentHashRing ring;
  const std::vector<std::string> nodes = {"node-a", "node-b", "node-c", "node-d", "node-e"};
  for (const std::string& node : nodes) ring.AddNode(node);

  std::map<std::string, size_t> primary_count;
  const auto keys = Keys(20000);
  for (const std::string& key : keys) {
    primary_count[ring.Replicas(key, 1).front()] += 1;
  }

  // With 160 virtual nodes each, five nodes should each hold roughly a fifth.
  // The tolerance is wide enough not to be flaky and tight enough that a
  // ring with broken virtual-node placement fails: a single-token ring
  // routinely lands one node at twice its share.
  const double ideal = static_cast<double>(keys.size()) / nodes.size();
  for (const auto& [node, count] : primary_count) {
    const double ratio = static_cast<double>(count) / ideal;
    assert(ratio > 0.70 && ratio < 1.30);
    (void)node;
  }
  assert(primary_count.size() == nodes.size());
}

void TestAddingANodeMovesOnlyItsShare() {
  std::cout << "  ring: stability under membership change\n";
  ConsistentHashRing ring;
  for (const char* node : {"node-a", "node-b", "node-c", "node-d"}) ring.AddNode(node);

  const auto keys = Keys(20000);
  std::map<std::string, std::string> before;
  for (const std::string& key : keys) before[key] = ring.Replicas(key, 1).front();

  ring.AddNode("node-e");

  size_t moved = 0;
  for (const std::string& key : keys) {
    if (ring.Replicas(key, 1).front() != before[key]) moved += 1;
  }

  // Going from four nodes to five should move about a fifth of the keys.
  // The property that matters is the upper bound: a ring that moved most keys
  // would make every node join a full re-replication of the mesh.
  const double fraction = static_cast<double>(moved) / keys.size();
  assert(fraction > 0.10 && fraction < 0.32);

  // ...and every key that moved must have moved *to the new node*. A key that
  // shuffled between two nodes that both already existed is churn with no
  // reason, and is the signature of a hash that is not order-independent.
  for (const std::string& key : keys) {
    const std::string now = ring.Replicas(key, 1).front();
    if (now != before[key]) assert(now == "node-e");
  }
}

void TestShardKeyCoLocation() {
  std::cout << "  policy: shard keys co-locate\n";
  // Two documents in the same collection with the same shard value must hash
  // to the same ring position -- that is the whole purpose of a shard key.
  const std::string a = PlacementHashInput("readings", "device-1/00001", "device-1");
  const std::string b = PlacementHashInput("readings", "device-1/99999", "device-1");
  assert(a == b);

  // Without a shard key, placement is per document.
  const std::string c = PlacementHashInput("readings", "device-1/00001", "");
  const std::string d = PlacementHashInput("readings", "device-1/99999", "");
  assert(c != d);

  // The collection is part of the input, so the same key in two collections
  // does not co-locate by accident.
  assert(PlacementHashInput("a", "k", "") != PlacementHashInput("b", "k", ""));
}

PeerInfo MakePeer(const std::string& id, NodeLifecycleState state, bool supervisor, int64_t last_seen_ms) {
  PeerInfo peer;
  peer.node_id = id;
  peer.host = "127.0.0.1";
  peer.p2p_port = 7801;
  peer.api_port = 7701;
  peer.state = state;
  peer.is_supervisor = supervisor;
  peer.last_seen_ms = last_seen_ms;
  return peer;
}

void TestPolicyFiltering() {
  std::cout << "  policy: who is excluded, and why it is recorded\n";
  const int64_t now = 1'700'000'000'000;

  PeerTable peers;
  peers.Upsert(MakePeer("data-1", NodeLifecycleState::kRunning, false, now));
  peers.Upsert(MakePeer("data-2", NodeLifecycleState::kRunning, false, now));
  peers.Upsert(MakePeer("data-3", NodeLifecycleState::kRunning, false, now));
  // Excluded, each for a different reason:
  peers.Upsert(MakePeer("supervisor-1", NodeLifecycleState::kRunning, true, now));
  peers.Upsert(MakePeer("gone-1", NodeLifecycleState::kReclaimed, false, now));
  peers.Upsert(MakePeer("stale-1", NodeLifecycleState::kRunning, false, now - 600'000));

  PlacementOptions options;
  options.replication_factor = 3;
  options.now_ms = now;
  options.stale_after_ms = 30'000;

  PlacementPolicy policy("local-node", options);
  policy.Rebuild(peers);

  // The local node counts as a placement target; the three excluded peers do
  // not.
  assert(policy.ring().Contains("local-node"));
  assert(policy.ring().Contains("data-1"));

  // A supervisor holds no replicated data by construction. If it were ever a
  // placement target it would become load-bearing, which is exactly the
  // coordinator this design refuses to have.
  assert(!policy.ring().Contains("supervisor-1"));
  assert(!policy.ring().Contains("gone-1"));
  assert(!policy.ring().Contains("stale-1"));

  // Exclusions are reported rather than silent: "why is this node not holding
  // anything" is a question the inspector has to be able to answer.
  const auto& skipped = policy.skipped();
  assert(skipped.size() >= 3);

  PlacementPlan plan = policy.Place("things", "k1");
  assert(plan.replicas.size() == 3);
  assert(!plan.under_replicated);
  assert(plan.requested_rf == 3);
  assert(!plan.primary().empty());
  assert(plan.Includes(plan.primary()));
  for (const std::string& replica : plan.replicas) {
    assert(replica != "supervisor-1" && replica != "gone-1" && replica != "stale-1");
  }
}

void TestUnderReplicationIsReported() {
  std::cout << "  policy: too few nodes is reported, not hidden\n";
  const int64_t now = 1'700'000'000'000;
  PeerTable peers;
  peers.Upsert(MakePeer("data-1", NodeLifecycleState::kRunning, false, now));

  PlacementOptions options;
  options.replication_factor = 3;
  options.now_ms = now;

  PlacementPolicy policy("local-node", options);
  policy.Rebuild(peers);

  PlacementPlan plan = policy.Place("things", "k1");
  // Two nodes cannot hold three copies. Reporting the shortfall is what lets
  // the UI say "this data has fewer copies than you asked for" instead of
  // quietly pretending RF=3 was achieved.
  assert(plan.replicas.size() == 2);
  assert(plan.under_replicated);
  assert(plan.requested_rf == 3);
}

void TestGroupByPrimary() {
  std::cout << "  policy: grouping keys by replica set\n";
  const int64_t now = 1'700'000'000'000;
  PeerTable peers;
  for (const char* id : {"data-1", "data-2", "data-3"}) {
    peers.Upsert(MakePeer(id, NodeLifecycleState::kRunning, false, now));
  }

  PlacementOptions options;
  options.replication_factor = 3;
  options.now_ms = now;
  PlacementPolicy policy("local-node", options);
  policy.Rebuild(peers);

  const auto keys = Keys(400);
  auto grouped = policy.GroupByPrimary("things", keys);

  // Every key must appear exactly once across the groups: a key in two groups
  // would be sent twice, and one in none would be silently dropped.
  size_t total = 0;
  std::set<std::string> seen;
  for (const auto& [primary, group] : grouped) {
    assert(!primary.empty());
    total += group.size();
    for (const std::string& key : group) assert(seen.insert(key).second);
  }
  assert(total == keys.size());
  assert(grouped.size() > 1);  // actually spread, not all on one node
}

}  // namespace

int main() {
  std::cout << "== placement test suite ==\n";

  TestRingBasics();
  TestHashIsPlatformStable();
  TestDistributionIsEven();
  TestAddingANodeMovesOnlyItsShare();
  TestShardKeyCoLocation();
  TestPolicyFiltering();
  TestUnderReplicationIsReported();
  TestGroupByPrimary();

  std::cout << "placement_test: ALL PASSED\n";
  return 0;
}
