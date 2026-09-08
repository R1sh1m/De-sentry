#pragma once
// Replica placement: which nodes should hold a given key
// (docs/architecture-v2.md Sec 3.3).
//
// Consistent hashing with virtual nodes, RF=3 by default. The properties
// that make it the right choice here, rather than a fitness-ranked
// assignment table:
//
//   * **Every node computes the same answer independently.** The ring is a
//     pure function of the membership set and the key. No node has to be
//     asked where a key belongs, which is what keeps the data plane flat --
//     there is no placement authority to elect, and nothing pauses when a
//     particular node is offline. (docs/comparison.md Sec 2 records why a
//     coordinator-based routing design was deliberately not adopted.)
//   * **Membership changes move ~1/N of the keys, not all of them.** Adding
//     a laptop to a five-node home mesh re-homes a fifth of the ring, not
//     the whole dataset.
//   * **It degrades honestly.** With fewer live nodes than RF, Replicas()
//     returns everyone it has and says so via `under_replicated`, rather
//     than silently returning a short list that a caller might read as a
//     full complement.
//
// Fitness (net/peer.h) enters only as a **tie-break and a filter**, never as
// the primary ordering: an unreachable or degraded node is skipped, and
// among otherwise equivalent candidates the fitter one is preferred. Letting
// fitness drive placement outright would make the mapping non-deterministic
// across nodes -- two peers measuring different latencies would disagree
// about where a key lives, which is exactly the failure a consistent hash
// exists to prevent.

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "desentry/net/peer.h"

namespace desentry {

// Virtual nodes per physical node. 160 is the usual figure (Dynamo, Cassandra
// use the same order of magnitude): enough that the key distribution across
// a handful of nodes stays within a few percent of even, cheap enough that a
// 50-node ring is 8000 map entries.
constexpr size_t kVirtualNodesPerPeer = 160;

class ConsistentHashRing {
 public:
  void AddNode(const std::string& node_id, size_t virtual_nodes = kVirtualNodesPerPeer);
  void RemoveNode(const std::string& node_id);
  void Clear();

  // Distinct physical nodes for `key`, walking the ring clockwise from the
  // key's position. Returns at most `count`; fewer if the ring holds fewer
  // distinct nodes.
  std::vector<std::string> Replicas(const std::string& key, size_t count) const;

  size_t NodeCount() const;
  size_t VirtualNodeCount() const;
  bool Contains(const std::string& node_id) const;

  // Exposed so the placement unit test can assert the distribution is
  // actually even rather than trusting that it is.
  static uint64_t HashToRing(const std::string& value);

 private:
  mutable std::mutex mu_;
  std::map<uint64_t, std::string> ring_;  // ring position -> node_id
  std::map<std::string, size_t> nodes_;   // node_id -> its virtual node count
};

struct PlacementPlan {
  std::string key;                       // the value actually hashed
  std::vector<std::string> replicas;     // ordered: primary first
  uint32_t requested_rf = 0;
  bool under_replicated = false;         // fewer live nodes than requested_rf
  std::vector<std::string> skipped;      // candidates filtered out, for the UI to explain
  // Nodes that would be replicas for this key if they were reachable right
  // now. A node that goes away is dropped from the ring, so `replicas` names
  // only the peers that can take the write -- which leaves nobody to notice
  // that an absent owner is owed it. These are the owners the transit store
  // holds bytes for (net/network_manager.cpp, ledger/transit_store.h).
  std::vector<std::string> displaced_owners;

  const std::string& primary() const {
    static const std::string kNone;
    return replicas.empty() ? kNone : replicas.front();
  }
  bool Includes(const std::string& node_id) const;
};

// The value hashed onto the ring for a document. With no shard_key this is
// "collection/key"; with one, it is the document's value for that field, so
// every document sharing a shard key lands on the same replicas (which is
// what makes a per-tenant or per-device collection co-locate).
std::string PlacementHashInput(const std::string& collection, const std::string& key,
                                const std::string& shard_key_value);

// Builds the ring from a peer table plus this node's own id, applying the
// filters described in the header comment.
struct PlacementOptions {
  uint32_t replication_factor = 3;
  // Peers not seen within this window are excluded as placement targets.
  int64_t stale_after_ms = 30000;
  int64_t now_ms = 0;  // 0 == use the wall clock
  bool include_degraded = false;
};

class PlacementPolicy {
 public:
  PlacementPolicy(std::string local_node_id, const PlacementOptions& options)
      : local_node_id_(std::move(local_node_id)), options_(options) {}

  // Rebuilds the ring from the current membership. Cheap enough to call on
  // every placement decision at LAN scale; the app calls it on membership
  // change rather than per write.
  void Rebuild(const PeerTable& peers);

  PlacementPlan Place(const std::string& collection, const std::string& key,
                       const std::string& shard_key_value = std::string()) const;

  // Every key in `keys` grouped by the replica set it lands on -- what the
  // supervisor uses to decide which peer to hand a batch to.
  std::map<std::string, std::vector<std::string>> GroupByPrimary(
      const std::string& collection, const std::vector<std::string>& keys) const;

  const ConsistentHashRing& ring() const { return ring_; }
  const std::vector<std::string>& skipped() const { return skipped_; }

 private:
  std::string local_node_id_;
  PlacementOptions options_;
  ConsistentHashRing ring_;
  // The same ring plus the peers that were only excluded for being absent
  // (stale or degraded). Placement never uses it; it exists to answer "who
  // *should* have had this write", which is the question the transit store
  // is the answer to. Peers excluded for what they are rather than for being
  // away -- supervisors, reclaimed nodes, un-handshaked placeholders -- are
  // not in it: nothing is ever owed to them.
  ConsistentHashRing ring_with_absent_;
  std::vector<std::string> skipped_;
};

}  // namespace desentry
