# MongoDB — Architecture & Schema

## Overview

MongoDB is a distributed, document-oriented database that stores data as **BSON** (Binary JSON) documents. Unlike relational databases that organize data into rows and tables with a fixed schema, MongoDB uses a flexible, JSON-like model where each record is a self-describing document. This makes it a natural fit for application objects that do not map cleanly to rigid relational schemas.

At its core, MongoDB is built around three ideas:

- **Document model** — data is grouped into documents (key-value pairs, arrays, nested sub-documents) rather than normalized rows. Application objects can often be persisted as a single document.
- **Horizontal scalability** — data can be partitioned (sharded) across many machines, with a router (`mongos`) that hides the distribution from clients.
- **High availability** — a group of `mongod` processes (a *replica set*) automatically replicates writes and fails over without operator intervention.

MongoDB's logical hierarchy is:

```
Deployment (standalone | replica set | sharded cluster)
 └── Database
      └── Collection (like a table)
           └── Document (BSON, like a row)
                └── Field / embedded sub-document / array
```

The default storage engine since MongoDB 3.2 is **WiredTiger**, which provides document-level concurrency control, a write-ahead journal, compression, and an internal cache.

---

## Deployment topologies

MongoDB can be deployed in three principal topologies. Choosing one depends on availability, durability, and scale requirements.

### Standalone

A single `mongod` process. Simplest to run; no replication or failover. Suitable for development, local testing, and small single-host workloads where downtime is acceptable.

```
[ Application ]
      |
   [ mongod ]   (single node, port 27017)
```

There is no redundancy: if the node fails, the database is unavailable until it is restarted, and any unreplicated data lost to disk failure is gone.

### Replica set

A group of `mongod` instances that maintain the same data set. A replica set has:

- **One primary** — accepts all writes (by default).
- **One or more secondaries** — replicate the primary's oplog and can serve reads (with a read preference).
- Optionally **arbiters** — vote in elections but hold no data, used to get an odd number of voters cheaply.

```
[ Driver ]
    |
[ Primary ]  <--- oplog replication ---> [ Secondary ]  [ Secondary ]
```

Replica sets provide automatic failover: if the primary becomes unreachable, an election promotes a secondary. Clients are notified and reconnect to the new primary transparently.

### Sharded cluster

A sharded cluster partitions data across multiple *shards* to scale beyond the capacity of a single machine. It consists of:

- **`mongos`** — the query router. Clients connect to `mongos`, which routes operations to the correct shard(s) based on the **shard key**. `mongos` is stateless.
- **Config servers** — store the cluster metadata: which chunks live on which shards, shard key ranges, and balancer state. In MongoDB 3.4+ these are themselves a replica set.
- **Shards** — each shard is a *replica set* (not a single node), so every partition is also highly available.

```
                 [ mongos router ]  (one or many)
                  /      |       \
        [ Config servers (repl set) ]
                  |       |       |
            [ Shard A ] [ Shard B ] [ Shard C ]
            (repl set)  (repl set)  (repl set)
```

In a sharded cluster, write and read paths go through `mongos`. In a plain replica set (no sharding), the driver connects directly to the primary and handles failover discovery itself.

---

## Replica sets

Replica sets are the foundation of MongoDB's high-availability story.

### Primary and secondaries

- The **primary** receives all writes by default. It records every change to its **oplog** (operations log).
- **Secondaries** continuously apply the primary's oplog to their own data set, keeping an eventually-consistent copy.

### Oplog

The oplog is a capped, idempotent collection (`local.oplog.rs`) of all insert/update/delete operations in order. Because entries are idempotent, a secondary can replay them safely even if interrupted. The oplog enables:

- Initial sync and ongoing replication.
- Point-in-time recovery and change streams.

If a secondary falls behind, it simply catches up from where its oplog cursor left off (as long as the primary still retains those entries).

### Automatic failover and election

When the primary becomes unreachable (network partition, crash, maintenance), the secondaries detect the timeout and hold an **election** to choose a new primary. MongoDB uses a Raft-like consensus protocol:

1. A secondary notices the primary is down (no heartbeats).
2. It requests votes from other voting members.
3. If it receives a majority, it becomes primary.

Elections are gated by **priority** (configurable per member) and **votes**. A member will not become primary unless it can see a majority of the set. This prevents *split-brain* in network partitions.

### Read preference

By default, reads go to the primary (strongest consistency). Clients can set a **read preference** to route reads differently:

| Read preference | Behavior |
|-----------------|----------|
| `primary` (default) | Reads from primary only. |
| `primaryPreferred` | Primary if available, else a secondary. |
| `secondary` | Reads from a secondary (eventual consistency). |
| `secondaryPreferred` | Secondary if available, else primary. |
| `nearest` | Lowest-latency member regardless of type. |

### Write concern

**Write concern** controls how many members must acknowledge a write before it is considered successful:

- `w: 1` — only the primary must acknowledge (default).
- `w: "majority"` — a majority of the replica set must acknowledge; protects against loss on failover.
- `j: true` — the write must be journaled (durable to disk) before acknowledging.

Higher write concern increases durability at the cost of latency.

---

## Sharding

Sharding distributes data across shards so the cluster can grow horizontally.

### Shard key

The **shard key** is a field (or fields) present in every document that determines how documents are distributed. Choosing a good shard key is critical: it should have high cardinality, avoid a "hot" shard, and match common query patterns so `mongos` can target a single shard (a *targeted* query) rather than broadcasting to all shards (a *scatter-gather* query).

### Chunks

MongoDB divides shard-key range space into **chunks** (default 64 MB). As data grows, chunks are split when they exceed the size threshold. The **balancer** subsequently moves chunks between shards to keep data evenly distributed.

### Balancer

The balancer is a background process (running on a config server primary) that migrates chunks to balance load. It operates while the system is online; chunk migrations are throttled to limit impact on foreground traffic.

### Hashed vs ranged sharding

- **Ranged sharding** — chunks are contiguous ranges of the shard key. Efficient for range queries on the shard key, but a monotonically increasing key (e.g., a timestamp) creates a hot shard.
- **Hashed sharding** — the shard key is hashed, spreading writes evenly across shards. Excellent write distribution; poor for range queries on the original key.

```js
// Range shard key on a category field
sh.shardCollection("shop.products", { category: 1 })

// Hashed shard key on _id for even distribution
sh.shardCollection("shop.events", { _id: "hashed" })
```

> **Caveat:** The shard key cannot be changed after sharding without dumping and reloading the collection. Choose it carefully.

---

## Storage engine (WiredTiger)

WiredTiger is the default and only generally-available storage engine for MongoDB (the legacy MMAPv1 engine was removed in MongoDB 4.2).

### Document-level locking

WiredTiger uses **document-level concurrency** with optimistic concurrency control. Writers on different documents do not block each other; only concurrent writers to the *same* document serialize via an intent lock. This is a major improvement over the collection-level locking of older engines.

### Journal (write-ahead log)

Before modifying data files, WiredTiger writes the change to an on-disk **journal** (WAL). The journal allows recovery after a crash: on restart, uncheckpointed operations are replayed from the journal. Journal commits are batched (every 100 ms by default, or on `j: true`).

### Compression

WiredTiger compresses data on disk:

- **Snappy** (default) for collection data — good balance of speed and ratio.
- **zstd** (available) for higher compression.
- **zlib** also available.
- Indexes are compressed with a prefix compression scheme.

This reduces disk footprint but adds some CPU cost on read/write.

### Cache

WiredTiger maintains an in-memory **cache** (default up to 50% of RAM, capped at ~256 MB minimum / configurable via `cacheSizeGB`). Frequently accessed documents and indexes live in the cache for fast reads. The operating system page cache is also used as a second layer.

---

## Write path

When an application issues an insert/update/delete:

1. **Routing** — the driver (or `mongos` in a sharded cluster) determines the target:
   - Sharded cluster: the operation is routed by **shard key** to the owning shard.
   - Replica set: the driver sends the write to the **primary**.

