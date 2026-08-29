# Use Cases

Companion to [architecture.md](architecture.md) and [pros_cons.md](pros_cons.md).
This file covers where Apache Cassandra excels, where it does not, and a
concrete scenario.

## Ideal use cases

- **Time-series / IoT telemetry.** High-volume, append-only sensor, metrics, and
  event streams with time-bounded partitions. TWCS compaction and time-based
  partition keys make retention and reads efficient. Examples: smart-meter
  readings, industrial IoT, vehicle telemetry.

- **Write-heavy event logging and audit trails.** Application logs, clickstreams,
  activity feeds, and audit records that are written far more than read and
  rarely updated. Cassandra's LSM engine thrives here.

- **Session and identity / profile stores.** User profiles, session state,
  device tokens, and preference stores that must be always available and fast at
  global scale (e.g. `WHERE user_id = ?`).

- **Multi-region active-active applications.** Systems that must accept writes
  in every datacenter with low local latency and survive region outages —
  `NetworkTopologyStrategy` + `LOCAL_QUORUM` deliver this without manual
  failover.

- **Messaging and notification pipelines.** Inbox/outbox models, message
  queues' backing store, notification feeds, and chat history, where data is
  partitioned by user or conversation and appended over time.

- **Recommendation / personalization blobs.** Per-user feature vectors,
  precomputed recommendations, and "items for you" lists served by primary key
  lookups with low tail latency.

- **High-throughput ingestion front door.** Acting as the durable, scalable
  landing zone that absorbs spikes (e.g. from Kafka) before downstream
  processing.

## When NOT to use / anti-patterns

- **Reporting and analytics with joins.** If you need `JOIN`, `GROUP BY` across
  entities, ad-hoc SQL exploration, or BI dashboards, use a relational DB or a
  warehouse (Spark can still read Cassandra for ETL).

- **Strongly consistent transactions across entities.** Money movement,
  inventory with strict correctness across items, or any workflow needing
  multi-row/multi-table ACID should use a relational/NewSQL system. Cassandra's
  LWTs are too slow and limited for this.

- **Small datasets.** A single Postgres/MySQL instance is simpler, cheaper, and
  gives you joins, constraints, and transactions. Don't stand up a 3-node
  replicated cluster for a few GB.

- **Complex relational queries / flexible filtering.** Cassandra requires you to
  know your query patterns up front and model tables for them. Arbitrary
  `WHERE` predicates, secondary-index-heavy access, and OLAP-style scans are
  anti-patterns.

- **Frequent read-modify-write of the same row.** Unless using counters or LWTs
  (both with caveats), concurrent updates to the same cell can lose data due to
  last-write-wins. Hot counters and "update the same row constantly" patterns
  are risky.

- **Schema-less, freely evolving document queries.** If you mostly do nested
  document lookups with arbitrary nested filters, a document DB (MongoDB) or
  search engine may fit better. Cassandra's strength is wide-column,
  query-specific modeling.

## Example scenario: a multi-region IoT platform

**Problem.** A fleet of 5 million devices across `us-east`, `eu-west`, and
`ap-south` reports temperature and status every few seconds. Requirements:
always-writable even if a region partitions, low-latency local ingestion,
time-windowed retention (90 days), and per-device recent-read queries plus
global fleet rollups via Spark.

**Why Cassandra fits.** Write-heavy time-series with a clear partition key,
active-active multi-DC, and flat latency at scale.

### Data model sketch

```sql
-- One keyspace, replicated 3 in each DC.
CREATE KEYSPACE IF NOT EXISTS iot
  WITH replication = {
    'class': 'NetworkTopologyStrategy',
    'us-east': 3,
    'eu-west': 3,
    'ap-south': 3
  };

-- Writes land in the device's local DC (LOCAL_QUORUM), replicate globally.
CREATE TABLE iot.readings (
  device_id   uuid,
  bucket      date,          -- one partition per device per day (bounded size)
  ts          timestamp,
  metric      text,
  value       double,
  PRIMARY KEY ((device_id, bucket), ts, metric)
) WITH CLUSTERING ORDER BY (ts DESC)
  AND compaction = {
    'class': 'TimeWindowCompactionStrategy',
    'compaction_window_unit': 'DAYS',
    'compaction_window_size': 1
  }
  AND default_time_to_live = 7776000;   -- 90 days auto-expiry

-- Query: latest 100 readings for a device today (served by PK lookup).
SELECT * FROM iot.readings
WHERE device_id = ? AND bucket = '2026-08-29'
LIMIT 100;

-- Device registry: profile/metadata, one row per device.
CREATE TABLE iot.devices (
  device_id  uuid PRIMARY KEY,
  owner      text,
  model      text,
  region     text,
  last_seen  timestamp
);

-- Per-owner view (denormalized) for "show me my devices' latest status".
CREATE TABLE iot.readings_by_owner (
  owner      text,
  device_id  uuid,
  bucket     date,
  ts         timestamp,
  metric     text,
  value      double,
  PRIMARY KEY ((owner, bucket), ts, device_id)
) WITH CLUSTERING ORDER BY (ts DESC);
```

**Notes on the model.**
- The composite partition key `(device_id, bucket)` keeps each partition small
  and bounded — the single most important rule for time-series in Cassandra.
- `TWCS` + `default_time_to_live` means whole day-windows are dropped cheaply
  when they expire (no expensive tombstone scans).
- The `readings_by_owner` table is **intentional denormalization** to serve a
  different query without a join — exactly the Cassandra way.
- Spark (or `dsbulk`) can scan `iot.readings` for fleet-level rollups without
  affecting the OLTP read path.

**Consistency choice.** Ingest with `LOCAL_QUORUM` (fast, durable within the
region); critical cross-region guarantees use `QUORUM`/`EACH_QUORUM`
sparingly. Run `nodetool repair` on a schedule to keep replicas consistent.
