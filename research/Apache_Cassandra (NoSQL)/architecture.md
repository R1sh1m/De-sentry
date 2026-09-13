# Apache Cassandra — Architecture & Schema

> Interactive viewer: [architecture.html](architecture.html) · Pros/cons: [pros_cons.md](pros_cons.md) · Use cases: [use_cases.md](use_cases.md) · Deployment: [deployment.md](deployment.md)

## Overview

Apache Cassandra is a **distributed, masterless (peer-to-peer), wide-column NoSQL
database** designed for massive scale, high write throughput, and always-on
availability across multiple datacenters. It was originally inspired by Amazon
Dynamo (distribution) and Google Bigtable (storage model) and is now an Apache
top-level project.

Architecturally it is an **AP system** under the CAP theorem: when a network
partition occurs, Cassandra chooses **Availability** and **Partition tolerance**
over strict consistency, offering *tunable* consistency instead of forfeiting
availability. Its storage engine is **LSM-tree (Log-Structured Merge-tree)**
based — writes are buffered in memory and flushed sequentially to immutable
sorted files, which makes it extremely write-efficient.

**When to reach for it:**
- You need to store and serve huge volumes of data (hundreds of TB to PB) with
  predictable low-latency reads/writes.
- Your workload is write-heavy or append-heavy (time-series, events, logs).
- You require **multi-datacenter active-active** replication with no single
  point of failure.
- Linear horizontal scaling by simply adding commodity nodes is a hard
  requirement.
- You can model your data around a well-known partition key (query-first
  modeling).

**When it is the wrong tool:** ad-hoc analytics with joins, strongly consistent
multi-entity transactions, small datasets, or complex relational queries. See
[use_cases.md](use_cases.md) and [pros_cons.md](pros_cons.md).

---

## Logical architecture

Cassandra has no master node and no shared disk. All nodes are equal peers.
Understanding the following concepts is key to using it well.

### The token ring and consistent hashing
The cluster is a **ring** of tokens in the range `[-2^63, 2^63-1]` produced by
the **Murmur3** partitioner (the default; `RandomPartitioner`/`ByteOrderedPartitioner`
are legacy). A row's **partition key** is hashed to a token; the node that owns
the range containing that token is its primary replica. This consistent-hashing
ring is what lets any node route a request without a lookup service.

### vnodes (virtual nodes)
Instead of one token range per node, each node is assigned many small, randomly
distributed **vnodes** (`num_tokens`, default **256**). Benefits:
- Data and load are spread more evenly across the cluster.
- Adding/removing a node moves only a fraction of ranges (auto-rebalance),
  avoiding manual resharding and "hot" nodes.

### Gossip and failure detection
Nodes exchange state (liveness, schema version, token ranges) via a
**gossip protocol** every second over ports **7000/7001**. An **accrual
failure detector** (Phi) decides whether a peer is unreachable; once marked
down it is excluded from replica sets and routing until it recovers.

### Snitch and topology
The **snitch** maps IPs to datacenter/rack and tells the coordinator which
replicas are closest (`GossipingPropertyFileSnitch`, `Ec2Snitch`, `Ec2MultiRegionSnitch`,
etc.). It drives replica placement and lets `LOCAL_QUORUM` prefer local replicas
to avoid cross-DC latency.

### Coordinator
Any node that receives a client request becomes its **Coordinator** for that
request. It validates the CQL, computes the replica set via the partitioner, and
fans the operation out to the replicas according to the requested consistency
level. The coordinator itself stores nothing for that request — it orchestrates.

### Tunable consistency
Consistency is a **per-statement** knob, not a cluster-wide mode:
- `ONE` — one replica acknowledges.
- `TWO` / `THREE` — N replicas.
- `QUORUM` — majority of all replicas: `floor(RF/2)+1`.
- `LOCAL_QUORUM` — majority within the local datacenter (no cross-DC round trip).
- `ALL` — every replica.
- Plus `ANY` (for writes, at least one copy somewhere) and `SERIAL`/`LOCAL_SERIAL`
  for lightweight transactions (linearizable via Paxos).

Read and write levels can be set independently; picking `W + R > RF` prevents
stale reads.

---

## Write path

1. **Client sends CQL** (`INSERT`/`UPDATE`) over the native protocol (9042) to
   any node, specifying a consistency level.
