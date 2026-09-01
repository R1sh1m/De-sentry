# De-Sentry: Node Capability Score (NCS) — Hardware Benchmarking & Ranking

## 1. Overview

The **Node Capability Score (NCS)** is a normalized, multi-dimensional score that quantifies a node's raw hardware capability independent of what data types it stores. It answers the question:

> *"Objectively, how capable is this hardware at being a database node?"*

The NCS is computed from **five hardware dimensions**:

| Dimension | Captures |
|---|---|
| **Storage** | Disk capacity and free space |
| **I/O** | Sequential and random read/write throughput |
| **CPU** | Processing power, parallelism, and current headroom |
| **Memory** | RAM availability, bandwidth, and access latency |
| **Network** | Throughput and latency to every peer in the consortium |

The NCS integrates into the existing **Fitness Score** from `routing_and_specialization.md` as a hardware-aware multiplier on the declared capability score — a node that claims image specialization but has an HDD with 80 MB/s throughput scores lower than the same claim on an NVMe at 3 GB/s.

---

## 2. Where NCS Fits in the System

```
┌───────────────────────────────────────────────────────┐
│               Fitness Score (routing decision)         │
│                                                       │
│  fitness(N, T) =                                      │
│    0.45 × capability_score(N,T) × ncs_multiplier(N,T) │  ← NCS feeds here
│  + 0.25 × (1 − load_ratio(N))                        │
│  + 0.20 × performance_score(N, T)  ← NCS provides     │  ← predicted value
│  + 0.10 × availability_score(N)    ← NCS contributes  │  ← uptime proxy
└───────────────────────────────────────────────────────┘

NCS provides:
  1. ncs_multiplier(N, T)    — scales declared capability by actual hardware
  2. predicted performance   — for new nodes with no empirical write history
  3. load awareness          — free RAM + free disk feed into load_ratio
```

---

## 3. Hardware Metrics Collected

### 3.1 Storage Metrics

| Metric | Unit | How Measured |
|---|---|---|
| `disk_total_bytes` | bytes | OS API (`statvfs` / `GetDiskFreeSpaceEx`) |
| `disk_free_bytes` | bytes | OS API |
| `disk_type` | enum: NVMe \| SSD \| HDD \| RamDisk | `/sys/block/*/queue/rotational` (Linux), WMI (Windows) |
| `seq_write_mb_s` | MB/s | Write 256 MB sequential file, measure wall time |
| `seq_read_mb_s` | MB/s | Read same 256 MB file back sequentially |
| `rand_write_iops_4k` | IOPS | Write 4 KB blocks at random offsets over 10 s |
| `rand_read_iops_4k` | IOPS | Read 4 KB blocks at random offsets over 10 s |
| `rand_read_iops_64k` | IOPS | Read 64 KB blocks at random offsets (relevant for image/video) |

> **Benchmark file**: written to `./data/.bench_tmp` during the benchmark run and deleted afterward. Write flags: `O_DIRECT` on Linux, `FILE_FLAG_NO_BUFFERING` on Windows — to bypass OS cache and measure actual device speed.

### 3.2 CPU Metrics

| Metric | Unit | How Measured |
|---|---|---|
| `cpu_physical_cores` | count | `std::thread::hardware_concurrency()` / cpuid |
| `cpu_logical_threads` | count | `std::thread::hardware_concurrency()` |
| `cpu_base_freq_mhz` | MHz | `/proc/cpuinfo` or WMI |
| `cpu_l3_cache_mb` | MB | cpuid leaf |
| `sha256_throughput_mb_s` | MB/s | Hash 512 MB of data, measure throughput ← **ledger-relevant** |
| `sort_score` | normalized | Sort 10M 64-bit integers, normalize against reference |
| `compress_score` | normalized | LZ4-compress 100 MB buffer, measure throughput |
| `cpu_load_1m` | 0.0–N_threads | `/proc/loadavg` or Windows PDH counter |

> SHA-256 throughput is the **primary CPU benchmark** because it directly represents how fast the node can process Change Ledger and Routing Ledger entries. A node that hashes faster writes and verifies ledgers faster.

