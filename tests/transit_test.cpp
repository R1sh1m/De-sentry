// Transit test suite: coordinated holder sets, striping, and the capacity gate.
//
// Workstream B replaced "every online replica holds for every offline owner"
// with deterministic top-H holder sets, striped envelopes for big documents,
// and a capacity gate that refuses rather than overfills. The properties
// pinned here:
//
//   * **Selection is deterministic and capped.** Same (owner, key, candidates)
//     picks the same holders on every call and every node; at most
//     max_holders; chunk rotation spreads striped chunks across holders.
//   * **Chunk hashes are unambiguous.** Per-chunk hashes differ per index and
//     from the whole-doc hash, so intent/claimed pairs can never
//     cross-match between a chunked and a whole hold of the same key.
//   * **The hold is all-or-nothing.** A full holder refuses the whole
//     document with OutOfSpace *before* storing anything -- a partial hold
//     would strand unclaimed intents that pin the checkpoint gate.
//   * **The intent tail survives the ledger.** Routing metadata round-trips
//     through append/read, the chain still verifies (the tail is outside the
//     signed content), and non-transit records are byte-identical to before.
//   * **The wire codec degrades honestly.** New fields round-trip; an entry
//     from an old holder (no tail) decodes to whole-document defaults.
//
// Plain assert(), no framework (-UNDEBUG keeps them live in every build type).

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "desentry/common/platform.h"
#include "desentry/crdt/hlc.h"
#include "desentry/engine/node_engine.h"
#include "desentry/ledger/transit_store.h"
#include "desentry/net/placement.h"
#include "desentry/net/wire_protocol.h"
#include "desentry/storage/wal.h"

using namespace desentry;

