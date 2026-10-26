// ChangeFeed test suite: basic sanity checks for Wait() behavior.
//
// This test validates the core semantics of ChangeFeed without relying on
// internal Snapshot() implementation details. It covers:
//   * Wait() returns an empty batch on timeout when no entries exist.
//   * Wait() returns existing entries immediately.
//   * Wait() returns entries after they are appended (Publish is implicit).
//   * Wait() returns an error after Stop() is called.
//   * Wait() respects the limit parameter and reports truncation when the
//     cursor is beyond the current tip.
//
// Plain assert(), no test framework (compiled with -UNDEBUG).

#include <cassert>
#include <iostream>
#include <memory>
#include <thread>
#include <chrono>

#include "desentry/ledger/change_feed.h"
#include "desentry/common/platform.h"
#include "desentry/storage/wal.h"
#include "desentry/crdt/hlc.h"

using namespace desentry;

namespace {

// Global clock for deterministic timestamps.
HybridLogicalClock g_clock("ledger-feed-test");

std::string TestRoot() { return AppDataDir() + "/desentry_test_ledger_feed"; }

void Fresh(const std::string& path) {
  RemoveTree(path);
  MakeDirs(path);
}

// Open a WAL file inside the given directory.
std::unique_ptr<WriteAheadLog> OpenWal(const std::string& dir) {
  Fresh(dir);
  std::string wal_path = dir + "/ledger.wal";
  auto wal_or = WriteAheadLog::Open(wal_path);
  assert(wal_or.ok());
  return std::move(wal_or.value());
}

void Append(WriteAheadLog* wal, const std::string& payload) {
  WriteAheadLog::AppendOptions opts;
  opts.hlc = g_clock.Now();
  auto st = wal->Append(WalRecordType::kPut, "test", "k", payload, opts);
  assert(st.ok());
}

// Test 1: Wait returns empty batch on timeout with no records.
void TestWaitNoEntries() {
  std::cout << "  feed: Wait() empty on timeout\n";
  auto wal = OpenWal(TestRoot() + "/TestWaitNoEntries");
  ChangeFeed feed(wal.get());
  auto res = feed.Wait(kInvalidLsn, 100, 0);
  assert(res.ok());
  const auto& batch = res.value();
  assert(batch.entries.empty());
  assert(!batch.truncated);
}

// Test 2: Wait returns existing entries immediately.
void TestWaitImmediate() {
  std::cout << "  feed: Wait() immediate entries\n";
  auto wal = OpenWal(TestRoot() + "/TestWaitImmediate");
  Append(wal.get(), "data1");
  ChangeFeed feed(wal.get());
  auto res = feed.Wait(kInvalidLsn, 0, 0);
  assert(res.ok());
  const auto& batch = res.value();
  assert(!batch.entries.empty());
}

// Test 3: Wait sees entries appended after the call starts.
void TestWaitAfterAppend() {
  std::cout << "  feed: Wait() after async append\n";
  auto wal = OpenWal(TestRoot() + "/TestWaitAfterAppend");
  ChangeFeed feed(wal.get());
  std::thread t([wal = wal.get()]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Append(wal, "later");
  });
  auto res = feed.Wait(kInvalidLsn, 500, 0);
  assert(res.ok());
  const auto& batch = res.value();
  assert(!batch.entries.empty());
  t.join();
}

// Test 4: Wait returns error after Stop().
void TestWaitAfterStop() {
  std::cout << "  feed: Wait() error after Stop()\n";
  auto wal = OpenWal(TestRoot() + "/TestWaitAfterStop");
  ChangeFeed feed(wal.get());
  feed.Stop();
  auto res = feed.Wait(kInvalidLsn, 100, 0);
  assert(!res.ok());
}

// Test 5: Limit and truncation behavior.
void TestLimitAndTruncate() {
  std::cout << "  feed: limit and truncation\n";
  auto wal = OpenWal(TestRoot() + "/TestLimitTruncate");
  Append(wal.get(), "x");
  Append(wal.get(), "y");
  Append(wal.get(), "z");
  ChangeFeed feed(wal.get());
  // Limit to 2 entries.
  auto res = feed.Wait(kInvalidLsn, 0, 2);
  assert(res.ok());
  const auto& batch = res.value();
  assert(batch.entries.size() == 2);
  // Cursor beyond tip should yield truncated flag.
  lsn_t beyond = batch.tip + 10;
  auto res2 = feed.Wait(beyond, 0, 0);
  assert(res2.ok());
  const auto& batch2 = res2.value();
  assert(batch2.entries.empty());
  assert(batch2.truncated);
}

} // namespace

int main() {
  std::cout << "== ChangeFeed test suite ==\n";
  Fresh(TestRoot());
  TestWaitNoEntries();
  TestWaitImmediate();
  TestWaitAfterAppend();
  TestWaitAfterStop();
  TestLimitAndTruncate();
  std::cout << "All ChangeFeed tests passed\n";
  return 0;
}
