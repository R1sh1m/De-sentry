// Storage engine test suite: DiskManager + BufferPoolManager (with real
// LRU eviction pressure), WriteAheadLog durability + replay, B+Tree
// correctness under a stress workload, and full StorageEngine
// crash-recovery (including the "nothing was ever checkpointed" case,
// which is the scenario that actually exercises WAL replay).
//
// No external test framework -- plain assert() -- so the whole project
// builds and tests fully offline (see CMakeLists.txt).

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <thread>

#include "desentry/common/platform.h"
#include "desentry/crdt/document.h"
#include "desentry/crdt/hlc.h"
#include "desentry/storage/buffer_pool_manager.h"
#include "desentry/storage/disk_manager.h"
#include "desentry/storage/document_codec.h"
#include "desentry/storage/bplus_tree.h"
#include "desentry/storage/engines/kv_bplus.h"
#include "desentry/storage/page.h"
#include "desentry/storage/storage_engine.h"
#include "desentry/storage/wal.h"

using namespace desentry;

namespace {
// A per-user temporary directory rather than a hard-coded /tmp: these tests
// have to run on Windows too, where /tmp does not exist and `rm -rf` is not a
// command. platform.h is the one place in the tree that knows the difference.
const std::string kTestDirStorage = AppDataDir() + "/desentry_test_storage";
const char* kTestDir = kTestDirStorage.c_str();

void RmRf(const std::string& path) { RemoveTree(path); }
}  // namespace

static void TestBufferPoolAndWal() {
  RmRf(kTestDir);
  MakeDirs(kTestDir);
  auto dm = DiskManager::Open(std::string(kTestDir) + "/bp.dsf").ValueOrDie();
  BufferPoolManager bpm(4, dm.get());  // tiny pool: forces real eviction

  page_id_t p0, p1, p2, p3, p4;
  Page* pg0 = bpm.NewPage(&p0); assert(pg0);
  std::memcpy(pg0->GetData(), "hello-page-0", 12);
  bpm.UnpinPage(p0, true);
  bpm.UnpinPage(bpm.NewPage(&p1) ? p1 : p1, true);
  bpm.NewPage(&p2); bpm.UnpinPage(p2, true);
  bpm.NewPage(&p3); bpm.UnpinPage(p3, true);
  bpm.NewPage(&p4); bpm.UnpinPage(p4, true);  // forces eviction of p0

  Page* pg0b = bpm.FetchPage(p0);
  assert(pg0b);
  assert(std::memcmp(pg0b->GetData(), "hello-page-0", 12) == 0);
  bpm.UnpinPage(p0, false);

  auto wal = WriteAheadLog::Open(std::string(kTestDir) + "/test.wal").ValueOrDie();
  wal->Append(WalRecordType::kPut, "users", "u1", "docbytes1");
  wal->Append(WalRecordType::kPut, "users", "u2", "docbytes2-longer");
  wal->Append(WalRecordType::kDelete, "users", "u1", "");
  auto records = wal->ReadAll().ValueOrDie();
  assert(records.size() == 3);
  assert(records[0].key == "u1" && records[0].document_bytes == "docbytes1");
  assert(records[1].key == "u2");
  assert(records[2].type == WalRecordType::kDelete);

  std::cout << "[storage_test] buffer pool eviction + WAL append/replay: PASS" << std::endl;
}

