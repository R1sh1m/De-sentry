# InfluxDB — Architecture & Schema

## Overview

InfluxDB is a purpose-built **time-series database (TSDB)** optimized for high-throughput
ingestion and efficient querying of time-stamped data such as metrics, sensor telemetry,
and events. Unlike general-purpose relational or document databases, InfluxDB treats time as
a first-class dimension and structures data around *measurements*, *tags*, *fields*, and a
*timestamp*.

The modern architecture (InfluxDB 3, "Core" / "Enterprise") is built on open-source columnar
technology:

- **Apache Arrow** for in-memory columnar representation.
- **Apache DataFusion** as the SQL query engine.
- **Apache Parquet** as the durable on-disk format.
- **Object storage** (S3-compatible or local files) as the system of record.

Earlier versions (InfluxDB 1.x and 2.x) used the proprietary **TSM (Time-Structured Merge)**
engine and the **Flux** query language. Both models are covered below with version notes.

This document describes the logical architecture, write/read paths, storage concepts, the
data model, retention, and scaling characteristics of InfluxDB.

---

## Versions

InfluxDB has evolved through three major generations. **Pin your image version** — do not
rely on the `latest` tag, because the semantics of `latest` change on **2026-09-15** (see
limitations below).

### v1.x (legacy)
- Storage engine: **TSM** (Time-Structured Merge tree).
- Query language: **InfluxQL** (SQL-like) plus the first generation of **Flux**.
- Model: `database` / `retention policy` / `measurement`.
- Still used in many legacy deployments but no longer the recommended starting point.

### v2.x
- Storage engine: still **TSM** for the open-source OSS build.
- Query language: **Flux** (a functional, pipe-forward language) is primary; InfluxQL
  available via compatibility.
- Model: `organization` / `bucket` (a bucket = database + retention policy).
- UI, task engine, and alerting integrated. Port **8086**.
- The `influx` CLI and `influx write` / `influx query` commands.

### v3 — Core and Enterprise (current)
- Storage engine: **InfluxDB 3 storage engine** writing **Parquet/Arrow** to **object
  storage**. TSM is no longer used.
- Query engine: **Apache DataFusion + Arrow**; supports **SQL** and **InfluxQL**.
- Model: `database` (v3 reintroduces the simpler database concept; no org/bucket split in
  Core).
- Port **8181** for v3 Core.
- **v3 Core** is open-source, single-node. **v3 Enterprise** adds clustering, HA, and
  additional enterprise features under a license.
- The `influxdb3` CLI (`influxdb3 write`, `influxdb3 query`, `influxdb3 create database`,
  `influxdb3 retention`).

> **IMPORTANT — `latest` tag change:** On **2026-09-15** the `influxdb:latest` Docker tag
> begins pointing to **InfluxDB 3 Core** instead of 2.x. If you pin `latest` today, your
> deployment will silently switch engines and query languages. Always use explicit tags such
> as `influxdb:3-core` or `influxdb:2`.

---

## Architecture

InfluxDB 3 follows a layered, streaming-oriented design:

```
Client tier      Telegraf / client libs / line protocol / SQL clients
        |
Ingest tier      Write API  ->  Ingester  ->  WAL  ->  Object storage (Parquet/Arrow)
        |
Query tier       Query engine (Apache DataFusion + Arrow)  : SQL & InfluxQL
        |
Storage tier     InfluxDB 3 storage engine on object storage
                 - time-partitioned shards
                 - Parquet/Arrow files
                 - index on time + tags
                 - retention enforcement
```

- **Client tier**: Producers send data using the **line protocol** over the Write API, or
  query via SQL/InfluxQL. Telegraf is the reference collector/agent, but any HTTP client or
  language library works.
- **Ingest tier**: The **ingester** accepts points, acknowledges after writing to the
  **Write-Ahead Log (WAL)** for durability, then persists committed data into **object
  storage** in time-based shards as Parquet/Arrow.
- **Query tier**: The **query engine** (DataFusion) plans queries, pushes down time and tag
  predicates, and streams results as Arrow batches.
- **Storage tier**: Durable **object storage** holds columnar Parquet files partitioned by
  time and series, with an index over time and tags to prune scans. Retention is enforced by
  dropping expired partitions.

For **v2**, the equivalent flow writes into the TSM engine (`wal` + `tsm` data files) on the
local filesystem, and queries run through Flux/InfluxQL.

---

## Write path

1. A client serializes a data point in **line protocol** and POSTs it to the Write API
   (v3: `/api/v3/write`; v2: `/api/v2/write`).
2. The **ingester** parses the line protocol, validates the schema, and assigns the point to
   the appropriate **time-based shard**.
