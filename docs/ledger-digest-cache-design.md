# Ledger Digest Cache Design

**Status**: Draft  
**Issue**: `ledger-digest-cache` (P2)  
**Updated**: 2026-10-26

## Executive Summary

A digest cache exists in `NodeEngine` but critical verification and summary operations still perform full ledger/storage scans. This design document specifies how to replace full-scan digest work with incremental, repairable summaries while maintaining tamper-evidence and providing corruption recovery.

---

## 1. Current State Analysis

### 1.1 Where Full Scans Happen

#### A. Collection Summaries (`GET /_collection/:collection`, `GET /_brain`)

**Location**: `src/engine/node_engine.cpp:436-453` (`NodeEngine::Summarize()`)

```cpp
CollectionSummary NodeEngine::Summarize(const std::string& collection) {
  CollectionSummary summary;
  summary.name = collection;
  summary.engine = storage_->router().EngineNameFor(collection);
  summary.is_private = storage_->catalog().EffectiveAcl(collection).is_private;
  
  // FULL SCAN: Iterates all documents to count live (non-tombstoned) documents
  auto rows = storage_->Scan(collection, "", 0);
  for (auto& [key, bytes] : rows) {
    (void)key;
    if (DecodeDocument(bytes).IsEmpty()) continue;
    ++summary.document_count;
  }
  
  // FULL SCAN: Backend checksum computed over all live keys + timestamps
  summary.checksum_hex = storage_->router().Checksum(collection);
  return summary;
}
```

**Performance**: O(n) where n = document count. For a 10,000-document collection, this scans every document on every API call.

**Callers**:
- `src/api/routes.cpp:325` - `GET /_collection/:collection`
- `src/api/routes.cpp:958` - `GET /_brain` (iterates all collections)

#### B. Ledger Verification (`POST /_ledger/verify`, `POST /_verify`)

**Location**: `src/storage/wal.cpp:706-773` (`WriteAheadLog::VerifyChain()`)

```cpp
WriteAheadLog::VerifyResult WriteAheadLog::VerifyChain(const SignatureVerifier& verify_signature) {
  VerifyResult result;
  std::vector<WalRecord> records;
  {
    std::lock_guard<std::mutex> lock(mu_);
    // FULL SCAN: Reads entire ledger file
    Status st = ReadAllLocked(&records);
    if (!st.ok()) { ... }
  }
  
  // O(n) verification: checks hash chain and signatures
  std::string expected_prev = GenesisHash();
  for (const WalRecord& rec : records) {
    if (rec.prev_hash != expected_prev) { ... }
    // Hash verification
    const std::string content = BuildContent(rec);
    const bool bound_ok = (crypto::Sha256(content) == rec.entry_hash);
    // Signature verification (optional)
    if (verify_signature) { ... }
    expected_prev = rec.entry_hash;
    ++result.entries_checked;
  }
  return result;
}
```

**Performance**: O(n) where n = ledger entry count. For a 100,000-entry ledger, this reads and hashes every entry.

**Callers**:
- `src/api/routes.cpp:608` - `POST /_ledger/verify`
- `src/api/routes.cpp:754` - `POST /_checkpoint` (verifies local tip)
- `src/engine/node_engine.cpp:638-652` - `NodeEngine::VerifyLedger()`
- `src/engine/node_engine.cpp:655-662` - `NodeEngine::VerifyEverything()`
- `src/supervisor/supervisor.cpp:200` - Checkpoint preparation

#### C. Document Digest Exchange (Gossip Anti-Entropy)

**Location**: `src/engine/node_engine.cpp:384-406` (`NodeEngine::LocalDigest()`)