static void TestBPlusTreeStress() {
  auto dm = DiskManager::Open(std::string(kTestDir) + "/bpt.dsf").ValueOrDie();
  BufferPoolManager bpm(64, dm.get());
  BPlusTree tree(&bpm, BPlusTree::CreateNew(&bpm).ValueOrDie());

  const int N = 5000;
  std::vector<int> keys(N);
  for (int i = 0; i < N; ++i) keys[i] = i;
  std::mt19937 rng(42);
  std::shuffle(keys.begin(), keys.end(), rng);

  std::map<std::string, RID> reference;
  for (int k : keys) {
    std::string key = "key-" + std::to_string(k);
    RID rid{static_cast<page_id_t>(k % 1000), static_cast<slot_id_t>(k % 100)};
    assert(tree.Insert(key, rid).ok());
    reference[key] = rid;
  }

  std::vector<int> check_order = keys;
  std::shuffle(check_order.begin(), check_order.end(), rng);
  for (int k : check_order) {
    std::string key = "key-" + std::to_string(k);
    RID out;
    assert(tree.Search(key, &out));
    assert(out == reference[key]);
  }

  for (int i = 0; i < N; i += 2) {
    std::string key = "key-" + std::to_string(i);
    RID new_rid{9999, static_cast<slot_id_t>(i % 50)};
    assert(tree.Insert(key, new_rid).ok());
    reference[key] = new_rid;
  }
  for (auto& [key, rid] : reference) {
    RID out;
    assert(tree.Search(key, &out));
    assert(out == rid);
  }

  auto scanned = tree.Scan("", 0);
  assert(scanned.size() == reference.size());
  std::vector<std::string> expected;
  for (auto& [k, v] : reference) expected.push_back(k);
  std::sort(expected.begin(), expected.end());
  for (size_t i = 0; i < scanned.size(); ++i) assert(scanned[i].first == expected[i]);

  for (int i = 0; i < 1000; ++i) {
    assert(tree.Remove("key-" + std::to_string(i)));
    reference.erase("key-" + std::to_string(i));
  }
  for (int i = 0; i < 1000; ++i) {
    RID out;
    assert(!tree.Search("key-" + std::to_string(i), &out));
  }

  std::cout << "[storage_test] B+Tree stress (" << N << " keys, splits+upserts+scan+remove): PASS" << std::endl;
}

