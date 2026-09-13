# Deployment — InfluxDB (Local Docker)

This guide covers **local-only** Docker deployments. For new projects, use **InfluxDB 3
Core** (`influxdb:3-core`, port **8181**). If you need the v2 model (buckets, Flux, UI on
port **8086**), use `influxdb:2`.

> **Pin your versions.** On **2026-09-15** the `influxdb:latest` tag starts pointing to
> **InfluxDB 3 Core**. Do not use `latest` in production or reproducible setups.

---

## Prerequisites

- Docker Engine 20.10+ (or Docker Desktop).
- A working directory for persistent data.
- (Optional) `docker compose` v2 plugin.

Verify Docker is available:

```bash
docker --version
docker compose version
```

Create a working directory and enter it:

```bash
mkdir -p influxdb-lab && cd influxdb-lab
```

---

## Single node v3 Core via `docker run`

Run InfluxDB 3 Core, mapping port **8181** and persisting data to a local volume:

```bash
docker run -d \
  --name influxdb3-core \
  -p 8181:8181 \
  -v "$PWD/data:/var/lib/influxdb" \
  influxdb:3-core
```

`influxdb3 serve` is the container's default command, so the server starts
automatically. It uses a local **file** object store by default.

Create a database (v3 uses `database`, not v2 `bucket`):

```bash
docker exec influxdb3-core influxdb3 create database mydb
```

Confirm it exists:

```bash
docker exec influxdb3-core influxdb3 show databases
```

---

## docker-compose with v3-core

Create a `compose.yaml`:

```yaml
services:
  influxdb3-core:
    image: influxdb:3-core
    container_name: influxdb3-core
    ports:
      - "8181:8181"
    command: influxdb3 serve --node-id=node0 --object-store=file
    volumes:
      - ./data:/var/lib/influxdb
    restart: unless-stopped
```

Bring it up:

```bash
docker compose up -d
```

Create the database once the container is healthy:

```bash
docker compose exec influxdb3-core influxdb3 create database mydb
```

### Note — v2 compose alternative

If you require the v2 model (bucket/org/Flux, port **8086**), use this `compose.yaml`
instead. The init environment auto-creates an org, bucket, user, and password on first
start:

```yaml
services:
  influxdb2:
    image: influxdb:2
    container_name: influxdb2
    ports:
      - "8086:8086"
    environment:
      DOCKER_INFLUXDB_INIT_MODE: setup
      DOCKER_INFLUXDB_INIT_USERNAME: admin
      DOCKER_INFLUXDB_INIT_PASSWORD: changeme123
      DOCKER_INFLUXDB_INIT_ORG: myorg
      DOCKER_INFLUXDB_INIT_BUCKET: mybucket
      DOCKER_INFLUXDB_INIT_ADMIN_TOKEN: myadmintoken
    volumes:
      - ./data2:/var/lib/influxdb2
      - ./config2:/etc/influxdb2
    restart: unless-stoped
```

> Remember: the v2 image keeps the `latest` semantics only until 2026-09-15, after which
> `latest` resolves to 3 Core. Always pin `influxdb:2` explicitly.

---

## Verify

### Write a point (v3)

Using the `influxdb3 write` CLI:

```bash
docker exec influxdb3-core influxdb3 write mydb \
  --precision ns \
  "cpu,host=server01,region=us-east usage=0.64,load=1.2 $(date +%s)000000000"
```

Or via the HTTP Write API with `curl` (no auth in Core by default):

```bash
curl -X POST "http://localhost:8181/api/v3/write?db=mydb&precision=ns" \
  --data-binary 'cpu,host=server01,region=us-east usage=0.64,load=1.2 '"$(date +%s)000000000"
```

### Query with SQL (v3)

```bash
docker exec influxdb3-core influxdb3 query mydb \
  "SELECT * FROM cpu WHERE time > now() - interval '1 hour'"
```

Or via the SQL HTTP API:

```bash
curl -G "http://localhost:8181/api/v3/query_sql" \
  --data-urlencode "db=mydb" \
  --data-urlencode "q=SELECT * FROM cpu WHERE time > now() - interval '1 hour'"
```

### v2 verification (if using influxdb:2)

Write:

```bash
docker exec influxdb2 influx write \
  --org myorg --bucket mybucket \
  --token myadmintoken \
  "cpu,host=server01 usage=0.64,load=1.2 $(date +%s)000000000"
```

Query (InfluxQL):

```bash
docker exec influxdb2 influx query \
  --org myorg --token myadmintoken \
  'from(bucket:"mybucket") |> range(start:-1h)'
```

---

## Config notes

- **Object store: file vs S3.** v3 Core defaults to a local **file** object store
  (`--object-store=file`). For shared/durable storage, point it at S3:

  ```bash
  influxdb3 serve --node-id=node0 --object-store=s3 \
    --bucket=<BUCKET> --aws-access-key-id=<KEY> --aws-secret-access-key=<SECRET>
  ```

  (Credentials can also come from the environment / IAM role.) File mode is fine for
  local-only and single-node use.

- **Retention.** Set a retention period per database in v3 with the `influxdb3 retention`
  command:

  ```bash
  docker exec influxdb3-core influxdb3 retention create mydb 30d
  ```

  This drops data older than 30 days by deleting expired shards. In v2, set retention at
  bucket creation (`influx bucket create --retention 30d`).

- **Node id.** `--node-id=node0` identifies the node; required for multi-process
  coordination in Enterprise, harmless for single-node Core.

---

## Limitations

- **Pin versions.** The `latest` tag changes to **InfluxDB 3 Core** on **2026-09-15**.
  Always use explicit tags (`influxdb:3-core`, `influxdb:2`) in `docker run` and
  `compose.yaml` to avoid silent engine/language switches.

- **OSS Core is single-node only.** InfluxDB 3 Core (OSS) has **no native HA or
  clustering**. For high availability and multi-node scale you must use **InfluxDB 3
  Enterprise** (licensed). Plan capacity accordingly for local/single-node deployments.

- **No auth by default in Core.** `influxdb3 serve` in Core does not enforce
  authentication out of the box. Enable auth/token controls as needed before exposing the
  port beyond localhost. Bind to `127.0.0.1:8181` if only local access is required.

- **Local file store is single-host.** With `--object-store=file`, data lives on the
  container's volume; back it up or use S3 if you need durability beyond the host disk.

- **v2 Flux vs v3 SQL.** Mixing v2 (`influx` CLI, Flux, buckets) and v3 (`influxdb3` CLI,
  SQL, databases) commands will not interoperate; keep tooling aligned with the chosen
  major version.
