// CRDT correctness test suite: idempotence, commutativity, associativity
// (the formal join-semilattice properties ARCHITECTURE.md §6.4 claims),
// the delete-vs-concurrent-update race at field granularity, OR-Set
// concurrent add-survives-unseen-remove, and the binary codec round-trip.

#include <cassert>
#include <iostream>

#include "desentry/common/json.h"
#include "desentry/crdt/document.h"
#include "desentry/crdt/hlc.h"

using namespace desentry;

static void TestIdempotence() {
  HybridLogicalClock clock("nodeA");
  auto j = JsonValue::Parse(R"({"name":"Asha","age":21,"tags":["admin","staff"]})");
  auto doc = CrdtValue::FromJson(j, clock.Now());
  auto merged = CrdtValue::Merge(doc, doc);
  assert(merged.ToJson().Dump() == doc.ToJson().Dump());
  std::cout << "[crdt_test] idempotence: PASS" << std::endl;
}

static void TestCommutativityAndAssociativity() {
  HybridLogicalClock clockA("nodeA"), clockB("nodeB");
  auto base = CrdtValue::FromJson(JsonValue::Parse(R"({"name":"Asha","age":21,"tags":["admin","staff"]})"), clockA.Now());

  auto docA = CrdtValue::ApplyJsonUpdate(base, JsonValue::Parse(R"({"name":"Asha","age":22,"tags":["admin","staff"]})"), clockA.Now());
  auto docB = CrdtValue::ApplyJsonUpdate(base, JsonValue::Parse(R"({"name":"Asha Khan","age":21,"tags":["admin","staff"]})"), clockB.Now());
  auto docC = CrdtValue::ApplyJsonUpdate(base, JsonValue::Parse(R"({"name":"Asha","age":21,"tags":["admin","staff","vip"]})"), clockA.Now());

  auto ab = CrdtValue::Merge(docA, docB);
  auto ba = CrdtValue::Merge(docB, docA);
  assert(ab.ToJson().CanonicalDump() == ba.ToJson().CanonicalDump());

  auto left = CrdtValue::Merge(CrdtValue::Merge(docA, docB), docC);
  auto right = CrdtValue::Merge(docA, CrdtValue::Merge(docB, docC));
  assert(left.ToJson().CanonicalDump() == right.ToJson().CanonicalDump());

  std::cout << "[crdt_test] commutativity + associativity: PASS (converged to " << ab.ToJson().Dump() << ")" << std::endl;
}

static void TestDeleteVsUpdateRace() {
  HybridLogicalClock clockA("nodeA");
  auto base = CrdtValue::FromJson(JsonValue::Parse(R"({"name":"Asha","age":21})"), clockA.Now());

  auto ts_delete = clockA.Now();
  auto deleted = CrdtValue::ApplyJsonUpdate(base, JsonValue::MakeObject(), ts_delete);
  assert(deleted.IsEmpty());

  // Timestamps are constructed explicitly (rather than via two independent
  // clocks' Now() calls) to make the ordering deterministic for the test.
  // This matters for a subtle, correct reason: two *independent* HLCs that
  // have never observed each other's messages (no Observe() call) offer no
  // guarantee that "called later in wall-clock time" implies "compares
  // greater" -- their logical counters race independently. In the real
  // running system this isn't an issue: MergeRemote() always calls
  // clock_->Observe() on every remote timestamp it sees (node_engine.cpp),
  // so a node's clock is causally advanced past anything it has received
  // before it produces its next Now(). A unit test asserting a specific
  // before/after outcome should pin that down explicitly rather than rely
  // on timing, exactly as production distributed-systems tests do.

  // Update strictly older than the delete: delete must win.
  HLCTimestamp older{ts_delete.physical_ms > 1000 ? ts_delete.physical_ms - 1000 : 0, 0, "nodeB"};
  auto stale_update = CrdtValue::ApplyJsonUpdate(base, JsonValue::Parse(R"({"name":"Asha","age":99})"), older);
  auto merged1 = CrdtValue::Merge(deleted, stale_update);
  assert(merged1.IsEmpty());

  // Update strictly newer than the delete (fresh field touch): field must resurrect.
  HLCTimestamp newer{ts_delete.physical_ms + 1000, 0, "nodeB"};
  auto fresh_update = CrdtValue::ApplyJsonUpdate(base, JsonValue::Parse(R"({"name":"Asha","age":100})"), newer);
  auto merged2 = CrdtValue::Merge(deleted, fresh_update);
  assert(!merged2.IsEmpty());
  assert(merged2.ToJson().Get("age").AsInt() == 100);

  std::cout << "[crdt_test] delete-vs-concurrent-update field-level race: PASS" << std::endl;
}

static void TestOrSetConcurrentAddSurvivesRemove() {
  HybridLogicalClock clockA("nodeA"), clockB("nodeB");
  auto base = CrdtValue::FromJson(JsonValue::Parse(R"({"tags":["x"]})"), clockA.Now());

  auto a_removed = CrdtValue::ApplyJsonUpdate(base, JsonValue::Parse(R"({"tags":[]})"), clockA.Now());
  auto b_added = CrdtValue::ApplyJsonUpdate(base, JsonValue::Parse(R"({"tags":["x","y"]})"), clockB.Now());
  auto merged = CrdtValue::Merge(a_removed, b_added);

  auto tags = merged.ToJson().Get("tags").AsArray();
  bool has_x = false, has_y = false;
  for (auto& t : tags) { if (t.AsString() == "x") has_x = true; if (t.AsString() == "y") has_y = true; }
  assert(!has_x && has_y);

  std::cout << "[crdt_test] OR-Set concurrent add survives unseen remove: PASS" << std::endl;
}

static void TestBinaryCodecRoundTrip() {
  HybridLogicalClock clock("nodeX");
  auto j = JsonValue::Parse(R"({"name":"Asha","age":21,"tags":["a","b","c"],"nested":{"x":1,"y":[true,false,null,3.14]}})");
  auto doc = CrdtValue::FromJson(j, clock.Now());
  auto doc2 = CrdtValue::ApplyJsonUpdate(doc, JsonValue::Parse(R"({"name":"Asha","age":22,"tags":["a","c"],"nested":{"x":1,"y":[true,false,null,3.14]}})"), clock.Now());

  std::string bytes = doc2.Encode();
  auto decoded = CrdtValue::Decode(bytes);
  assert(decoded.ToJson().Dump() == doc2.ToJson().Dump());

  auto merged = CrdtValue::Merge(doc2, decoded);
  assert(merged.ToJson().Dump() == doc2.ToJson().Dump());

  std::cout << "[crdt_test] binary codec round-trip + merge (" << bytes.size() << " bytes): PASS" << std::endl;
}

int main() {
  TestIdempotence();
  TestCommutativityAndAssociativity();
  TestDeleteVsUpdateRace();
  TestOrSetConcurrentAddSurvivesRemove();
  TestBinaryCodecRoundTrip();
  std::cout << "[crdt_test] ALL CRDT TESTS PASSED" << std::endl;
  return 0;
}
