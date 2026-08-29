# Apache Cassandra — Local Deployment (Docker)

> Scope: **local-only** deployment for development and learning, using Docker
> and `docker-compose`. NOT for production. Companion docs:
> [architecture.md](architecture.md), [use_cases.md](use_cases.md).

- **Cassandra version:** `5.0.9` (use the `cassandra:latest` image tag).
- **Java:** bundled inside the official image — you do **not** install Java
  separately.
- **Ports:**
  - `9042` — CQL native protocol (clients, `cqlsh`).
  - `7000` — internode gossip (plain).
  - `7001` — internode gossip (TLS).
  - `7199` — JMX (monitoring/`nodetool`).

## Prerequisites

- **Docker** installed and running (Docker Desktop or Docker Engine).
- **4 GB+ RAM** recommended for a single node; a 3-node cluster benefits from
  more. The JVM heap defaults to 1/4 of container memory.
- Reserve memory explicitly to avoid the OOM killer killing the JVM:

```powershell
# PowerShell (Windows / Docker Desktop)
docker run --name cass1 -d -p 9042:9042 -p 7199:7199 --memory=4g cassandra:latest
```

```bash
# Linux / macOS
docker run --name cass1 -d -p 9042:9042 -p 7199:7199 --memory=4g cassandra:latest
```

> Tip: `cassandra:latest` currently resolves to `5.0.9`. Pin it in real
> environments with `cassandra:5.0.9` for reproducibility.

## Single node

```bash
# Start one node, publishing CQL and JMX.
docker run --name cass1 -d -p 9042:9042 -p 7199:7199 --memory=4g cassandra:latest

# Wait for it to become ready (look for "Starting listening for CQL clients").
docker logs -f cass1

# Open a CQL shell inside the container.
docker exec -it cass1 cqlsh
```

Inside `cqlsh` you should see a prompt like `cqlsh>`. Try:

```sql
SELECT release_version FROM system.local;
-- should return 5.0.9
```

Stop / start / remove:

```bash
docker stop cass1
docker start cass1
docker rm -f cass1   # destroys the container AND its data (no volume mounted)
```

> Without a volume, removing the container deletes all data. For persistence
> across container recreation, mount `/var/lib/cassandra` (see the cluster
> example below).

## 3-node cluster via docker-compose

The official image reads cluster topology from environment variables:
- `CASSANDRA_SEEDS` — comma-separated seed node addresses.
- `CASSANDRA_LISTEN_ADDRESS` — address the node binds for internode comms.
- `CASSANDRA_BROADCAST_ADDRESS` — address other nodes/clients use to reach it.
- `CASSANDRA_DC` / `CASSANDRA_RACK` — datacenter/rack labels (snitch).
- `CASSANDRA_ENDPOINT_SNITCH` — set to `GossipingPropertyFileSnitch` (the
  standard production-grade snitch that uses gossip + local properties).

### compose.yaml

```yaml
# compose.yaml — 3-node local Cassandra cluster (dev only)
services:
  cass1:
    image: cassandra:latest
    container_name: cass1
    mem_limit: 4g
    environment:
      - CASSANDRA_SEEDS=cass1,cass2
      - CASSANDRA_LISTEN_ADDRESS=cass1
      - CASSANDRA_BROADCAST_ADDRESS=cass1
      - CASSANDRA_DC=dc1
      - CASSANDRA_RACK=rack1
      - CASSANDRA_ENDPOINT_SNITCH=GossipingPropertyFileSnitch
    ports:
      - "9042:9042"
      - "7199:7199"
    volumes:
      - cass1_data:/var/lib/cassandra
      - cass1_conf:/etc/cassandra

  cass2:
    image: cassandra:latest
    container_name: cass2
    mem_limit: 4g
    environment:
      - CASSANDRA_SEEDS=cass1,cass2
      - CASSANDRA_LISTEN_ADDRESS=cass2
      - CASSANDRA_BROADCAST_ADDRESS=cass2
      - CASSANDRA_DC=dc1
      - CASSANDRA_RACK=rack2
      - CASSANDRA_ENDPOINT_SNITCH=GossipingPropertyFileSnitch
    volumes:
      - cass2_data:/var/lib/cassandra
      - cass2_conf:/etc/cassandra
    depends_on:
      - cass1

  cass3:
    image: cassandra:latest
    container_name: cass3
    mem_limit: 4g
    environment:
      - CASSANDRA_SEEDS=cass1,cass2
      - CASSANDRA_LISTEN_ADDRESS=cass3
      - CASSANDRA_BROADCAST_ADDRESS=cass3
      - CASSANDRA_DC=dc1
      - CASSANDRA_RACK=rack3
      - CASSANDRA_ENDPOINT_SNITCH=GossipingPropertyFileSnitch
    volumes:
      - cass3_data:/var/lib/cassandra
      - cass3_conf:/etc/cassandra
    depends_on:
      - cass1

volumes:
  cass1_data:
  cass1_conf:
  cass2_data:
  cass2_conf:
  cass3_data:
  cass3_conf:
```