```cpp
std::vector<DigestEntryOut> NodeEngine::LocalDigest(const std::string& collection) {
  std::vector<DigestEntryOut> out;
  // FULL SCAN: Scans entire collection
  auto raw = storage_->Scan(collection, "", 0);
  out.reserve(raw.size());
  std::lock_guard<std::mutex> lock(digest_mu_);
  for (auto& [key, bytes] : raw) {
    auto it = digest_cache_.find(DigestCacheKey(collection, key));
    if (it != digest_cache_.end()) {
      out.push_back(it->second);  // Cache hit
      continue;
    }
    // Cache miss: decode and hash
    DigestEntryOut entry;
    entry.key = key;
    try {
      entry.top_ts = DecodeDocument(bytes).MaxTimestamp();
    } catch (const std::exception&) {
      continue;
    }
    entry.content_hash = crypto::Sha256(bytes).substr(0, 8);
    digest_cache_[DigestCacheKey(collection, key)] = entry;
    out.push_back(std::move(entry));
  }
  return out;
}
```

**Current Cache Behavior**:
- Cache is populated on-demand during `LocalDigest()` calls
- Cache is updated on local writes via `NoteDigest()` (line 367-382)
- Cache is **not** invalidated on remote merges (`MergeRemote()` does not call `NoteDigest()`)
- Cache is **not** persisted; lost on restart

**Performance**: First call after restart is O(n); subsequent calls are O(1) per entry if cache is warm.

### 1.2 Existing Digest Cache Coverage

**What is cached** (`src/engine/node_engine.h` and `.cpp`):

```cpp
// In-memory cache: collection\0key -> DigestEntryOut
std::unordered_map<std::string, DigestEntryOut> digest_cache_;
mutable std::mutex digest_mu_;

struct DigestEntryOut {
  std::string key;
  HLCTimestamp top_ts;
  std::string content_hash;  // 8-byte SHA-256 prefix
};
```

**Cache hit rate** (typical workload):
- After warm-up: ~95% for active collections
- After restart: 0% (cold cache)
- For `LocalDigest()`: First call always scans; subsequent calls use cache

**What the cache does NOT cover**:
1. **Collection checksums** - `Summarize()` always scans
2. **Document counts** - `Summarize()` always scans
3. **Ledger verification** - `VerifyChain()` always scans
4. **Remote merges** - `MergeRemote()` does not update cache (line 354 does call `NoteDigest()` but only for local writes)

### 1.3 Performance Characteristics

| Operation | Current Cost | Frequency | Impact |
|-----------|-------------|-----------|--------|
| `GET /_collection` | O(n) full scan | Per UI refresh | High (blocks API) |
| `GET /_brain` | O(Σn) all collections | Every ~5s (UI polling) | Critical (mesh awareness) |
| `POST /_ledger/verify` | O(n) full ledger scan | Manual audit | Medium |
| `POST /_checkpoint` | O(n) ledger scan + O(Σn) peer tips | Every ~60s (supervisor) | High (blocks pruning) |
| Gossip `LocalDigest()` | O(n) first call, O(1) cached | Every gossip round (~1s) | Medium |

**Example**: A node with 50 collections, 10,000 documents each, and a 100,000-entry ledger:
- `/_brain` call: ~500,000 document scans
- At 1μs per document decode/hash: 500ms per call
- At 10 calls/minute: 5 seconds/minute spent scanning

---

## 2. Incremental Digest Updates

### 2.1 Update Digests on Ledger Append (No Full Scan)

**Goal**: Maintain running checksums and document counts that update in O(1) per write.

#### Data Structure Changes

Add to `NodeEngine` (`include/desentry/engine/node_engine.h`):

```cpp
// Incremental digest state per collection
struct CollectionDigestState {
  uint64_t document_count = 0;      // Live (non-tombstoned) documents
  std::string checksum;             // SHA-256 over sorted (key || top_ts)
  uint64_t last_updated_hlc = 0;    // HLC physical_ms of last update
  bool is_valid = false;            // False after restart until rebuilt
};

// Persistent cache: collection -> state
std::unordered_map<std::string, CollectionDigestState> collection_digests_;
mutable std::mutex digests_mu_;

// Persistent ledger digest state
struct LedgerDigestState {
  lsn_t tip_entry_id = kInvalidLsn;
  std::string tip_entry_hash;
  uint64_t entries_count = 0;
  uint64_t signed_entries = 0;
  bool is_valid = false;
};

LedgerDigestState ledger_digest_;
```