2. That node becomes the **Coordinator** and validates/parses the CQL.
3. The **partitioner** (Murmur3) hashes the partition key to a token; the
   coordinator determines the **replica set** using the keyspace's replication
   strategy and replication factor.
4. On **each replica**, the mutation is appended to the **commitlog** (write-ahead
   log) for durability, then inserted into the in-memory **memtable**.
5. Replicas send acknowledgements back to the coordinator. The coordinator waits
   until it has enough acks to satisfy the consistency level (`ONE`, `QUORUM`,
   `LOCAL_QUORUM`, `ALL`), then returns success to the client.
6. When a memtable exceeds its threshold it is **flushed** to an immutable
   **SSTable** on disk. The commitlog segment is discarded once all its
   memtables have flushed.
7. Background **compaction** later merges SSTables, keeping the newest value per
   cell and purging tombstones.

Because writes are sequential appends (commitlog) plus in-memory inserts
(memtable), Cassandra sustains very high write throughput.

---

## Read path

1. **Client sends a CQL `SELECT`** to any node (the coordinator) with a
   consistency level.
2. The coordinator hashes the partition key, finds the **replica set**, and
   sends read commands to the replicas required by the consistency level
   (typically the closest `R` replicas).
3. On each replica the data is assembled from, in order:
   - the **memtable** (newest in-memory writes),
   - the **row cache** (if the partition is cached),
   - then **SSTables**, located efficiently via the **Bloom filter** (skip
     files that definitely don't contain the partition), the **partition
     index/summary**, and the **clustering-column index** within the partition.
4. The coordinator **merges** the returned fragments and reconciles conflicts
   with **last-write-wins** — the cell with the highest timestamp wins; a
   tombstone (delete marker) hides deleted data.
5. If a digest mismatch is detected, the coordinator performs a **read repair**,
   pushing the reconciled, newer data to inconsistent replicas.
6. The reconciled row is returned to the client.

---

## Storage engine

Cassandra's LSM-tree engine is the reason for its write performance.

### Commitlog
A sequentially written, append-only WAL per node. Guarantees durability: if a
node crashes before a memtable flushes, the commitlog is replayed on restart to
rebuild lost memtables. Never read on the normal path — only for recovery.

### Memtable
An in-memory, sorted (by partition key, then clustering columns) buffer of
recent mutations. Reads check it first. Flushed to SSTable when it exceeds
`memtable_flush_writers` / size thresholds.

### SSTable
**Sorted-string table** — immutable, on-disk, ordered files produced by
flushing a memtable. For each partition Cassandra keeps, per SSTable:
- **Data.db** — the actual cells.
- **Index.db** — partition offsets.
- **Summary.db** — sparse sample of the index (for fast skips).
- **Filter.db** — the **Bloom filter**.
- **Statistics.db / Digest** — metadata for compaction and validation.

Because SSTables are immutable, an `UPDATE` is just a newer SSTable; a `DELETE`
is a **tombstone**. Over time many SSTables accumulate for a partition.

### Compaction
Background merge that collapses SSTables, keeps the newest timestamp per cell,
and reclaims space by dropping overwritten/stale data and expired tombstones.
Strategies:
- **STCS** (Size-Tiered Compaction Strategy) — default; best for write-heavy,
  append-mostly workloads.
- **LCS** (Leveled Compaction Strategy) — bounds read amplification; best for
  read-heavy / update-heavy workloads.
- **TWCS** (Time-Window Compaction Strategy) — ideal for time-series data with
  TTLs, where whole time windows can be dropped at once.

### Caches and indexes
- **Key cache** — caches partition-index offsets (on by default).
- **Row cache** — caches entire hot partitions (off-heap; use sparingly).
- **Bloom filter** — probabilistic, near-zero-cost negative lookup to skip
  SSTables that can't contain a partition.

---

## Replication & consistency

- **Replication factor (RF)** is set per keyspace (e.g. `RF=3`).
- **SimpleStrategy** — places the next N nodes clockwise on the ring. Use only
  for single-DC development.
- **NetworkTopologyStrategy** — places replicas per datacenter (e.g. `3` in
  `dc1`, `2` in `dc2`) while avoiding same-rack collisions when possible. Use
  this in production / multi-DC.
- **Tunable consistency** (see above) lets you trade latency/availability for
  durability per statement. `QUORUM`/`LOCAL_QUORUM` are the common sweet spots.
- **Hinted handoff**, **read repair**, and **anti-entropy repair**
  (`nodetool repair`) keep replicas consistent over time. Hinted handoff and
  read repair handle transient issues; `nodetool repair` (ideally via
  incremental repair) fixes silent drift and is essential for correctness.

---

## Data / schema model

Cassandra is **query-first**: you design tables to serve specific queries, not
to normalize data. Core objects:

- **Keyspace** — the top-level container (like a database/schema), holding
  replication settings.
- **Table (column family)** — a set of rows sharing a schema.
- **Partition key** — determines which node(s) store the row (via the
  partitioner). Choosing a good, high-cardinality partition key is the single
  most important modeling decision.
- **Clustering columns** — order rows *within* a partition; enable efficient
  range scans and ordering.
- **Primary key** = partition key (+ optional clustering columns).
- **Static columns** — shared across all rows in a partition.

### Concrete CQL example

```sql
-- A keyspace replicated across two datacenters, 3 replicas in dc1, 2 in dc2.
CREATE KEYSPACE IF NOT EXISTS iot
  WITH replication = {
    'class': 'NetworkTopologyStrategy',
    'dc1': 3,
    'dc2': 2
  }
  AND durable_writes = true;

USE iot;

-- Time-series sensor readings: partition by device per day,
-- cluster by reading time so the newest are easy to range-scan.
CREATE TABLE IF NOT EXISTS sensor_readings (
  device_id   uuid,
  day         date,
  recorded_at timestamp,
  metric      text,
  value       double,
  PRIMARY KEY ((device_id, day), recorded_at, metric)
) WITH CLUSTERING ORDER BY (recorded_at DESC, metric ASC)
  AND compaction = {
    'class': 'TimeWindowCompactionStrategy',
    'compaction_window_unit': 'DAYS',
    'compaction_window_size': 1
  };

INSERT INTO sensor_readings (device_id, day, recorded_at, metric, value)
VALUES (a1b2c3d4-..., '2026-08-29', toTimestamp(now()), 'temp_c', 21.4);

SELECT * FROM sensor_readings
WHERE device_id = a1b2c3d4-... AND day = '2026-08-29'
LIMIT 100;
```

Notes:
- The **composite partition key** `(device_id, day)` keeps each partition small
  and bounded (one device's data for one day), which is a best practice for
  time-series.
- **Clustering columns** make `ORDER BY recorded_at DESC` free and support
  efficient slices.
- Denormalization (multiple tables for the same entity) is normal and expected.

---

## Scaling

- **Add a node:** bootstrap a new Cassandra instance, point it at the existing
  seed nodes, and it automatically:
  - joins the ring via gossip,
  - picks up its share of vnodes (random token ranges),
  - streams the relevant data from current owners (no downtime, no manual
    resharding).
- **Remove a node:** `nodetool decommission` streams its data to other nodes
  before it leaves; the ring rebalances automatically.
- **No manual sharding:** consistent hashing + vnodes mean you never partition
  or re-partition by hand. Capacity planning is mostly "add nodes until you have
  enough RF × headroom."
- **Multi-DC:** add nodes in a new DC with `NetworkTopologyStrategy`;
  `LOCAL_QUORUM` keeps local reads/writes fast while `QUORUM` across DCs gives
  global durability.

---

## Failure handling

- **Node down:** gossip/Phi failure detection marks it down; coordinators route
  around it. Writes at `ONE`/`QUORUM` (with RF≥2/3) succeed without it.
- **Hinted handoff:** when a replica is briefly down, peers store **hints**
  (pending mutations) and replay them on recovery — covers short outages.
- **Read repair:** on reads, if replicas disagree, the coordinator reconciles
  (last-write-wins) and pushes the correct value back — self-healing on access.
- **Anti-entropy repair:** for longer outages, silent divergence, or deleted
  data, run `nodetool repair` (prefer **incremental repair**) regularly; it
  uses Merkle trees to find and fix differing ranges.
- **Network partition:** an AP system keeps serving within each side of the
  partition (availability); reconciliation happens later via repair. This is the
  deliberate CAP trade-off.
- **Tombstones & repair:** deletes are tombstones that must be propagated via
  repair before `gc_grace_seconds` expires, or deleted data can resurrect.