2. **Validation** — the primary checks the write against schema validation rules (if any) and indexes; it builds the BSON document.

3. **Storage engine write** — WiredTiger writes to the **journal** (WAL) for durability, updates the document in the **in-memory cache**, and schedules the data-file update at the next checkpoint.

4. **Oplog** — the operation is appended to the primary's **oplog** so secondaries can replicate it.

5. **Replication** — secondaries pull the oplog and apply the change to their own WiredTiger storage, in order.

6. **Acknowledgement** — the primary waits until the configured **write concern** is satisfied (e.g., `w: "majority"`, `j: true`) and then replies to the client.

```
Driver -> [mongos] -> Primary (validate, journal, cache, oplog)
                                   |
                            oplog replication
                                   v
                            Secondary(s) apply
                                   |
                            ack per write concern -> Driver
```

---

## Read path & aggregation pipeline

By default, reads go to the **primary** for strong consistency. With a **read preference**, they can be routed to the **nearest** or a **secondary**.

1. The driver/`mongos` selects the target member per read preference.
2. MongoDB checks for a usable **index**; if found, it uses an index scan, otherwise a collection scan (`COLLSCAN`).
3. Documents are read from the WiredTiger cache (or disk if evicted).
4. The result set is returned, applying filters, sort, projection, and limits.

### Aggregation pipeline

MongoDB's aggregation framework processes documents through an **ordered pipeline** of stages. Each stage transforms the documents and passes them to the next.

```js
db.orders.aggregate([
  { $match: { status: "shipped" } },                 // filter
  { $group: {                                        // group
      _id: "$customerId",
      total: { $sum: "$amount" },
      count: { $sum: 1 }
  } },
  { $sort: { total: -1 } },                          // sort
  { $limit: 10 }                                     // top 10
])
```

Common stages include `$match`, `$project`, `$group`, `$lookup` (left-outer join), `$unwind`, `$sort`, `$facet`, `$out`, and many more. Pipelines can run entirely inside the database engine, minimizing data transfer to the client.

---

## Data / schema model

MongoDB organizes data as: **database → collection → BSON document**.

- A **database** is a container of collections.
- A **collection** is a group of documents (like a table but without a fixed schema).
- A **document** is a BSON object of fields, including nested sub-documents and arrays.

### Embedding vs referencing

Two patterns exist for relating data:

- **Embedding (denormalization)** — nest related data inside the parent document. Best when data is accessed together and "owns" the child (one-to-one / one-to-few). Avoid if the embedded array grows without bound.

```js
// Embedded: a blog post with its comments
{
  _id: ObjectId("..."),
  title: "MongoDB internals",
  author: { name: "Ada", email: "ada@example.com" },
  comments: [
    { user: "Bob", text: "Great!", ts: ISODate("2026-01-02") },
    { user: "Cy",  text: "Thanks", ts: ISODate("2026-01-03") }
  ]
}
```

- **Referencing (normalization)** — store a reference (`_id`) to another document, like a foreign key. Best for one-to-many / many-to-many, or when the referenced data is shared. Use `$lookup` to join at query time.

```js
// Referenced: separate users collection
db.users.insertOne({ _id: 1, name: "Ada", email: "ada@example.com" })
db.posts.insertOne({ _id: 10, title: "MongoDB internals", authorId: 1 })

// join
db.posts.aggregate([
  { $match: { _id: 10 } },
  { $lookup: { from: "users", localField: "authorId", foreignField: "_id", as: "author" } }
])
```

### `_id` and schema validation

- Every document must have an `_id` field that is unique within its collection. If omitted, the driver assigns an `ObjectId`.
- `_id` is automatically indexed.

MongoDB supports optional **schema validation** so you can enforce structure without losing all flexibility:

```js
db.createCollection("products", {
  validator: {
    $jsonSchema: {
      bsonType: "object",
      required: ["name", "price", "category"],
      properties: {
        name:     { bsonType: "string", minLength: 1 },
        price:    { bsonType: "decimal", minimum: 0 },
        category: { enum: ["book", "electronics", "apparel"] },
        inStock:  { bsonType: "bool" }
      }
    }
  },
  validationLevel: "strict",     // validate on insert and update
  validationAction: "error"      // reject invalid writes
})
```

