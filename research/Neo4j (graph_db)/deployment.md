# Deployment — Neo4j (Local, Docker-based)

> DBMS: **Neo4j** — Native graph database (Cypher)
> Scope: **LOCAL-ONLY** deployment using Docker / Docker Compose.
> Image: Neo4j 5 LTS (`neo4j:5`, currently ~5.26).

This guide covers running Neo4j on a single machine for development, testing, and
small production workloads using Docker. All commands are runnable as written.

Ports used by Neo4j:

| Port  | Protocol | Purpose                                  |
|-------|----------|------------------------------------------|
| 7474  | HTTP     | Neo4j Browser UI and REST/transactional API |
| 7687  | Bolt     | Binary protocol used by drivers and cypher-shell |

---

## Prerequisites

1. **Docker** installed and running.
   - Verify:
     ```bash
     docker --version
     docker info
     ```

2. **Linux only — raise `vm.max_map_count`** (IMPORTANT).
   Neo4j (and Elasticsearch-style engines) require a higher virtual memory map
   count than the Linux default. If this is too low, Neo4j will fail to start
   with a "max virtual memory areas" error.

   Set it now (survives until reboot):
   ```bash
   sudo sysctl -w vm.max_map_count=262144
   ```

   Make it persistent across reboots by adding to `/etc/sysctl.conf`:
   ```bash
   echo "vm.max_map_count=262144" | sudo tee -a /etc/sysctl.conf
   sudo sysctl -p
   ```

   Verify:
   ```bash
   cat /proc/sys/vm/max_map_count
   # expect: 262144
   ```

   > Windows / macOS (Docker Desktop) users do **not** need this step — the
   > setting is handled inside the Docker VM.

3. **Enough memory.** Give Docker at least 2–4 GB of RAM. Neo4j needs heap +
   page cache; the default container settings are modest but you should size
   explicitly for real data (see Config notes).

---

## Single node (docker run)

The simplest way to start Neo4j is a single container. Data persists via a bind
mount to `./data` and `./logs` on the host.

```bash
docker run -d \
  --name neo4j \
  -p 7474:7474 \
  -p 7687:7687 \
  -e NEO4J_AUTH=neo4j/yourpassword \
  -v $PWD/data:/data \
  -v $PWD/logs:/logs \
  neo4j:5
```

Notes:

- `NEO4J_AUTH=neo4j/yourpassword` sets the initial admin password. **You must set
  this on first run**; Neo4j will refuse to start without authentication
  configured (or use `NEO4J_AUTH=none` only for throwaway local dev).
- `$PWD/data` holds the database store; keep it to avoid data loss on container
  removal.
- To use a fixed password from a file or secret, mount it and set
  `NEO4J_AUTH=neo4j/<password>` accordingly.

Inspect logs / status:

```bash
docker logs -f neo4j
docker ps        # confirm "healthy" / "Up"
```

---

## docker-compose

A `docker-compose.yml` gives a reproducible, version-controlled setup with named
volumes, plugins, and a healthcheck. Create a file `docker-compose.yml`:

```yaml
services:
  neo4j:
    image: neo4j:5
    container_name: neo4j
    ports:
      - "7474:7474"   # HTTP / Browser
      - "7687:7687"   # Bolt
    environment:
      NEO4J_AUTH: neo4j/yourpassword
      # Enable APOC (popular procedure library) on startup
      NEO4J_PLUGINS: '["apoc"]'
      # Optional memory tuning (see Config notes)
      NEO4J_server_memory_heap_initial__size: 1G
      NEO4J_server_memory_heap_max__size: 1G
      NEO4J_server_memory_pagecache_size: 2G
    volumes:
      - ./data:/data
      - ./logs:/logs
      - ./import:/import
      - ./plugins:/plugins
    healthcheck:
      test: ["CMD-SHELL", "wget -q -O /dev/null http://localhost:7474 || exit 1"]
      interval: 30s
      timeout: 10s
      retries: 5
      start_period: 60s
    restart: unless-stopped
```

Start it:

```bash
docker compose up -d
docker compose ps
docker compose logs -f neo4j
```

`NEO4J_PLUGINS='["apoc"]'` auto-downloads and installs the **APOC** library
(procedures & functions for data integration, graph refactoring, etc.). You can
also add `"graph-data-science"` or `"apoc-extended"` to the list.

The mounted volumes:

| Host path    | Container path | Purpose                                  |
|--------------|----------------|------------------------------------------|
| `./data`     | `/data`        | Database store (persistent).             |
| `./logs`     | `/logs`        | Log files.                               |
| `./import`   | `/import`      | Seed CSV/import files (used by `LOAD CSV`). |
| `./plugins`  | `/plugins`     | Manually placed plugins/JARs.            |

To stop and remove (keeps data because it is on the host bind mount):

```bash
docker compose down
```