### 3.3 Memory Metrics

| Metric | Unit | How Measured |
|---|---|---|
| `ram_total_bytes` | bytes | OS API |
| `ram_free_bytes` | bytes | OS API (available, not just free) |
| `ram_bandwidth_gb_s` | GB/s | memcpy benchmark: copy 512 MB, measure throughput |
| `ram_latency_ns` | ns | Pointer-chasing benchmark (random linked-list traversal, 64 MB) |

### 3.4 Network Metrics (Per Peer)

| Metric | Unit | How Measured |
|---|---|---|
| `latency_ms` | ms | Round-trip ping (existing health ping, moving average over last 60 pings) |
| `bandwidth_mb_s` | MB/s | Transfer a 10 MB payload to each peer, measure throughput |

Network benchmarks run **after** peer connections are established, not during the initial startup benchmark. The initial `bandwidth_mb_s` is estimated from `latency_ms` using a conservative formula until measured.

### 3.5 GPU Metrics (Optional — Image/ML Nodes)

| Metric | Unit | How Measured |
|---|---|---|
| `gpu_present` | bool | CUDA / OpenCL probe |
| `gpu_vram_bytes` | bytes | Driver API |
| `gpu_compute_tflops` | TFLOPS | Optional: run small GEMM benchmark |

GPU metrics are **optional** and only collected if `[capabilities] gpu_accelerated = true` is set in `node_profile.toml`. Not used in the core NCS formula by default; exposed as a separate `gpu_score` field for future use.

---

## 4. NCS Component Score Formulas

All component scores are normalized to `[0.0, 1.0]` using `clamp(value / reference_max, 0.0, 1.0)`.

**Reference maxima** represent "excellent" hardware, not theoretical maximums. Any node at or above the reference gets a score of 1.0.

### 4.1 Storage Score

```
storage_score =
    0.40 × clamp(disk_free_bytes  / 500_GB,        0, 1)  // available capacity
  + 0.35 × clamp(seq_write_mb_s  / 3_000,          0, 1)  // sequential write (NVMe ref)
  + 0.25 × clamp(rand_write_iops_4k / 500_000,     0, 1)  // random write IOPS
```

> Sequential write is weighted more than random because most De-Sentry writes (ledger appends, brain file generation) are sequential. Random write matters for the primary store B-Tree index updates.

### 4.2 I/O Score

```
io_score =
    0.35 × clamp(seq_read_mb_s    / 3_000,          0, 1)  // sequential read
  + 0.30 × clamp(seq_write_mb_s   / 3_000,          0, 1)  // sequential write
  + 0.20 × clamp(rand_read_iops_4k  / 500_000,      0, 1)  // random read IOPS
  + 0.15 × clamp(rand_read_iops_64k / 100_000,      0, 1)  // large-block random read
```

> The 64K random read metric is specifically important for image/video nodes where individual files are large enough that random access patterns involve large block sizes.

### 4.3 CPU Score

```
available_cpu_fraction = 1 − clamp(cpu_load_1m / cpu_logical_threads, 0, 1)

cpu_score =
    0.35 × clamp(sha256_throughput_mb_s / 5_000,   0, 1)  // ledger hashing speed
  + 0.25 × clamp(cpu_logical_threads    / 16,       0, 1)  // parallelism
  + 0.20 × clamp(cpu_base_freq_mhz      / 4_000,   0, 1)  // single-thread speed
  + 0.20 × available_cpu_fraction                           // current headroom
```

> SHA-256 throughput gets the highest CPU weight — it directly limits how fast the ledger can process and verify entries under load.

### 4.4 Memory Score

```
memory_score =
    0.45 × clamp(ram_free_bytes    / 8_GB,          0, 1)  // available working memory
  + 0.35 × clamp(ram_bandwidth_gb_s / 50,           0, 1)  // bandwidth (DDR5 ref: ~50 GB/s)
  + 0.20 × clamp(100 / ram_latency_ns,              0, 1)  // low latency → high score
```

> Memory bandwidth matters for nodes doing in-memory schema inference (CSV parser), full-text indexing, or sorting large result sets. `100 / latency_ns` normalizes so that 100 ns latency → 1.0, 200 ns → 0.5, etc.