Bring it up:

```bash
docker compose up -d
# or, older Docker Compose v1:
# docker-compose up -d
```

Watch the nodes join (gossip takes ~10–30s to stabilize):

```bash
docker logs -f cass3
```

## Verify the cluster

```bash
# From any node, check ring membership and state (UN = Up/Normal).
docker exec -it cass1 nodetool status
```

Expected output shows three nodes with `UN` status, e.g.:

```
Datacenter: dc1
===============
Status=Up/Down
|/ State=Normal/Leaving/Joining/Moving
--  Address     Load       Tokens  Owns  Host ID                               Rack
UN  cass1       ...        256     ...   xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx  rack1
UN  cass2       ...        256     ...   xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx  rack2
UN  cass3       ...        256     ...   xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx  rack3
```

You can also verify from `cqlsh`:

```bash
docker exec -it cass1 cqlsh -e "SELECT peer, data_center, rack FROM system.peers;"
```

## Smoke test: keyspace, table, insert, select

```bash
docker exec -it cass1 cqlsh
```

```sql
CREATE KEYSPACE IF NOT EXISTS demo
  WITH replication = {
    'class': 'SimpleStrategy',
    'replication_factor': 3
  };

USE demo;

CREATE TABLE IF NOT EXISTS sensor_readings (
  device_id   uuid,
  day         date,
  recorded_at timestamp,
  metric      text,
  value       double,
  PRIMARY KEY ((device_id, day), recorded_at, metric)
) WITH CLUSTERING ORDER BY (recorded_at DESC);

INSERT INTO sensor_readings (device_id, day, recorded_at, metric, value)
VALUES (a1b2c3d4-0000-0000-0000-000000000001, '2026-08-29', toTimestamp(now()), 'temp_c', 21.4);

SELECT * FROM sensor_readings
WHERE device_id = a1b2c3d4-0000-0000-0000-000000000001 AND day = '2026-08-29';
```

Because `replication_factor = 3` and you have 3 nodes, the data is replicated
across all of them. Confirm replication by querying from a different node:

```bash
docker exec -it cass3 cqlsh -e "SELECT * FROM demo.sensor_readings;"
```

## Config notes

Configuration lives in `/etc/cassandra` (mounted as a volume above). Key files:

- **`cassandra.yaml`** — main config: `cluster_name`, `seed_provider`,
  `listen_address`, `rpc_address`, `endpoint_snitch`, `num_tokens`,
  `authenticator`/`authorizer`.
- **`jvm-server.options`** (in `conf/` for 5.x) — JVM heap sizing via
  `-Xms`/`-Xmx`. A common rule: set both to **no more than 1/4 to 1/2 of
  container memory**, and never exceed ~31 GB (compressed-oops boundary).
- **`cassandra-rackdc.properties`** — used by `GossipingPropertyFileSnitch` to
  declare `dc=dc1` / `rack=rack1`.

Important settings for the local cluster:
- `num_tokens: 256` (default) — the vnode count that enables auto-rebalance.
  Lower it (e.g. 16) only on very small dev boxes to speed bootstrap.
- `endpoint_snitch: GossipingPropertyFileSnitch` — already set via env above.
- `cluster_name` must match across all nodes or they will refuse to join.

Edit inside the container, then restart that node:

```bash
docker exec -it cass1 bash -c "sed -i 's/num_tokens:.*/num_tokens: 16/' /etc/cassandra/cassandra.yaml"
docker restart cass1
```

## Limitations (read before you rely on this)

- **Single-host cluster is for development only.** All nodes run on one host;
  there is no real fault isolation — if the host or Docker dies, the whole
  "cluster" dies. It teaches the concepts but proves nothing about resilience.
- **Set seeds carefully.** Seeds are the bootstrap contact points. With
  `CASSANDRA_SEEDS=cass1,cass2` every node lists the same seeds; that is fine
  for 3 nodes. In larger/real clusters, use a small, stable set of seed nodes
  per DC and avoid listing every node as a seed.
- **JVM heap sizing matters.** The default auto-heap can be too large for a
  4 GB container (risk of host OOM) or too small under load (GC pressure).
  Tune `jvm-server.options` `-Xms`/`-Xmx` to roughly 1/4–1/2 of reserved
  memory and keep it under the compressed-oops limit.
- **No auth by default.** The image ships with `AllowAllAuthenticator`. Enable
  `PasswordAuthenticator`/`CassandraAuthorizer` before any non-local use.
- **No persistence without volumes.** Data lives in `/var/lib/cassandra`; the
  compose file above mounts named volumes so it survives container recreation,
  but it is still on the single host's disk.
- **Networking is container-local.** The `CASSANDRA_BROADCAST_ADDRESS` values
  (`cass1`, etc.) resolve only inside the Docker network. To connect from the
  host app, use `localhost:9042` (published port), not the container name.
- **Repair is still your job.** Even locally, run `docker exec -it cass1
  nodetool repair` periodically to understand the operational model; it is
  mandatory in production to avoid data divergence and tombstone resurrection.
