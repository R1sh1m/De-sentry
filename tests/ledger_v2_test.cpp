// Ledger v2 test suite: the hash chain, transit, and the checkpoint gate.
//
// The ledger is what makes history tamper-evident, and the checkpoint is the
// one operation in the system that deletes history. So the tests here are
// weighted towards the ways each can go wrong:
//
//   * A chain that verifies after a record is altered would make the whole
//     structure decorative -- so tampering is simulated directly on the file.
//   * A checkpoint that proceeds without quorum, or with a replica reporting
//     a different hash at the same height, would delete history the mesh has
//     not agreed on. Every refusal path is checked, not just the happy one.
//   * A checkpoint that pruned a TRANSIT_INTENT whose CLAIMED had not arrived
//     would throw away a write that an offline node still needs. That is
//     condition (2) of the gate, and it is checked here too.
//
// Plain assert(), no framework (-UNDEBUG keeps them live in every build type).

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "desentry/common/platform.h"
#include "desentry/crdt/hlc.h"
#include "desentry/ledger/checkpoint.h"
#include "desentry/ledger/transit_store.h"
#include "desentry/storage/wal.h"

using namespace desentry;

namespace {

std::string TestRoot() { return AppDataDir() + "/desentry_test_ledger"; }

void Fresh(const std::string& path) {
  RemoveTree(path);
  MakeDirs(path);
}

HybridLogicalClock g_clock("ledger-test");

std::unique_ptr<WriteAheadLog> OpenWal(const std::string& path) {
  auto wal_or = WriteAheadLog::Open(path);
  assert(wal_or.ok());
  return std::move(wal_or.value());
}

// -- the hash chain ---------------------------------------------------------

void TestChainLinksAndVerifies() {
  std::cout << "  chain: entries link and verify\n";
  const std::string dir = TestRoot() + "/chain";
  Fresh(dir);
  const std::string path = dir + "/ledger.wal";

  auto wal = OpenWal(path);
  for (int i = 0; i < 20; ++i) {
    WriteAheadLog::AppendOptions options;
    options.hlc = g_clock.Now();
    assert(wal->Append(WalRecordType::kPut, "things", "k" + std::to_string(i), "bytes", options).ok());
  }

  auto entries_or = wal->ReadAll();
  assert(entries_or.ok());
  const auto& entries = entries_or.value();
  assert(entries.size() == 20);

  // Each entry's prev_hash is the previous entry's entry_hash. That is the
  // chain: an entry cannot be changed without changing every entry after it.
  for (size_t i = 1; i < entries.size(); ++i) {
    assert(entries[i].prev_hash == entries[i - 1].entry_hash);
    assert(!entries[i].entry_hash.empty());
    assert(entries[i].lsn > entries[i - 1].lsn);
  }

  const auto verified = wal->VerifyChain();
  assert(verified.ok);
  assert(verified.entries_checked == 20);

  // The tip is the last entry, and it is what a peer compares against.
  const auto tip = wal->Tip();
  assert(tip.entry_id == entries.back().lsn);
  assert(tip.entry_hash == entries.back().entry_hash);
}

void TestTamperingIsDetected() {
  std::cout << "  chain: an altered record fails verification\n";
  const std::string dir = TestRoot() + "/tamper";
  Fresh(dir);
  const std::string path = dir + "/ledger.wal";

  {
    auto wal = OpenWal(path);
    for (int i = 0; i < 10; ++i) {
      WriteAheadLog::AppendOptions options;
      options.hlc = g_clock.Now();
      assert(wal->Append(WalRecordType::kPut, "things", "key" + std::to_string(i), "payload-aaaa",
                          options)
                 .ok());
    }
  }

  // Flip a byte inside one record's payload, leaving its length and framing
  // intact -- the change a careless edit or a corrupted sector would make.
  {
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    assert(file.is_open());
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    assert(size > 64);
    // Somewhere in the middle, well past the header of the first record.
    const std::streamoff offset = size / 2;
    file.seekg(offset);
    char byte = 0;
    file.read(&byte, 1);
    file.seekp(offset);
    byte = static_cast<char>(byte ^ 0x5a);
    file.write(&byte, 1);
    file.close();
  }

  {
    auto wal_or = WriteAheadLog::Open(path);
    if (!wal_or.ok()) {
      // Refusing to open a corrupt ledger is also an acceptable outcome: what
      // must never happen is opening it and reporting the chain as intact.
      return;
    }
    auto wal = std::move(wal_or.value());
    const auto verified = wal->VerifyChain();
    assert(!verified.ok);
    assert(!verified.reason.empty());
  }
}

void TestSignaturesAreRecorded() {
  std::cout << "  chain: origin signatures are counted\n";
  const std::string dir = TestRoot() + "/signed";
  Fresh(dir);

  auto wal = OpenWal(dir + "/ledger.wal");

  // Two unsigned entries first: a v1 ledger, or a node whose identity has not
  // been wired in yet, produces these and must still verify structurally.
  for (int i = 0; i < 2; ++i) {
    WriteAheadLog::AppendOptions options;
    options.hlc = g_clock.Now();
    assert(wal->Append(WalRecordType::kPut, "c", "unsigned" + std::to_string(i), "x", options).ok());
  }

  // Then a signer that stamps every entry. The signature is opaque to the WAL;
  // what it must do is store it, chain over it, and report how many entries
  // carry one -- an audit that could not distinguish signed from unsigned
  // history would overstate what the chain proves.
  wal->SetOrigin("node-signer", [](const std::string& message) {
    return std::string("sig:") + message.substr(0, 8);
  });
  for (int i = 0; i < 3; ++i) {
    WriteAheadLog::AppendOptions options;
    options.hlc = g_clock.Now();
    assert(wal->Append(WalRecordType::kPut, "c", "signed" + std::to_string(i), "x", options).ok());
  }

  const auto verified = wal->VerifyChain();
  assert(verified.ok);
  assert(verified.entries_checked == 5);
  assert(verified.signed_entries == 3);
  assert(verified.unsigned_entries == 2);

  auto entries = wal->ReadAll().value();
  assert(entries.back().origin_node_id == "node-signer");
  assert(!entries.back().origin_signature.empty());
  assert(entries.front().origin_signature.empty());
}

void TestKeyHashIsDerivedNotStored() {
  std::cout << "  chain: key hashes are derived the same way everywhere\n";
  // Every node computes SHA-256(collection || 0x00 || key). The separator is
  // what stops ("ab", "c") and ("a", "bc") hashing alike -- two different
  // documents sharing a ledger identity would converge onto each other.
  assert(LedgerKeyHash("ab", "c") != LedgerKeyHash("a", "bc"));
  assert(LedgerKeyHash("things", "k1") == LedgerKeyHash("things", "k1"));
  assert(LedgerKeyHash("things", "k1").size() == kWalHashLen);
}

// -- the checkpoint gate ----------------------------------------------------

ReplicaTip Tip(const std::string& node, lsn_t entry_id, const std::string& hash, bool verified = true) {
  ReplicaTip tip;
  tip.node_id = node;
  tip.entry_id = entry_id;
  tip.entry_hash = hash;
  tip.self_verified = verified;
  tip.signature_valid = true;
  tip.signature = "sig";
  return tip;
}

void TestQuorumSize() {
  std::cout << "  checkpoint: quorum sizes\n";
  // 2f+1 with f = (rf-1)/2. At RF=3 that is all three: checkpointing deletes
  // history, so the odd core has to be unanimous rather than merely a
  // majority. The cost of waiting is retained garbage; the cost of being
  // wrong is deleted history.
  assert(CheckpointQuorumSize(1) == 1);
  assert(CheckpointQuorumSize(3) == 3);
  assert(CheckpointQuorumSize(5) == 5);
  assert(CheckpointQuorumSize(4) == 3);
}

void TestCheckpointProceedsOnAgreement() {
  std::cout << "  checkpoint: proceeds when replicas agree\n";
  const std::string hash(kWalHashLen, '\x11');
  const auto local = Tip("self", 100, hash);
  const std::vector<ReplicaTip> replicas = {Tip("peer-a", 100, hash), Tip("peer-b", 100, hash)};

  const auto decision = EvaluateCheckpoint(local, replicas, 3);
  assert(decision.proceed);
  assert(decision.agreeing == 3);
  assert(decision.required == 3);
  assert(decision.conflicting == 0);
  assert(decision.checkpoint_lsn == 100);
  assert(decision.agreed_entry_hash == hash);
}

void TestCheckpointRefusesOnConflict() {
  std::cout << "  checkpoint: a conflicting tip aborts it\n";
  const std::string ours(kWalHashLen, '\x11');
  const std::string theirs(kWalHashLen, '\x22');
  const auto local = Tip("self", 100, ours);

  // Same height, different hash: the two nodes have different histories. This
  // is the one case where pruning would destroy the evidence needed to work
  // out what happened, so it must abort rather than fall back to a lower
  // checkpoint.
  const std::vector<ReplicaTip> replicas = {Tip("peer-a", 100, ours), Tip("peer-b", 100, theirs)};
  const auto decision = EvaluateCheckpoint(local, replicas, 3);
  assert(!decision.proceed);
  assert(decision.conflicting >= 1);
  assert(!decision.reason.empty());
}

void TestCheckpointRefusesWithoutQuorum() {
  std::cout << "  checkpoint: too few replicas refuses\n";
  const std::string hash(kWalHashLen, '\x11');
  const auto local = Tip("self", 100, hash);

  // One agreeing peer out of the two needed.
  const auto decision = EvaluateCheckpoint(local, {Tip("peer-a", 100, hash)}, 3);
  assert(!decision.proceed);
  assert(decision.agreeing < decision.required);
  assert(!decision.reason.empty());
}

void TestUnverifiedReplicasDoNotCount() {
  std::cout << "  checkpoint: an unverified replica is not a vote\n";
  const std::string hash(kWalHashLen, '\x11');
  const auto local = Tip("self", 100, hash);

  // A replica that reports the right hash but has not verified its own chain
  // is asserting agreement it has not checked. Counting it would let a node
  // with a corrupt ledger authorise the deletion of everyone's history.
  const std::vector<ReplicaTip> replicas = {
      Tip("peer-a", 100, hash, /*verified=*/false),
      Tip("peer-b", 100, hash, /*verified=*/false),
  };
  const auto decision = EvaluateCheckpoint(local, replicas, 3);
  assert(!decision.proceed);
  assert(decision.unverified == 2);
}

void TestBehindPeersAreNotConflicts() {
  std::cout << "  checkpoint: a lagging replica is not a conflict\n";
  const std::string hash(kWalHashLen, '\x11');
  const auto local = Tip("self", 100, hash);

  // A peer at entry 80 is simply behind -- it has not seen the last twenty
  // writes yet. Treating that as a conflict would mean a mesh with any gossip
  // latency could never checkpoint at all.
  const std::vector<ReplicaTip> replicas = {
      Tip("peer-a", 80, std::string(kWalHashLen, '\x33')),
      Tip("peer-b", 100, hash),
  };
  const auto decision = EvaluateCheckpoint(local, replicas, 3);
  assert(decision.conflicting == 0);
  // It still cannot proceed at RF=3, because agreement is short of quorum --
  // but for the right reason.
  assert(!decision.proceed);
  assert(decision.agreeing == 2);
}

void TestUnclaimedIntentsBlockPruning() {
  std::cout << "  checkpoint: an unclaimed intent holds the prune back\n";
  const std::string dir = TestRoot() + "/intents";
  Fresh(dir);

  auto wal = OpenWal(dir + "/ledger.wal");

  auto append = [&](WalRecordType type, const std::string& key) {
    WriteAheadLog::AppendOptions options;
    options.hlc = g_clock.Now();
    options.transit_owner = "owner-node";
    auto lsn = wal->Append(type, "things", key, "bytes", options);
    assert(lsn.ok());
    return lsn.value();
  };

  append(WalRecordType::kPut, "a");
  append(WalRecordType::kTransitIntent, "held-1");
  append(WalRecordType::kTransitClaimed, "held-1");
  append(WalRecordType::kTransitIntent, "held-2");  // never claimed
  const lsn_t last = append(WalRecordType::kPut, "b");

  const auto entries = wal->ReadAll().value();
  const auto unclaimed = UnclaimedIntentsBelow(entries, last);

  // held-1 was claimed, so its bytes are no longer needed. held-2 was not:
  // pruning it would throw away a write that an offline node has not yet
  // collected, which is data loss dressed up as garbage collection.
  assert(unclaimed.size() == 1);
  assert(unclaimed.front() == LedgerKeyHash("things", "held-2"));

  // Below the claimed pair only, nothing is outstanding.
  const auto safe = UnclaimedIntentsBelow(entries, 3);
  assert(safe.empty());
}

void TestPruneKeepsTheChainVerifiable() {
  std::cout << "  chain: pruning leaves a verifiable chain\n";
  const std::string dir = TestRoot() + "/prune";
  Fresh(dir);
  const std::string path = dir + "/ledger.wal";

  lsn_t checkpoint = 0;
  {
    auto wal = OpenWal(path);
    for (int i = 0; i < 50; ++i) {
      WriteAheadLog::AppendOptions options;
      options.hlc = g_clock.Now();
      assert(wal->Append(WalRecordType::kPut, "c", "k" + std::to_string(i), "v", options).ok());
    }
    checkpoint = 30;
    auto pruned = wal->Prune(checkpoint);
    assert(pruned.ok());
    // Verification after a prune has to succeed. A chain that only verified
    // from entry zero would mean checkpointing permanently broke the audit --
    // exactly the property the checkpoint exists to preserve.
    assert(wal->VerifyChain().ok);
  }

  {
    // ...and it must still verify after a restart, from the truncated file.
    auto wal = OpenWal(path);
    const auto verified = wal->VerifyChain();
    assert(verified.ok);
    auto entries = wal->ReadAll().value();
    assert(!entries.empty());
    assert(entries.front().lsn >= checkpoint);
    assert(wal->Tip().entry_id == entries.back().lsn);
  }
}

}  // namespace

int main() {
  std::cout << "== ledger v2 test suite ==\n";
  Fresh(TestRoot());

  std::cout << "-- hash chain --\n";
  TestChainLinksAndVerifies();
  TestTamperingIsDetected();
  TestSignaturesAreRecorded();
  TestKeyHashIsDerivedNotStored();

  std::cout << "-- checkpoint gate --\n";
  TestQuorumSize();
  TestCheckpointProceedsOnAgreement();
  TestCheckpointRefusesOnConflict();
  TestCheckpointRefusesWithoutQuorum();
  TestUnverifiedReplicasDoNotCount();
  TestBehindPeersAreNotConflicts();
  TestUnclaimedIntentsBlockPruning();
  TestPruneKeepsTheChainVerifiable();

  RemoveTree(TestRoot());
  std::cout << "ledger_v2_test: ALL PASSED\n";
  return 0;
}
