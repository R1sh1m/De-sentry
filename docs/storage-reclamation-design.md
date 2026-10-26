# Storage Reclamation and Page-Level Checksums Design Document

**Status:** Design Draft  
**Priority:** P2 (important for long-lived collections)  
**Last Updated:** 2026-10-26  
**Author:** Design Subagent  

---

## Executive Summary

This document specifies the design for implementing **storage reclamation (vacuum)** and **page-level checksums** in the De-Sentry database engine. These features address two critical limitations for production deployments:

1. **Deleted space is never reclaimed** — the bump allocator only grows, making long-lived collections with high write/delete churn waste disk space indefinitely.
2. **Raw pages lack integrity protection** — B+Tree and slotted pages can suffer torn writes during structural operations (splits, merges), with detection relying solely on WAL replay rather than per-page checksums.

The design prioritizes **crash safety**, **backward compatibility**, and **minimal performance overhead** while maintaining the engine's existing simplicity and correctness guarantees.

---

## 1. On-Disk Compatibility Story

### 1.1 Current State

The current on-disk format has **no explicit version number** in the data file header. Files are raw concatenations of 4 KiB pages. The `DiskManager` determines file size by dividing total bytes by `kPageSize` (4096), treating any partial final page as a torn tail that reads as zero.

**Existing files:**
- No file header or magic number
- No page-level checksums
- Page IDs are implicit (sequential from 0)
- No free-page tracking

### 1.2 File Format Version Upgrade Path

We introduce a **file format version** stored in a new sidecar header file, following the existing pattern used for encryption metadata (`.enc` sidecar).

**New sidecar file:** `<collection_name>.meta`

```cpp
#pragma pack(push, 1)
struct CollectionMetaHeader {
  uint32_t magic = 0x44534D45;  // "DSME" = De-Sentry Meta Entry
  uint32_t version;              // Format version (starts at 1)
  uint64_t created_ms;           // Unix timestamp (ms) of collection creation
  uint64_t last_modified_ms;     // Unix timestamp (ms) of last metadata update
  uint32_t page_checksum_algo;   // 0=none, 1=CRC32, 2=CRC64, 3=xxHash32
  uint32_t reserved[8];          // Future expansion
  uint32_t header_crc32;         // CRC32 over bytes 0..47
};
#pragma pack(pop)
```

**Version semantics:**
- **Version 0** (implicit): Legacy format, no checksums, no vacuum support
- **Version 1**: CRC32 page checksums, free-page bitmap for vacuum
- **Version 2+**: Reserved for future extensions (compression, etc.)

### 1.3 Migration Strategy

**Offline migration required.** The migration process:

1. **Pre-migration checks:**
   - Verify WAL is empty (all entries checkpointed and applied)
   - Ensure no active transactions or pending writes
   - Create a full backup of the data file

2. **Migration steps:**
   ```
   desentryd --migrate-collection <name> --target-version 1
   ```
   - Reads all pages sequentially
   - Computes and appends CRC32 checksum to each page header
   - Initializes free-page bitmap (empty, since no reclamation yet)
   - Writes new `.meta` sidecar with version=1
   - Updates catalog to mark collection as migrated

3. **Rollback:** Not supported. Migration is one-way. Backup must be retained until verification completes.

4. **Online migration (future):** Would require:
   - Dual-format writes during migration window
   - Copy-on-read to upgrade pages lazily
   - Not in scope for Phase 1-3

### 1.4 Backward Compatibility Rules

| Scenario | Behavior |
|----------|----------|
| **Old engine reading new format** | Fails at open with `Corruption("unsupported format version N")` |
| **New engine reading old format** | Allowed in read-only mode; vacuum disabled; checksums not verified |
| **Mixed-version mesh** | No impact — replication is at document level, not page level |

---

## 2. Page-Level Checksums

### 2.1 Checksum Algorithm: CRC32

