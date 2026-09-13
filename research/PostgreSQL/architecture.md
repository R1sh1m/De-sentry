# PostgreSQL — Architecture & Schema

## Overview

PostgreSQL is an advanced open-source **object-relational database management system**
(ORDBMS) renowned for its standards compliance, extensibility, and rock-solid
**ACID** guarantees. It uses **Multiversion Concurrency Control (MVCC)** so that
readers never block writers and writers never block readers.

- **First release:** 1996 (derived from the POSTGRES project at UC Berkeley, 1986)
- **License:** PostgreSQL License (permissive, BSD-like) — free for commercial use
- **Latest stable line:** PostgreSQL 17
- **Implementation language:** C
- **Process model:** one OS process per connection (forked from the `postmaster`)
- **Default port:** 5432

At a high level PostgreSQL is organized into:

1. **Client tier** — applications, drivers, ORMs
2. **Connection tier** — postmaster + backend processes, optional poolers
3. **Query tier** — parser, rewriter, planner/optimizer, executor
4. **Memory / shared tier** — shared buffers, WAL buffers, work memory
5. **Storage / engine tier** — heap files, indexes, MVCC, VACUUM
6. **Durability / replication tier** — WAL, checkpointer, background writer, replicas

---

## Process architecture

PostgreSQL uses a **multi-process** architecture. The `postmaster` is the
parent daemon; it spawns a dedicated backend process for every client connection
and a set of long-lived **background workers**.

```
                +-------------------+
                |     postmaster    |  (listens on :5432)
                +--------+----------+
        forks            | spawns
        +----------------+-------------------+
        |                |                   |
   backend #1 ...   background workers   autovacuum launcher
   backend #N         (see below)        stats collector
```

### postmaster
- Starts at boot, reads `postgresql.conf`, allocates shared memory, starts
  background workers.
- Listens for new TCP connections and forks a **backend** for each.
- Restarts failed background workers; a backend crash does **not** take down
  the whole server (only that connection).

### Per-connection backends (`postgres`)
- Handle authentication, parse, plan, execute, and return results for one client.
- Hold **private** memory (e.g. `work_mem` per sort/hash operation).
- On disconnect the process exits and its memory is released.

### Background workers
| Worker | Purpose |
|--------|---------|
| **Background writer** | Periodically writes dirty shared-buffer pages to disk to reduce checkpoint spikes |
| **Checkpointer** | Performs checkpoints — flushes all dirty buffers and updates the redo pointer |
| **WAL writer** | Batches WAL records from the WAL buffer to the WAL files on disk |
| **Autovacuum launcher/workers** | Reclaims dead tuples (MVCC bloat) and updates planner statistics |
| **Archiver** | Copies completed WAL segment files to an archive location (for PITR) |
| **Stats collector** | Collects table/index usage, live/dead tuple counts used by the planner |
| **Logical replication launcher** | Manages logical replication workers |

---

## Memory

PostgreSQL memory splits into **shared** (global, across backends) and
**local/per-backend** regions.

### Shared memory
| Structure | Tunable | Purpose |
|-----------|---------|---------|
| **Shared Buffers** | `shared_buffers` (default 128 MB, often 25% of RAM) | Cache of table/index pages; the primary read cache |
| **WAL buffers** | `wal_buffers` (default 16 MB) | Staging area for WAL records before flush |
| **CLOG / commit status** | — | Tracks transaction commit/abort status for visibility |

### Per-backend (local) memory
| Structure | Tunable | Purpose |
|-----------|---------|---------|
| **work_mem** | `work_mem` (default 4 MB) | Per sort/hash-join operation; can be allocated many times per query |
| **maintenance_work_mem** | `maintenance_work_mem` (default 64 MB) | For VACUUM, CREATE INDEX, ALTER TABLE |
| **temp_buffers** | `temp_buffers` (default 8 MB) | For temporary tables used in a session |

