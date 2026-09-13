# Deployment — MongoDB (Local, Docker only)

This guide covers running MongoDB locally with Docker for development and testing. All commands are runnable as-is. The current major series is **MongoDB 8.0.x**.

> **Image note:** MongoDB publishes the official community image at `mongodb/mongodb-community-server:latest` (maintained by MongoDB). The older, now-deprecated `mongo` image also exists but is no longer the recommended source. This guide uses the official `mongodb/mongodb-community-server` image.

> **⚠ AVX CPU requirement (important):** MongoDB **5.0+** binaries require the **AVX** (Advanced Vector Extensions) instruction set. Many older CPUs (especially pre-2011/Some VMs and budget laptops) lack AVX. If `docker run` fails with an illegal-instruction / `SIGILL` crash, your CPU lacks AVX. In that case, use an older image that does not require AVX, e.g.:
> - `mongodb/mongodb-community-server:4.4` (or `mongo:4.4`)
> - `mongo:4.4` / `mongo:4.2`
> Verify AVX support on Linux with: `grep -o 'avx[^ ]*' /proc/cpuinfo | head -1`. On Windows use a tool like CPU-Z.

---

## Prerequisites

1. **Docker** installed and running.
   - Verify: `docker --version` and `docker info`.
2. **AVX check** (for MongoDB 5.0+): ensure your CPU supports AVX, or plan to use a 4.4 image (see note above).
3. **`mongosh`** (optional on host): you can run the shell inside the container with `docker exec`, so installing it on the host is optional. To install on the host: `npm install -g mongosh` (or use the MongoDB shell download).

> Note: the community image does **not** honor `MONGO_INITDB_ROOT_USERNAME` / `MONGO_INITDB_ROOT_PASSWORD` (those env vars were for the old `mongo` image). With the community image, create users manually via `mongosh` after startup.

---

## Single node

The simplest deployment — one `mongod` on port 27017. **Data is ephemeral** (lost when the container is removed) unless you mount a volume (see Persisting data).

```powershell
docker run --name mongodb -d -p 27017:27017 mongodb/mongodb-community-server:latest
```

Check it started:

```powershell
docker logs mongodb
```

Connect with the built-in shell:

```powershell
docker exec -it mongodb mongosh
```

Inside `mongosh`, try:

```js
use shop
db.products.insertOne({ name: "Test", price: 9.99 })
db.products.find()
```

Stop / start / remove:

```powershell
docker stop mongodb
docker start mongodb
docker rm -f mongodb     # WARNING: destroys the container and its ephemeral data
```

If your CPU lacks AVX, use a 4.4 image instead:

```powershell
docker run --name mongodb -d -p 27017:27017 mongodb/mongodb-community-server:4.4
```

---

## Replica set

A replica set gives you failover and is required for features like transactions and change streams. Run each member with `--replSet rs0`, then initialize.

### Option A — Compose with three nodes

Create `docker-compose.yml`:

```yaml
version: "3.8"
services:
  mongo1:
    image: mongodb/mongodb-community-server:latest
    container_name: mongo1
    command: ["mongod", "--replSet", "rs0", "--bind_ip_all"]
    ports:
      - "27017:27017"
    volumes:
      - mongo1_data:/data/db

  mongo2:
    image: mongodb/mongodb-community-server:latest
    container_name: mongo2
    command: ["mongod", "--replSet", "rs0", "--bind_ip_all"]
    ports:
      - "27018:27017"
    volumes:
      - mongo2_data:/data/db

  mongo3:
    image: mongodb/mongodb-community-server:latest
    container_name: mongo3
    command: ["mongod", "--replSet", "rs0", "--bind_ip_all"]
    ports:
      - "27019:27017"
    volumes:
      - mongo3_data:/data/db

volumes:
  mongo1_data:
  mongo2_data:
  mongo3_data:
```

Start:

```powershell
docker compose up -d
```

### Option B — Three separate `docker run` commands

```powershell
docker run --name mongo1 -d -p 27017:27017 mongodb/mongodb-community-server:latest mongod --replSet rs0 --bind_ip_all
docker run --name mongo2 -d -p 27018:27017 mongodb/mongodb-community-server:latest mongod --replSet rs0 --bind_ip_all
docker run --name mongo3 -d -p 27019:27017 mongodb/mongodb-community-server:latest mongod --replSet rs0 --bind_ip_all
```

### Initialize the replica set

Connect to the first node and run `rs.initiate()` with the member topology:

```powershell
docker exec -it mongo1 mongosh
```

```js
rs.initiate({
  _id: "rs0",
  members: [
    { _id: 0, host: "host.docker.internal:27017" },
    { _id: 1, host: "host.docker.internal:27018" },
    { _id: 2, host: "host.docker.internal:27019" }
  ]
})
```

