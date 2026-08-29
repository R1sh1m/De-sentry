# PostgreSQL — Local Deployment (Docker, single host)

> **Scope:** Local-only, Docker-based deployment for development and testing.
> All examples use **PostgreSQL 17** (`postgres:17`), port **5432**.
> Not a production-hardening guide — see the "Limitations" section.

## Prerequisites

- **Docker Engine** installed and running:
  - Windows / macOS: Docker Desktop
  - Linux: `docker` + `docker compose` (v2 plugin)
- Verify:

```bash
docker --version
docker compose version
```

- Free port **5432** on the host (change the host mapping if occupied).
- At least ~1 GB free disk for the image plus space for your data volume.

---

## Single node

Run a standalone PostgreSQL 17 container:

```bash
docker run --name pg \
  -e POSTGRES_PASSWORD=secret \
  -e POSTGRES_DB=appdb \
  -p 5432:5432 \
  -d postgres:17
```

- `POSTGRES_PASSWORD` — password for the default `postgres` superuser.
- `POSTGRES_DB` — database created on first boot (`appdb`).
- `-p 5432:5432` — maps host port 5432 to container port 5432.
- `-d` — detached (background) mode.

Connect with the bundled `psql` client:

```bash
docker exec -it pg psql -U postgres -d appdb
```

Run a quick check inside `psql`:

```sql
SELECT version();
```

---

## docker-compose

A reusable single-node service with a healthcheck and a named volume.

`compose.yml`:

```yaml
services:
  postgres:
    image: postgres:17
    container_name: pg
    environment:
      POSTGRES_USER: postgres
      POSTGRES_PASSWORD: secret
      POSTGRES_DB: appdb
    ports:
      - "5432:5432"
    volumes:
      - pgdata:/var/lib/postgresql/data
    healthcheck:
      test: ["CMD-SHELL", "pg_isready -U postgres -d appdb"]
      interval: 10s
      timeout: 5s
      retries: 5
    restart: unless-stopped

volumes:
  pgdata:
```

Start it:

```bash
docker compose up -d
```

Check health:

```bash
docker compose ps
docker exec -it pg pg_isready -U postgres -d appdb
```

---

## Persisting data

Data lives in `/var/lib/postgresql/data` inside the container. Without a
volume it is lost when the container is removed. The compose file above uses a
**named volume** `pgdata`, which persists across container recreation.

- Inspect the volume:

```bash
docker volume ls
docker volume inspect pgdata
```

- Back up the data directory (offline or via `pg_dump`):

```bash
docker exec -it pg pg_dump -U postgres appdb > appdb.sql
```

---

## Verify

After starting, create a table, insert, and select:

```bash
docker exec -it pg psql -U postgres -d appdb
```

```sql
SELECT version();

CREATE TABLE demo (
    id   SERIAL PRIMARY KEY,
    name TEXT NOT NULL
);

INSERT INTO demo (name) VALUES ('alice'), ('bob');

SELECT * FROM demo ORDER BY id;
```

Expected output shows the two inserted rows.

---

## Config notes

### Where config lives
- Inside the image, the main config is at
  `/var/lib/postgresql/data/postgresql.conf` (generated on init).
- Debian/Ubuntu packages place it under `/etc/postgresql/<ver>/main/`, but the
  official image manages it via the data directory.

### Setting parameters via environment
The official image supports some env-driven settings:

- `POSTGRES_INITDB_ARGS` — passed to `initdb` (e.g. locale/encoding):

```bash
docker run --name pg -e POSTGRES_PASSWORD=secret \
  -e POSTGRES_INITDB_ARGS="--encoding=UTF8 --locale=C" \
  -d postgres:17
```

- `POSTGRES_DB`, `POSTGRES_USER`, `POSTGRES_PASSWORD` — bootstrap identity.

### Initialization scripts
Any `*.sql`, `*.sql.gz`, or `*.sh` file mounted into
`/docker-entrypoint-initdb.d` runs **once** on first init:

```yaml
services:
  postgres:
    image: postgres:17
    environment:
      POSTGRES_PASSWORD: secret
      POSTGRES_DB: appdb
    volumes:
      - ./init:/docker-entrypoint-initdb.d:ro
      - pgdata:/var/lib/postgresql/data
volumes:
  pgdata:
```

`init/01-schema.sql`:

```sql
CREATE TABLE IF NOT EXISTS tenants (
    id   BIGSERIAL PRIMARY KEY,
    name TEXT NOT NULL
);
```