### 4.5 Network Score

```
// Per-peer score
peer_score(P) =
    0.50 × clamp(bandwidth_mb_s(P) / 1_000,        0, 1)  // 1 Gbps reference
  + 0.50 × clamp(10 / latency_ms(P),               0, 1)  // 1 ms → 1.0, 10 ms → 0.1

// Aggregate across all peers (weighted average: closer peers weight more)
network_score = mean(peer_score(P) for P in reachable_peers)
```

> `10 / latency_ms` normalizes so that 10 ms → 1.0, 20 ms → 0.5. On loopback (course demo), latency will be ~0.1 ms → near-perfect score; this is expected.

---

## 5. Overall NCS Formula

```
NCS(N) = w_storage × storage_score(N)
        + w_io      × io_score(N)
        + w_cpu     × cpu_score(N)
        + w_memory  × memory_score(N)
        + w_network × network_score(N)
```

### Default Global Weights

| Component | Weight | Rationale |
|---|---|---|
| I/O | **0.30** | Highest — database performance is almost always I/O bound |
| CPU | **0.25** | Second — ledger hashing, compression, parsing |
| Storage | **0.20** | Capacity determines how much data the node can hold |
| Memory | **0.15** | Affects in-memory operations and caching |
| Network | **0.10** | Lowest — on loopback all nodes are equal; more relevant in multi-machine deployments |

---

## 6. Type-Specific NCS Weighting (`ncs_multiplier`)

Different data types stress different hardware dimensions. The NCS weights are **re-applied per data type** to compute `ncs_multiplier(N, T)` — the factor that scales a node's declared capability score in the fitness formula.

```
ncs_multiplier(N, T) = normalized NCS computed with type-specific weights for T
```

### Weight Table by Data Type

| Data Type | Storage | I/O | CPU | Memory | Network |
|---|---|---|---|---|---|
| `image/*`, `video/*` | 0.25 | **0.40** | 0.10 | 0.15 | 0.10 |
| `text/csv`, `text/xml`, `application/json` | 0.15 | 0.20 | **0.30** | **0.25** | 0.10 |
| `text/plain`, Markdown, HTML | 0.15 | 0.15 | **0.35** | **0.25** | 0.10 |
| Time-series, metrics | 0.20 | **0.30** | 0.20 | 0.20 | 0.10 |
| Audio (`audio/*`) | 0.25 | **0.35** | 0.15 | 0.15 | 0.10 |
| Binary / `application/octet-stream` | **0.30** | **0.30** | 0.15 | 0.15 | 0.10 |
| Generic / `*/*` | 0.20 | 0.25 | 0.20 | 0.20 | 0.15 |

**Rationale:**
- **Images/video** are I/O dominated — large sequential reads and writes dwarf computation.
- **CSV/XML/JSON** are CPU and memory dominated — parsing, schema inference, columnar transformation.
- **Text/FTS** is CPU and memory dominated — inverted index construction is compute-intensive.
- **Generic** gives slightly more weight to network — the fallback node needs to relay data.

---

## 7. Benchmark Schedule

### 7.1 Startup (Full Benchmark — runs once at node boot)

```
1. Storage + I/O benchmark  (~3–8 seconds, depends on disk speed)
   - Write 256 MB sequential
   - Read 256 MB sequential
   - 10 s random 4K write burst
   - 10 s random 4K read burst
   - 10 s random 64K read burst

2. CPU benchmark  (~2–3 seconds)
   - SHA-256 of 512 MB buffer (multi-threaded)
   - Sort 10M uint64 values
   - LZ4 compress 100 MB

3. Memory benchmark  (~1–2 seconds)
   - 512 MB memcpy bandwidth
   - 64 MB pointer-chase latency

4. Network benchmark  (~1 s per peer, runs after connections established)
   - 10 MB payload to each peer (bidirectional)
   - Latency from existing health ping moving average

Total startup benchmark time: ~20–30 seconds
```

### 7.2 Lightweight Refresh (Every `NCS_LIGHT_REFRESH_S` = 30 s)