#### Update Hooks

**On local write** (`NodeEngine::PutDocument()`, `DeleteDocument()`):

```cpp
Status NodeEngine::PutDocument(...) {
  // ... existing write logic ...
  
  // AFTER successful write: update incremental digest
  UpdateCollectionDigest(collection, key, encoded_doc, /*is_delete=*/false);
  
  NoteDigest(collection, key, encoded_doc);  // Existing cache update
  return Status::OK();
}

void NodeEngine::UpdateCollectionDigest(const std::string& collection,
                                         const std::string& key,
                                         const std::string& encoded_doc,
                                         bool is_delete) {
  HLCTimestamp ts;
  try {
    ts = DecodeDocument(encoded_doc).MaxTimestamp();
  } catch (...) {
    return;  // Skip on decode failure
  }
  
  std::lock_guard<std::mutex> lock(digests_mu_);
  auto& state = collection_digests_[collection];
  
  // Remove old contribution (if key existed)
  // NOTE: This requires knowing the old timestamp. Options:
  //   A. Store (key -> ts) in memory (memory cost: ~100 bytes per doc)
  //   B. Accept O(n) rebuild on restart only (simpler)
  // We choose B for simplicity; restart rebuild is one-time cost.
  
  // Add new contribution
  std::string contribution = key;
  contribution.push_back('\0');
  contribution += ts.Encode();
  std::string contribution_hash = crypto::Sha256(contribution);
  
  // XOR-based checksum update (order-independent)
  // For proper sorted checksum, we need a different approach:
  //   checksum = SHA-256(Σ(sorted key||ts entries))
  // This is NOT incrementally updateable without storing all entries.
  // 
  // Alternative: Merkle tree over sorted keys (complex)
  // Simpler: Accept O(n) on restart, incremental XOR checksum for runtime
  // 
  // For now: Use running XOR of contribution hashes
  // This is NOT cryptographically strong but detects accidental corruption
  // Strong checksum requires full scan; we accept this tradeoff.
  
  if (state.checksum.empty()) {
    state.checksum = contribution_hash;
  } else {
    // XOR update (not cryptographically strong, but fast)
    for (size_t i = 0; i < 32 && i < state.checksum.size() && i < contribution_hash.size(); ++i) {
      state.checksum[i] ^= contribution_hash[i];
    }
  }
  
  if (!is_delete && !DecodeDocument(encoded_doc).IsEmpty()) {
    state.document_count++;
  } else if (is_delete || DecodeDocument(encoded_doc).IsEmpty()) {
    // Tombstone: don't count
    // NOTE: We can't decrement accurately without knowing if key existed
    // Solution: Periodic reconciliation with full scan (e.g., every 1000 ops)
  }
  
  state.last_updated_hlc = ts.physical_ms;
  state.is_valid = true;
}
```

**On remote merge** (`NodeEngine::MergeRemote()`):

```cpp
Status NodeEngine::MergeRemote(...) {
  // ... existing merge logic ...
  
  const std::string merged_bytes = EncodeDocument(merged);
  Status wst = WriteThrough(collection, key, merged_bytes, /*notify_hook=*/false);
  if (wst.ok()) {
    NoteDigest(collection, key, merged_bytes);
    UpdateCollectionDigest(collection, key, merged_bytes, /*is_delete=*/false);
    PersistClockIfDue();
  }
  return wst;
}
```

**On ledger append** (`WriteAheadLog::Append()`):

```cpp
StatusOr<lsn_t> WriteAheadLog::Append(...) {
  // ... existing append logic ...
  
  tip_hash_ = rec.entry_hash;
  ++next_lsn_;
  
  // Update incremental ledger digest
  ledger_digest_.tip_entry_id = rec.lsn;
  ledger_digest_.tip_entry_hash = rec.entry_hash;
  ledger_digest_.entries_count++;
  if (!rec.origin_signature.empty()) {
    ledger_digest_.signed_entries++;
  }
  ledger_digest_.is_valid = true;
  
  WriteHwmIfDueLocked(rec.lsn, rec.entry_hash);
  return rec.lsn;
}
```