**Decision:** Use **CRC32** (ISO 3309 / Ethernet polynomial) for all page checksums.

**Rationale:**

| Algorithm | Speed (GB/s) | Detection Strength | Code Size | Hardware Support |
|-----------|--------------|-------------------|-----------|------------------|
| **CRC32** | ~10-20 | Excellent for burst errors | ~50 lines | SSE4.2, ARM CRC |
| CRC64 | ~5-10 | Marginally better | ~100 lines | Limited |
| xxHash32 | ~20-30 | Good, but not error-optimized | ~200 lines | None |
| SHA-256 | ~0.5 | Cryptographic | ~500 lines | SHA-NI (limited) |

**Why CRC32 wins:**
1. **Already in codebase** — `include/desentry/common/crc32.h` exists and is used by WAL and segment pages
2. **Perfect for torn-write detection** — CRC32 excels at detecting burst errors (consecutive bit flips), which is exactly what a torn page write produces
3. **Negligible overhead** — ~50ns per 4 KiB page on modern hardware (<0.1% of typical I/O latency)
4. **Hardware acceleration** — SSE4.2 `_mm_crc32_u32` on x86, `crc32` instruction on ARMv8

### 2.2 Checksum Placement: Page Header

**Decision:** Store checksum **at the start of each page**, as part of an expanded page header.

**Current page layout (B+Tree leaf node):**
```
Offset 0: BNodeHeader (is_leaf, num_keys, parent_page_id, next_leaf_page_id) = 12 bytes
Offset 12: Keys and RIDs...
```

**New page layout (with checksum):**
```
Offset 0:  PageHeader {
            uint32_t checksum;      // CRC32 of bytes [16..4096)
            uint32_t reserved;      // Alignment, future use
            uint8_t  is_leaf;
            uint8_t  page_type;     // 1=BTree leaf, 2=BTree internal, 3=slotted
            uint16_t version;       // Page format version (for future changes)
            page_id_t page_id;      // Redundant ID for corruption detection
            page_id_t next_leaf_page_id;
           }                        // Total: 16 bytes
Offset 16: Keys and RIDs...
```

**Why header (not footer):**
1. **Single I/O for validation** — Read page, verify checksum immediately without seeking to end
2. **Consistent with segment pages** — `SegmentStore` already uses header checksums (16-byte header with CRC32)
3. **Simpler buffer pool integration** — Checksum verification happens in `FetchPage`, not after

**Why not encrypt the checksum:** The checksum protects against **physical corruption** (torn writes, bit rot), not tampering. At-rest encryption (if enabled) already protects confidentiality. Checksum is computed over plaintext before encryption.

### 2.3 When to Compute/Verify

| Operation | Checksum Action |
|-----------|-----------------|
| **Page write** (`DiskManager::WritePage`) | Compute CRC32 over payload, store in header before writing |
| **Page read** (`DiskManager::ReadPage`) | Verify CRC32 after reading; return `Corruption` on mismatch |
| **Page allocation** (`DiskManager::AllocatePage`) | Zero-fill page, compute checksum of zeros |
| **Buffer pool eviction** | No checksum action (already verified on read, updated on write) |
| **WAL replay** | Verify checksum after applying changes to page |

**Torn write detection:**
- If a 4 KiB page write is interrupted (crash, power loss), the page will be partially old and partially new data
- CRC32 will **not match** either the old or new expected value
- `ReadPage` returns `Corruption("page %d checksum mismatch: expected 0x%08x, got 0x%08x")`
- Recovery: WAL replay re-applies the complete write, overwriting the torn page

### 2.4 Header Structure Changes

**New `PageHeader` struct** (in `include/desentry/storage/page.h`):