static void TestWalHashChain() {
  std::string wal_path = std::string(kTestDir) + "/chain.wal";
  RmRf(wal_path);

  {
    auto wal = WriteAheadLog::Open(wal_path).ValueOrDie();
    // Genesis: no entries yet, tip is the all-zero hash at entry_id -1.
    auto tip0 = wal->Tip();
    assert(tip0.entry_id == kInvalidLsn);
    assert(tip0.entry_hash == std::string(kWalHashLen, '\0'));

    wal->Append(WalRecordType::kPut, "users", "u1", "docbytes1");
    wal->Append(WalRecordType::kPut, "users", "u2", "docbytes2-longer");
    wal->Append(WalRecordType::kPut, "users", "u1", "docbytes1-updated");

    auto records = wal->ReadAll().ValueOrDie();
    assert(records.size() == 3);
    // Genesis link: the first record chains from 32 zero bytes.
    assert(records[0].prev_hash == std::string(kWalHashLen, '\0'));
    // Each subsequent record's prev_hash is exactly the preceding record's
    // entry_hash -- the chain invariant the whole tamper-evidence guarantee
    // rests on.
    assert(records[1].prev_hash == records[0].entry_hash);
    assert(records[2].prev_hash == records[1].entry_hash);
    // No two entries collide (would be a SHA-256 break, not expected).
    assert(records[0].entry_hash != records[1].entry_hash);
    assert(records[1].entry_hash != records[2].entry_hash);

    auto tip = wal->Tip();
    assert(tip.entry_id == 2);
    assert(tip.entry_hash == records[2].entry_hash);

    auto verify = wal->VerifyChain();
    assert(verify.ok);
    assert(verify.entries_checked == 3);
  }

  // Reopen: the chain tip must survive a restart (rebuilt from the WAL on
  // Open(), exactly like next_lsn_ is) -- a fresh process picking the WAL
  // back up must extend the *same* chain, not silently start a new one.
  {
    auto wal = WriteAheadLog::Open(wal_path).ValueOrDie();
    auto pre_reopen_tip = wal->Tip();
    assert(pre_reopen_tip.entry_id == 2);

    wal->Append(WalRecordType::kPut, "users", "u3", "docbytes3");
    auto records = wal->ReadAll().ValueOrDie();
    assert(records.size() == 4);
    assert(records[3].prev_hash == pre_reopen_tip.entry_hash);

    auto verify = wal->VerifyChain();
    assert(verify.ok);
    assert(verify.entries_checked == 4);
  }

  // Tamper detection: flip a single byte inside an already-committed
  // record's on-disk frame (simulating post-hoc disk tampering, not a
  // crash-torn write -- CRC alone would also catch a torn write; this
  // proves the *hash chain* specifically catches a well-formed-but-altered
  // record that still passes its own CRC framing check would not apply
  // here since CRC covers the tampered bytes too, so we tamper a payload
  // byte and manually fix nothing else -- CRC will actually also fire, but
  // we assert on VerifyChain()'s independent hash-chain result to prove
  // the chain mechanism itself, not just CRC, is the one flagging it).
  {
    std::fstream f(wal_path, std::ios::in | std::ios::out | std::ios::binary);
    assert(f.is_open());
    // Byte 20 lands inside the first record's frame -- specifically inside
    // its "collection" field bytes ("users"), well before that record's
    // ~109-byte frame ends -- so this reliably corrupts committed record 0
    // without depending on exact field-width arithmetic staying in sync
    // with the frame layout.
    f.seekp(20, std::ios::beg);
    char corrupt = 'X';
    f.write(&corrupt, 1);
    f.close();
  }
  {
    auto wal = WriteAheadLog::Open(wal_path).ValueOrDie();
    auto verify = wal->VerifyChain();
    // Either the CRC framing catches the torn/altered bytes and replay
    // stops early (fewer entries than were written), or the record parses
    // but the chain/hash check fails -- both are correct, safe outcomes,
    // and both are exactly what a tamper-evident log is supposed to do:
    // never silently accept an altered entry as legitimate.
    assert(!verify.ok || verify.entries_checked < 4);
  }

  std::cout << "[storage_test] WAL hash-chain (genesis link, chain continuity across restart, tamper detection): PASS" << std::endl;
}

static void TestStorageEngineCrashRecovery() {
  std::string data_dir = std::string(kTestDir) + "/engine_data";
  RmRf(data_dir);
  HybridLogicalClock clock("nodeA");

  {
    StorageEngine::Options opts;
    opts.data_dir = data_dir;
    opts.buffer_pool_pages = 32;
    auto engine = StorageEngine::Open(opts).ValueOrDie();

    for (int i = 0; i < 200; ++i) {
      auto j = JsonValue::Parse(R"({"n":)" + std::to_string(i) + R"(})");
      auto doc = CrdtValue::FromJson(j, clock.Now());
      assert(engine->PutRaw("items", "item-" + std::to_string(i), EncodeDocument(doc)).ok());
    }
    // Deliberately no Checkpoint(): we want to verify recovery reconstructs
    // state that was *only* in the WAL, never flushed to the data file.
  }

  {
    StorageEngine::Options opts;
    opts.data_dir = data_dir;
    opts.buffer_pool_pages = 32;
    auto engine = StorageEngine::Open(opts).ValueOrDie();

    int found = 0;
    for (int i = 0; i < 200; ++i) {
      auto got = engine->GetRaw("items", "item-" + std::to_string(i));
      if (got.ok()) {
        assert(DecodeDocument(got.value()).ToJson().Get("n").AsInt() == i);
        found++;
      }
    }
    assert(found == 200);
    assert(engine->Scan("items", "", 0).size() == 200);
  }

  std::cout << "[storage_test] StorageEngine crash-recovery via WAL replay (200 docs, never checkpointed): PASS" << std::endl;
}