```js
// Insert a valid document
db.products.insertOne({
  name: "Wireless Mouse",
  price: NumberDecimal("29.99"),
  category: "electronics",
  inStock: true
})
```

### BSON types

Documents use BSON types beyond JSON: `ObjectId`, `Date` (`ISODate`), `Decimal128` (exact decimals for money), `BinData`, arrays, embedded documents, `Null`, `Timestamp`, `Long`, etc.

---

## Indexes

Indexes improve query performance by avoiding full collection scans. MongoDB automatically indexes `_id`; other indexes are created explicitly.

| Index type | Use |
|------------|-----|
| Single field | `{ name: 1 }` — index on one field. |
| Compound | `{ category: 1, price: -1 }` — multiple fields; supports prefix queries. |
| Multikey (array) | Automatic when a field holds an array. |
| Text | Full-text search across string content. |
| Geospatial | `2dsphere` for GeoJSON; `2d` for legacy points. |
| TTL | Time-to-live; auto-deletes documents after a duration. |
| Unique | Enforces uniqueness (beyond `_id`). |
| Partial / Sparse | Index only documents matching a filter / with the field present. |

```js
// Compound index
db.products.createIndex({ category: 1, price: -1 })

// Text index
db.articles.createIndex({ title: "text", body: "text" })
db.articles.find({ $text: { $search: "mongodb sharding" } })

// TTL index — documents expire 1 hour after createdAt
db.sessions.createIndex({ createdAt: 1 }, { expireAfterSeconds: 3600 })

// 2dsphere geo index
db.places.createIndex({ location: "2dsphere" })
db.places.find({
  location: { $near: { $geometry: { type: "Point", coordinates: [-73.9, 40.7] }, $maxDistance: 1000 } }
})
```

Indexes consume RAM and disk; large or redundant indexes slow writes and increase storage. Use `explain()` to inspect query plans:

```js
db.products.find({ category: "electronics" }).explain("executionStats")
```

---

## Scaling & HA

### Horizontal scaling (sharding)

Sharding lets a MongoDB cluster grow beyond a single server's CPU, memory, and disk. Add shards and let the balancer redistribute chunks. Reads/writes that target a single shard (good shard key) scale linearly.

### Vertical scaling

A single replica-set member can also be scaled up (more CPU/RAM/disk). WiredTiger's cache benefits from more RAM.

### High availability

- **Replica sets** provide automatic failover with no manual intervention.
- **Shards are replica sets**, so every partition survives node loss.
- **Config servers** are a replica set, so cluster metadata is also redundant.
- **`mongos`** routers are stateless and can be run many times (behind a load balancer) for availability.

### Durability

- Journal (WAL) protects individual writes.
- Replication (`w: majority`) protects against primary loss.
- Combined, MongoDB offers tunable durability vs. latency.

### Multi-document transactions

MongoDB supports ACID **multi-document transactions** (single-replica-set since 4.0, distributed across shards since 4.2). These provide all-or-nothing semantics but carry overhead and should be used judiciously — most designs prefer the document model to avoid cross-document transactions.

```js
const session = db.getMongo().startSession()
session.startTransaction()
try {
  session.getDatabase("shop").orders.insertOne({ item: "x", qty: 2 }, { session })
  session.getDatabase("shop").inventory.updateOne({ item: "x" }, { $inc: { qty: -2 } }, { session })
  session.commitTransaction()
} catch (e) {
  session.abortTransaction()
}
```

---

## Summary

MongoDB is a flexible, horizontally scalable document database built on BSON, WiredTiger, replica sets, and sharding. Its strengths — a schemaless document model, rich queries and aggregation, strong HA, and linear scale via sharding — make it well suited to modern application backends with evolving schemas. Careful choices around shard key, indexes, read preference, and write concern are essential to getting the most out of it.