Cheap dynamic metrics only — no disk I/O benchmark:

```
- disk_free_bytes     (statvfs call, < 1 ms)
- ram_free_bytes      (OS API, < 1 ms)
- cpu_load_1m         (read from /proc/loadavg or PDH, < 1 ms)
- network latency     (from heartbeat ping moving average)
```

These feed directly into the NCS without re-running the full benchmark. A node that was idle at startup but is now at 95% CPU load will have its NCS degraded in real time.

### 7.3 Full Re-Benchmark (On Demand or Every `NCS_FULL_REFRESH_H` = 6 h)

The full storage and CPU benchmark suite re-runs every 6 hours (configurable). This catches:
- SSD wear-induced throughput degradation
- Disk filling up
- Hardware throttling under sustained load

Triggered via:
- Timer (`NCS_FULL_REFRESH_H`)
- Coordinator request: `POST /api/v1/benchmark/run`
- Significant change detected in lightweight refresh (e.g., free disk drops by >20%)

---

## 8. NCS C++ Structure

```cpp
struct StorageMetrics {
    uint64_t disk_total_bytes;
    uint64_t disk_free_bytes;
    std::string disk_type;          // "NVMe", "SSD", "HDD", "RamDisk"
    double seq_read_mb_s;
    double seq_write_mb_s;
    double rand_read_iops_4k;
    double rand_write_iops_4k;
    double rand_read_iops_64k;
};

struct CpuMetrics {
    int physical_cores;
    int logical_threads;
    int base_freq_mhz;
    int l3_cache_mb;
    double sha256_throughput_mb_s;
    double sort_score;              // normalized [0,1]
    double compress_score;          // normalized [0,1]
    double load_1m;                 // dynamic — updated on light refresh
};

struct MemoryMetrics {
    uint64_t ram_total_bytes;
    uint64_t ram_free_bytes;        // dynamic — updated on light refresh
    double bandwidth_gb_s;
    double latency_ns;
};

struct PeerNetworkMetrics {
    std::string peer_id;
    double latency_ms;              // dynamic — from ping moving average
    double bandwidth_mb_s;
};

struct HardwareProfile {
    StorageMetrics   storage;
    CpuMetrics       cpu;
    MemoryMetrics    memory;
    std::vector<PeerNetworkMetrics> network;
    int64_t          last_full_benchmark_us;
    int64_t          last_light_refresh_us;
};

struct NCSResult {
    double storage_score;
    double io_score;
    double cpu_score;
    double memory_score;
    double network_score;
    double overall;                 // weighted aggregate
    std::unordered_map<std::string, double> type_multipliers;
    // e.g. { "image/*": 0.91, "text/csv": 0.63, "*/*": 0.74 }
};

class NodeCapabilityScorer {
public:
    HardwareProfile run_full_benchmark();
    void            run_light_refresh(HardwareProfile& profile);
    NCSResult       compute_ncs(const HardwareProfile& profile) const;
    double          ncs_multiplier(const NCSResult& ncs, const std::string& mime_type) const;
};
```

---

## 9. NCS in the Brain File

Every brain file includes the full hardware profile and NCS result:

```json
{
  "node_id": "A",
  "hardware_profile": {
    "last_full_benchmark_us": 1735000000000000,
    "last_light_refresh_us":  1735000060000000,
    "storage": {
      "disk_total_bytes":    1099511627776,
      "disk_free_bytes":      549755813888,
      "disk_type":           "NVMe",
      "seq_read_mb_s":       3200.0,
      "seq_write_mb_s":      2800.0,
      "rand_read_iops_4k":   450000,
      "rand_write_iops_4k":  380000,
      "rand_read_iops_64k":   95000
    },
    "cpu": {
      "physical_cores":        8,
      "logical_threads":      16,
      "base_freq_mhz":      3600,
      "l3_cache_mb":          16,
      "sha256_throughput_mb_s": 4200.0,
      "sort_score":           0.85,
      "compress_score":       0.78,
      "load_1m":              0.42
    },
    "memory": {
      "ram_total_bytes":  17179869184,
      "ram_free_bytes":   12884901888,
      "bandwidth_gb_s":        45.2,
      "latency_ns":            68.0
    },
    "network": {
      "B": { "latency_ms": 0.4, "bandwidth_mb_s": 950.0 },
      "C": { "latency_ms": 0.3, "bandwidth_mb_s": 980.0 }
    }
  },
  "ncs": {
    "overall":        0.74,
    "storage_score":  0.69,
    "io_score":       0.88,
    "cpu_score":      0.82,
    "memory_score":   0.71,
    "network_score":  0.96,
    "type_multipliers": {
      "image/*":             0.91,
      "video/*":             0.88,
      "text/csv":            0.63,
      "application/json":    0.65,
      "text/plain":          0.60,
      "*/*":                 0.74
    }
  }
}
```

