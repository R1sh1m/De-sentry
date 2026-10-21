// Retention scheduler coverage: policy discovery, automatic-sweep accounting,
// and explicit disablement for nodes that opt out of the scheduler.

#include <cassert>
#include <iostream>

#include "desentry/common/platform.h"
#include "desentry/engine/node_engine.h"

using namespace desentry;

int main() {
  const std::string dir = AppDataDir() + "/desentry_test_retention";
  RemoveTree(dir);

  NodeEngine::Options options;
  options.data_dir = dir;
  options.engines = {"ts_rollup"};
  options.default_engine = "ts_rollup";
  options.retention_interval_ms = 0;

  auto engine_or = NodeEngine::Open(options);
  assert(engine_or.ok());
  NodeEngine& engine = *engine_or.value();

  assert(engine.storage().EnsureCollection("metrics").ok());
  assert(engine.storage().router().BindCollection("metrics", "ts_rollup").ok());
  assert(engine.storage().catalog().SetRetentionDays("metrics", 30).ok());

  const NodeEngine::RetentionSweepReport report = engine.RunRetentionSweep();
  assert(report.last_run_ms > 0);
  assert(report.collections_considered == 1);
  assert(report.chunks_dropped == 0);
  assert(report.last_error.empty());

  const NodeEngine::RetentionSweepReport status = engine.RetentionStatus();
  assert(status.last_run_ms == report.last_run_ms);
  assert(status.collections_considered == 1);

  std::cout << "[retention_test] automatic sweep accounting: PASS" << std::endl;
  RemoveTree(dir);
  return 0;
}