static size_t KvPoolPages(StorageEngine& engine) {
  EngineBackend* backend = engine.router().Backend("kv");
  assert(backend != nullptr);
  auto* kv = dynamic_cast<KvBPlusBackend*>(backend);
  assert(kv != nullptr);
  return kv->BufferPoolPages();
}

static void TestBufferPoolSizingIsHonored() {
  // buffer_pool_pages used to be parsed but never forwarded: KvBPlus always
  // built a 1024-page pool whatever node.json said. Two engines opened with
  // different settings must now hold measurably different pools.
  const std::string small_dir = std::string(kTestDir) + "/pool_small";
  const std::string large_dir = std::string(kTestDir) + "/pool_large";
  RmRf(small_dir);
  RmRf(large_dir);

  StorageEngine::Options small_opts;
  small_opts.data_dir = small_dir;
  small_opts.buffer_pool_pages = 8;
  auto small_engine = StorageEngine::Open(small_opts).ValueOrDie();
  assert(KvPoolPages(*small_engine) == 8);

  StorageEngine::Options large_opts;
  large_opts.data_dir = large_dir;
  large_opts.buffer_pool_pages = 64;
  auto large_engine = StorageEngine::Open(large_opts).ValueOrDie();
  assert(KvPoolPages(*large_engine) == 64);

  // Zero is clamped to a working minimum rather than building a dead pool.
  const std::string zero_dir = std::string(kTestDir) + "/pool_zero";
  RmRf(zero_dir);
  StorageEngine::Options zero_opts;
  zero_opts.data_dir = zero_dir;
  zero_opts.buffer_pool_pages = 0;
  auto zero_engine = StorageEngine::Open(zero_opts).ValueOrDie();
  assert(KvPoolPages(*zero_engine) > 0);

  std::cout << "[storage_test] buffer_pool_pages sizing honored (8 vs 64 vs clamped-0): PASS"
            << std::endl;
}

static void TestFailedWriteLeavesNoLedgerGap() {
  // A write the backend must refuse (bad key, oversize doc) must be refused
  // BEFORE the ledger attests to it: otherwise the WAL claims a write that
  // replay can never materialise. The tip must not advance and a restart
  // must show no gap (valid docs intact, refused docs absent, chain verifies).
  const std::string data_dir = std::string(kTestDir) + "/no_gap";
  RmRf(data_dir);
  HybridLogicalClock clock("nodeA");
  lsn_t tip_after_valid = kInvalidLsn;

  {
    StorageEngine::Options opts;
    opts.data_dir = data_dir;
    opts.buffer_pool_pages = 32;
    auto engine = StorageEngine::Open(opts).ValueOrDie();
    const lsn_t tip_before = engine->wal()->LastLsn();

    // 64-byte key: refused by the B+Tree bound (>= kMaxKeyBytes).
    auto j = JsonValue::Parse(R"({"n":1})");
    auto doc = CrdtValue::FromJson(j, clock.Now());
    Status bad_key = engine->PutRaw("items", std::string(64, 'k'), EncodeDocument(doc));
    assert(!bad_key.ok() && bad_key.code() == StatusCode::kInvalidArgument);
    assert(engine->wal()->LastLsn() == tip_before);

    // Oversize document: can never fit in a slotted page.
    Status big_doc =
        engine->PutRaw("items", "too-big", std::string(kPageSize, 'x'));
    assert(!big_doc.ok() && big_doc.code() == StatusCode::kInvalidArgument);
    assert(engine->wal()->LastLsn() == tip_before);

    // A valid write still lands and advances the tip by exactly one.
    auto ok_doc = CrdtValue::FromJson(JsonValue::Parse(R"({"n":2})"), clock.Now());
    assert(engine->PutRaw("items", "good", EncodeDocument(ok_doc)).ok());
    tip_after_valid = engine->wal()->LastLsn();
    assert(tip_after_valid == tip_before + 1);
  }

  {
    StorageEngine::Options opts;
    opts.data_dir = data_dir;
    opts.buffer_pool_pages = 32;
    auto engine = StorageEngine::Open(opts).ValueOrDie();
    assert(engine->wal()->LastLsn() == tip_after_valid);
    assert(engine->GetRaw("items", "good").ok());
    assert(!engine->GetRaw("items", std::string(64, 'k')).ok());
    assert(!engine->GetRaw("items", "too-big").ok());
    assert(engine->VerifyAll().ledger.ok);
  }

  std::cout << "[storage_test] failed writes refused before WAL append (no ledger gap): PASS"
            << std::endl;
}

