# Pros and Cons

This document gives a balanced view of Apache Cassandra's strengths and
weaknesses so you can make an informed choice. See also
[architecture.md](architecture.md) and [use_cases.md](use_cases.md).

## Pros

- **Linear horizontal scalability.** Adding nodes adds capacity (throughput and
  storage) in near-linear proportion, with no manual resharding thanks to
  consistent hashing and vnodes. Clusters routinely run at hundreds of nodes and
  petabyte scale.

- **Masterless — no single point of failure.** Every node is equal; any node
  can serve any request as coordinator. There is no master to fail over or to
  become a bottleneck, which makes the system inherently resilient and
  operationally simpler in terms of "who is the leader."

- **Tunable consistency.** You choose per statement how many replicas must
  acknowledge (`ONE`, `QUORUM`, `LOCAL_QUORUM`, `ALL`, etc.). This lets you
  dial the availability/latency vs. consistency trade-off to fit each query
  rather than being locked into one policy.

- **Very high write throughput.** The LSM-tree engine (sequential commitlog +
  in-memory memtable, later flushed to immutable SSTables) is optimized for
  write-heavy and append-heavy workloads, sustaining hundreds of thousands of
  writes/sec per node.

- **Multi-datacenter active-active.** `NetworkTopologyStrategy` and tunable
  consistency make geographically distributed, active-active deployments
  first-class. `LOCAL_QUORUM` keeps local latency low while still replicating
  globally; there is no cold standby you have to fail over to.

- **Proven at massive scale.** Battle-tested by Netflix, Apple, Instagram,
  Spotify, and others for workloads with extreme scale and uptime requirements.

- **Predictable low latency at scale.** Because reads/writes are O(1)-ish to a
  partition via the partitioner + Bloom filters + indexes, latency stays flat as
  data grows (as long as your partition key is well chosen).

- **No read-before-write for normal writes.** Plain inserts/updates are blind
  appends; you don't pay a read cost to write, which is ideal for telemetry and
  event ingestion.

## Cons

- **Poor fit for ad-hoc analytics and joins.** Cassandra has no joins, no
  subqueries, and very limited aggregations. You cannot ask arbitrary
  relational questions; you must model tables around the queries you need. Heavy
  analytics belong in a warehouse or Spark (which *can* read Cassandra).

- **Eventual consistency / last-write-wins can lose updates.** Without
  `SERIAL`/lightweight transactions, two concurrent writes resolve by
  **timestamp**, not by merging — the later writer silently wins, so
  read-modify-write counters or "decrement inventory" patterns can lose
  updates unless you use counters or LWTs (which are expensive).

- **No referential integrity or ACID across entities.** There are no foreign
  keys, no constraints, no multi-row/multi-table transactions (except limited
  single-partition atomicity and costly Paxos-based LWTs). Correctness of
  denormalized data is the application's responsibility.

- **Requires denormalization and careful data modeling.** You must duplicate
  data into query-specific tables and pick partition keys that avoid hot or
  oversized partitions. Bad modeling (e.g. a single giant partition) causes
  severe hot-spotting and read latency. This is a real design burden.

- **Operational complexity.** Running it well means understanding gossip,
  vnodes, repair (`nodetool repair` must be run routinely), compaction
  strategy selection, JVM heap sizing, and disk I/O. "Masterless" removes
  leader-failover pain but shifts work to cluster-wide operational hygiene.

- **Read-before-write for lightweight transactions and some updates.** LWTs
  (`IF NOT EXISTS`, conditional updates) use Paxos and are several times slower
  than normal writes. Similarly, `COUNTER` increments or updating a row whose
  existence you must check incur extra round trips.

- **Repair and compaction overhead.** Anti-entropy repair and compaction are
  essential for correctness and space reclamation but consume I/O, CPU, and
  sometimes latency. Under-provisioned clusters can suffer compaction backlog
  or repair storms; `gc_grace_seconds`/tombstone handling must be managed or
  deleted data can resurrect.

- **Not memory/disk cheap for small data.** The machinery (JVM, replication
  factor of 3, multiple SSTables per partition) is overkill for small datasets
  that a single Postgres instance would serve more simply and cheaply.

## Nuance: it's a spectrum, not a verdict

Cassandra is not "better" or "worse" than an RDBMS in the abstract — it trades
multi-entity transactional guarantees and relational flexibility for scale,
availability, and write throughput. The right question is whether your access
patterns are **known in advance and partitionable** (Cassandra shines) or
**ad-hoc, relational, and consistency-critical across entities** (use a
relational/NewSQL store). Many architectures use both: Cassandra for the hot
write path and a warehouse/OLTP DB for analytics and transactions.
