# Use Cases — InfluxDB

## Ideal

InfluxDB excels wherever the core entity is a stream of timestamped observations.

- **Infrastructure / system monitoring.** Collect CPU, memory, disk, network, and process
  metrics from servers, containers, and orchestrators. Tag by `host`, `region`, `service`
  to slice and dice in dashboards.

- **Application metrics.** Track request rates, latency histograms, error counts, queue
  depths, and custom business metrics emitted by services. Pair with Telegraf exporters
  or direct client libraries.

- **IoT sensor telemetry.** Ingest high-frequency readings from sensors, PLCs, and edge
  devices (temperature, vibration, pressure, location). Time-partitioned storage handles
  the volume and retention needs of sensor fleets.

- **Real-time analytics dashboards.** Power Grafana or custom dashboards that show live
  and historical trends, anomaly detection, and aggregations over sliding windows (e.g.,
  `mean(usage) over last 5 minutes`).

- **DevOps / observability.** Centralize metrics for SRE/ops use cases: alerting on
  thresholds, capacity planning, and post-incident analysis over historical windows.

- **Event and audit time-series.** Store discrete events (logins, transactions, clicks)
  as points when the query pattern is time-range + tag filtering rather than relational
  joins.

## When NOT to use

- **Relational / transactional applications.** If you need ACID transactions across
  multiple entities, referential integrity, and complex multi-table joins, use a
  relational database (PostgreSQL, MySQL). InfluxDB is not a system of record for
  transactional data.

- **Document storage.** If your data is heterogeneous, nested documents with evolving
  shapes, use a document database (MongoDB). InfluxDB's measurement/tag/field model is
  rigid by design.

- **Highly relational data.** Data with many inter-entity relationships and join-heavy
  queries does not fit the time-series model. v2 has no joins; v3 SQL joins are limited
  and not a substitute for an RDBMS.

- **Arbitrary ad-hoc joins.** Analytical workloads that require joining many dimensions
  on the fly belong in a data warehouse (e.g., ClickHouse, BigQuery, Snowflake) rather
  than InfluxDB.

- **Low-cardinality, simple key-value needs.** If you just need a small key-value or
  config store, InfluxDB is overkill and its cardinality constraints will work against
  you.

- **General-purpose OLTP with updates/deletes.** InfluxDB is optimized for append of
  immutable points. Frequent in-place updates or deletions of arbitrary rows are not its
  strength.

## Example scenario — Factory sensor monitoring

A manufacturing plant monitors hundreds of machines. Each machine has sensors reporting
temperature, vibration, and throughput every few seconds. Operators need live dashboards,
alerts when a sensor exceeds thresholds, and monthly retention with 30-day detail.

### Data model

- **measurement**: `machine_metrics`
- **tags** (indexed, low cardinality): `plant`, `line`, `machine_id`
- **fields** (values): `temperature`, `vibration`, `throughput`
- **timestamp**: measurement time

### Line protocol (sample writes)

```
machine_metrics,plant=p1,line=l1,machine_id=machine07 temperature=62.4,vibration=0.03,throughput=120 1700000000000000000
machine_metrics,plant=p1,line=l1,machine_id=machine07 temperature=63.1,vibration=0.04,throughput=118 1700000060000000000
machine_metrics,plant=p1,line=l2,machine_id=machine12 temperature=58.0,vibration=0.02,throughput=140 1700000060000000000
```

### SQL sketches (v3)

Recent readings for one machine:

```sql
SELECT * FROM machine_metrics
WHERE machine_id = 'machine07'
  AND time > now() - interval '1 hour';
```

Average temperature per machine over the last day:

```sql
SELECT machine_id, mean(temperature) AS avg_temp
FROM machine_metrics
WHERE time > now() - interval '1 day'
GROUP BY machine_id;
```

Alert candidates — machines running hot:

```sql
SELECT machine_id, max(temperature) AS peak_temp
FROM machine_metrics
WHERE time > now() - interval '15 minutes'
GROUP BY machine_id
HAVING max(temperature) > 70;
```

### Why InfluxDB fits here

- High ingest from many sensors is handled by the WAL + object-storage write path.
- Tagging by `plant`/`line`/`machine_id` keeps cardinality manageable and enables fast
  filtered queries.
- Retention policy automatically drops data older than the window.
- Grafana dashboards and alerts plug directly into the SQL/InfluxQL endpoints.
