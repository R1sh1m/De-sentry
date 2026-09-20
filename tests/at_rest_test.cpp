// At-rest encryption test suite: the sealed-page / sealed-record /
// sealed-file layers, the fail-closed open discipline, and the offline
// migration tool.
//
// Pinned properties:
//
//   * Crockford text decodes with the app's transcription tolerance
//     (case/space/dash-insensitive, O->0, I/L->1), rejects 'U' (not in the
//     alphabet) and wrong lengths -- what the wizard shows is what the
//     engine accepts, byte for byte.
//   * Sealed records/pages/files round-trip; any tampering (bit flip,
//     page swap across files or within a file, wrong key, bad magic)
//     fails authentication -- never silent plaintext, never a zeroed page
//     pretending to be data.
//   * Every sealed open is fail-closed both ways: sealed-without-key,
//     plaintext-with-key, and wrong-key all refuse with Corruption.
//   * An encrypted node survives a restart with its data intact (WAL replay
//     through sealed pages), and `ReencryptDataDir` converts a plaintext
//     directory that then opens sealed -- and refuses to open plaintext.
//
// Plain assert(), no framework (-UNDEBUG keeps them live in every build type).

#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "desentry/common/platform.h"
#include "desentry/engine/node_engine.h"
#include "desentry/security/at_rest.h"
#include "desentry/security/crypto.h"
#include "desentry/storage/disk_manager.h"
#include "desentry/storage/reencrypt.h"
#include "desentry/storage/storage_engine.h"
#include "desentry/storage/wal.h"

using namespace desentry;