> **Tip:** Because `work_mem` is *per operation*, a complex query with many
> sorts can multiply memory usage. Size it carefully on high-concurrency systems.

---

## MVCC (Multiversion Concurrency Control)

PostgreSQL implements MVCC by keeping **multiple versions** of each row. Every
row (tuple) carries transaction bookkeeping:

- **`xmin`** — transaction ID that *created* the tuple (inserted/updated it).
- **`xmax`** — transaction ID that *deleted/updated* the tuple (0 if still live).
- **`t_xact`** — infomask flags (committed/aborted, hint bits).
- **`ctid`** — physical location (block, offset) of the tuple.

### Visibility & snapshots
When a transaction starts (at the appropriate isolation level), it takes a
**snapshot** of currently active transactions. A tuple is **visible** to a
transaction `T` if:

- `xmin` is committed **and** `xmin` is not in `T`'s active set (or `xmin == T`),
- **and** (`xmax == 0` **or** `xmax` is not committed / is in `T`'s active set).

Because visibility is computed from row metadata and a snapshot, **readers do
not take locks and never block writers, and writers never block readers.** This
is what makes PostgreSQL concurrency so smooth.

### The cost: bloat and VACUUM
Old row versions that no longer visible to *any* transaction become **dead
tuples**. They still occupy space until removed.

- **`VACUUM`** marks dead tuples as free space for reuse (does not return space
  to the OS).
- **`VACUUM FULL`** rewrites the table and compacts it (exclusive lock).
- **`AUTOVACUUM`** runs in the background automatically based on
  `autovacuum_vacuum_threshold` + `scale_factor` of tuples changed.

Disciplined autovacuum tuning is essential on write-heavy tables to avoid
**bloat**, which degrades performance and can cause transaction-ID wraparound
issues.

---

## WAL & durability

The **Write-Ahead Log (WAL)** is PostgreSQL's durability backbone, implementing
the *log-ahead* rule: **a change must be written to WAL before its page is
flushed to the heap/index files.**

1. Backend modifies a page in Shared Buffers and creates WAL records.
2. WAL records go to the **WAL buffer**, then the **WAL writer** flushes them to
   WAL segment files in `pg_wal/`.
3. `fsync` (controlled by `synchronous_commit`) guarantees the bytes hit disk.
4. Later, the **background writer / checkpointer** flushes the dirty data pages.

This gives **crash recovery**: on restart, PostgreSQL replays WAL from the last
checkpoint forward to reconstruct a consistent state — no lost committed
transactions (unless `synchronous_commit=off`).

- WAL segment size: 16 MB by default (configurable at initdb).
- `wal_level`: `minimal` | `replica` | `logical` (controls how much is logged).

---

## Replication

PostgreSQL supports two families of replication:

### Streaming physical replication (WAL shipping)
- A **primary** streams WAL to one or more **standby** servers.
- Standbys apply WAL continuously, staying nearly in sync.
- **Async** (default) — replica may lag slightly; **sync** — primary waits for
  replica acknowledgement (`synchronous_standby_names`) for zero data loss.
- Standbys can be **hot** (read-only queries) or **warm** (not queryable).
- Built on `primary_conninfo`, `standby.signal`, and `pg_basebackup`.

### Logical replication
- Replicates at the **table/row** level using a **publication/subscription**
  model.
- Allows selective replication, different versions/architectures, and
  heterogeneous downstream consumers.
- Enables zero-downtime upgrades and partial replication.

### Sync vs async
| Mode | Durability | Latency | Use |
|------|-----------|---------|-----|
| async | possible tiny loss on failover | low | most apps, read scaling |
| sync | no committed-data loss | higher | financial, compliance |

---

## Query processing

A SQL statement flows through four main stages:

```
SQL ──> Parser ──> Rewriter ──> Planner/Optimizer ──> Executor ──> Result
```

1. **Parser** — checks syntax and produces a parse tree.
2. **Rewriter** — applies **rules** and expands views / `WITH` (CTE) references.
3. **Planner / Optimizer** — generates possible plans, estimates cost using
   statistics, and picks the cheapest (seq scan vs index scan, join order,
   join algorithm: nested loop / hash / merge).
4. **Executor** — runs the plan, fetches/stores tuples via access methods.

### EXPLAIN
Use `EXPLAIN` / `EXPLAIN ANALYZE` to inspect plans:

```sql
EXPLAIN (ANALYZE, BUFFERS)
SELECT o.id, c.name
FROM orders o
JOIN customers c ON c.id = o.customer_id
WHERE o.total > 1000;
```

Key nodes: `Seq Scan`, `Index Scan`, `Bitmap Heap Scan`, `Hash Join`,
`Nested Loop`, `Aggregate`, `Sort`, `Gather` (parallel).

---

## Storage

PostgreSQL stores data in **heap files** (unordered row storage). Each table and
index is a **relation** backed by files in the tablespace directory.

- **Heap (relation)** — the table's row store; rows are appended/updated in
  pages (8 KB default).
- **TOAST** — large column values (text, bytea) are compressed/out-of-line
  stored automatically.

### Index types
| Index | Use case |
|-------|----------|
| **B-tree** | default; equality/range on scalar types |
| **GiST** | geometric, full-text, overlapping ranges |
| **GIN** | composite/array/JSONB, full-text (`@@`) |
| **BRIN** | naturally ordered, huge tables (e.g. time-series by timestamp) |
| **hash** | equality only (rarely needed; B-tree covers it) |
| **SP-GiST** | partitioned space (e.g. quadtree, k-d tree) |

---

## Write path

```
App ──SQL──> backend
  ├─ parse ─ rewriter ─ planner ─ executor
  ├─ modify page in Shared Buffers
  ├─ write WAL record → WAL buffer → WAL writer → WAL files (fsync)
  ├─ (dirty page marked; flush later by background writer / checkpointer)
  └─ commit returns after WAL durable (unless sync_commit=off)
        │
        └─ WAL streamed to standby(s) for replication
```

1. Backend receives SQL and parses/plans/executes.
2. New row version written to Shared Buffers; old version's `xmax` set.
3. **WAL written first** (durability guarantee).
4. Background writer/checkpointer eventually flushes heap/index pages to disk.
5. WAL is streamed to replicas for physical replication.

---

## Read path

```
App ──SELECT──> backend
  ├─ planner chooses Index Scan or Seq Scan
  ├─ read pages from Shared Buffers (cache hit) or disk
  ├─ MVCC visibility check (xmin/xmax, snapshot)
  ├─ return only visible rows
  └─ cache hits avoid disk I/O entirely
```

1. Planner picks the cheapest access path (index vs sequential scan).
2. Pages are read from Shared Buffers if present, else from disk.
3. Each tuple is checked for MVCC visibility against the transaction snapshot.
4. Only visible rows are returned; dead tuples are skipped.

---

## Data / schema model

PostgreSQL has a strict containment hierarchy:

```
Cluster (= one server instance)
 └─ Database
     └─ Schema (namespace)
         └─ Table / View / Materialized View / Sequence / Function / Type
```

- **Database** — separate catalog; cross-database queries are not native.
- **Schema** — namespace to organize objects (default `public`).
- **Table** — rows of typed columns.
- **Types** — rich: `int`, `numeric`, `text`, `uuid`, `jsonb`, `array`,
  `date/timestamp`, plus user-defined (`CREATE TYPE ... AS ENUM`, composite,
  domain).

### Constraints
- `PRIMARY KEY`, `FOREIGN KEY`, `UNIQUE`, `NOT NULL`, `CHECK`, `EXCLUDE`.

### Views & materialized views
- **View** — stored `SELECT` (virtual, re-evaluated each query).
- **Materialized view** — stored result, refreshed with `REFRESH MATERIALIZED VIEW`.

### Extensions
PostgreSQL is highly **extensible**:
- **PostGIS** — geospatial types & functions.
- **pgvector** — vector embeddings for AI/LLM similarity search.
- **pg_stat_statements** — query statistics.
- **citext**, **hstore**, **pgcrypto**, **PostgreSQL** FDW, etc.

### Concrete SQL examples

```sql
-- Create a table with constraints
CREATE TABLE customers (
    id          BIGSERIAL PRIMARY KEY,
    email       TEXT NOT NULL UNIQUE,
    name        TEXT NOT NULL,
    country     TEXT DEFAULT 'US',
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT chk_email CHECK (email ~* '^.+@.+\..+$')
);

-- Create a related table with a foreign key
CREATE TABLE orders (
    id          BIGSERIAL PRIMARY KEY,
    customer_id BIGINT NOT NULL REFERENCES customers(id) ON DELETE CASCADE,
    total       NUMERIC(12,2) NOT NULL CHECK (total >= 0),
    status      TEXT NOT NULL DEFAULT 'pending',
    placed_at   TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Index for fast lookups
CREATE INDEX idx_orders_customer ON orders (customer_id);
CREATE INDEX idx_orders_placed   ON orders (placed_at);
```

### JOIN query

```sql
-- Top customers by spend in the last 30 days
SELECT c.id, c.name, c.email,
       COUNT(o.id)            AS order_count,
       SUM(o.total)           AS total_spend
FROM customers c
JOIN orders o ON o.customer_id = c.id
WHERE o.placed_at >= now() - INTERVAL '30 days'
  AND o.status = 'paid'
GROUP BY c.id, c.name, c.email
ORDER BY total_spend DESC
LIMIT 10;
```

### Extension example

```sql
-- Enable a spatial extension
CREATE EXTENSION IF NOT EXISTS postgis;

-- Enable a vector extension for embeddings
CREATE EXTENSION IF NOT EXISTS vector;

-- Use pgvector for similarity search
CREATE TABLE documents (
    id      BIGSERIAL PRIMARY KEY,
    content TEXT,
    embedding vector(1536)
);
CREATE INDEX idx_docs_embedding
    ON documents USING hnsw (embedding vector_cosine_ops);
```

---

## Partitioning

PostgreSQL supports table partitioning to improve manageability and query
performance on large tables.

| Method | Key | Example |
|--------|-----|---------|
| **Range** | contiguous ranges | `PARTITION BY RANGE (placed_at)` |
| **List** | discrete values | `PARTITION BY LIST (country)` |
| **Hash** | hash of value | `PARTITION BY HASH (customer_id)` |

```sql
CREATE TABLE orders_2024 (
    LIKE orders INCLUDING ALL
) PARTITION BY RANGE (placed_at);

CREATE TABLE orders_2024_q1 PARTITION OF orders_2024
    FOR VALUES FROM ('2024-01-01') TO ('2024-04-01');
```

Partition pruning lets the planner skip irrelevant partitions (great for
time-series and multi-tenant data).

---

## Scaling

### Vertical scaling
- PostgreSQL scales well up changing to bigger machines; tune
  `shared_buffers`, `effective_cache_size`, `work_mem`, parallelism
  (`max_parallel_workers_per_gather`).

### Read scaling
- Add **read replicas** (streaming replication) and route reads to them.
- Use **connection poolers** (PgBouncer / Pgpool-II) to avoid per-connection
  process overhead.

### Write scaling
- **Partitioning** to keep individual tables small.
- **Connection pooling** to reduce backend count.
- **Citus** (extension) for true horizontal sharding across nodes.

### Connection pooling
- Each connection = one backend process + memory. At thousands of connections,
  use **PgBouncer** in `transaction` mode to multiplex onto a small pool of
  real backends.

---

## Summary

PostgreSQL couples enterprise-grade reliability (ACID, MVCC, WAL) with
exceptional extensibility (custom types, extensions, rich indexing). Its
multi-process design and disciplined VACUUM regime are the keys to stable
high-performance operation, while streaming and logical replication provide
flexible scaling and availability.
