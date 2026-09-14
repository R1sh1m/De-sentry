// Liveness test suite: graded suspicion tiers and the heartbeat wire payload.
//
// Workstream D decoupled failure detection into a fast passive-first lane
// (timestamp checks, RPCs only for silent peers) and a slow measured lane.
// This file pins the two pieces that must be exactly right for that to be
// safe:
//
//   * **Suspicion tiers are deterministic and graded.** A fresh peer is
//     healthy, silence past the threshold is suspect, silence past 3x is
//     dead, and a bad probe record upgrades faster than silence alone.
//     Slow condemn (state flips only on dead), fast recovery (one answer
//     clears it) -- the asymmetry a failure detector needs to avoid
//     flapping placements on one dropped packet.
//   * **The heartbeat payload round-trips and degrades honestly.** A
//     truncated payload (fewer fields than the current version writes)
//     still decodes with defaults rather than throwing, so a mixed-version
//     mesh never turns an old peer's heartbeat into an error.
//   * **Quota honesty survives the new report path.** An unlimited node
//     reports free == 0 with quota_limited == false and must not read as
//     full (quota_reported stays false); a limited node reports a real
//     figure. Transit load figures ride along for holder-set sizing.
//
// Plain assert(), no framework (-UNDEBUG keeps them live in every build type).

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

#include "desentry/net/peer.h"
#include "desentry/net/wire_protocol.h"

using namespace desentry;