```cpp
#pragma pack(push, 1)
struct PageHeader {
  uint32_t checksum;       // CRC32 of bytes [sizeof(PageHeader)..kPageSize)
  uint32_t reserved;       // Must be zero; reserved for future use
  uint8_t  page_type;      // PageType enum (see below)
  uint8_t  format_version; // Page format version (starts at 1)
  uint16_t flags;          // Bit flags (e.g., kIsDirty, kIsPinned)
  page_id_t page_id;       // Redundant ID for corruption detection
  page_id_t next_page_id;  // For linked pages (B+Tree leaves, segment chains)
};
#pragma pack(pop)

enum class PageType : uint8_t {
  kUnknown = 0,
  kBTreeLeaf = 1,
  kBTreeInternal = 2,
  kSlottedPage = 3,
  kSegmentPage = 4,  // Already has its own header; may not use this
};

constexpr size_t kPageHeaderSize = 16;
constexpr size_t kPagePayloadSize = kPageSize - kPageHeaderSize;  // 4080 bytes
```

**Impact on existing structures:**
- `BNodeHeader` moves from offset 0 to offset 16
- `SlottedPageHeader` moves from offset 0 to offset 16
- All offset calculations in `bplus_tree.cpp` and `slotted_page.cpp` must be updated
- **Static assertions** must verify new structures fit within `kPagePayloadSize`

---

## 3. Crash-Safe Vacuum Implementation

### 3.1 Free Page Tracking: Bitmap + WAL

**Decision:** Use a **free-page bitmap** persisted in a dedicated system page, with all modifications logged to WAL.

**Data structure:**
```cpp
class FreePageManager {
 public:
  // Initialize or load the free-page bitmap
  static StatusOr<std::unique_ptr<FreePageManager>> Open(BufferPoolManager* bpm, page_id_t meta_page_id);
  
  // Allocate a free page (returns kInvalidPageId if none available)
  page_id_t AllocateFreePage();
  
  // Return a page to the free list
  void FreePage(page_id_t page_id);
  
  // Persist bitmap to disk (called at checkpoint)
  Status Flush();
  
  // Get count of free pages
  size_t FreePageCount() const;
  
  // Get total allocated pages
  size_t TotalPages() const;
  
 private:
  // Bitmap: bit[i] = 1 means page i is free, 0 means allocated
  // Stored as array of uint64_t; bit i is at (bitmap[i / 64] >> (i % 64)) & 1
  std::vector<uint64_t> bitmap_;
  page_id_t next_free_hint_;  // Hint for next allocation (optimization)
  BufferPoolManager* bpm_;
  page_id_t meta_page_id_;    // Page storing bitmap persistence
  mutable std::mutex mu_;
};
```

**Bitmap persistence:**
- Stored in a dedicated **meta page** (allocated from the bump allocator initially)
- Bitmap size: `ceil(total_pages / 64)` × 8 bytes
  - Example: 1 million pages → 15.6 KB bitmap → fits in 4 pages
- Updated lazily; flushed at checkpoint time
- **WAL-logged:** Every `FreePage()` call appends a `kVacuumFreePage` record to WAL before modifying bitmap

### 3.2 Vacuum Process

**Vacuum is a background process** that:
1. Scans slotted pages for tombstoned slots
2. Compacts pages by removing tombstones and shifting records
3. Frees entirely-empty pages back to the bitmap

**Vacuum algorithm (per collection):**

```cpp
Status VacuumCollection(const std::string& collection) {
  // Phase 1: Identify candidate pages (high tombstone ratio)
  std::vector<page_id_t> candidates;
  for (auto page_id : collection_pages) {
    Page* page = bpm->FetchPage(page_id);
    double tombstone_ratio = ComputeTombstoneRatio(page);
    bpm->UnpinPage(page_id, false);
    if (tombstone_ratio > kVacuumThreshold) {  // e.g., 30%
      candidates.push_back(page_id);
    }
  }
  
  // Phase 2: Compact pages
  for (auto page_id : candidates) {
    Page* page = bpm->FetchPage(page_id);
    Status st = CompactSlottedPage(page);  // Removes tombstones, shifts records
    if (!st.ok()) {
      bpm->UnpinPage(page_id, true);  // Dirty, but may be corrupt
      return st;
    }
    bool page_now_empty = IsPageEmpty(page);
    bpm->UnpinPage(page_id, true);  // Dirty
    
    // Phase 3: Free empty pages
    if (page_now_empty) {
      WAL_APPEND(kVacuumFreePage, collection, page_id);
      free_page_manager->FreePage(page_id);
    }
  }
  
  return Status::OK();
}
```