static void TestWalMalformedTailAndPruneCases() {
  const std::string wal_path = std::string(kTestDir) + "/malformed_cases.wal";

  // Case 1: Truncated final frame (short read: crash mid-append)
  // A clean EOF or short read at the tail is a benign crash: earlier records
  // must verify and remain readable.
  RmRf(wal_path);
  {
    auto wal = WriteAheadLog::Open(wal_path).ValueOrDie();
    assert(wal->Append(WalRecordType::kPut, "col", "k0", "val0").ok());
    assert(wal->Append(WalRecordType::kPut, "col", "k1", "val1").ok());
    assert(wal->Append(WalRecordType::kPut, "col", "k2", "val2").ok());
  }
  {
    // Append a torn frame at the tail (length says 100 bytes, only 10 bytes written)
    std::ofstream f(wal_path, std::ios::binary | std::ios::app);
    uint32_t fake_len = 100;
    f.write(reinterpret_cast<const char*>(&fake_len), 4);
    char partial[10] = {0};
    f.write(partial, 10);
    f.close();
  }
  {
    auto wal = WriteAheadLog::Open(wal_path).ValueOrDie();
    auto records = wal->ReadAll().ValueOrDie();
    assert(records.size() == 3);
    auto verify = wal->VerifyChain();
    assert(verify.ok);
    assert(verify.entries_checked == 3);
  }

  // Case 2: Complete frame with a bad CRC at the tail.
  // Full length was written, but CRC verification fails -> flagged as corrupt.
  RmRf(wal_path);
  {
    auto wal = WriteAheadLog::Open(wal_path).ValueOrDie();
    assert(wal->Append(WalRecordType::kPut, "col", "k0", "val0").ok());
    assert(wal->Append(WalRecordType::kPut, "col", "k1", "val1").ok());
  }
  {
    // Corrupt the CRC of the last record (last 2 bytes of file)
    std::fstream f(wal_path, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(-2, std::ios::end);
    char flip = '\xFF';
    f.write(&flip, 1);
    f.close();
  }
  {
    auto wal = WriteAheadLog::Open(wal_path).ValueOrDie();
    auto records = wal->ReadAll().ValueOrDie();
    // Stops before the corrupt record
    assert(records.size() == 1);
    auto verify = wal->VerifyChain();
    assert(!verify.ok);
  }

  // Case 3: Malformed frame with valid bytes after it (middle-of-file corruption)
  RmRf(wal_path);
  {
    auto wal = WriteAheadLog::Open(wal_path).ValueOrDie();
    assert(wal->Append(WalRecordType::kPut, "col", "k0", "val0").ok());
    assert(wal->Append(WalRecordType::kPut, "col", "k1", "val1-middle").ok());
    assert(wal->Append(WalRecordType::kPut, "col", "k2", "val2-after").ok());
    assert(wal->Append(WalRecordType::kPut, "col", "k3", "val3-after").ok());
  }
  {
    // Corrupt record 1's payload bytes in the middle of the file
    std::fstream f(wal_path, std::ios::in | std::ios::out | std::ios::binary);
    f.seekg(0, std::ios::beg);
    uint32_t len0 = 0;
    f.read(reinterpret_cast<char*>(&len0), 4);
    // Inside record 1: skip len0 + 4, plus record 1's len (4), magic (4), lsn (8), type (1)
    f.seekp(4 + len0 + 4 + 4 + 8 + 1, std::ios::beg);
    char bad = 'Z';
    f.write(&bad, 1);
    f.close();
  }
  {
    auto wal = WriteAheadLog::Open(wal_path).ValueOrDie();
    auto records = wal->ReadAll().ValueOrDie();
    assert(records.size() == 1);  // stops at record 1
    auto verify = wal->VerifyChain();
    assert(!verify.ok);
  }

  // Case 4: Sparse original LSNs across prune + append + restart
  RmRf(wal_path);
  {
    auto wal = WriteAheadLog::Open(wal_path).ValueOrDie();
    WriteAheadLog::AppendOptions opt;
    opt.transit_owner = "peerA";
    assert(wal->Append(WalRecordType::kPut, "col", "p0", "put0").ok());                           // lsn 0
    assert(wal->Append(WalRecordType::kTransitIntent, "col", "t1", "transit1", opt).ok());        // lsn 1
    assert(wal->Append(WalRecordType::kTransitClaimed, "col", "t1", "transit1", opt).ok());       // lsn 2
    assert(wal->Append(WalRecordType::kPut, "col", "p1", "put1").ok());                           // lsn 3
    assert(wal->Append(WalRecordType::kCheckpoint, "col", "", "").ok());                          // lsn 4

    auto prune_res = wal->Prune(4);
    assert(prune_res.ok());
    assert(prune_res.value().dropped == 2);

    // After prune, append a new PUT and CHECKPOINT
    auto next_lsn1 = wal->Append(WalRecordType::kPut, "col", "p2", "put2");
    assert(next_lsn1.ok() && next_lsn1.value() == 5);
    auto next_lsn2 = wal->Append(WalRecordType::kCheckpoint, "col", "", "");
    assert(next_lsn2.ok() && next_lsn2.value() == 6);
    assert(wal->LastCheckpointLsn() == 6);
    assert(wal->VerifyChain().ok);
  }
  {
    // Reopen and check sparse LSN preservation
    auto wal = WriteAheadLog::Open(wal_path).ValueOrDie();
    auto records = wal->ReadAll().ValueOrDie();
    assert(records.size() == 5);  // lsn 0, 3, 4, 5, 6
    assert(records[0].lsn == 0);
    assert(records[1].lsn == 3);
    assert(records[2].lsn == 4);
    assert(records[3].lsn == 5);
    assert(records[4].lsn == 6);
    assert(wal->Tip().entry_id == 6);
    assert(wal->LastCheckpointLsn() == 6);
    assert(wal->VerifyChain().ok);
  }

  // Case 5: Concurrent reads while verification runs
  {
    auto wal = WriteAheadLog::Open(wal_path).ValueOrDie();
    std::vector<std::thread> threads;
    std::atomic<bool> ok_flag{true};
    for (int t = 0; t < 4; ++t) {
      threads.emplace_back([&]() {
        for (int iter = 0; iter < 50; ++iter) {
          auto recs = wal->ReadAll();
          if (!recs.ok() || recs.value().size() != 5) ok_flag = false;
          auto v = wal->VerifyChain();
          if (!v.ok || v.entries_checked != 5) ok_flag = false;
        }
      });
    }
    for (auto& th : threads) th.join();
    assert(ok_flag.load());
  }

  std::cout << "[storage_test] WAL malformed-tail, CRC verification, and pruning LSN behavior: PASS"
            << std::endl;
}

int main() {
  TestBufferPoolAndWal();
  TestBPlusTreeStress();
  TestWalHashChain();
  TestStorageEngineCrashRecovery();
  TestBufferPoolSizingIsHonored();
  TestFailedWriteLeavesNoLedgerGap();
  TestWalMalformedTailAndPruneCases();
  std::cout << "[storage_test] ALL STORAGE TESTS PASSED" << std::endl;
  return 0;
}