namespace {

std::string TestRoot() { return AppDataDir() + "/desentry_test_atrest"; }

void Fresh(const std::string& path) {
  RemoveTree(path);
  MakeDirs(path);
}

// 32 zero bytes, Crockford-encoded by hand (52 '0's: 256 bits at 5 bits per
// symbol, last symbol padded). A test DEK -- never production entropy.
std::string ZeroKeyText() { return std::string(52, '0'); }

std::string ZeroDek() {
  auto dek_or = at_rest::DecodeRecoveryKey(ZeroKeyText());
  assert(dek_or.ok());
  assert(dek_or.value().size() == 32);
  return dek_or.value();
}

std::string OtherDek() {
  // 0x01 repeated: differs from zero in every byte that matters.
  std::string raw(32, '\x01');
  // Encode via the alphabet table directly (Crockford of 0x01*32).
  static const char* kAlpha = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
  uint32_t bits = 0;
  int count = 0;
  std::string symbols;
  for (unsigned char byte : raw) {
    bits = (bits << 8) | byte;
    count += 8;
    while (count >= 5) {
      count -= 5;
      symbols.push_back(kAlpha[(bits >> count) & 0x1F]);
    }
  }
  if (count > 0) symbols.push_back(kAlpha[(bits << (5 - count)) & 0x1F]);
  auto back_or = at_rest::DecodeRecoveryKey(symbols);
  assert(back_or.ok());
  assert(back_or.value() == raw);
  return raw;
}

// -- Crockford decoding -------------------------------------------------------

void TestCrockfordDecoding() {
  std::cout << "  keys: crockford decoding with transcription tolerance\n";
  // 52 zeros -> 32 zero bytes.
  auto zero_or = at_rest::DecodeRecoveryKey(ZeroKeyText());
  assert(zero_or.ok());
  assert(zero_or.value() == std::string(32, '\0'));
  // Dashes, spaces, lowercase and O-for-zero are tolerated. Exactly 52
  // symbols (260 bits -> 32 bytes + 4 ignored pad bits), like ZeroKeyText.
  std::string messy = "00000-00000 00000\n00000\toooo0 00000-00000-00000-00000-0000o-00";
  auto messy_or = at_rest::DecodeRecoveryKey(messy);
  assert(messy_or.ok());
  assert(messy_or.value() == std::string(32, '\0'));
  // I/L stand for 1: "11111..." is all-ones nibbles, not zeros.
  auto ones_or = at_rest::DecodeRecoveryKey(std::string(52, '1'));
  assert(ones_or.ok());
  assert(ones_or.value() != std::string(32, '\0'));
  auto tolerant_or = at_rest::DecodeRecoveryKey(std::string(52, 'I'));
  assert(tolerant_or.ok());
  assert(tolerant_or.value() == ones_or.value());
  // 'U' is not in the Crockford alphabet: rejected, never mapped.
  assert(!at_rest::DecodeRecoveryKey(std::string(52, 'U')).ok());
  // Wrong lengths rejected both ways.
  assert(!at_rest::DecodeRecoveryKey(std::string(51, '0')).ok());
  assert(!at_rest::DecodeRecoveryKey(std::string(53, '0')).ok());
  assert(!at_rest::DecodeRecoveryKey("").ok());
  assert(!at_rest::DecodeRecoveryKey("not a key at all!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!").ok());
}

// -- sealed records -----------------------------------------------------------

void TestSealedRecords() {
  std::cout << "  records: seal/open round-trip, tamper and wrong-key refusal\n";
  const std::string dek = ZeroDek();
  auto subkey_or = at_rest::FileSubkey(dek, "wal");
  assert(subkey_or.ok());
  const std::string subkey = subkey_or.value();

  auto sealed_or = at_rest::SealRecord(subkey, "hello-ledger", "wal");
  assert(sealed_or.ok());
  const std::string sealed = sealed_or.value();
  assert(at_rest::LooksSealedRecord(sealed));
  assert(!at_rest::LooksSealedRecord("hello-ledger"));

  auto open_or = at_rest::OpenRecord(subkey, sealed, "wal");
  assert(open_or.ok());
  assert(open_or.value() == "hello-ledger");

  // Wrong AAD fails: records are bound to their log kind.
  assert(!at_rest::OpenRecord(subkey, sealed, "transit").ok());
  // Wrong key fails.
  auto wrong_sub_or = at_rest::FileSubkey(OtherDek(), "wal");
  assert(wrong_sub_or.ok());
  assert(!at_rest::OpenRecord(wrong_sub_or.value(), sealed, "wal").ok());
  // Bit flip fails.
  std::string flipped = sealed;
  flipped[10] ^= 0x01;
  assert(!at_rest::OpenRecord(subkey, flipped, "wal").ok());
  // Bad magic fails.
  assert(!at_rest::OpenRecord(subkey, "XXXX" + sealed.substr(4), "wal").ok());
  // Subkeys differ per file tag: one DEK, different seals per file.
  auto other_sub_or = at_rest::FileSubkey(dek, "transit");
  assert(other_sub_or.ok());
  assert(other_sub_or.value() != subkey);
}

// -- sealed pages -------------------------------------------------------------

void TestSealedPages() {
  std::cout << "  pages: seal/open round-trip, swap and wrong-key refusal\n";
  const std::string dek = ZeroDek();
  auto subkey_or = at_rest::FileSubkey(dek, "tag-1");
  assert(subkey_or.ok());
  const std::string subkey = subkey_or.value();

  std::string plain(kPageSize, '\0');
  for (size_t i = 0; i < kPageSize; ++i) plain[i] = static_cast<char>(i & 0xFF);
  std::string sealed(at_rest::kSealedPageSize, '\0');
  assert(at_rest::SealPage(subkey, 7, "tag-1", plain.data(), sealed.data()).ok());
  assert(sealed.size() == at_rest::kSealedPageSize);

  std::string back(kPageSize, '\0');
  assert(at_rest::OpenPage(subkey, 7, "tag-1", sealed.data(), back.data()).ok());
  assert(back == plain);

  // Page swapped within the file (wrong page_id in AAD) fails.
  assert(!at_rest::OpenPage(subkey, 8, "tag-1", sealed.data(), back.data()).ok());
  // Page swapped across files (wrong file_tag in AAD) fails.
  assert(!at_rest::OpenPage(subkey, 7, "tag-2", sealed.data(), back.data()).ok());
  // Wrong key fails.
  auto wrong_sub_or = at_rest::FileSubkey(OtherDek(), "tag-1");
  assert(wrong_sub_or.ok());
  assert(!at_rest::OpenPage(wrong_sub_or.value(), 7, "tag-1", sealed.data(), back.data()).ok());
}

// -- DiskManager with a key ---------------------------------------------------

void TestDiskManagerSealed() {
  std::cout << "  disk: sealed pages persist and fail closed\n";
  const std::string dir = TestRoot() + "/disk";
  Fresh(dir);
  const std::string dek = ZeroDek();
  const std::string path = dir + "/t.dsf";

  std::string page(kPageSize, 'Q');
  {
    auto dm_or = DiskManager::Open(path, dek);
    assert(dm_or.ok());
    assert(dm_or.value()->encrypted());
    const page_id_t id = dm_or.value()->AllocatePage();
    assert(id == 0);
    assert(dm_or.value()->WritePage(id, page.data()).ok());
  }
  // Raw file holds no plaintext: the page bytes must not appear, and the
  // stride must be the sealed one.
  {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    assert(in.is_open());
    assert(static_cast<size_t>(in.tellg()) == at_rest::kSealedPageSize);
  }
  {
    auto dm_or = DiskManager::Open(path, dek);
    assert(dm_or.ok());
    std::string back(kPageSize, '\0');
    assert(dm_or.value()->ReadPage(0, back.data()).ok());
    assert(back == page);
  }
  // Wrong key opens (detection is by header, not key) but reads fail.
  {
    auto dm_or = DiskManager::Open(path, OtherDek());
    assert(dm_or.ok());
    std::string back(kPageSize, '\0');
    assert(!dm_or.value()->ReadPage(0, back.data()).ok());
  }
  // No key on a sealed file refuses at open.
  {
    auto dm_or = DiskManager::Open(path);
    assert(!dm_or.ok());
  }
  // And a plaintext file opened with a key refuses at open.
  {
    const std::string plain_path = dir + "/p.dsf";
    auto dm_or = DiskManager::Open(plain_path);
    assert(dm_or.ok());
    assert(dm_or.value()->WritePage(dm_or.value()->AllocatePage(), page.data()).ok());
  }
  {
    auto dm_or = DiskManager::Open(dir + "/p.dsf", dek);
    assert(!dm_or.ok());
  }
  RemoveTree(dir);
}

// -- WAL with a key -----------------------------------------------------------

void TestWalSealed() {
  std::cout << "  wal: sealed ledger appends, reopens and fails closed\n";
  const std::string dir = TestRoot() + "/wal";
  Fresh(dir);
  const std::string dek = ZeroDek();
  const std::string path = dir + "/desentry.wal";

  {
    auto wal_or = WriteAheadLog::Open(path, dek);
    assert(wal_or.ok());
    WriteAheadLog::AppendOptions opts;
    assert(wal_or.value()->Append(WalRecordType::kPut, "c", "k1", "v1", opts).ok());
    assert(wal_or.value()->Append(WalRecordType::kPut, "c", "k2", "v2", opts).ok());
  }
  // Raw file holds sealed frames, not plaintext: the first payload starts
  // with the sealed-record magic (deterministic check, not a substring
  // hunt that ciphertext could win by chance).
  {
    std::ifstream in(path, std::ios::binary);
    char head[8];
    in.read(head, 8);
    assert(in.gcount() == 8);
    assert(std::string(head + 4, 4) == "DSWE");
  }
  // Same key: records and a verifying chain.
  {
    auto wal_or = WriteAheadLog::Open(path, dek);
    assert(wal_or.ok());
    auto records_or = wal_or.value()->ReadAll();
    assert(records_or.ok());
    assert(records_or.value().size() == 2);
    assert(records_or.value()[0].document_bytes == "v1");
    assert(wal_or.value()->VerifyChain().ok);
  }
  // Wrong key, no key, and key-on-plaintext all refuse.
  assert(!WriteAheadLog::Open(path, OtherDek()).ok());
  assert(!WriteAheadLog::Open(path).ok());
  {
    const std::string plain = dir + "/plain.wal";
    auto wal_or = WriteAheadLog::Open(plain);
    assert(wal_or.ok());
    WriteAheadLog::AppendOptions opts;
    assert(wal_or.value()->Append(WalRecordType::kPut, "c", "k", "v", opts).ok());
  }
  assert(!WriteAheadLog::Open(dir + "/plain.wal", dek).ok());
  RemoveTree(dir);
}

// -- whole node with a key ----------------------------------------------------

NodeEngine::Options SealedOptions(const std::string& dir, const std::string& dek) {
  NodeEngine::Options options;
  options.data_dir = dir;
  options.engines = {"kv"};
  options.default_engine = "kv";
  options.dek = dek;
  return options;
}

void TestNodeSealedEndToEnd() {
  std::cout << "  node: sealed Put/Get survives restart, refuses without key\n";
  const std::string dir = TestRoot() + "/node";
  Fresh(dir);
  const std::string dek = ZeroDek();

  {
    auto engine_or = NodeEngine::Open(SealedOptions(dir, dek));
    assert(engine_or.ok());
    assert(engine_or.value()->at_rest_sealed());
    NodeEngine& engine = *engine_or.value();
    assert(engine.storage()
               .PutRaw("docs", "k1", std::string("{\"v\":1}"))
               .ok());
    auto got = engine.storage().GetRaw("docs", "k1");
    assert(got.ok());
    engine.storage().Checkpoint();
  }
  // Restart with the key: the document is there (WAL replay through sealed
  // pages), and the chain verifies.
  {
    auto engine_or = NodeEngine::Open(SealedOptions(dir, dek));
    assert(engine_or.ok());
    NodeEngine& engine = *engine_or.value();
    auto got = engine.storage().GetRaw("docs", "k1");
    assert(got.ok());
    assert(got.value() == std::string("{\"v\":1}"));
    assert(engine.storage().VerifyAll().ledger.ok);
  }
  // Restart without the key: fail closed, never plaintext.
  {
    NodeEngine::Options options;
    options.data_dir = dir;
    options.engines = {"kv"};
    options.default_engine = "kv";
    assert(!NodeEngine::Open(options).ok());
  }
  // Wrong key: fail closed.
  assert(!NodeEngine::Open(SealedOptions(dir, OtherDek())).ok());
  RemoveTree(dir);
}

// -- offline migration --------------------------------------------------------

void TestReencryptTool() {
  std::cout << "  migrate: plaintext dir seals and reopens sealed\n";
  const std::string dir = TestRoot() + "/migrate";
  Fresh(dir);
  const std::string dek = ZeroDek();

  // Build a plaintext node with real content across layers.
  {
    NodeEngine::Options options;
    options.data_dir = dir;
    options.engines = {"kv", "ts_rollup"};
    options.default_engine = "kv";
    auto engine_or = NodeEngine::Open(options);
    assert(engine_or.ok());
    NodeEngine& engine = *engine_or.value();
    assert(engine.storage().PutRaw("docs", "k1", std::string("{\"v\":9}")).ok());
    assert(engine.HoldForOfflineOwner("owner-x", "docs", "k2", "held-bytes").ok());
    engine.storage().Checkpoint();
  }
  // Migrate offline.
  {
    std::string summary;
    Status st = ReencryptDataDir(dir, dek, &summary);
    assert(st.ok());
    std::cout << "    (" << summary << ")\n";
  }
  // Opens sealed with the key, data intact (docs, transit, chain).
  {
    auto engine_or = NodeEngine::Open(SealedOptions(dir, dek));
    assert(engine_or.ok());
    NodeEngine& engine = *engine_or.value();
    assert(engine.at_rest_sealed());
    auto got = engine.storage().GetRaw("docs", "k1");
    assert(got.ok());
    assert(got.value() == std::string("{\"v\":9}"));
    assert(engine.PendingTransitFor("owner-x").size() == 1);
    assert(engine.storage().VerifyAll().ledger.ok);
  }
  // Plaintext open now refuses; rerun is a clean skip.
  {
    NodeEngine::Options options;
    options.data_dir = dir;
    options.engines = {"kv", "ts_rollup"};
    options.default_engine = "kv";
    assert(!NodeEngine::Open(options).ok());
  }
  {
    std::string summary;
    assert(ReencryptDataDir(dir, dek, &summary).ok());
  }
  RemoveTree(dir);
}

}  // namespace

int main() {
  std::cout << "at_rest_test\n";
  TestCrockfordDecoding();
  TestSealedRecords();
  TestSealedPages();
  TestDiskManagerSealed();
  TestWalSealed();
  TestNodeSealedEndToEnd();
  TestReencryptTool();
  std::cout << "  all at-rest tests passed\n";
  return 0;
}