---

## 10. Routing Ledger Entry: `HARDWARE_BENCHMARK`

Every time a full or light benchmark completes, a `HARDWARE_BENCHMARK` entry is appended to the Routing Ledger (written by the node, forwarded to the coordinator):

```json
{
  "entry_type": "HARDWARE_BENCHMARK",
  "node_id":    "A",
  "timestamp_us": 1735000000000000,
  "benchmark_type": "full",
  "ncs_overall":    0.74,
  "ncs_delta":     +0.03,
  "hardware_profile": { ... },
  "triggered_by": "startup",
  "prev_hash": "...",
  "entry_hash": "..."
}
```

`ncs_delta` is the change from the previous benchmark. A significant negative delta (e.g., `ncs_delta < -0.10`) triggers a `RANKING_UPDATE` from the coordinator, repricing this node's fitness scores across all MIME types.

---

## 11. Reference Hardware Table

Normalization reference values used in the NCS formulas:

| Metric | Reference ("excellent") | "Good" (≈0.5 score) | "Minimal" (≈0.1 score) |
|---|---|---|---|
| `seq_read_mb_s` | 3,000 | 550 | 80 |
| `seq_write_mb_s` | 3,000 | 500 | 60 |
| `rand_read_iops_4k` | 500,000 | 80,000 | 100 |
| `rand_read_iops_64k` | 100,000 | 20,000 | 500 |
| `sha256_throughput_mb_s` | 5,000 | 1,200 | 200 |
| `cpu_logical_threads` | 16 | 8 | 2 |
| `cpu_base_freq_mhz` | 4,000 | 2,500 | 1,000 |
| `ram_free_bytes` | 8 GB | 4 GB | 512 MB |
| `ram_bandwidth_gb_s` | 50 | 25 | 8 |
| `ram_latency_ns` | 100 ns | 200 ns | 500 ns |
| `network_bandwidth_mb_s` | 1,000 | 500 | 10 |
| `network_latency_ms` | 1 | 5 | 50 |

> **Note**: Reference values are set conservatively so that modern consumer hardware (e.g., a 2023 laptop with NVMe) scores around 0.6–0.8 overall — leaving room for high-end server hardware to score near 1.0 without saturating the scale.

---

## 12. Interaction with Fitness Score (Updated Formula)

The full updated fitness formula incorporating NCS:

```
// For new nodes: use NCS-predicted performance
// For experienced nodes: use empirical write latency measurements
effective_performance(N, T) =
    IF has_empirical_data(N, T):
        α × empirical_performance(N, T) + (1 − α) × ncs_predicted_performance(N, T)
    ELSE:
        ncs_predicted_performance(N, T)

where α = min(1.0, write_count(N, T) / 100)
// Blend from pure NCS prediction → pure empirical as write history grows

// Full fitness formula
fitness(N, T) =
    0.45 × capability_score(N, T) × ncs_multiplier(N, T)
  + 0.25 × (1 − load_ratio(N))         // uses NCS: free disk + free RAM
  + 0.20 × effective_performance(N, T)  // uses NCS until empirical data available
  + 0.10 × availability_score(N)
```

The blending factor `α` ramps from 0 → 1 as the node accumulates write history for type T. At 100 writes, the empirical measurement fully dominates and NCS prediction is just a 0% weight backup. This prevents new nodes from being discriminated against just because they have no write history.