namespace {

PeerInfo MakePeer(const std::string& id, int64_t last_seen_ms) {
  PeerInfo p;
  p.node_id = id;
  p.host = "127.0.0.1";
  p.p2p_port = 7801;
  p.last_seen_ms = last_seen_ms;
  p.state = NodeLifecycleState::kRunning;
  return p;
}

void TestSuspicionTiers() {
  std::cout << "  suspicion: silence-based tiers\n";
  const int64_t threshold = 5000;
  const int64_t now = 1000000;

  // Never heard from: unknown, not condemned.
  PeerInfo fresh = MakePeer("never-seen", 0);
  assert(fresh.Suspicion(now, threshold) == PeerSuspicion::kSuspect);

  // Freshly seen: healthy.
  PeerInfo healthy = MakePeer("healthy", now - 1000);
  assert(healthy.Suspicion(now, threshold) == PeerSuspicion::kHealthy);

  // Boundary: exactly at the threshold is still healthy (strictly-greater
  // comparisons), just past it is suspect.
  PeerInfo at_edge = MakePeer("edge", now - threshold);
  assert(at_edge.Suspicion(now, threshold) == PeerSuspicion::kHealthy);
  PeerInfo past_edge = MakePeer("past-edge", now - threshold - 1);
  assert(past_edge.Suspicion(now, threshold) == PeerSuspicion::kSuspect);

  // Mid-silence: suspect. Past 3x: dead, with the 3x boundary inclusive of
  // suspect (strictly-greater again).
  PeerInfo mid = MakePeer("mid", now - 2 * threshold);
  assert(mid.Suspicion(now, threshold) == PeerSuspicion::kSuspect);
  PeerInfo at_dead = MakePeer("at-dead", now - 3 * threshold);
  assert(at_dead.Suspicion(now, threshold) == PeerSuspicion::kSuspect);
  PeerInfo dead = MakePeer("dead", now - 3 * threshold - 1);
  assert(dead.Suspicion(now, threshold) == PeerSuspicion::kDead);

  // Non-positive threshold falls back to the 5000ms default rather than
  // dividing by zero or condemning everyone.
  PeerInfo fallback = MakePeer("fallback", now - 4000);
  assert(fallback.Suspicion(now, 0) == PeerSuspicion::kHealthy);
  assert(PeerSuspicionName(PeerSuspicion::kHealthy) == std::string("healthy"));
  assert(PeerSuspicionName(PeerSuspicion::kSuspect) == std::string("suspect"));
  assert(PeerSuspicionName(PeerSuspicion::kDead) == std::string("dead"));
}

void TestSuspicionProbeRecord() {
  std::cout << "  suspicion: probe-record upgrades\n";
  const int64_t threshold = 5000;
  const int64_t now = 1000000;

  // A peer that answers but keeps failing is worse than one that is merely
  // quiet: sustained failure upgrades a fresh peer to suspect...
  PeerInfo flaky = MakePeer("flaky", now - 500);
  flaky.fitness.probes = 4;
  flaky.fitness.success_rate = 0.3;
  assert(flaky.Suspicion(now, threshold) == PeerSuspicion::kSuspect);

  // ...and persistent failure to dead, even when recently heard from.
  PeerInfo failing = MakePeer("failing", now - 500);
  failing.fitness.probes = 10;
  failing.fitness.success_rate = 0.1;
  assert(failing.Suspicion(now, threshold) == PeerSuspicion::kDead);

  // But one bad round does not condemn: below the probe-count floors the
  // silence reading stands.
  PeerInfo one_bad = MakePeer("one-bad", now - 500);
  one_bad.fitness.probes = 2;
  one_bad.fitness.success_rate = 0.0;
  assert(one_bad.Suspicion(now, threshold) == PeerSuspicion::kHealthy);

  // A merely mediocre record (one failure in four) stays healthy.
  PeerInfo mediocre = MakePeer("mediocre", now - 500);
  mediocre.fitness.probes = 4;
  mediocre.fitness.success_rate = 0.75;
  assert(mediocre.Suspicion(now, threshold) == PeerSuspicion::kHealthy);
}

void TestHeartbeatCodec() {
  std::cout << "  heartbeat: payload round-trip\n";
  assert(MessageTypeName(MessageType::kHeartbeat) == std::string("HEARTBEAT"));

  HeartbeatPayload hb;
  hb.node_id = "node-abc";
  hb.ledger_tip_entry_id = 42;
  hb.free_quota_mb = 512;
  hb.quota_limited = true;
  hb.transit_bytes_held = 1024;
  hb.transit_budget_bytes = 65536;
  hb.lifecycle_state = 3;

  HeartbeatPayload back = HeartbeatPayload::Decode(hb.Encode());
  assert(back.node_id == "node-abc");
  assert(back.ledger_tip_entry_id == 42);
  assert(back.free_quota_mb == 512);
  assert(back.quota_limited);
  assert(back.transit_bytes_held == 1024);
  assert(back.transit_budget_bytes == 65536);
  assert(back.lifecycle_state == 3);

  // A first-version payload (only the original three fields) still decodes:
  // newer fields default rather than throw. This is the mixed-version rule.
  std::string prefix;
  {
    // Re-encode by hand with just node_id + tip + free_quota_mb.
    // (ByteWriter layout: Bytes == U32 len + raw.)
    const std::string id = "old-peer";
    uint32_t len = static_cast<uint32_t>(id.size());
    prefix.append(reinterpret_cast<const char*>(&len), 4);
    prefix += id;
    int64_t tip = 7;
    prefix.append(reinterpret_cast<const char*>(&tip), 8);
    uint64_t free_mb = 100;
    prefix.append(reinterpret_cast<const char*>(&free_mb), 8);
  }
  HeartbeatPayload old = HeartbeatPayload::Decode(prefix);
  assert(old.node_id == "old-peer");
  assert(old.ledger_tip_entry_id == 7);
  assert(old.free_quota_mb == 100);
  assert(!old.quota_limited);
  assert(old.transit_bytes_held == 0);
  assert(old.transit_budget_bytes == 0);
}

void TestReportHonesty() {
  std::cout << "  heartbeat: quota honesty in the peer table\n";
  PeerTable table;
  PeerInfo p = MakePeer("node-x", 999000);
  table.Upsert(p);

  // Limited node with a real figure: reported, and stored.
  table.RecordReport("node-x", 50, 256, true, 4096, 1 << 20);
  PeerInfo got;
  assert(table.Get("node-x", &got));
  assert(got.fitness.ledger_freshness_entry_id == 50);
  assert(got.fitness.free_quota_mb == 256);
  assert(got.fitness.quota_reported);
  assert(got.fitness.transit_bytes_held == 4096);
  assert(got.fitness.transit_budget_bytes == (1u << 20));

  // Unlimited node: free == 0 must NOT read as "reported full".
  table.RecordReport("node-x", 51, 0, false, 0, 0);
  assert(table.Get("node-x", &got));
  assert(!got.fitness.quota_reported);
  assert(got.fitness.free_quota_mb == 0);

  // Unknown peer: silently ignored, never created.
  table.RecordReport("ghost", 1, 1, true, 0, 0);
  assert(!table.Contains("ghost"));

  // Transit figures must not leak into the comparability score: two peers
  // differing only in transit load score identically (Score is fitness for
  // replica quality; load is the holder selector's input, not the ranker's).
  PeerInfo a = MakePeer("a", 999000);
  PeerInfo b = MakePeer("b", 999000);
  a.fitness.transit_bytes_held = 0;
  b.fitness.transit_bytes_held = 999999;
  assert(a.fitness.Score(100) == b.fitness.Score(100));
}

}  // namespace

int main() {
  std::cout << "liveness_test\n";
  TestSuspicionTiers();
  TestSuspicionProbeRecord();
  TestHeartbeatCodec();
  TestReportHonesty();
  std::cout << "  all liveness tests passed\n";
  return 0;
}