**Key design decisions:**

| Decision | Rationale |
|----------|-----------|
| **Vacuum is manual/scheduled, not continuous** | Avoids background I/O contention; users can schedule during low-activity periods |
| **Per-collection, not global** | Allows targeting specific high-churn collections; avoids locking entire storage engine |
| **Tombstone ratio threshold** | Prevents wasteful compaction of pages with few deletes; default 30% |
| **Empty pages freed immediately** | Maximizes space reclamation; no need to keep empty pages |

### 3.3 Crash Recovery During Vacuum

**Scenario:** Crash occurs mid-vacuum. What state can the database be in?

| Crash Point | Recovery Action |
|-------------|-----------------|
| **Before WAL append** | Vacuum is treated as never started; pages remain tombstoned |
| **After WAL append, before page compaction** | WAL replay re-does the compaction (idempotent) |
| **After compaction, before bitmap update** | Bitmap is stale (page marked allocated but empty); harmless, just leaks space until next vacuum |
| **After bitmap update, before flush** | Bitmap in memory is correct; flush will persist it; no data loss |

**WAL record types for vacuum:**

```cpp
enum class WalRecordType : uint8_t {
  // ... existing types ...
  kVacuumFreePage = 8,   // page_id freed back to bitmap
  kVacuumCompactPage = 9,  // page compacted (for idempotent replay)
};
```

**WAL record for `kVacuumFreePage`:**
```cpp
struct WalRecord {
  // ... common fields ...
  page_id_t freed_page_id;  // Page being freed
  std::string collection;   // Collection owning the page
  uint32_t tombstone_count; // Number of tombstones removed (for auditing)
};
```

**Idempotent replay:**
- `kVacuumFreePage`: If page is already free, no-op (bitmap bit already 1)
- `kVacuumCompactPage`: Re-apply compaction (deterministic; same input → same output)

### 3.4 WAL Integration: Should Vacuum Be Logged?

**Decision:** **Yes, vacuum operations must be WAL-logged.**

**Rationale:**
1. **Crash recovery:** Without WAL, a crash mid-vacuum could leave pages in an inconsistent state (tombstones removed but not compacted, or compacted but not freed)
2. **Replication:** Vacuum is a **local optimization** and should NOT be replicated. WAL entries for vacuum are marked as `local_only = true` and filtered out by gossip
3. **Audit trail:** WAL provides an audit log of space reclamation (useful for debugging storage growth)

**Implementation:**
```cpp
// In FreePageManager::FreePage()
Status st = wal_->Append(WalRecordType::kVacuumFreePage, 
                         collection, 
                         "",  // key is empty
                         EncodePageId(page_id));
if (!st.ok()) return st;  // WAL append failed; do not free page

// Now update bitmap
bitmap_[page_id / 64] |= (1ULL << (page_id % 64));
```

---

## 4. Testing Strategy

### 4.1 Unit Tests

**Checksum verification tests** (`tests/page_checksum_test.cpp`):

