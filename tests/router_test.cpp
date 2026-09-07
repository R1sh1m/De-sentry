// Storage router test suite.
//
// Runs the same six-part contract against every from-scratch backend --
// put/get, ordered scan, CRDT merge, quota rejection, checksum parity and
// self-verification -- plus the router's own routing, binding and
// cross-engine index behaviour.
//
// The point of running one identical suite over all five engines is that the
// router's core promise is that nothing above it can tell which backend it
// got. A per-engine test written to each engine's strengths would pass while
// that promise quietly broke.
//
// Plain assert(), no framework, so the tree builds and tests offline
// (CMakeLists.txt compiles test binaries with -UNDEBUG so these stay live in
// every build type).

#include <cassert>
#include <cstdio>
#include <iostream>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "desentry/common/platform.h"
#include "desentry/crdt/document.h"
#include "desentry/crdt/hlc.h"
#include "desentry/storage/catalog.h"
#include "desentry/storage/document_codec.h"
#include "desentry/storage/engines/columnar_lite.h"
#include "desentry/storage/engines/graph_adj.h"
#include "desentry/storage/engines/ts_rollup.h"
#include "desentry/storage/engines/vector_hnsw_lite.h"
#include "desentry/storage/router.h"

using namespace desentry;

namespace {

std::string TestRoot() { return AppDataDir() + "/desentry_test_router"; }

void Fresh(const std::string& path) {
  RemoveTree(path);
  MakeDirs(path);
}

HybridLogicalClock g_clock("router-test");

// A document stamped at the given clock reading. Built through JsonValue
// because that is the only way a document is ever created in the engine --
// a test that assembled CRDT nodes directly would be testing a path nothing
// else uses.
std::string MakeDoc(const JsonValue::Object& fields, HLCTimestamp hlc) {
  return EncodeDocument(CrdtValue::FromJson(JsonValue(fields), hlc));
}

std::string MakeDoc(const std::string& field, const std::string& value, HLCTimestamp hlc) {
  JsonValue::Object fields;
  fields.emplace_back(field, JsonValue(value));
  return MakeDoc(fields, hlc);
}

std::string ReadField(const std::string& encoded, const std::string& field) {
  const JsonValue json = DecodeDocument(encoded).ToJson();
  const JsonValue* value = json.Find(field);
  return value == nullptr ? std::string() : value->AsString();
}

// -- the contract every backend must satisfy --------------------------------

void RunBackendContract(const std::string& engine_name) {
  std::cout << "  backend: " << engine_name << "\n";
  const std::string dir = TestRoot() + "/be_" + engine_name;
  Fresh(dir);

  auto made = MakeBackend(engine_name);
  assert(made.ok());
  auto backend = std::move(made.value());
  assert(backend->Name() == engine_name);
  assert(backend->Open(dir, 0).ok());

  // -- put / get ------------------------------------------------------------
  const std::string doc_a = MakeDoc("name", "alpha", g_clock.Now());
  assert(backend->Put("things", "k1", doc_a).ok());
  auto got = backend->Get("things", "k1");
  assert(got.ok());
  assert(ReadField(got.value(), "name") == "alpha");

  // A key that was never written is NotFound, not an empty document -- the
  // difference matters to every caller that distinguishes "absent" from
  // "present but empty".
  assert(!backend->Get("things", "missing").ok());

  // -- ordered scan ---------------------------------------------------------
  for (const char* key : {"k2", "k4", "k3", "k5"}) {
    assert(backend->Put("things", key, MakeDoc("name", key, g_clock.Now())).ok());
  }
  auto rows = backend->Scan("things", "", 0);
  assert(rows.size() == 5);
  for (size_t i = 1; i < rows.size(); ++i) {
    assert(rows[i - 1].first < rows[i].first);  // ordered, not merely complete
  }

  // Scan from a start key is inclusive and respects the limit.
  auto page = backend->Scan("things", "k3", 2);
  assert(page.size() == 2);
  assert(page[0].first == "k3");
  assert(page[1].first == "k4");

  // -- CRDT merge -----------------------------------------------------------
  // A remote write with a later HLC wins; an earlier one does not. This is
  // the property that lets two peers with different engine bindings converge.
  const std::string later = MakeDoc("name", "from-peer", g_clock.Now());
  assert(backend->MergeRemote("things", "k1", later).ok());
  assert(ReadField(backend->Get("things", "k1").value(), "name") == "from-peer");

  HLCTimestamp stale;
  stale.physical_ms = 1;
  assert(backend->MergeRemote("things", "k1", MakeDoc("name", "ancient", stale)).ok());
  assert(ReadField(backend->Get("things", "k1").value(), "name") == "from-peer");

  // Merging is commutative over the same pair of writes, which is what makes
  // gossip order irrelevant.
  assert(backend->Put("things", "k9", later).ok());
  assert(backend->MergeRemote("things", "k9", doc_a).ok());
  assert(ReadField(backend->Get("things", "k9").value(), "name") == "from-peer");

  // -- checksum -------------------------------------------------------------
  const std::string checksum = backend->Checksum("things");
  assert(!checksum.empty());
  assert(backend->Checksum("things") == checksum);        // stable
  assert(backend->Checksum("nothing-here") != checksum);  // discriminating

  // -- verify + flush -------------------------------------------------------
  assert(backend->Flush().ok());
  assert(backend->Verify().ok());

  auto collections = backend->ListCollections();
  assert(std::find(collections.begin(), collections.end(), std::string("things")) != collections.end());

  RemoveTree(dir);
}

// Quota is tested separately: it needs a budget small enough to hit, and the
// contract test above deliberately runs unlimited so a quota bug cannot make
// the other assertions fail for the wrong reason.
void RunQuotaContract(const std::string& engine_name) {
  const std::string dir = TestRoot() + "/quota_" + engine_name;
  Fresh(dir);

  auto made = MakeBackend(engine_name);
  assert(made.ok());
  auto backend = std::move(made.value());
  // One MiB: small enough that a few hundred KiB of documents exceeds it,
  // large enough that a backend's own metadata pages fit.
  assert(backend->Open(dir, 1).ok());
  assert(backend->QuotaLimit() == 1ull * 1024 * 1024);

  const std::string payload(4096, 'x');
  bool refused = false;
  for (int i = 0; i < 4000 && !refused; ++i) {
    Status st = backend->Put("bulk", "key" + std::to_string(i),
                              MakeDoc("blob", payload, g_clock.Now()));
    if (!st.ok()) {
      // Refusal must be specifically about space, and must be clean: the
      // backend has to remain usable and self-consistent afterwards.
      assert(st.code() == StatusCode::kOutOfSpace);
      refused = true;
    }
  }
  assert(refused);
  assert(backend->Verify().ok());
  assert(backend->QuotaUse() > 0);

  // A merge is never refused for quota. Refusing a replication merge would
  // make the node permanently divergent, which is a correctness failure --
  // engine_common.h grants merges a small overdraft for exactly this reason.
  Status merge = backend->MergeRemote("bulk", "key0", MakeDoc("blob", payload, g_clock.Now()));
  assert(merge.ok());

  RemoveTree(dir);
}

// -- the router itself ------------------------------------------------------

void TestRoutingAndBinding() {
  std::cout << "  router: binding and routing\n";
  const std::string dir = TestRoot() + "/router";
  Fresh(dir);

  auto catalog_or = Catalog::Open(dir + "/catalog.json");
  assert(catalog_or.ok());
  auto& catalog = *catalog_or.value();

  StorageRouter::Options options;
  options.data_dir = dir;
  options.quota_mb = 64;
  options.db_share_pct = 60;
  options.engines = {"kv", "columnar_lite", "ts_rollup", "graph_adj", "vector_hnsw_lite"};
  options.default_engine = "kv";
  options.catalog = &catalog;
  options.node_id = "router-test-node";

  auto router_or = StorageRouter::Open(options);
  assert(router_or.ok());
  auto& router = *router_or.value();

  // An unbound collection lands on the node's default engine.
  assert(router.EngineNameFor("unbound") == "kv");

  // Binding moves it, and the binding survives a lookup by name.
  assert(router.BindCollection("events", "columnar_lite").ok());
  assert(router.EngineNameFor("events") == "columnar_lite");
  assert(router.Backend("columnar_lite") != nullptr);

  // An engine that is not in this node's configured list is refused rather
  // than silently falling back -- a collection quietly stored somewhere other
  // than where it was asked for is the worst possible outcome here.
  assert(!router.BindCollection("events2", "duckdb").ok());

  // Writes route to the bound backend and are readable through the router.
  const std::string doc = MakeDoc("kind", "click", g_clock.Now());
  assert(router.Put("events", "e1", doc).ok());
  auto read = router.Get("events", "e1");
  assert(read.ok());
  assert(ReadField(read.value(), "kind") == "click");
  // ...and are physically in the bound backend, not in the default one.
  assert(router.Backend("columnar_lite")->Get("events", "e1").ok());
  assert(!router.Backend("kv")->Get("events", "e1").ok());

  // Rebinding a populated collection is refused: the rows are already laid
  // out for the old engine, and a rebind that silently stranded them would
  // look like data loss.
  assert(!router.BindCollection("events", "kv").ok());

  // The router's checksum for a collection is the bound backend's checksum.
  assert(router.Checksum("events") == router.Backend("columnar_lite")->Checksum("events"));

  assert(router.Verify().ok());
  assert(router.Flush().ok());

  // The node's data-plane budget is its quota times db_share_pct, not the
  // whole quota: the ledger, transit store and caches have their own shares.
  assert(router.QuotaLimit() == 64ull * 1024 * 1024 * 60 / 100);

  RemoveTree(dir);
}

void TestBudgetIsSharedBetweenEngines() {
  std::cout << "  router: engines share one budget\n";
  const std::string dir = TestRoot() + "/router_budget";
  Fresh(dir);

  auto catalog_or = Catalog::Open(dir + "/catalog.json");
  assert(catalog_or.ok());
  auto& catalog = *catalog_or.value();

  StorageRouter::Options options;
  options.data_dir = dir;
  options.quota_mb = 100;
  options.db_share_pct = 50;
  options.engines = {"kv", "columnar_lite"};
  options.default_engine = "kv";
  options.catalog = &catalog;

  auto router_or = StorageRouter::Open(options);
  assert(router_or.ok());
  auto& router = *router_or.value();

  // Two engines must not each be handed the whole data-plane budget -- that
  // would let a node with N engines use N times its quota.
  const uint64_t plane = 100ull * 1024 * 1024 * 50 / 100;
  uint64_t sum = 0;
  for (const std::string& name : router.AvailableEngines()) {
    sum += router.Backend(name)->QuotaLimit();
  }
  assert(sum <= plane);

  RemoveTree(dir);
}

void TestCrossEngineIndex() {
  std::cout << "  router: cross-engine index\n";
  const std::string dir = TestRoot() + "/index";
  Fresh(dir);
  const std::string path = dir + "/cross_engine.log";

  {
    auto index_or = CrossEngineIndex::Open(path);
    assert(index_or.ok());
    auto& index = *index_or.value();

    assert(index.Upsert({"k1", "events", "columnar_lite", "node-a", 100}).ok());
    assert(index.Upsert({"k2", "readings", "ts_rollup", "node-a", 101}).ok());
    assert(index.Upsert({"k1", "events", "kv", "node-b", 102}).ok());  // supersedes

    auto found = index.Lookup("k1");
    assert(found.ok());
    assert(found.value().engine == "kv");
    assert(found.value().node_id == "node-b");
    assert(!index.Lookup("nope").ok());
    assert(index.ByEngine("ts_rollup").size() == 1);
    assert(index.ByCollection("events").size() == 1);
    assert(index.Flush().ok());
  }

  {
    // The index survives a reopen -- it is an fsync'd append log, and an
    // index that forgot its contents on restart would send every lookup to
    // the wrong node until it was rebuilt.
    auto index_or = CrossEngineIndex::Open(path);
    assert(index_or.ok());
    auto& index = *index_or.value();
    assert(index.Size() == 2);
    assert(index.Lookup("k1").value().engine == "kv");

    // Compaction drops superseded records without changing what is visible.
    assert(index.Compact().ok());
    assert(index.Size() == 2);
    assert(index.Lookup("k1").value().engine == "kv");
  }

  RemoveTree(dir);
}

// -- engine-specific behaviour ----------------------------------------------
// The contract above proves the engines are interchangeable. These prove each
// one actually does the thing it exists for -- otherwise five identical
// key-value stores would pass every test in this file.

void TestColumnarEncoding() {
  std::cout << "  columnar_lite: segment encoding\n";
  std::vector<EngineRow> rows;
  // Shared prefixes and repeated bytes: exactly what front coding and RLE are
  // for, and what a naive encoder would store at full size.
  for (int i = 0; i < 200; ++i) {
    char key[32];
    std::snprintf(key, sizeof(key), "sensor/room-a/reading-%04d", i);
    rows.emplace_back(key, std::string(64, 'v'));
  }

  const std::string segment = ColumnarLiteBackend::EncodeSegment(rows);
  size_t raw = 0;
  for (const auto& row : rows) raw += row.first.size() + row.second.size();
  assert(segment.size() < raw);  // it actually compresses

  auto decoded_or = ColumnarLiteBackend::DecodeSegment(segment);
  assert(decoded_or.ok());
  const auto& decoded = decoded_or.value();
  assert(decoded.size() == rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    assert(decoded[i].first == rows[i].first);
    assert(decoded[i].second == rows[i].second);
  }

  // Keys can be read without materialising the payloads, which is what makes
  // a key-only scan cheap on this engine.
  auto keys_or = ColumnarLiteBackend::DecodeSegmentKeys(segment);
  assert(keys_or.ok());
  const auto& keys = keys_or.value();
  assert(keys.size() == rows.size());
  assert(keys.front() == rows.front().first);
  assert(keys.back() == rows.back().first);

  // An empty segment round-trips rather than crashing -- collections are
  // created before they are written to.
  auto empty_or = ColumnarLiteBackend::DecodeSegment(ColumnarLiteBackend::EncodeSegment({}));
  assert(empty_or.ok());
  assert(empty_or.value().empty());
}

void TestTimeSeriesRollups() {
  std::cout << "  ts_rollup: range query and rollups\n";
  const std::string dir = TestRoot() + "/ts";
  Fresh(dir);

  TsRollupBackend backend;
  assert(backend.Open(dir, 0).ok());

  // Two hours of one-minute samples across two series.
  const int64_t base = 1'700'000'000'000;
  for (int i = 0; i < 120; ++i) {
    for (const char* series : {"temp", "humidity"}) {
      JsonValue::Object point;
      point.emplace_back("series", JsonValue(series));
      point.emplace_back("timestamp_ms", JsonValue(static_cast<int64_t>(base + i * 60'000)));
      point.emplace_back("value", JsonValue(static_cast<double>(i)));
      char key[64];
      std::snprintf(key, sizeof(key), "%s/%lld", series, static_cast<long long>(base + i * 60'000));
      assert(backend.Put("readings", key, MakeDoc(point, g_clock.Now())).ok());
    }
  }

  auto rollups = backend.Rollups("readings", "temp", base, base + 120 * 60'000, 60 * 60'000);
  assert(!rollups.empty());
  const auto& buckets = rollups.begin()->second;
  assert(!buckets.empty());
  // Each hourly bucket must aggregate its minute samples, not merely list them.
  for (const TsRollupBucket& bucket : buckets) {
    assert(bucket.count > 0);
    assert(bucket.min <= bucket.max);
    assert(bucket.sum >= bucket.min);
  }

  // Retention drops old chunks. Pruning to a point after every sample must
  // leave the collection empty but still readable and verifiable.
  auto pruned = backend.Prune("readings", base + 200 * 60'000);
  assert(pruned.ok());
  assert(backend.Verify().ok());

  RemoveTree(dir);
}

void TestVectorSearch() {
  std::cout << "  vector_hnsw_lite: nearest neighbours\n";
  const std::string dir = TestRoot() + "/vec";
  Fresh(dir);

  VectorHnswLiteBackend backend;
  assert(backend.Open(dir, 0).ok());

  // Ten orthogonal-ish vectors in a small space: each one's nearest
  // neighbour must be itself, which is the weakest useful correctness claim
  // an ANN index can make and the one a broken index fails.
  constexpr int kDim = 16;
  constexpr int kCount = 40;
  std::vector<std::vector<float>> vectors;
  for (int i = 0; i < kCount; ++i) {
    std::vector<float> v(kDim, 0.0f);
    v[i % kDim] = 1.0f;
    v[(i * 7) % kDim] += 0.5f;
    vectors.push_back(v);

    JsonValue::Array components;
    for (float component : v) components.emplace_back(static_cast<double>(component));
    JsonValue::Object doc;
    doc.emplace_back("vector", JsonValue(std::move(components)));
    assert(backend.Put("embeddings", "v" + std::to_string(i), MakeDoc(doc, g_clock.Now())).ok());
  }

  for (int i = 0; i < kCount; i += 7) {
    auto hits = backend.Search("embeddings", vectors[i], 3);
    assert(hits.ok());
    assert(!hits.value().empty());
    assert(hits.value().front().key == "v" + std::to_string(i));
    // Scores must be ordered best-first; an unordered result set makes
    // "top k" meaningless.
    for (size_t j = 1; j < hits.value().size(); ++j) {
      assert(hits.value()[j - 1].score >= hits.value()[j].score);
    }
  }

  assert(backend.Verify().ok());
  RemoveTree(dir);
}

void TestGraphEdges() {
  std::cout << "  graph_adj: edges and traversal\n";
  const std::string dir = TestRoot() + "/graph";
  Fresh(dir);

  GraphAdjBackend backend;
  assert(backend.Open(dir, 0).ok());

  auto put_child = [&](const std::string& key, const std::string& parent) {
    JsonValue::Object doc;
    doc.emplace_back("class", JsonValue("Node"));
    if (!parent.empty()) doc.emplace_back("parent", JsonValue(parent));
    assert(backend.Put("tree", key, MakeDoc(doc, g_clock.Now())).ok());
  };

  put_child("root", "");
  put_child("a", "root");
  put_child("b", "root");
  put_child("a1", "a");

  assert(backend.OutEdges("tree", "root").size() == 2);
  assert(backend.InEdges("tree", "a").size() == 1);
  assert(backend.Descendants("tree", "root", 3).size() == 3);
  assert(backend.Ancestors("tree", "a1").size() == 2);

  auto roots = backend.Roots("tree");
  assert(roots.size() == 1 && roots[0] == "root");

  // Rewriting one document must not disturb edges another document declared.
  // This is the bug the `owner` field on GraphEdge exists to prevent: without
  // it, re-writing "a" clobbered the edge "root -> b".
  put_child("a", "root");
  assert(backend.OutEdges("tree", "root").size() == 2);

  // Re-parenting removes the old edge and adds the new one -- not both.
  put_child("a1", "b");
  assert(backend.InEdges("tree", "a1").size() == 1);
  assert(backend.Descendants("tree", "a", 3).empty());

  assert(backend.Verify().ok());
  RemoveTree(dir);
}

}  // namespace

int main() {
  std::cout << "== router test suite ==\n";
  Fresh(TestRoot());

  std::cout << "-- backend contract --\n";
  for (const char* engine : {"kv", "columnar_lite", "ts_rollup", "vector_hnsw_lite", "graph_adj"}) {
    RunBackendContract(engine);
  }

  std::cout << "-- quota contract --\n";
  for (const char* engine : {"kv", "columnar_lite", "graph_adj"}) {
    std::cout << "  backend: " << engine << "\n";
    RunQuotaContract(engine);
  }

  std::cout << "-- router --\n";
  TestRoutingAndBinding();
  TestBudgetIsSharedBetweenEngines();
  TestCrossEngineIndex();

  std::cout << "-- engine specifics --\n";
  TestColumnarEncoding();
  TestTimeSeriesRollups();
  TestVectorSearch();
  TestGraphEdges();

  RemoveTree(TestRoot());
  std::cout << "router_test: ALL PASSED\n";
  return 0;
}