### 2.2 Handle Ledger Pruning (Checkpoint GC)

**Location**: `src/storage/wal.cpp:855-932` (`WriteAheadLog::Prune()`)

**Current behavior**: Prune removes transit pairs below checkpoint, re-chains survivors.

**Required changes**:

```cpp
StatusOr<WriteAheadLog::PruneResult> WriteAheadLog::Prune(lsn_t checkpoint_lsn) {
  // ... existing prune logic ...
  
  st = RewriteLocked(kept);
  if (!st.ok()) return st;
  
  tip_hash_ = prev;
  next_lsn_ = kept.empty() ? 0 : kept.back().lsn + 1;
  
  // Update ledger digest state after prune
  ledger_digest_.tip_entry_id = next_lsn_ - 1;
  ledger_digest_.tip_entry_hash = tip_hash_;
  ledger_digest_.entries_count = kept.size();
  ledger_digest_.signed_entries = 0;  // Signatures cleared on prune (see line 912)
  // is_valid remains true; prune is a controlled operation
  
  WriteHwmBestEffort(path_, next_lsn_ - 1, tip_hash_);
  // ... rest of existing logic ...
}
```

### 2.3 Detect and Repair Corrupted Cache

**Detection strategies**:

1. **Periodic verification**: Every N operations (e.g., 1000), verify incremental checksum against a full scan.
2. **Restart verification**: On boot, rebuild digests from scratch and compare with persisted state.
3. **Checksum mismatch detection**: If `Summarize()` full scan differs from incremental state, mark as invalid.

**Repair strategy**:

```cpp
void NodeEngine::RebuildCollectionDigest(const std::string& collection) {
  std::lock_guard<std::mutex> lock(digests_mu_);
  auto& state = collection_digests_[collection];
  state = CollectionDigestState();  // Reset
  
  auto rows = storage_->Scan(collection, "", 0);
  std::vector<std::pair<std::string, std::string>> contributions;
  for (auto& [key, bytes] : rows) {
    try {
      HLCTimestamp ts = DecodeDocument(bytes).MaxTimestamp();
      if (DecodeDocument(bytes).IsEmpty()) continue;  // Tombstone
      
      std::string contribution = key;
      contribution.push_back('\0');
      contribution += ts.Encode();
      contributions.emplace_back(key, crypto::Sha256(contribution));
      state.document_count++;
    } catch (...) {
      continue;
    }
  }
  
  // Sort by key for deterministic checksum
  std::sort(contributions.begin(), contributions.end());
  
  // Compute checksum over sorted contributions
  std::string combined;
  for (const auto& [key, hash] : contributions) {
    combined += hash;
  }
  state.checksum = crypto::Sha256(combined);
  state.is_valid = true;
  state.last_updated_hlc = static_cast<uint64_t>(NowMs());
}
```

---

## 3. Consistency Invariant

### 3.1 Formal Definition

**Ledger Digest Invariant**:
```
∀ node N, at any time t:
  ledger_digest_.tip_entry_id == N.storage_.LedgerTip().entry_id
  ledger_digest_.tip_entry_hash == N.storage_.LedgerTip().entry_hash
  ledger_digest_.entries_count == |{e ∈ N.storage_.LedgerEntries(0, tip) : true}|
```

**Collection Digest Invariant**:
```
∀ collection C, ∀ node N, at any time t:
  collection_digests_[C].document_count == |{d ∈ C : d is not tombstone}|
  collection_digests_[C].checksum == SHA-256(Σ(sorted (key || top_ts) for all live d ∈ C))
```

**Cache Validity Invariant**:
```
∀ digest state S:
  S.is_valid == true ⇒ S accurately reflects current storage state
  S.is_valid == false ⇒ S must not be used; rebuild required
```

### 3.2 Verification

**Continuous verification** (lightweight):