```cpp
TEST(PageChecksumTest, ValidPagePassesVerification) {
  Page page;
  InitTestPage(&page, PageType::kSlottedPage);
  WriteTestData(&page);
  ComputeAndStoreChecksum(&page);
  
  // Corrupt a single bit
  page.GetData()[100] ^= 0x01;
  
  EXPECT_FALSE(VerifyChecksum(&page));
}

TEST(PageChecksumTest, TornWriteDetected) {
  // Simulate torn write: first half old data, second half new data
  Page page;
  InitTestPage(&page, PageType::kBTreeLeaf);
  std::string old_checksum = ComputeChecksum(&page);
  
  // Overwrite second half
  std::memset(page.GetData() + 2048, 0xFF, 2048);
  
  EXPECT_FALSE(VerifyChecksum(&page));
}

TEST(PageChecksumTest, ZeroPageHasValidChecksum) {
  Page page;
  page.ResetMemory();  // Zero-fill
  ComputeAndStoreChecksum(&page);
  
  EXPECT_TRUE(VerifyChecksum(&page));
}
```

**Tamper detection tests:**

```cpp
TEST(PageChecksumTest, TamperedPageIdDetected) {
  Page page;
  InitTestPage(&page, PageType::kSlottedPage);
  ComputeAndStoreChecksum(&page);
  
  // Tamper with page_id in header
  PageHeader* hdr = reinterpret_cast<PageHeader*>(page.GetData());
  hdr->page_id = 999;  // Wrong ID
  
  EXPECT_FALSE(VerifyChecksum(&page));
}
```

### 4.2 Vacuum Crash-Recovery Tests

**Integration test** (`tests/vacuum_crash_test.py`):

```python
def test_vacuum_crash_mid_compaction():
    # Setup: Create collection with 100 documents, delete 50
    collection = create_collection("vacuum_test")
    for i in range(100):
        collection.put(f"key{i}", f"value{i}")
    for i in range(0, 100, 2):
        collection.delete(f"key{i}")
    
    # Start vacuum, crash during compaction
    vacuum_pid = start_vacuum(collection)
    sleep(0.5)  # Let vacuum start
    kill(vacuum_pid, SIGKILL)
    
    # Restart and verify
    restart_node()
    collection = open_collection("vacuum_test")
    
    # All remaining documents should be accessible
    for i in range(1, 100, 2):
        assert collection.get(f"key{i}") == f"value{i}"
    
    # Verify WAL replay re-did vacuum (idempotent)
    assert collection.stats().tombstone_count == 0
```

**Bitmap persistence test:**

```python
def test_vacuum_bitmap_persists():
    # Free some pages
    for page_id in [10, 20, 30]:
        free_page_manager.free_page(page_id)
    
    checkpoint()
    restart_node()
    
    # Bitmap should be restored
    assert free_page_manager.is_free(10)
    assert free_page_manager.is_free(20)
    assert free_page_manager.is_free(30)
```

### 4.3 Performance Benchmarks

**Checksum overhead benchmark** (`benchmarks/checksum_overhead.cpp`):

```cpp
BENCHMARK(ChecksumCompute_4KiB_Page) {
  Page page;
  RandomFill(&page);
  
  for (auto _ : state) {
    uint32_t checksum = CRC32(page.GetData() + kPageHeaderSize, kPagePayloadSize);
    benchmark::DoNotOptimize(checksum);
  }
}
// Expected: ~50ns per page on modern hardware
```

**Vacuum throughput benchmark** (`benchmarks/vacuum_throughput.py`):

```python
def benchmark_vacuum_throughput():
    # Create 10 GB collection with 50% deletes
    collection = create_collection("bench_vacuum", size_gb=10)
    delete_half(collection)
    
    start = time.time()
    vacuum(collection)
    elapsed = time.time() - start
    
    reclaimed_gb = collection.size_before - collection.size_after
    print(f"Vacuum throughput: {reclaimed_gb / elapsed:.2f} GB/s")
    print(f"Space reclaimed: {reclaimed_gb:.2f} GB ({reclaimed_gb / 10 * 100:.1f}%)")
```

**Expected overhead:**
- **Checksums:** <0.1% latency increase on page reads/writes
- **Vacuum:** Background process; no impact on foreground operations when throttled correctly

---

## 5. Implementation Phases

### Phase 1: Page Checksums Only (No Vacuum)

