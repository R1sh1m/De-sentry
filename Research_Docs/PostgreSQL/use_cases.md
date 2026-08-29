# Use Cases

Where PostgreSQL shines, where it does not, and a concrete scenario.

## Ideal

### Transactional / OLTP systems
- Order processing, billing, inventory — anywhere correctness and ACID matter.
- High-concurrency reads/writes benefit from MVCC's non-blocking behavior.

### Financial, ERP & CRM systems
- Strong consistency, constraints, and auditability.
- Point-in-time recovery supports compliance and forensic restores.

### Geospatial applications (PostGIS)
- Storage and queries for points, lines, polygons, and rasters.
- Routing, mapping, proximity search — a de-facto standard for spatial data.

```sql
CREATE EXTENSION IF NOT EXISTS postgis;
SELECT name
FROM restaurants
WHERE ST_DWithin(geom, ST_MakePoint(-73.985, 40.748), 1000);
```

### JSON document + relational hybrid
- `jsonb` stores flexible documents while keeping relational integrity.
- Index JSON with **GIN** for fast key/value lookups inside documents.

```sql
CREATE TABLE events (
    id BIGSERIAL PRIMARY KEY,
    payload JSONB NOT NULL
);
CREATE INDEX idx_events_payload ON events USING GIN (payload);
SELECT * FROM events WHERE payload @> '{"type":"click"}';
```

### Analytics via columnar extensions
- `cstore_fdw` / `pg_analytics` / `Hydra` add columnar storage for OLAP
  without leaving PostgreSQL.
- Rich SQL (window functions, CTEs) suits reporting workloads.

### LLM embeddings with pgvector
- Store and query high-dimensional vectors for semantic search and RAG.

```sql
CREATE EXTENSION IF NOT EXISTS vector;
CREATE TABLE chunks (id BIGSERIAL PRIMARY KEY, emb vector(1536));
CREATE INDEX ON chunks USING hnsw (emb vector_cosine_ops);
SELECT id FROM chunks ORDER BY emb <=> :query_embedding LIMIT 5;
```

### General application backends
- Web/mobile backends, SaaS platforms, content management — the default
  relational choice with broad driver and ORM support.

---

## When NOT to use

### Extreme horizontal write scale without sharding
- Single-primary design bottlenecks writes; use Citus or another distributed
  store if you cannot shard at the application level.

### Ephemeral caches
- Redis / Memcached are purpose-built for volatile, low-latency key-value
  caching. PostgreSQL's durability overhead is unnecessary.

### Deep hierarchical / graph traversals
- Neo4j, Amazon Neptune, or ArangoDB handle recursive graph queries more
  naturally than adjacency modeling in SQL.

### Time-series at very high cardinality
- InfluxDB, TimescaleDB (a PostgreSQL extension, still worth considering),
  or dedicated TSDBs manage high-cardinality metrics more efficiently.

### Massive full-text search at scale
- Elasticsearch / OpenSearch offer better relevance ranking, analyzers, and
  horizontal search scaling than native `tsvector`.

---

## Example scenario: multi-tenant SaaS application

A B2B SaaS product where many organizations (tenants) share one PostgreSQL
cluster. We isolate tenant data with a `tenant_id` column and Row-Level
Security, and scale reads with replicas.

### Schema sketch

```sql
-- Tenants
CREATE TABLE tenants (
    id      BIGSERIAL PRIMARY KEY,
    name    TEXT NOT NULL,
    plan    TEXT NOT NULL DEFAULT 'free'
);

-- Projects owned by a tenant (shared schema, tenant_id column)
CREATE TABLE projects (
    id        BIGSERIAL PRIMARY KEY,
    tenant_id BIGINT NOT NULL REFERENCES tenants(id) ON DELETE CASCADE,
    name      TEXT NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_projects_tenant ON projects (tenant_id);

-- Tasks within projects
CREATE TABLE tasks (
    id         BIGSERIAL PRIMARY KEY,
    tenant_id  BIGINT NOT NULL REFERENCES tenants(id) ON DELETE CASCADE,
    project_id BIGINT NOT NULL REFERENCES projects(id) ON DELETE CASCADE,
    title      TEXT NOT NULL,
    done       BOOLEAN NOT NULL DEFAULT false,
    due        DATE
);
CREATE INDEX idx_tasks_tenant ON tasks (tenant_id);
-- Composite index to support tenant-scoped project lookups
CREATE INDEX idx_tasks_tenant_project ON tasks (tenant_id, project_id);

-- Row-Level Security to enforce tenant isolation automatically
ALTER TABLE projects ENABLE ROW LEVEL SECURITY;
ALTER TABLE tasks   ENABLE ROW LEVEL SECURITY;

CREATE POLICY tenant_isolation_projects ON projects
    USING (tenant_id = current_setting('app.tenant_id')::BIGINT);

CREATE POLICY tenant_isolation_tasks ON tasks
    USING (tenant_id = current_setting('app.tenant_id')::BIGINT);
```

### Query sketch

```sql
-- Application sets the tenant context before running queries
SET app.tenant_id = '42';

-- The RLS policy transparently scopes every query to tenant 42
SELECT p.name AS project, COUNT(t.id) AS open_tasks
FROM projects p
LEFT JOIN tasks t ON t.project_id = p.id AND t.done = false
WHERE p.tenant_id = 42
GROUP BY p.id, p.name
ORDER BY open_tasks DESC;

-- Analytics: completion rate over the last 30 days (can run on a read replica)
SELECT
    date_trunc('day', due) AS day,
    AVG(CASE WHEN done THEN 1.0 ELSE 0 END) AS completion_rate
FROM tasks
WHERE tenant_id = 42
  AND due >= current_date - INTERVAL '30 days'
GROUP BY 1
ORDER BY 1;
```

### Scaling notes for the scenario
- **Reads** (dashboard analytics) served from **streaming read replicas**.
- **Writes** consolidated on the primary; busy tenants can be moved to their
  own partition or schema.
- **Connection pooling** via PgBouncer handles bursty web traffic.
- **Partitioning** by `created_at` keeps large `tasks` tables manageable.
- **Logical replication** can feed a downstream data warehouse for heavy OLAP.

---

## Decision summary

| Workload | Use PostgreSQL? |
|----------|-----------------|
| SaaS backend, finance, ERP | ✅ Yes |
| Geo / vector / JSON hybrid | ✅ Yes (extensions) |
| Read-heavy apps | ✅ Yes (replicas) |
| Graph / cache / huge TSDB | ⚠️ Consider specialized DB |
| Un-sharded extreme writes | ⚠️ Add Citus |