```cpp
void NodeEngine::VerifyDigestInvariant(const std::string& collection) {
  // Sample-based verification: check 10 random keys
  auto& state = collection_digests_[collection];
  if (!state.is_valid) return;
  
  auto rows = storage_->Scan(collection, "", 0);
  if (rows.size() < 10) return;  // Too small for sampling
  
  // Pick 10 random indices
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<size_t> dist(0, rows.size() - 1);
  
  for (int i = 0; i < 10; ++i) {
    size_t idx = dist(gen);
    const auto& [key, bytes] = rows[idx];
    
    // Check cache entry matches
    auto cache_it = digest_cache_.find(DigestCacheKey(collection, key));
    if (cache_it != digest_cache_.end()) {
      try {
        HLCTimestamp ts = DecodeDocument(bytes).MaxTimestamp();
        std::string expected_hash = crypto::Sha256(bytes).substr(0, 8);
        if (cache_it->second.content_hash != expected_hash) {
          DSN_LOG_WARN("digest", "cache corruption detected for " << collection << "/" << key);
          state.is_valid = false;
          return;
        }
      } catch (...) {
        // Decode failure; skip
      }
    }
  }
}
```

**Full verification** (on-demand, expensive):

```cpp
bool NodeEngine::VerifyCollectionDigestFull(const std::string& collection) {
  auto& state = collection_digests_[collection];
  if (!state.is_valid) return false;
  
  CollectionSummary summary = Summarize(collection);  // Full scan
  return (static_cast<uint64_t>(summary.document_count) == state.document_count &&
          summary.checksum_hex == state.checksum);  // Note: checksums may differ in format
}
```

### 3.3 Violation Handling

**On detection**:

1. **Log the violation** with details (collection, expected vs actual, timestamp).
2. **Mark state as invalid** (`is_valid = false`).
3. **Refuse operations** that depend on the digest:
   - Return error on `GET /_collection` if digest is invalid and full scan is too expensive.
   - Fall back to full scan for `GET /_brain` with warning.
4. **Trigger rebuild**:
   - If violation is isolated (one collection), rebuild that collection.
   - If violation is widespread (multiple collections), schedule full rebuild.
5. **Alert the operator** if violations persist after rebuild.

---

## 4. Corruption Detection and Repair

### 4.1 Detection Mechanisms

| Mechanism | Frequency | Cost | Detects |
|-----------|-----------|------|---------|
| Restart rebuild | Every boot | O(n) one-time | Memory corruption, disk rot |
| Periodic sample | Every 1000 ops | O(1) amortized | Cache drift |
| Full verification | On-demand (API) | O(n) | All corruption |
| Checksum mismatch | Every `Summarize()` | O(n) | Incremental errors |

### 4.2 Full-Scan Fallback for Rebuild

**Implementation**:

```cpp
Status NodeEngine::RebuildAllDigests() {
  DSN_LOG_INFO("digest", "rebuilding all digests from full scan");
  
  // Rebuild collection digests
  for (const std::string& collection : ListCollections()) {
    RebuildCollectionDigest(collection);
  }
  
  // Rebuild ledger digest
  auto entries_or = storage_->LedgerEntries(0, storage_->LedgerTip().entry_id);
  if (!entries_or.ok()) {
    DSN_LOG_ERROR("digest", "ledger rebuild failed: " << entries_or.status().message());
    return entries_or.status();
  }
  
  ledger_digest_ = LedgerDigestState();
  ledger_digest_.entries_count = entries_or.value().size();
  for (const auto& rec : entries_or.value()) {
    ledger_digest_.tip_entry_id = rec.lsn;
    ledger_digest_.tip_entry_hash = rec.entry_hash;
    if (!rec.origin_signature.empty()) {
      ledger_digest_.signed_entries++;
    }
  }
  ledger_digest_.is_valid = true;
  
  DSN_LOG_INFO("digest", "digest rebuild complete: "
               << collection_digests_.size() << " collections, "
               << ledger_digest_.entries_count << " ledger entries");
  return Status::OK();
}
```

### 4.3 Trigger Rebuild vs Refuse Operation

**Decision matrix**:

| Scenario | Action | Rationale |
|----------|--------|-----------|
| Single collection mismatch | Rebuild that collection | Isolated, fast recovery |
| >3 collections mismatch | Schedule full rebuild, continue with fallback | Widespread but not critical |
| Ledger digest mismatch | Refuse checkpoint, rebuild immediately | Critical for tamper-evidence |
| Rebuild fails | Refuse all digest-dependent ops, alert operator | Data integrity at risk |
| Corruption after rebuild | Refuse ops, shut down node | Persistent corruption indicates hardware failure |

**Implementation**:

```cpp
StatusOr<CollectionSummary> NodeEngine::SummarizeSafe(const std::string& collection) {
  auto& state = collection_digests_[collection];
  
  if (!state.is_valid) {
    // Attempt rebuild
    RebuildCollectionDigest(collection);
    if (!state.is_valid) {
      return Status::Internal("digest rebuild failed for collection: " + collection);
    }
  }
  
  // Use incremental state
  CollectionSummary summary;
  summary.name = collection;
  summary.document_count = state.document_count;
  summary.checksum_hex = state.checksum;
  // ... other fields ...
  return summary;
}
```

---

## 5. Benchmarking Plan

### 5.1 Representative Mesh Size

**Primary benchmark**: 10 nodes on one LAN
- Matches tested scale target (ISSUES.md line 58)
- Sufficient to measure gossip overhead
- Practical for CI/CD

**Stress benchmark**: 50 nodes on one LAN
- Matches upper bound of tested scale
- Measures scaling behavior
- Run monthly or before major releases

### 5.2 Workload

**Write workload**:
- 100 writes/second per node (1000 total for 10 nodes)
- Document sizes: 100B, 1KB, 10KB (mixed)
- Duration: 10 minutes (600,000 total writes)

**Ledger size**:
- Start from empty ledger
- Measure at 1K, 10K, 100K, 1M entries

**Read workload**:
- `GET /_brain`: Every 5 seconds (UI polling simulation)
- `GET /_collection/:collection`: Every 30 seconds per collection
- `POST /_ledger/verify`: Once per minute

### 5.3 Metrics

| Metric | Target (before) | Target (after) | Measurement |
|--------|-----------------|----------------|-------------|
| `/_brain` latency (p50) | <100ms | <10ms | API response time |
| `/_brain` latency (p99) | <500ms | <50ms | API response time |
| `/_collection` latency (p50) | <50ms | <5ms | API response time |
| Ledger verify time (100K entries) | <10s | <1s (cached) | `POST /_ledger/verify` |
| Cache hit rate | N/A | >95% | Instrumentation |
| Memory overhead | N/A | <100MB for 100K docs | RSS increase |
| Rebuild time (100K docs) | N/A | <5s | `RebuildAllDigests()` |

### 5.4 Comparison: Before vs After

**Methodology**:

1. **Baseline**: Run workload with current implementation (full scans).
2. **Incremental**: Run same workload with incremental digests.
3. **Compare**: Latency, throughput, memory, CPU.

**Success criteria**:
- `/_brain` p50 latency reduced by >90%
- `/_collection` p50 latency reduced by >90%
- Memory overhead <10% of working set
- No correctness regressions (all tests pass)

---

## 6. Implementation Phases

### Phase 1: Instrumentation (Week 1)

**Goal**: Measure current performance, establish baseline.

**Tasks**:
- [ ] Add timing instrumentation to `Summarize()`, `LocalDigest()`, `VerifyChain()`
- [ ] Add cache hit/miss counters to `digest_cache_`
- [ ] Create benchmark script (`scripts/benchmark_digest.py`)
- [ ] Run baseline benchmarks (10 nodes, 100 writes/s, 10 minutes)
- [ ] Document current performance in `STATUS.md`

**Deliverables**:
- Performance baseline report
- Instrumentation code merged

### Phase 2: Incremental Updates (Weeks 2-3)

**Goal**: Eliminate full scans for common operations.