> On Linux, use the container IPs or service names (with a Docker network) instead of `host.docker.internal`. On a single-host test you can also use `localhost` if ports are published: `"localhost:27017"`, etc. Wait for `rs.status()` to show one `PRIMARY` and two `SECONDARY`.

Check status:

```js
rs.status()
```

Now writes go to the primary and replicate to secondaries. To read from a secondary:

```js
db.getMongo().setReadPref("secondary")
```

> **AVX caveat applies here too:** if your CPU lacks AVX, pin the image to `:4.4` for all three nodes.

---

## Persisting data

Without a volume, all data is lost when the container is removed. Persist `/data/db` with a named volume.

Single node with a named volume:

```powershell
docker run --name mongodb -d -p 27017:27017 `
  -v mongodb_data:/data/db `
  mongodb/mongodb-community-server:latest
```

Compose already uses named volumes (`mongo1_data`, etc.) above. To inspect:

```powershell
docker volume ls
docker volume inspect mongodb_data
```

To back up with `mongodump`:

```powershell
docker exec -it mongodb mongodump --out /tmp/backup
docker cp mongodb:/tmp/backup ./backup
```

---

## Verify

Connect and exercise a basic write/read cycle:

```powershell
docker exec -it mongodb mongosh
```

```js
use testdb
db.users.insertOne({ name: "Ada", email: "ada@example.com", createdAt: new Date() })
db.users.find({ name: "Ada" })
db.users.countDocuments()
db.users.createIndex({ email: 1 }, { unique: true })
```

For a replica set, verify replication:

```js
// on primary
db.orders.insertOne({ item: "x", qty: 3 })

// on a secondary (after setting read preference)
db.orders.find()
```

---

## Configuration notes

### `mongod.conf`

The community image uses a default config; you can supply your own by mounting a file and pointing to it with `--config`.

Example `mongod.conf`:

```yaml
storage:
  dbPath: /data/db
  journal:
    enabled: true
systemLog:
  destination: file
  path: /var/log/mongodb/mongod.log
  logAppend: true
net:
  bindIp: 0.0.0.0
  port: 27017
replication:
  replSetName: rs0
security:
  authorization: enabled
```

Mount and use it:

```powershell
docker run --name mongodb -d -p 27017:27017 `
  -v ${PWD}/mongod.conf:/etc/mongod.conf `
  -v mongodb_data:/data/db `
  mongodb/mongodb-community-server:latest mongod --config /etc/mongod.conf
```

### Authentication

The community image does **not** auto-create a root user from env vars. Enable auth and create users via `mongosh`:

```powershell
docker exec -it mongodb mongosh
```

```js
use admin
db.createUser({
  user: "root",
  pwd: "changeMeStrongly",
  roles: [{ role: "root", db: "admin" }]
})
```

Then restart with `--auth` (or `security.authorization: enabled` in config) and connect with credentials:

```powershell
docker exec -it mongodb mongosh -u root -p changeMeStrongly --authenticationDatabase admin
```

For replica-set member-to-member auth, use a **keyfile**:

```powershell
openssl rand -base64 756 > mongodb-keyfile
chmod 400 mongodb-keyfile   # (Linux/macOS)
docker run --name mongo1 -d -p 27017:27017 `
  -v ${PWD}/mongodb-keyfile:/data/keyfile `
  mongodb/mongodb-community-server:latest `
  mongod --replSet rs0 --bind_ip_all --auth --keyFile /data/keyfile
```

> On Windows PowerShell, generate the keyfile with: `openssl rand -base64 756 > mongodb-keyfile` (requires OpenSSL) or create it inside the container.

### Resource limits

Constrain CPU/memory so MongoDB does not starve the host:

```powershell
docker run --name mongodb -d -p 27017:27017 `
  --memory=2g --cpus=2 `
  -v mongodb_data:/data/db `
  mongodb/mongodb-community-server:latest
```

---

## Limitations

- **Ephemeral without a volume** — removing the container deletes all data unless `/data/db` is mounted.
- **Replica set needs an odd number of voters** — use 3 data-bearing members, or 2 members + 1 arbiter, to avoid election ties. Even numbers risk split-brain / no-majority scenarios.
- **Single node has no HA** — it is for dev/test only; a crash means downtime and possible data loss up to the last journal flush.
- **Resource heavy** — WiredTiger wants RAM for its cache; constrained containers may perform poorly with large working sets.
- **AVX requirement (5.0+)** — older CPUs must use the 4.4 image; features introduced after 4.4 will be unavailable.
- **Community image differs from `mongo`** — no `MONGO_INITDB_*` env handling; users/roles must be created manually via `mongosh`.
- **Not for production as shown** — these are local/dev setups. Production should use TLS, auth, monitored resource limits, and preferably a managed service (Atlas) or a properly orchestrated cluster.