To wipe everything including data:

```bash
docker compose down -v
rm -rf ./data ./logs ./import ./plugins
```

---

## Verify

### 1. Open the Browser UI

Navigate to:

```
http://localhost:7474
```

Log in with user `neo4j` and the password you set (`yourpassword`). Run a sanity
query:

```cypher
MATCH (n) RETURN count(n);
```

On a fresh database this returns `0`. Create a sample node to confirm writes:

```cypher
CREATE (p:Person {name:'Alice'}) RETURN p;
MATCH (n) RETURN count(n);   // now returns 1
```

### 2. Cypher Shell over Bolt

Run Cypher directly from the host using the container's `cypher-shell`:

```bash
docker exec -it neo4j cypher-shell -u neo4j -p yourpassword "RETURN 1;"
```

A longer example:

```bash
docker exec -it neo4j cypher-shell -u neo4j -p yourpassword \
  "CREATE (:Person {name:'Bob'}); MATCH (n:Person) RETURN n.name;"
```

### 3. Healthcheck / connectivity

From the host (if you have `cypher-shell` installed locally) or via a driver.
Bolt connectivity test with `cypher-shell` pointed at `localhost:7687`:

```bash
cypher-shell -a bolt://localhost:7687 -u neo4j -p yourpassword "RETURN 1;"
```

---

## Config notes

Neo4j configuration lives in `neo4j.conf`. With Docker you can either:

**(a) Use environment variables** — any `neo4j.conf` setting can be set by
prefixing with `NEO4J_` and replacing dots/underscores with double underscores.
Examples:

```bash
-e NEO4J_server_memory_heap_max__size=2G
-e NEO4J_server_memory_pagecache_size=4G
-e NEO4J_dbms_security_auth__enabled=true
-e NEO4J_server_bolt_listen__address=0.0.0.0:7687
```

**(b) Mount a custom `neo4j.conf`** via a `/conf` volume:

```bash
docker run -d --name neo4j \
  -p 7474:7474 -p 7687:7687 \
  -e NEO4J_AUTH=neo4j/yourpassword \
  -v $PWD/neo4j.conf:/conf/neo4j.conf \
  -v $PWD/data:/data \
  neo4j:5
```

### Memory sizing (important)

Neo4j performance is dominated by two memory regions:

- **Heap** — query/transaction working memory.
  `NEO4J_server_memory_heap_initial__size` / `..._heap_max__size`.
- **Page cache** — caches the native graph store in memory.
  `NEO4J_server_memory_pagecache_size`.

Rule of thumb: size the page cache to hold your graph's working set, and the heap
for query state. Total used ≈ heap + page cache + overhead. Example for an 8 GB
machine:

```yaml
environment:
  NEO4J_server_memory_heap_initial__size: 2G
  NEO4J_server_memory_heap_max__size: 2G
  NEO4J_server_memory_pagecache_size: 4G
```

### Plugins

- `NEO4J_PLUGINS='["apoc"]'` installs APOC automatically.
- For Graph Data Science: `NEO4J_PLUGINS='["graph-data-science"]'`
  (note: GDS is an Enterprise/licensed component for some features).

---

## Limitations

### Community Edition = single instance

The `neo4j:5` Docker image runs **Neo4j Community Edition** by default, which is
a **single instance**. This means:

- **No Causal Clustering** — no Raft-based Core/Read Replica HA.
- **No automatic failover** — if the container/host dies, the database is
  unavailable until restarted.
- **No read replicas** — no built-in horizontal read scaling.
- For true HA/clustering you need **Neo4j Enterprise Edition** (licensed), which
  is a different image/license, or you must manage failover externally (e.g.
  orchestration + shared storage + restore procedures).

### Set the password on first run

The password set via `NEO4J_AUTH` only applies on the **first start** with an
empty `/data`. If you already have a database, changing `NEO4J_AUTH` has no
effect — use `ALTER USER` in Cypher or `neo4j-admin dbms set-initial-password`
before first start.

### Memory limits

Containers inherit Docker's memory limits. If you under-allocate, Neo4j may be
OOM-killed or exhibit GC thrashing. Always size heap + page cache within the
container's available RAM.

### Not for production HA out-of-the-box

This local setup is excellent for development, demos, and small single-node
production. For mission-critical HA, plan for Enterprise clustering, monitored
backups (`neo4j-admin database backup`), and resource sizing.

### Data durability

Bind-mounting `/data` to the host preserves data across container recreation.
But **back up regularly** — a deleted volume or corrupted store is unrecoverable
without a backup.

```bash
# Example offline backup (stop container first, or use neo4j-admin backup)
docker compose stop neo4j
tar -czf neo4j-backup-$(date +%F).tar.gz ./data
docker compose start neo4j
```

---

*End of Neo4j deployment documentation (local / Docker).*
