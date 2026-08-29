# Pros and Cons

A balanced assessment of PostgreSQL for architectural decision-making.

## Pros

### Rock-solid ACID & MVCC
- Full **ACID** compliance with durable transactions backed by WAL.
- **MVCC** gives non-blocking reads/writes — readers never block writers and
  vice versa — producing excellent concurrency under mixed workloads.
- Mature crash recovery and point-in-time-recovery (PITR) via WAL archiving.
- Four **isolation levels**: Read Uncommitted (maps to Read Committed),
  Read Committed (default), Repeatable Read, and Serializable (using
  Serializable Snapshot Isolation / SSI) — no dirty reads, and true
  serializable behavior without locks in most cases.
- `SAVEPOINT`s allow partial rollback within a transaction, useful for
  complex workflows and ORMs.
- Two-phase commit (`PREPARE TRANSACTION`) supports distributed transactions
  when coordinated externally.
- Crash recovery replays WAL from the last checkpoint forward, guaranteeing
  that every committed transaction survives a power loss.

Example — a transactional block with a savepoint:

```sql
BEGIN;
INSERT INTO accounts(id, balance) VALUES (1, 100);
SAVEPOINT s1;
UPDATE accounts SET balance = balance - 50 WHERE id = 1;
-- oops, conditional logic in app decides to undo just this step:
ROLLBACK TO SAVEPOINT s1;
COMMIT;
```

### Rich, standards-compliant SQL
- Window functions, CTEs (including `WITH RECURSIVE`), `GROUPING SETS`,
  `FILTER`, `LATERAL` joins, `TABLESAMPLE`, and Common Table Expressions.
- Advanced types: `jsonb`, arrays, ranges (`int4range`, `tstzrange`),
  `uuid`, `numeric`, geometric, network (inet/cidr).
- First-class support for `CHECK`, `EXCLUDE`, partial indexes, and
  expression/functional indexes.
- `RETURNING` clauses let `INSERT`/`UPDATE`/`DELETE` return affected rows in
  one round trip.
- `UPSERT` via `INSERT ... ON CONFLICT DO UPDATE/DO NOTHING` (native
  conflict handling).
- Generated/computed columns, `ENUM` types, and domains with constraints.
- Broad SQL standard conformance, making migrations from other RDBMS easier.
- Array and `UNNEST` support enable set-based operations without joins.

Example — window functions and `FILTER` in one query:

```sql
SELECT department,
       COUNT(*) FILTER (WHERE active) AS active_emps,
       AVG(salary) OVER (PARTITION BY department) AS dept_avg
FROM employees
GROUP BY department, salary;
```

### Extensible by design
- **Extensions** let you add types, functions, operators, and index access
  methods without forking the codebase (PostGIS, pgvector, pg_stat_statements,
  hstore, citext, Postgres_FDW).
- User-defined types (`CREATE TYPE ... AS ENUM`, composite, domain),
  custom aggregates, and procedural languages (PL/pgSQL, PL/Python, PL/Perl).
- Foreign Data Wrappers (FDWs) make PostgreSQL a federation layer to other
  data stores.
- You can register custom **operator classes** and **index access methods**,
  which is how PostGIS and pgvector plug in new index types (GiST/GIN/HNSW).
- Background worker APIs allow writing custom daemons inside the server.
- This extensibility is a major reason PostgreSQL adapts to new workloads
  (spatial, vector, time-series) without abandoning the relational core.

### Strong indexing
- Multiple index types: **B-tree, GIN, GiST, BRIN, hash, SP-GiST**.
- Covering indexes (`INCLUDE`), partial indexes, and multicolumn indexes.
- PG 17 adds improved BRIN and SLRU improvements for heavy indexing.
- Indexes can be created **concurrently** (`CREATE INDEX CONCURRENTLY`) without
  locking writes — critical for live, large tables.
- Unique, partial (`WHERE` clause), and expression/functional indexes cover
  nearly every access pattern.
- BRIN is especially cheap for naturally ordered, append-only huge tables.

### Free & permissive license
- PostgreSQL License is BSD/permissive — safe for commercial and closed-source
  use with no copyleft obligations.

### Huge ecosystem & community
- Massive adoption, abundant tooling (psql, pgAdmin, DBeaver), ORMs
  (Hibernate, SQLAlchemy, Django, Rails, Prisma), and managed offerings
  (AWS RDS/Aurora, GCP Cloud SQL, Azure Database for PostgreSQL, Neon, Supabase).
- Long, stable release cadence (major version yearly) with clear upgrade paths.
- Extensive documentation, active mailing lists, and a large talent pool.
- Rich monitoring via `pg_stat_*` views and extensions like
  `pg_stat_statements` and `pgwatch`.
- Backwards-compatible wire protocol; clients rarely need changes across
  minor versions.