### Tuning memory
Pass settings via `-c` flags or a mounted custom `postgresql.conf`.

Via run flags:

```bash
docker run --name pg -e POSTGRES_PASSWORD=secret -d postgres:17 \
  -c shared_buffers=256MB \
  -c max_connections=200 \
  -c work_mem=16MB
```

Or mount your own conf and set `postgresql.conf`:

```yaml
    command: postgres -c config_file=/etc/postgresql/postgresql.conf
    volumes:
      - ./postgresql.conf:/etc/postgresql/postgresql.conf:ro
      - pgdata:/var/lib/postgresql/data
```

Common knobs:
- `shared_buffers` — start at 25% of container RAM.
- `effective_cache_size` — ~50–75% of host RAM (hint for planner).
- `work_mem` — raise cautiously; multiplied per operation.
- `max_connections` — keep modest; use a pooler for high counts.

---

## Replication example (primary + one streaming replica)

Illustrative, runnable compose with a primary and one standby using streaming
replication. The replica clones the primary with `pg_basebackup` and stays
attached via `primary_conninfo`.

`docker-compose.replication.yml`:

```yaml
services:
  primary:
    image: postgres:17
    container_name: pg-primary
    environment:
      POSTGRES_USER: postgres
      POSTGRES_PASSWORD: secret
      POSTGRES_DB: appdb
    volumes:
      - primarydata:/var/lib/postgresql/data
    ports:
      - "5432:5432"
    healthcheck:
      test: ["CMD-SHELL", "pg_isready -U postgres -d appdb"]
      interval: 10s
      retries: 5

  replica:
    image: postgres:17
    container_name: pg-replica
    environment:
      POSTGRES_USER: postgres
      POSTGRES_PASSWORD: secret
      POSTGRES_DB: appdb
      # Tells the entrypoint to clone the primary on first boot
      PRIMARY_HOST: primary
      PRIMARY_PORT: "5432"
      PRIMARY_USER: postgres
      PRIMARY_PASSWORD: secret
    depends_on:
      primary:
        condition: service_healthy
    volumes:
      - replicatedata:/var/lib/postgresql/data
    ports:
      - "5433:5432"
    command: >
      bash -c "
      if [ ! -s /var/lib/postgresql/data/PG_VERSION ]; then
        pg_basebackup -h primary -p 5432 -U postgres -D /var/lib/postgresql/data
          -Fp -Xs -P -R -S replica_slot;
        touch /var/lib/postgresql/data/standby.signal;
      fi;
      postgres
      "
    restart: unless-stopped

volumes:
  primarydata:
  replicatedata:
```

Notes:
- The replica needs a **`standby.signal`** file in its data directory to start
  in standby mode, and **`primary_conninfo`** (written by `pg_basebackup -R`)
  pointing at the primary.
- `pg_basebackup -R` auto-generates `postgresql.auto.conf` with
  `primary_conninfo` and creates the replication slot reference.
- The primary must allow the replica's connection in `pg_hba.conf` (host
  replication entry) and set `max_wal_senders > 0`, `wal_level = replica`.
- For a fully automated setup, consider **Patroni** + etcd in production; this
  compose is illustrative for local learning.

Verify replication:

```bash
# On primary
docker exec -it pg-primary psql -U postgres -c "SELECT * FROM pg_stat_replication;"

# Insert on primary, read on replica
docker exec -it pg-primary psql -U postgres -d appdb -c "CREATE TABLE t(id int); INSERT INTO t VALUES (1);"
docker exec -it pg-replica  psql -U postgres -d appdb -c "SELECT * FROM t;"
```

---

## Limitations

- **Authentication:** default is `scram-sha-256` (md5 in older images). Use
  strong passwords; do not hard-code secrets in compose for shared environments
  (use Docker secrets or `.env` files excluded from git).
- **Network exposure:** only publish port 5432 on **trusted** networks.
  Binding to `0.0.0.0` on a public host without a firewall is unsafe.
- **Resource limits:** set Docker memory/CPU limits for the container to avoid
  the DB consuming all host RAM and triggering OOM kills.
- **fsync / durability:** never run PostgreSQL on a filesystem/host with
  `fsync` disabled or on volatile storage you care about.
- **Not production HA:** this guide covers local single-host and a basic
  replica. Real HA needs automated failover (Patroni/repmgr), monitoring, and
  backup/restore testing.
- **Data-at-rest:** consider volume encryption for sensitive data.
