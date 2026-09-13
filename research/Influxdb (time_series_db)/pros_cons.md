# Pros and Cons — InfluxDB

## Pros

- **Purpose-built for time-series.** InfluxDB is designed from the ground up for
  timestamped data. Its data model (measurement / tags / fields / timestamp) maps
  naturally to metrics, events, and telemetry, avoiding the impedance mismatch of
  forcing time-series into a relational schema.

- **High ingest throughput.** The write path appends points to a Write-Ahead Log and
  acknowledges quickly, while durable persistence to object storage (v3) or TSM files
  (v2) happens in the background. This decoupling sustains very high write rates,
  suitable for millions of points per second at scale in clustered/Enterprise tiers.

- **Efficient columnar storage.** v3 stores data as Parquet/Arrow on object storage.
  Columnar layout enables column pruning, compression, and cheap scans over recent time
  windows. v2's TSM engine similarly optimizes append-heavy series workloads.

- **SQL queries in v3.** InfluxDB 3 exposes a first-class **SQL** interface via Apache
  DataFusion, so analysts can use familiar SQL instead of learning a new language.
  InfluxQL remains available for compatibility.

- **Great for monitoring / metrics.** Native concepts of measurements, tags, and fields,
  plus integration with Telegraf and Grafana, make it a natural fit for infrastructure,
  application, and business metrics monitoring and dashboards.

- **Built-in retention.** Retention policies drop expired shards/partitions automatically,
  keeping storage bounded with minimal operational overhead. No manual purge jobs needed.

- **Open and extensible.** v3 is built on open standards (Arrow, DataFusion, Parquet,
  object storage). Telegraf provides a huge plugin ecosystem for collecting data from
  systems, apps, and cloud services.

- **Time-partitioned sharding.** Data is organized into time-based shards, which makes
  both retention (delete whole shards) and parallel reads efficient.

## Cons

- **Not a general-purpose database.** InfluxDB is a specialized TSDB. It is not intended
  for arbitrary relational, document, or graph workloads, and trying to use it as one
  leads to poor data modeling and performance.

- **v3 OSS Core lacks native HA / clustering.** InfluxDB 3 Core is single-node only.
  High availability and multi-node clustering require the **Enterprise** license. Teams
  needing fault tolerance must plan for Enterprise or accept single-node risk.

- **Series cardinality pitfalls.** Performance is highly sensitive to cardinality, the
  number of distinct series (measurement + tag combination). Tagging by high-cardinality
  dimensions (e.g., user IDs, URLs, UUIDs) explodes series count and can degrade
  ingestion and queries severely. Capacity planning around tags is essential.

- **Flux learning curve on v2.** v2's primary query language, **Flux**, is a functional,
  pipe-forward language that differs significantly from SQL. Teams must invest in learning
  it, and migrating existing SQL-based tooling is non-trivial (mitigated in v3 by SQL).

- **Limited schema model.** Data is constrained to measurement / tags / fields /
  timestamp. There is no support for nested relational structures, foreign keys, or
  arbitrary document shapes within a point. This is by design but limits flexibility.

- **No joins in v2.** InfluxQL (v1/v2) does not support joins across measurements in the
  SQL sense; combining data from multiple series typically requires Flux or client-side
  merging. v3's SQL engine improves this, but time-series joins remain a deliberate,
  narrow capability compared with a full RDBMS.

- **Eventual/background persistence nuance.** Because v3 acknowledges writes after the WAL
  and persists to object storage asynchronously, there is a small window where a just
  written point may not yet be visible in queries until it is flushed (queryable WAL
  mitigates this in practice, but it is a modeling consideration).

- **Retention is destructive.** Retention drops data permanently by design. Downsampling
  to preserve aggregates must be configured explicitly by the user (tasks/jobs); it is
  not automatic.