**Duration:** 2-3 weeks  
**Goal:** Add CRC32 checksums to all B+Tree and slotted pages

**Tasks:**
1. Add `PageHeader` struct to `include/desentry/storage/page.h`
2. Update `DiskManager::WritePage()` to compute checksum before write
3. Update `DiskManager::ReadPage()` to verify checksum after read
4. Update `BPlusTree` to use new header layout (offsets shifted by 16 bytes)
5. Update `SlottedPage` to use new header layout
6. Add unit tests for checksum verification
7. Add tamper detection tests
8. Document file format changes in `docs/storage-format.md`

**Deliverables:**
- Checksummed pages for `kv` backend (B+Tree)
- Checksum failure returns `Corruption` status
- All existing tests pass with new format

**Risks:**
- Offset calculation bugs in B+Tree/slotted page code
- Performance regression (mitigated by benchmarking)

### Phase 2: Vacuum Without Crash-Safety

**Duration:** 3-4 weeks  
**Goal:** Implement basic vacuum with free-page bitmap (no WAL logging)

**Tasks:**
1. Add `FreePageManager` class with bitmap-based tracking
2. Add `kVacuumFreePage` WAL record type (not yet logged)
3. Implement `VacuumCollection()` algorithm
4. Add CLI command: `desentryd --vacuum <collection>`
5. Add integration tests for vacuum
6. Add performance benchmarks

**Deliverables:**
- Manual vacuum command
- Free-page bitmap (in-memory, not persisted)
- Space reclamation works correctly

**Risks:**
- Vacuum may leave database in inconsistent state if crashed mid-operation
- Bitmap not persisted → vacuum must re-scan after restart

### Phase 3: Crash-Safe Vacuum with WAL Integration

**Duration:** 4-6 weeks  
**Goal:** Make vacuum crash-safe with WAL logging and bitmap persistence

**Tasks:**
1. Add WAL logging for `kVacuumFreePage` and `kVacuumCompactPage`
2. Implement idempotent WAL replay for vacuum operations
3. Persist bitmap to dedicated meta page
4. Add bitmap recovery from WAL on startup
5. Add crash-recovery tests (kill vacuum mid-operation, verify recovery)
6. Add scheduled vacuum (configurable interval)
7. Add monitoring metrics (free page count, vacuum duration, space reclaimed)

**Deliverables:**
- Crash-safe vacuum
- Persistent free-page bitmap
- Scheduled vacuum (optional, configurable)
- Full test coverage for crash scenarios

**Risks:**
- WAL replay complexity (idempotency must be proven)
- Bitmap persistence adds I/O overhead at checkpoint

---

## 6. Data Structure Changes

### 6.1 `PageHeader` (New)

**File:** `include/desentry/storage/page.h`

```cpp
#pragma pack(push, 1)
struct PageHeader {
  uint32_t checksum;       // CRC32 of bytes [16..4096)
  uint32_t reserved;       // Must be zero
  uint8_t  page_type;      // PageType enum
  uint8_t  format_version; // Starts at 1
  uint16_t flags;          // Reserved
  page_id_t page_id;       // Redundant ID
  page_id_t next_page_id;  // For linked pages
};
#pragma pack(pop)
```

### 6.2 `BNodeHeader` (Modified)

**File:** `include/desentry/storage/bplus_tree.h`

```cpp
#pragma pack(push, 1)
struct BNodeHeader {
  // Moved from offset 0 to offset 16
  uint8_t is_leaf;
  int16_t num_keys;
  page_id_t parent_page_id;
  page_id_t next_leaf_page_id;
};
#pragma pack(pop)

// Updated constants
constexpr size_t kHeaderSize = kPageHeaderSize + sizeof(BNodeHeader);  // Was: sizeof(BNodeHeader)
constexpr size_t kLeafKeysOffset = kHeaderSize;  // Was: kHeaderSize
// ... all other offsets shift by kPageHeaderSize
```

### 6.3 `SlottedPageHeader` (Modified)