3. The point is appended to the **WAL** (durable local log) so it survives a crash.
4. The ingester acknowledges the write to the client once the WAL is durable.
5. In the background, the ingester **compacts** buffered points and persists them as
   **Parquet/Arrow** files into **object storage**, organized by time partition and series.
6. The **index** (time + tags) is updated to reference the new data so future queries can
   locate it efficiently.

This design gives high ingest throughput: writes are fast (append to WAL) and durability is
decoupled from the slower object-storage persistence.

---

## Read path

1. A client issues a **SQL** or **InfluxQL** query (v3: `/api/v3/query_sql`; v2: Flux/InfluxQL
   endpoints).
2. The **query planner** (DataFusion) parses the statement and **pushes down** predicates on
   `time` and `tags` to minimize scanned data.
3. The planner consults the **index** to find the relevant shards/partitions and row groups.
4. The engine **reads the Parquet/Arrow** files from **object storage**, often only the
   needed columns (columnar pruning).
5. Results are materialized as **Arrow** batches and returned to the client (or rendered in
   the UI / Grafana).

Because data is columnar and time/tag indexed, range scans over recent windows are very fast.

---

## Storage concepts

- **Shard**: A time-partitioned slice of data (e.g., data for a specific time range). Shards
  let InfluxDB prune old data quickly for retention and parallelize reads.
- **Series**: A unique combination of *measurement + tag set + field*. Series cardinality is
  the number of distinct series; high cardinality (e.g., tagging by user ID) is the classic
  InfluxDB pitfall.
- **TSM vs Parquet/Arrow**:
  - v1/v2 use **TSM** (log-structured merge on the filesystem) — optimized for append-heavy
    time-series with compaction.
  - v3 uses **Parquet/Arrow** on object storage — open columnar format enabling DataFusion
    SQL, column pruning, and cheap storage.
- **Index**: Built on **time + tags** to locate series and time ranges without full scans.
  v3 leverages the columnar layout and object-store metadata; v2 maintains an in-memory/
  on-disk series index.
- **Retention**: Per-database (v3) or per-bucket (v2) policy that drops data older than a
  threshold by deleting whole shards/partitions.

---

## Data / schema model

InfluxDB is **schemaless** in the sense that you do not pre-declare measurements, tags, or
fields — they are created on first write. The logical model:

- **measurement**: Analogous to a table (e.g., `cpu`, `temperature`).
- **tags**: String key/value metadata that is **indexed** for fast filtering (e.g.,
  `host=server01`, `region=us-east`). Low cardinality.
- **fields**: The actual measured **values** (ints, floats, bools, strings). Not indexed.
- **timestamp**: The time of the measurement, in nanoseconds by default.

In v2 the container is a **bucket** (database + retention). In v3 the container is a
**database**.

### Line protocol syntax

```
<measurement>[,<tag_key>=<tag_value>[,...]] <field_key>=<field_value>[,...] [<timestamp>]
```

Whitespace separates measurement+tags from fields, and fields from the optional timestamp.

Example:

```
cpu,host=server01,region=us-east usage=0.64,load=1.2 1700000000000000000
temperature,device=sensorA,line=line1 value=21.5 1700000000000000000
```

### Concrete SQL query example

```sql
SELECT * FROM metrics WHERE time > now() - interval '1 hour';
```

This returns all fields of all series in the `metrics` measurement from the last hour,
demonstrating the v3 SQL interface on top of the time-series store.

---

## Retention & downsampling

- **Retention**: Configured per database (v3) or bucket (v2). When a shard's time range is
  entirely older than the retention window, it is deleted. This keeps storage bounded with
  negligible per-row overhead.
- **Downsampling**: InfluxDB does not automatically aggregate old data. In v2, **tasks** with
  Flux queries compute downsampled summaries into new buckets (e.g., roll 10s samples into
  5m averages). In v3, you run scheduled SQL/InfluxQL jobs (via your own scheduler or tasks)
  to write summarized data into a separate database/bucket. Downsampling is a user-defined
  pattern, not a built-in automatic process.

---

## Scaling & HA

- **v3 Core (OSS)**: **Single-node** only. It is designed to be simple to run and scales
  vertically (more CPU/IO/object storage). It does **not** include native clustering or HA.
- **v3 Enterprise**: Adds **clustering, high availability, and replication** features under an
  Enterprise license. This is the path for multi-node, fault-tolerant production deployments.
- **v2 OSS**: Also effectively single-node for the open-source build (v2 introduced clustering
  only in the commercial Cloud/Enterprise offerings).
- **Storage scaling**: Because v3 persists to object storage, capacity scales independently of
  compute; you can point multiple query/ingest processes at the same object store (with
  Enterprise coordination) or simply grow the bucket.

> Rule of thumb: start with **v3 Core** for new projects; move to **v3 Enterprise** when you
> need HA/clustering or multi-node scale.
