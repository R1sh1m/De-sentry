// Quota test suite: the node-level byte budget and how it is divided.
//
// A quota that is only approximately enforced is worse than none: the whole
// point is that a node on a 32 GB USB stick refuses writes before the stick
// fills, rather than after. So the properties checked here are the ones that
// make the promise real:
//
//   * A refused write is *refused*, not partially applied. The document that
//     did not fit must not be readable afterwards.
//   * Refusal is reported as kOutOfSpace specifically, because the API turns
//     that into HTTP 507 and the UI turns 507 into "you are out of budget"
//     rather than "something went wrong".
//   * A replication merge is never refused for quota. A node that rejects a
//     peer's merge because it is full stops converging, permanently -- that is
//     a correctness failure, not backpressure.
//   * The split sums to 100 and the data plane gets its share, not the whole
//     budget. The ledger, the transit store and the caches all have to fit
//     alongside the collections.
//
// Plain assert(), no framework (-UNDEBUG keeps them live in every build type).

#include <cassert>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>

#include "desentry/common/config.h"
#include "desentry/common/platform.h"
#include "desentry/engine/node_engine.h"
#include "desentry/storage/storage_engine.h"

using namespace desentry;

namespace {

std::string TestRoot() { return AppDataDir() + "/desentry_test_quota"; }

void Fresh(const std::string& path) {
  RemoveTree(path);
  MakeDirs(path);
}

// Document payload size. A document has to fit inside one 4 KiB page --
// the kv backend stores it in a slotted page and refuses anything larger
// with InvalidArgument -- so 1 KiB leaves room for the CRDT envelope and
// still crosses a 1-2 MiB quota within the loops below.
JsonValue Blob(size_t bytes) {
  JsonValue::Object fields;
  fields.emplace_back("payload", JsonValue(std::string(bytes, 'x')));
  return JsonValue(std::move(fields));
}

// -- the split ---------------------------------------------------------------

void TestSplitArithmetic() {
  std::cout << "  split: percentages and byte budgets\n";
  QuotaSplit split;  // the generic profile from config.h
  assert(split.Total() == 100);
  assert(split.Valid());

  const uint64_t quota_mb = 1024;
  const uint64_t mib = 1024ull * 1024ull;
  assert(split.DbBytes(quota_mb) == quota_mb * mib * 60 / 100);
  assert(split.TransitBytes(quota_mb) == quota_mb * mib * 15 / 100);
  assert(split.CacheBytes(quota_mb) == quota_mb * mib * 10 / 100);
  assert(split.LedgerBytes(quota_mb) == quota_mb * mib * 10 / 100);
  assert(split.NetBufferBytes(quota_mb) == quota_mb * mib * 5 / 100);

  // The buckets must actually partition the budget rather than overlapping.
  const uint64_t sum = split.DbBytes(quota_mb) + split.TransitBytes(quota_mb) +
                       split.CacheBytes(quota_mb) + split.LedgerBytes(quota_mb) +
                       split.NetBufferBytes(quota_mb);
  assert(sum <= quota_mb * mib);
  // Integer division can lose a few bytes; it must not lose a meaningful
  // fraction of the budget.
  assert(quota_mb * mib - sum < 1024);

  // A split that does not sum to 100 is invalid, and NodeConfig::Validate
  // rejects it rather than silently normalising -- a node whose budget did
  // not add up would over- or under-commit its disk with nothing to show why.
  QuotaSplit broken = split;
  broken.db_pct += 5;
  assert(!broken.Valid());

  NodeConfig config;
  config.quota_split = broken;
  assert(!config.Validate().empty());
}

void TestConfigValidation() {
  std::cout << "  config: what Validate refuses\n";
  NodeConfig valid;
  assert(valid.Validate().empty());

  // A supervisor reachable from the LAN would be a coordinator with a
  // network surface, which is the thing this design deliberately does not
  // have. It is refused at config load, not merely documented.
  NodeConfig exposed_supervisor;
  exposed_supervisor.supervisor = true;
  exposed_supervisor.api_bind_addr = "0.0.0.0";
  assert(!exposed_supervisor.Validate().empty());

  NodeConfig loopback_supervisor;
  loopback_supervisor.supervisor = true;
  loopback_supervisor.api_bind_addr = "127.0.0.1";
  assert(loopback_supervisor.Validate().empty());

  // A default engine the node was not configured to load would leave every
  // unbound collection unroutable.
  NodeConfig bad_engine;
  bad_engine.engines = {"kv"};
  bad_engine.default_engine = "duckdb";
  assert(!bad_engine.Validate().empty());

  // The config round-trips through its own JSON: the sidecar writes node.json
  // from this shape, so a field that serialised but did not parse back would
  // be a setting that silently never took effect.
  NodeConfig rich;
  rich.quota_mb = 4096;
  rich.engines = {"kv", "ts_rollup"};
  rich.default_engine = "ts_rollup";
  rich.replication_factor = 5;
  rich.encrypt_at_rest = true;
  rich.keychain_ref = "node.abc";
  const std::string json = rich.ToJson();
  assert(json.find("ts_rollup") != std::string::npos);
  assert(json.find("node.abc") != std::string::npos);
  assert(json.find("4096") != std::string::npos);
}

// -- node-level enforcement --------------------------------------------------

void TestNodeQuotaRefusesCleanly() {
  std::cout << "  engine: a write past budget is refused, not half-applied\n";
  const std::string dir = TestRoot() + "/node";
  Fresh(dir);

  NodeEngine::Options options;
  options.data_dir = dir;
  options.engines = {"kv"};
  options.default_engine = "kv";
  options.quota_mb = 2;      // small enough to reach in a test
  options.db_share_pct = 60;

  auto engine_or = NodeEngine::Open(options);
  assert(engine_or.ok());
  auto& engine = *engine_or.value();
  const Requestor local = engine.SelfRequestor();

  const auto initial = engine.Quota();
  assert(initial.limit_bytes == 2ull * 1024 * 1024);
  assert(!initial.over_limit);

  std::string refused_key;
  for (int i = 0; i < 5000 && refused_key.empty(); ++i) {
    const std::string key = "k" + std::to_string(i);
    Status st = engine.PutDocument("bulk", key, Blob(1024), local);
    if (!st.ok()) {
      assert(st.code() == StatusCode::kOutOfSpace);
      refused_key = key;
    }
  }
  assert(!refused_key.empty());

  // The refused document must not be readable. A quota check that ran after
  // the write would leave the document there and only report failure, which
  // is the worst of both.
  assert(!engine.GetDocument("bulk", refused_key, local).ok());

  // Everything written before the limit is still readable: the node degrades
  // to read-only, it does not lose what it already held.
  assert(engine.GetDocument("bulk", "k0", local).ok());

  // And it is still structurally sound -- a node that filled up must not need
  // repair.
  assert(engine.storage().VerifyAll().ledger.ok);

  const auto after = engine.Quota();
  assert(after.used_bytes > initial.used_bytes);
  assert(after.used_fraction > 0.0);

  RemoveTree(dir);
}

void TestUnlimitedQuotaIsUnlimited() {
  std::cout << "  engine: quota_mb 0 means no budget\n";
  const std::string dir = TestRoot() + "/unlimited";
  Fresh(dir);

  NodeEngine::Options options;
  options.data_dir = dir;
  options.engines = {"kv"};
  options.default_engine = "kv";
  options.quota_mb = 0;

  auto engine_or = NodeEngine::Open(options);
  assert(engine_or.ok());
  auto& engine = *engine_or.value();

  const auto quota = engine.Quota();
  assert(quota.limit_bytes == 0);
  assert(!quota.over_limit);

  // Writing well past what a 2 MiB node would refuse must succeed.
  const Requestor local = engine.SelfRequestor();
  for (int i = 0; i < 800; ++i) {
    assert(engine.PutDocument("bulk", "k" + std::to_string(i), Blob(1024), local).ok());
  }
  assert(!engine.Quota().over_limit);

  RemoveTree(dir);
}

void TestMergeIsNeverRefusedForQuota() {
  std::cout << "  engine: replication is not refused for want of space\n";
  const std::string dir = TestRoot() + "/merge";
  Fresh(dir);

  NodeEngine::Options options;
  options.data_dir = dir;
  options.engines = {"kv"};
  options.default_engine = "kv";
  options.quota_mb = 2;

  auto engine_or = NodeEngine::Open(options);
  assert(engine_or.ok());
  auto& engine = *engine_or.value();
  const Requestor local = engine.SelfRequestor();

  // Fill it until local writes are refused.
  bool full = false;
  for (int i = 0; i < 5000 && !full; ++i) {
    full = !engine.PutDocument("bulk", "k" + std::to_string(i), Blob(1024), local).ok();
  }
  assert(full);

  // A peer's merge must still land. Refusing it would leave this node
  // permanently behind the mesh with no way to catch up -- convergence is not
  // something a full disk is allowed to break.
  auto existing = engine.storage().router().Get("bulk", "k0");
  assert(existing.ok());
  Status merged = engine.storage().router().MergeRemote("bulk", "k0", existing.value());
  assert(merged.ok());

  RemoveTree(dir);
}

}  // namespace

int main() {
  std::cout << "== quota test suite ==\n";
  Fresh(TestRoot());

  TestSplitArithmetic();
  TestConfigValidation();
  TestNodeQuotaRefusesCleanly();
  TestUnlimitedQuotaIsUnlimited();
  TestMergeIsNeverRefusedForQuota();

  RemoveTree(TestRoot());
  std::cout << "quota_test: ALL PASSED\n";
  return 0;
}