**File:** `include/desentry/storage/slotted_page.h`

```cpp
#pragma pack(push, 1)
struct SlottedPageHeader {
  // Moved from offset 0 to offset 16
  uint16_t num_slots;
  uint16_t free_space_offset;
};
#pragma pack(pop)
```

### 6.4 `FreePageManager` (New)

**File:** `include/desentry/storage/free_page_manager.h`

```cpp
class FreePageManager {
 public:
  static StatusOr<std::unique_ptr<FreePageManager>> Open(BufferPoolManager* bpm, page_id_t meta_page_id);
  page_id_t AllocateFreePage();
  void FreePage(page_id_t page_id, const std::string& collection);
  Status Flush();
  size_t FreePageCount() const;
  size_t TotalPages() const;
  
 private:
  std::vector<uint64_t> bitmap_;
  page_id_t next_free_hint_;
  BufferPoolManager* bpm_;
  page_id_t meta_page_id_;
  mutable std::mutex mu_;
};
```

### 6.5 `WalRecordType` (Extended)

**File:** `include/desentry/storage/wal.h`

```cpp
enum class WalRecordType : uint8_t {
  // ... existing types ...
  kVacuumFreePage = 8,
  kVacuumCompactPage = 9,
};
```

---

## 7. File Format Changes

### 7.1 Data File Layout (Unchanged)

The data file (`<collection>.db`) remains a raw concatenation of 4 KiB pages. No header is added to the data file itself.

### 7.2 Meta Sidecar File (New)

**File:** `<collection>.meta`

```
Offset 0:  CollectionMetaHeader (64 bytes)
Offset 64: Reserved for future use (4032 bytes)
```

**Purpose:** Stores format version, checksum algorithm, and other metadata.

### 7.3 Free-Page Bitmap Page (New)

**Location:** Dedicated page (ID stored in catalog)

```
Offset 0:  PageHeader (16 bytes)
Offset 16: uint64_t bitmap[] (variable size)
```

**Size:** `ceil(total_pages / 64) × 8` bytes + padding to fill page

---

## 8. Implementation Timeline

| Phase | Duration | Start Date | End Date | Dependencies |
|-------|----------|------------|----------|--------------|
| **Phase 1: Checksums** | 2-3 weeks | Week 1 | Week 3 | None |
| **Phase 2: Basic Vacuum** | 3-4 weeks | Week 4 | Week 7 | Phase 1 complete |
| **Phase 3: Crash-Safe Vacuum** | 4-6 weeks | Week 8 | Week 13 | Phase 2 complete |
| **Testing & Benchmarking** | Ongoing | Week 1 | Week 13 | All phases |
| **Documentation** | Ongoing | Week 1 | Week 13 | All phases |

**Total estimated duration:** 13 weeks (3 months)

---

## 9. Risk Assessment

### 9.1 Technical Risks

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| **Checksum performance overhead** | Low | Medium | Benchmark early; use hardware-accelerated CRC32 |
| **Vacuum crashes leave inconsistent state** | Medium | High | WAL logging (Phase 3); thorough crash-recovery testing |
| **Bitmap persistence bugs** | Medium | Medium | Idempotent WAL replay; fuzz testing |
| **Migration data loss** | Low | Critical | Backup before migration; test migration on copies |
| **B+Tree offset calculation bugs** | Medium | High | Extensive unit tests; static assertions for structure sizes |

### 9.2 Schedule Risks

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| **Phase 3 takes longer than estimated** | High | Medium | Defer scheduled vacuum to Phase 4; ship manual vacuum first |
| **Testing reveals fundamental design flaws** | Medium | High | Design review before Phase 1; prototype critical paths |
| **Performance regression blocks release** | Low | High | Continuous benchmarking; rollback plan |