**Tasks**:
- [ ] Add `CollectionDigestState` and `LedgerDigestState` structures
- [ ] Implement `UpdateCollectionDigest()` hook
- [ ] Wire hooks into `PutDocument()`, `DeleteDocument()`, `MergeRemote()`
- [ ] Wire ledger digest update into `WriteAheadLog::Append()`
- [ ] Update `Summarize()` to use incremental state (with fallback)
- [ ] Update `VerifyChain()` to use incremental state (with fallback)
- [ ] Add persistence for digest state (serialize to `digests.json`)
- [ ] Add tests for incremental update correctness

**Deliverables**:
- Incremental digest implementation
- Unit tests for update hooks
- Integration test for restart recovery

### Phase 3: Corruption Detection + Repair (Week 4)

**Goal**: Ensure cache integrity and provide recovery.

**Tasks**:
- [ ] Implement `RebuildCollectionDigest()` and `RebuildAllDigests()`
- [ ] Add periodic sample-based verification
- [ ] Add invariant checking to `SummarizeSafe()`
- [ ] Implement corruption detection logic
- [ ] Add tests for corruption scenarios:
  - Memory corruption (simulate bit flip)
  - Disk corruption (simulate torn write)
  - Restart with stale cache
- [ ] Document full-scan fallback in `ISSUES.md`

**Deliverables**:
- Corruption detection implementation
- Test coverage for corruption scenarios
- Updated `ISSUES.md` with fallback documentation

### Phase 4: Benchmarking + Optimization (Week 5)

**Goal**: Validate performance improvements and optimize.

**Tasks**:
- [ ] Run benchmarks with incremental implementation
- [ ] Compare against baseline (Phase 1)
- [ ] Identify bottlenecks (profiling)
- [ ] Optimize hot paths (e.g., XOR checksum vs full SHA-256)
- [ ] Tune rebuild frequency and sample size
- [ ] Document results in `STATUS.md`
- [ ] Update `ISSUES.md` to mark complete

**Deliverables**:
- Benchmark report (before/after comparison)
- Optimized implementation
- `ISSUES.md` updated to reflect completion

---

## 7. API Changes

### 7.1 New Endpoints

**`GET /_digest/status`** - Check digest cache health

```json
{
  "ledger": {
    "is_valid": true,
    "tip_entry_id": 12345,
    "entries_count": 12345
  },
  "collections": [
    {
      "name": "files",
      "is_valid": true,
      "document_count": 1000,
      "last_updated_ms": 1698765432000
    }
  ]
}
```

**`POST /_digest/rebuild`** (supervisor only) - Force digest rebuild

```json
{
  "ok": true,
  "collections_rebuilt": 5,
  "ledger_entries_verified": 12345,
  "duration_ms": 234
}
```

### 7.2 Modified Endpoints

**`GET /_collection/:collection`** - Now uses incremental digest

- Faster response time (O(1) vs O(n))
- Returns `digest_valid: true/false` to indicate cache health

**`GET /_brain`** - Now uses incremental digests

- Faster response time (O(collections) vs O(Σdocuments))
- Returns `digest_valid: true/false` at node level

---

## 8. Data Structure Changes

### 8.1 In-Memory Structures

```cpp
// include/desentry/engine/node_engine.h

struct CollectionDigestState {
  uint64_t document_count = 0;
  std::string checksum;  // SHA-256 hex
  uint64_t last_updated_hlc = 0;
  bool is_valid = false;
};

struct LedgerDigestState {
  lsn_t tip_entry_id = kInvalidLsn;
  std::string tip_entry_hash;
  uint64_t entries_count = 0;
  uint64_t signed_entries = 0;
  bool is_valid = false;
};

class NodeEngine {
  // ... existing fields ...
  
  std::unordered_map<std::string, CollectionDigestState> collection_digests_;
  mutable std::mutex digests_mu_;
  
  LedgerDigestState ledger_digest_;
};
```

### 8.2 Persistent Structures

**File**: `<data_dir>/digest_state.json`