namespace {

std::string TestRoot() { return AppDataDir() + "/desentry_test_transit"; }

void Fresh(const std::string& path) {
  RemoveTree(path);
  MakeDirs(path);
}

HybridLogicalClock g_clock("transit-test");

// -- chunk math ---------------------------------------------------------------

void TestChunkCount() {
  std::cout << "  chunks: count math\n";
  assert(TransitChunkCount(0, 262144) == 1);  // empty doc still names one chunk
  assert(TransitChunkCount(1, 262144) == 1);
  assert(TransitChunkCount(262144, 262144) == 1);
  assert(TransitChunkCount(262145, 262144) == 2);
  assert(TransitChunkCount(1024 * 1024, 262144) == 4);
  assert(TransitChunkCount(100, 0) == 1);  // zero cap falls back to the default
}

void TestChunkKeyHash() {
  std::cout << "  chunks: key hashes are unambiguous\n";
  const std::string doc_hash = LedgerKeyHash("metrics", "sensor-1");
  assert(doc_hash.size() == 32);
  const std::string c0 = TransitChunkKeyHash(doc_hash, 0);
  const std::string c1 = TransitChunkKeyHash(doc_hash, 1);
  assert(c0.size() == 32 && c1.size() == 32);
  // Deterministic.
  assert(TransitChunkKeyHash(doc_hash, 0) == c0);
  // Distinct per index...
  assert(c0 != c1);
  // ...and distinct from the whole-document hash, so a chunked intent can
  // never match a whole-document claim for the same key.
  assert(c0 != doc_hash);
}

// -- holder selection ---------------------------------------------------------

std::vector<std::string> Candidates(size_t n) {
  std::vector<std::string> out;
  for (size_t i = 0; i < n; ++i) out.push_back("node-" + std::to_string(i));
  return out;
}

void TestHolderSelection() {
  std::cout << "  holders: deterministic, capped selection\n";
  const auto cands = Candidates(10);
  const std::string owner = "offline-node";
  const std::string key_hash = LedgerKeyHash("metrics", "sensor-1");

  // Deterministic: same inputs, same holders, every time.
  auto h1 = SelectTransitHolders(owner, key_hash, cands, 3);
  auto h2 = SelectTransitHolders(owner, key_hash, cands, 3);
  assert(h1 == h2);
  assert(h1.size() == 3);

  // Distinct holders -- three copies on one disk is not redundancy.
  assert(std::set<std::string>(h1.begin(), h1.end()).size() == 3);

  // Capped at max_holders even with many candidates...
  assert(SelectTransitHolders(owner, key_hash, cands, 1).size() == 1);
  // ...and capped at the candidate count when fewer exist.
  assert(SelectTransitHolders(owner, key_hash, Candidates(2), 3).size() == 2);
  // Degenerate inputs hold nothing rather than crashing.
  assert(SelectTransitHolders(owner, key_hash, {}, 3).empty());
  assert(SelectTransitHolders(owner, key_hash, cands, 0).empty());

  // Different keys spread across different holders (not a proof of
  // uniformity, just that the selection is key-dependent).
  std::set<std::string> seen;
  for (int i = 0; i < 20; ++i) {
    auto h = SelectTransitHolders(owner, LedgerKeyHash("metrics", "k" + std::to_string(i)),
                                  cands, 3);
    seen.insert(h.begin(), h.end());
  }
  assert(seen.size() > 3);

  // Chunk rotation: striped chunks of one document spread instead of
  // stacking on the same top-H.
  std::set<std::string> spread;
  for (uint32_t c = 0; c < 6; ++c) {
    auto h = SelectTransitHolders(owner, key_hash, cands, 2, c);
    spread.insert(h.begin(), h.end());
  }
  assert(spread.size() > 2);
}

// -- ledger intent tail ---------------------------------------------------------

void TestIntentTailRoundTrip() {
  std::cout << "  ledger: intent routing tail round-trips outside the signed content\n";
  const std::string dir = TestRoot() + "/tail";
  Fresh(dir);

  auto wal_or = WriteAheadLog::Open(dir + "/ledger.wal");
  assert(wal_or.ok());
  auto wal = std::move(wal_or.value());

  WriteAheadLog::AppendOptions put_opts;
  put_opts.hlc = g_clock.Now();
  assert(wal->Append(WalRecordType::kPut, "things", "k1", "bytes", put_opts).ok());

  WriteAheadLog::AppendOptions intent_opts;
  intent_opts.hlc = g_clock.Now();
  intent_opts.transit_holder = "holder-node";
  intent_opts.transit_size_bytes = 300000;
  intent_opts.transit_chunk_index = 2;
  intent_opts.transit_chunk_total = 3;
  const std::string doc_hash = LedgerKeyHash("metrics", "sensor-1");
  const std::string chunk_hash = TransitChunkKeyHash(doc_hash, 2);
  // The intent names the chunk hash as its key, exactly as the hold path does.
  assert(wal->Append(WalRecordType::kTransitIntent, "offline-node", "sensor-1", chunk_hash,
                     intent_opts)
             .ok());

  auto entries_or = wal->ReadAll();
  assert(entries_or.ok());
  assert(entries_or.value().size() == 2);

  // Non-transit records are untouched: no holder, whole-document defaults.
  const WalRecord& put = entries_or.value()[0];
  assert(put.transit_holder.empty());
  assert(put.transit_size_bytes == 0);
  assert(put.transit_chunk_index == 0);
  assert(put.transit_chunk_total == 1);

  // The intent carries its routing metadata...
  const WalRecord& intent = entries_or.value()[1];
  assert(intent.transit_holder == "holder-node");
  assert(intent.transit_size_bytes == 300000);
  assert(intent.transit_chunk_index == 2);
  assert(intent.transit_chunk_total == 3);
  assert(intent.key_hash == LedgerKeyHash("offline-node", "sensor-1"));

  // ...and the chain still verifies: the tail rides outside the signed
  // content, so old readers verify these records exactly as before.
  const auto verified = wal->VerifyChain();
  assert(verified.ok);
  assert(verified.entries_checked == 2);

  RemoveTree(dir);
}

// -- engine hold path -----------------------------------------------------------

NodeEngine::Options EngineOptions(const std::string& dir) {
  NodeEngine::Options options;
  options.data_dir = dir;
  options.engines = {"kv"};
  options.default_engine = "kv";
  return options;
}

size_t CountIntents(NodeEngine& engine) {
  auto entries_or = engine.LedgerEntries(0, engine.LedgerTip().entry_id);
  assert(entries_or.ok());
  size_t n = 0;
  for (const WalRecord& rec : entries_or.value()) {
    if (rec.type == WalRecordType::kTransitIntent) ++n;
  }
  return n;
}

void TestWholeDocHold() {
  std::cout << "  engine: small hold is one envelope, one intent\n";
  const std::string dir = TestRoot() + "/whole";
  Fresh(dir);

  NodeEngine::Options options = EngineOptions(dir);
  auto engine_or = NodeEngine::Open(options);
  assert(engine_or.ok());
  auto& engine = *engine_or.value();

  const std::string owner = "owner-node-aaa";
  assert(engine.HoldForOfflineOwner(owner, "metrics", "s1", "small-bytes").ok());

  auto pending = engine.PendingTransitFor(owner);
  assert(pending.size() == 1);
  assert(pending[0].chunk_total == 1);
  assert(pending[0].chunk_index == 0);
  assert(pending[0].doc_size_bytes == std::string("small-bytes").size());
  assert(pending[0].key_hash == LedgerKeyHash("metrics", "s1"));
  assert(pending[0].holder_node == engine.identity().node_id());
  assert(CountIntents(engine) == 1);

  // The intent names this holder, so the owner learns whom to ask.
  auto entries_or = engine.LedgerEntries(0, engine.LedgerTip().entry_id);
  assert(entries_or.value().back().transit_holder == engine.identity().node_id());

  RemoveTree(dir);
}

void TestStripedHold() {
  std::cout << "  engine: big hold stripes into per-chunk envelopes and intents\n";
  const std::string dir = TestRoot() + "/striped";
  Fresh(dir);

  NodeEngine::Options options = EngineOptions(dir);
  options.transit_chunk_bytes = 64 * 1024;
  auto engine_or = NodeEngine::Open(options);
  assert(engine_or.ok());
  auto& engine = *engine_or.value();

  const std::string big(300 * 1024, 'x');  // 300KiB -> 5 chunks of 64KiB
  const std::string owner = "owner-node-bbb";
  assert(engine.HoldForOfflineOwner(owner, "metrics", "big", big).ok());

  auto pending = engine.PendingTransitFor(owner);
  assert(pending.size() == 5);
  const std::string doc_hash = LedgerKeyHash("metrics", "big");
  std::set<std::string> hashes;
  size_t total_bytes = 0;
  for (const TransitEnvelope& env : pending) {
    assert(env.chunk_total == 5);
    assert(env.doc_size_bytes == big.size());
    assert(env.key_hash == TransitChunkKeyHash(doc_hash, env.chunk_index));
    hashes.insert(env.key_hash);
    total_bytes += env.encoded_doc.size();
  }
  // Five distinct chunk hashes covering every byte exactly once.
  assert(hashes.size() == 5);
  assert(total_bytes == big.size());
  assert(CountIntents(engine) == 5);

  RemoveTree(dir);
}

void TestCapacityGateIsAllOrNothing() {
  std::cout << "  engine: a full holder refuses the whole document, storing nothing\n";
  const std::string dir = TestRoot() + "/full";
  Fresh(dir);

  NodeEngine::Options options = EngineOptions(dir);
  options.transit_chunk_bytes = 64 * 1024;
  options.transit_budget_bytes = 100;  // room for almost nothing
  auto engine_or = NodeEngine::Open(options);
  assert(engine_or.ok());
  auto& engine = *engine_or.value();

  const std::string owner = "owner-node-ccc";
  const size_t intents_before = CountIntents(engine);
  const std::string big(300 * 1024, 'y');
  Status st = engine.HoldForOfflineOwner(owner, "metrics", "big", big);
  assert(!st.ok());
  assert(st.code() == StatusCode::kOutOfSpace);
  // All-or-nothing: no envelope stored, no intent recorded. A partial hold
  // would strand unclaimed intents that pin the checkpoint gate.
  assert(engine.PendingTransitFor(owner).empty());
  assert(CountIntents(engine) == intents_before);

  // ...while a document that fits still holds.
  assert(engine.HoldForOfflineOwner(owner, "metrics", "tiny", "z").ok());
  assert(engine.PendingTransitFor(owner).size() == 1);

  RemoveTree(dir);
}

void TestChunkSubsetHold() {
  std::cout << "  engine: holder selection assigns chunks, engine holds only those\n";
  const std::string dir = TestRoot() + "/subset";
  Fresh(dir);

  NodeEngine::Options options = EngineOptions(dir);
  options.transit_chunk_bytes = 64 * 1024;
  auto engine_or = NodeEngine::Open(options);
  assert(engine_or.ok());
  auto& engine = *engine_or.value();

  const std::string big(300 * 1024, 'w');  // 5 chunks
  const std::string owner = "owner-node-ddd";
  const std::vector<uint32_t> mine = {1, 3};
  assert(engine.HoldForOfflineOwner(owner, "metrics", "big", big, &mine).ok());

  auto pending = engine.PendingTransitFor(owner);
  assert(pending.size() == 2);
  std::set<uint32_t> indexes;
  for (const TransitEnvelope& env : pending) {
    assert(env.chunk_total == 5);
    indexes.insert(env.chunk_index);
  }
  assert(indexes == std::set<uint32_t>({1, 3}));
  assert(CountIntents(engine) == 2);

  // An out-of-range assignment is a caller bug: rejected, nothing stored.
  const std::vector<uint32_t> bad = {9};
  assert(!engine.HoldForOfflineOwner(owner, "metrics", "other", "qq", &bad).ok());
  assert(engine.PendingTransitFor(owner).size() == 2);

  RemoveTree(dir);
}

// -- wire codec -----------------------------------------------------------------

void TestTransitWireCodec() {
  std::cout << "  wire: transit entries round-trip, old entries decode to defaults\n";
  TransitEntry e;
  e.collection = "metrics";
  e.key = "big";
  e.key_hash = LedgerKeyHash("metrics", "big");
  e.encoded_doc = "chunk-bytes";
  e.intent_lsn = 12;
  e.holder_node = "holder";
  e.doc_size_bytes = 300000;
  e.chunk_index = 2;
  e.chunk_total = 5;

  TransitResponsePayload p;
  p.entries.push_back(e);
  p.truncated = true;
  p.next_offset = 7;
  TransitResponsePayload back = TransitResponsePayload::Decode(p.Encode());
  assert(back.entries.size() == 1);
  assert(back.entries[0].doc_size_bytes == 300000);
  assert(back.entries[0].chunk_index == 2);
  assert(back.entries[0].chunk_total == 5);
  assert(back.entries[0].holder_node == "holder");
  assert(back.truncated);
  assert(back.next_offset == 7);

  // An old holder's entry (no striping tail) decodes to whole-document
  // defaults rather than failing.
  std::string old_entry;
  {
    // Hand-encoded: collection, key, key_hash, encoded_doc, intent_lsn,
    // holder_node -- then nothing.
    const std::string parts[] = {"c", "k", std::string(32, 'h'), "bytes", "holder"};
    // length-prefixed blobs: u32 len + raw; intent_lsn: i64.
    auto blob = [](const std::string& s) {
      uint32_t len = static_cast<uint32_t>(s.size());
      std::string out(reinterpret_cast<const char*>(&len), 4);
      out += s;
      return out;
    };
    std::string entry_body = blob("c") + blob("k") + blob(std::string(32, 'h')) + blob("bytes");
    int64_t lsn = 3;
    entry_body.append(reinterpret_cast<const char*>(&lsn), 8);
    entry_body += blob("holder");
    uint32_t n = 1;
    old_entry.append(reinterpret_cast<const char*>(&n), 4);
    old_entry += entry_body;
    old_entry.push_back(0);  // truncated = false
  }
  TransitResponsePayload old_back = TransitResponsePayload::Decode(old_entry);
  assert(old_back.entries.size() == 1);
  assert(old_back.entries[0].chunk_total == 1);
  assert(old_back.entries[0].chunk_index == 0);
  assert(old_back.entries[0].doc_size_bytes == 0);
  assert(!old_back.truncated);

  // An empty query is offset 0: old peers send no payload at all.
  assert(TransitQueryPayload::Decode("").offset == 0);
  TransitQueryPayload q;
  q.offset = 256;
  assert(TransitQueryPayload::Decode(q.Encode()).offset == 256);
}

void TestLedgerDeltaVersions() {
  std::cout << "  wire: ledger deltas version cleanly across the routing tail\n";
  LedgerEntrySummary e;
  e.entry_id = 9;
  e.operation = 4;  // TRANSIT_INTENT
  e.key_hash = std::string(32, 'k');
  e.entry_hash = std::string(32, 'e');
  e.prev_hash = std::string(32, 'p');
  e.origin_node_id = "origin";
  e.collection = "owner-node";
  e.key = "sensor-1";
  e.transit_holder = "holder";
  e.transit_size_bytes = 300000;
  e.transit_chunk_index = 1;
  e.transit_chunk_total = 4;

  // v2 round-trips with its routing tail...
  LedgerDeltaPayload p;
  p.entries.push_back(e);
  p.entries.push_back(e);
  LedgerDeltaPayload back = LedgerDeltaPayload::Decode(p.Encode());
  assert(back.entries.size() == 2);
  assert(back.entries[0].transit_holder == "holder");
  assert(back.entries[0].transit_size_bytes == 300000);
  assert(back.entries[0].transit_chunk_index == 1);
  assert(back.entries[0].transit_chunk_total == 4);

  // ...while a v1 delta (entries end at key, no magic) decodes to
  // whole-document defaults instead of misparsing entry 2's bytes as
  // entry 1's tail. This is the case trailing-field tolerance gets wrong
  // inside repeated elements, and why the magic branch exists.
  std::string v1;
  {
    v1.push_back(0);  // hashes_only = false
    uint32_t n = 2;
    v1.append(reinterpret_cast<const char*>(&n), 4);
    auto blob = [](const std::string& s) {
      uint32_t len = static_cast<uint32_t>(s.size());
      std::string out(reinterpret_cast<const char*>(&len), 4);
      out += s;
      return out;
    };
    for (int i = 0; i < 2; ++i) {
      int64_t id = 9;
      v1.append(reinterpret_cast<const char*>(&id), 8);
      v1.push_back(4);
      v1 += blob(std::string(32, 'k')) + blob(std::string(32, 'e')) + blob(std::string(32, 'p'));
      v1 += blob("origin") + blob("");
      int64_t hlc = 0;
      v1.append(reinterpret_cast<const char*>(&hlc), 8);
      uint32_t logical = 0;
      v1.append(reinterpret_cast<const char*>(&logical), 4);
      v1 += blob("owner-node") + blob("sensor-1");
    }
  }
  LedgerDeltaPayload old_back = LedgerDeltaPayload::Decode(v1);
  assert(old_back.entries.size() == 2);
  assert(old_back.entries[0].entry_id == 9);
  assert(old_back.entries[0].collection == "owner-node");
  assert(old_back.entries[0].transit_holder.empty());
  assert(old_back.entries[0].transit_chunk_total == 1);
  assert(old_back.entries[1].entry_id == 9);
}

}  // namespace

int main() {
  std::cout << "transit_test\n";
  TestChunkCount();
  TestChunkKeyHash();
  TestHolderSelection();
  TestIntentTailRoundTrip();
  TestWholeDocHold();
  TestStripedHold();
  TestCapacityGateIsAllOrNothing();
  TestChunkSubsetHold();
  TestTransitWireCodec();
  TestLedgerDeltaVersions();
  std::cout << "  all transit tests passed\n";
  return 0;
}