### 9.3 Operational Risks

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| **Users skip migration and run old format** | Medium | Low | Read-only mode for old format; clear error messages |
| **Vacuum runs during peak load** | Medium | Medium | Default to manual vacuum; document scheduling best practices |
| **Free-page bitmap corruption** | Low | High | WAL replay recovery; periodic integrity checks |

---

## 10. Open Questions

### 10.1 Should segment pages use the new `PageHeader`?

**Current state:** Segment pages already have a 16-byte header with CRC32.

**Options:**
1. **Keep existing segment header** — No change; segment pages already have checksums
2. **Migrate to new `PageHeader`** — Uniformity across all page types, but requires migration

**Recommendation:** Keep existing segment header (Option 1). The existing header already provides checksum protection, and migration would add complexity without clear benefit.

### 10.2 Should vacuum be automatic or manual?

**Options:**
1. **Automatic (background)** — Convenient, but adds complexity and potential I/O contention
2. **Manual (CLI command)** — Simple, but requires user intervention
3. **Scheduled (configurable)** — Best of both, but adds scheduling logic

**Recommendation:** Start with manual (Phase 2), add scheduled as optional feature (Phase 3).

### 10.3 Should free pages be zeroed before reuse?

**Options:**
1. **Zero on free** — Prevents information leakage, but adds I/O overhead
2. **Zero on allocate** — Same security, but defers cost
3. **Don't zero** — Fastest, but may leak deleted data

**Recommendation:** Zero on allocate (Option 2). This is the standard approach (e.g., Linux `malloc`), and it prevents information leakage without penalizing the free path.

---

## 11. References

- **STATUS.md §5:** "The bump allocator does not reclaim space: no vacuum until Phase 2."
- **STATUS.md §5:** "No page-level checksums for a page torn mid-write during a structural split."
- **ISSUES.md:** `storage-reclamation` (P2 issue)
- **SegmentStore:** Existing CRC32 implementation (`src/storage/segment_store.cpp`)
- **WAL:** Existing CRC32 and hash chain implementation (`src/storage/wal.cpp`)
- **PostgreSQL VACUUM:** Inspiration for crash-safe vacuum design
- **InnoDB page format:** Inspiration for page header layout

---

## Appendix A: CRC32 Implementation

The existing CRC32 implementation in `include/desentry/common/crc32.h` should be used:

```cpp
// From include/desentry/common/crc32.h
uint32_t CRC32(const void* data, size_t length);
```

If hardware acceleration is available (SSE4.2, ARM CRC), the implementation should use it. Otherwise, fall back to a software table-based implementation.

---

## Appendix B: Migration Script Pseudocode

```python
def migrate_collection(collection_path, target_version=1):
    # Pre-flight checks
    if not wal_is_empty():
        raise Error("WAL must be empty before migration")
    
    backup(collection_path)
    
    # Read all pages
    pages = read_all_pages(collection_path)
    
    # Rewrite pages with new format
    for page in pages:
        # Shift existing data by 16 bytes
        new_data = PageHeader() + page.data
        
        # Compute checksum
        new_data.checksum = CRC32(new_data[16:])
        
        # Write back
        write_page(collection_path, page.id, new_data)
    
    # Write meta sidecar
    write_meta_file(collection_path + ".meta", {
        "version": target_version,
        "page_checksum_algo": 1,  # CRC32
    })
    
    # Update catalog
    catalog.mark_migrated(collection_name, target_version)
    
    print(f"Migration complete: {len(pages)} pages migrated")
```

---

## Appendix C: Configuration Options

**New configuration options** (in `node.json`):

```json
{
  "storage": {
    "vacuum": {
      "enabled": true,
      "schedule": "0 3 * * *",  // Cron expression (Phase 3)
      "tombstone_threshold": 0.3,  // Vacuum pages with >30% tombstones
      "max_pages_per_run": 1000  // Throttle vacuum (Phase 3)
    },
    "checksums": {
      "enabled": true,
      "algorithm": "crc32"  // Only option for now
    }
  }
}
```

---

**End of Document**