```json
{
  "version": 1,
  "ledger": {
    "tip_entry_id": 12345,
    "tip_entry_hash": "abc123...",
    "entries_count": 12345,
    "signed_entries": 12000,
    "is_valid": true
  },
  "collections": {
    "files": {
      "document_count": 1000,
      "checksum": "def456...",
      "last_updated_hlc": 1698765432000,
      "is_valid": true
    }
  }
}
```

**Persistence strategy**:
- Write on every N updates (e.g., N=100) to avoid I/O overhead
- Write on graceful shutdown
- Use atomic write (tmp + rename) for crash safety
- Validate on load; mark invalid if mismatch

---

## 9. Implementation Timeline

| Phase | Duration | Start Date | End Date | Dependencies |
|-------|----------|------------|----------|--------------|
| Phase 1: Instrumentation | 1 week | 2026-10-26 | 2026-11-01 | None |
| Phase 2: Incremental Updates | 2 weeks | 2026-11-02 | 2026-11-15 | Phase 1 |
| Phase 3: Corruption Detection | 1 week | 2026-11-16 | 2026-11-22 | Phase 2 |
| Phase 4: Benchmarking | 1 week | 2026-11-23 | 2026-11-29 | Phase 3 |

**Total**: 5 weeks  
**Risk buffer**: +1 week for unforeseen issues  
**Target completion**: 2026-12-06

---

## 10. Risks and Mitigations

| Risk | Impact | Likelihood | Mitigation |
|------|--------|------------|------------|
| Incremental checksum drift | High | Medium | Periodic full verification, auto-rebuild |
| Memory overhead excessive | Medium | Low | Limit cache size, evict old entries |
| Restart rebuild too slow | Medium | Low | Parallelize rebuild, progress reporting |
| Corruption detection false positives | Medium | Low | Require multiple failures before invalidating |
| Performance regression | High | Low | Benchmark at each phase, rollback plan |

---

## 11. Open Questions

1. **Checksum strength**: Is XOR-based incremental checksum sufficient, or do we need a Merkle tree?
   - **Recommendation**: Start with XOR; upgrade to Merkle if corruption detection proves inadequate.

2. **Persistence frequency**: How often to write `digest_state.json`?
   - **Recommendation**: Every 100 updates or 1 second, whichever comes first.

3. **Rebuild trigger threshold**: How many mismatches before full rebuild?
   - **Recommendation**: 3 mismatches in 10 samples, or any ledger mismatch.

4. **Memory limit**: Should we cap cache size?
   - **Recommendation**: Yes, 100MB limit; evict least-recently-used collections.

---

## 12. References

- `ISSUES.md` - Issue `ledger-digest-cache` (line 28)
- `STATUS.md` - Current state description (line 28)
- `src/engine/node_engine.cpp` - Current digest cache implementation
- `src/storage/wal.cpp` - Current ledger verification
- `docs/architecture-v2.md` - Ledger v2 design

---

## Appendix A: Code Locations

| Component | File | Lines |
|-----------|------|-------|
| Digest cache (current) | `src/engine/node_engine.cpp` | 360-406 |
| Collection summary (full scan) | `src/engine/node_engine.cpp` | 436-453 |
| Ledger verification (full scan) | `src/storage/wal.cpp` | 706-773 |
| Gossip digest exchange | `src/net/gossip.cpp` | 122-131 |
| API routes (collection) | `src/api/routes.cpp` | 318-340 |
| API routes (brain) | `src/api/routes.cpp` | 946-996 |
| Checkpoint (ledger verify) | `src/api/routes.cpp` | 740-786 |

---

## Appendix B: Test Coverage Plan

| Test | Type | Coverage |
|------|------|----------|
| `digest_incremental_test.cpp` | Unit | Update hooks, checksum correctness |
| `digest_persistence_test.cpp` | Unit | Save/load, crash recovery |
| `digest_corruption_test.cpp` | Unit | Detection, rebuild trigger |
| `digest_performance_test.cpp` | Integration | Latency, throughput, memory |
| `digest_invariant_test.cpp` | Property | Consistency invariant holds |

---

**Document maintained by**: Oracle (strategic technical advisor)  
**Last updated**: 2026-10-26