### Reliable replication & HA
- Built-in physical streaming replication (sync/async) and logical replication.
- Mature tooling: `pg_basebackup`, Patroni, repmgr, and managed failover.
- Synchronous replication with `synchronous_standby_names` for zero-data-loss
  configurations (financial/compliance use cases).
- Hot standbys accept read-only queries, enabling read scaling.
- Cascading replication and replication slots avoid WAL gaps on slow standbys.

---

## Cons

### Vertical scale primarily for writes
- A single primary handles writes; true horizontal write scaling requires
  sharding (e.g. **Citus**) or application-level partitioning.
- CPU/IO-bound write workloads eventually hit single-node ceilings.
- Read scaling is easy (add replicas), but every replica is a full copy, which
  is storage-expensive at very large sizes.
- Write throughput is bounded by single-node disk and CPU; very high ingest
  needs careful batching, `COPY`, and partitioning.

### MVCC bloat needs VACUUM discipline
- Dead tuples from updates/deletes accumulate and cause **bloat**, hurting
  performance and inflating storage.
- Autovacuum must be tuned; neglect can lead to index bloat and even
  transaction-ID wraparound emergencies.
- `VACUUM FULL` takes an exclusive lock and should be scheduled carefully.
- Long-running transactions hold back the `xmin` horizon, preventing dead
  tuples from being reclaimed and worsening bloat.

Example — monitoring bloat and autovacuum activity:

```sql
SELECT relname,
       n_dead_tup,
       last_vacuum,
       last_autovacuum
FROM pg_stat_user_tables
ORDER BY n_dead_tup DESC
LIMIT 10;
```
- Default autovacuum is conservative; busy tables often need per-table
  `autovacuum_vacuum_scale_factor` / threshold overrides.

### Connection-per-process can exhaust memory
- Each connection spawns an OS process and allocates memory; thousands of
  idle connections waste resources.
- Mitigation requires a **connection pooler** (PgBouncer) — an extra component
  to operate.
- Per-backend memory (`work_mem` × operations) plus the process footprint make
  high connection counts expensive compared to thread-per-connection designs.
- Without a pooler, ORM "connection storms" can exhaust `max_connections` and
  refuse new clients.

### Complex tuning
- Many knobs: `shared_buffers`, `effective_cache_size`, `work_mem`,
  `maintenance_work_mem`, `random_page_cost`, autovacuum thresholds,
  checkpoint settings.
- Optimal configuration is workload-dependent and often needs DBA expertise.
- Checkpoint tuning (`checkpoint_timeout`, `max_wal_size`) is subtle; bad
  settings cause I/O spikes.
- Planner cost constants must match actual hardware (SSD vs HDD) to get good
  plans.
- Memory settings interact: too-high `work_mem` × many connections = OOM.

Example — conservative starting tuning for a 8 GB container:

```ini
shared_buffers = 2GB          # ~25% of RAM
effective_cache_size = 6GB    # ~75% of RAM
work_mem = 16MB               # raised cautiously
maintenance_work_mem = 512MB
max_connections = 100         # use a pooler above this
```

### Not ideal for extreme write scale without sharding
- Out of the box, a single primary is a write bottleneck for very high ingest.
- Sharding with Citus adds operational complexity and some feature limitations
  (e.g. certain joins and foreign keys across shards are restricted).
- Distributed transactions and cross-shard queries add latency vs single node.

### Full-text search weaker than dedicated engines
- Built-in `tsvector`/`tsquery` is capable, but lacks the ranking sophistication
  and scale of Elasticsearch / OpenSearch for large search workloads.
- No built-in synonyms, percolation, or advanced analyzers out of the box;
  these require extensions or custom setup.

### Other limitations
- No native multi-master (without extensions like BDR).
- Cross-database queries are not native (use FDWs).
- Very large numbers of partitions can slow query planning.
- Adding/dropping some constraints or columns can take `ACCESS EXCLUSIVE`
  locks that block writes on busy tables (use `CONCURRENTLY` variants).
- Materialized views are not auto-refreshing; refreshes can be heavy.

---

## When the trade-offs matter

| Concern | PostgreSQL fit |
|---------|----------------|
| Strong consistency + rich SQL | ✅ Excellent |
| Geospatial / vector / hybrid data | ✅ Excellent (extensions) |
| Massive horizontal writes | ⚠️ Needs Citus/sharding |
| Heavy full-text at scale | ⚠️ Consider Elasticsearch |
| Graph traversals | ⚠️ Consider Neo4j |
| Ephemeral cache | ❌ Use Redis |

---

## Bottom line

PostgreSQL is the safest default choice for the vast majority of relational and
hybrid workloads. Its main costs are operational discipline (VACUUM, pooling,
tuning) and the need for sharding once write volume exceeds a single node.
