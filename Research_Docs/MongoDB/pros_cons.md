# Pros and Cons — MongoDB

MongoDB is a popular general-purpose document database. Like every technology, it is a trade-off: it excels in some workloads and is a poor fit for others. This document enumerates its strengths and weaknesses so architects can make an informed choice.

---

## Pros

### Flexible, schemaless document model

Documents are BSON and do not require a predefined schema. Fields can vary across documents in the same collection, so the data model can evolve as the application changes — no expensive `ALTER TABLE` migrations. This is ideal for rapidly iterating products.

```js
// Two documents in the same collection can have different shapes
db.users.insertMany([
  { _id: 1, name: "Ada",  email: "ada@example.com" },
  { _id: 2, name: "Bob",  email: "bob@example.com", phone: "+1-555", preferences: { theme: "dark" } }
])
```

Optional JSON Schema validation (`$jsonSchema`) lets teams enforce structure when they want it, keeping flexibility without chaos.

### Horizontal scale via sharding

MongoDB scales out by partitioning data across shards. A sharded cluster can grow to petabytes by adding shards; the `mongos` router hides distribution from clients. Writes and reads that target a good shard key scale close to linearly.

### Strong ecosystem and tooling

- Official drivers for nearly every language (Java, Python, Node.js, Go, C#, Rust, etc.).
- `mongosh` shell, **Compass** GUI, **Atlas** managed cloud service.
- Rich visualization, monitoring (Ops Manager / Atlas), and backup tooling.
- Large community, abundant documentation, and integration with most frameworks (Mongoose for Node, Spring Data, Django, etc.).

### Rich queries and aggregation

MongoDB offers a powerful query language (filtering, projections, array operators, regex, geospatial) and a full **aggregation pipeline** (`$match`, `$group`, `$lookup`, `$facet`, `$bucket`, etc.) that can do sophisticated analytics entirely inside the database.

```js
db.sales.aggregate([
  { $match: { date: { $gte: ISODate("2026-01-01") } } },
  { $group: { _id: "$region", revenue: { $sum: "$amount" } } },
  { $sort: { revenue: -1 } }
])
```

### Sticky high availability with replica sets

Replica sets provide automatic failover with no manual intervention. A Raft-like election promotes a new primary when the old one fails, and drivers transparently reconnect. Because every shard is itself a replica set, the whole cluster is resilient.

### JSON-like documents map naturally to application objects

Documents mirror the objects in application code (nested structures, arrays), reducing the object-relational impedance mismatch. Developers spend less time marshalling rows into objects.

### GridFS for large files

MongoDB can store files larger than 16 MB via **GridFS**, which splits a file into chunks and stores them as documents. This avoids needing a separate blob store for moderate file needs.

### Tunable consistency and durability

Write concern (`w`, `j`) and read concern (`local`, `majority`, `snapshot`) let teams trade latency for durability/consistency per operation, rather than being forced into one global setting.

### Multi-document ACID transactions

Since 4.0 (single replica set) and 4.2 (distributed), MongoDB supports ACID transactions across multiple documents and shards, closing a historic gap versus relational systems (with caveats on overhead).

---

## Cons

### Memory heavy

MongoDB relies heavily on RAM. WiredTiger caches data and indexes in memory (default up to 50% of RAM), and the operating system page cache adds another layer. Large working sets and large indexes can demand substantial memory; when the working set exceeds RAM, performance degrades sharply due to disk I/O.

### No true multi-document ACID joins across collections (historically limited)

- Before 4.0, MongoDB had **no** multi-document transactions; each document write was atomic only at the single-document level.
- From 4.0/4.2, multi-document and distributed transactions exist, but they are **expensive**: they take locks, consume resources, and are not meant for high-throughput hot paths. Relational databases still handle many small cross-row transactions more efficiently.

### Schema flexibility can cause inconsistency

The same freedom that speeds development can also produce inconsistent data: missing fields, type drift (`"price"` sometimes a string, sometimes a number), and "junk" documents. Without discipline (or validation), analytics and application logic become brittle.

```js
// Type drift — a real operational headache
{ _id: 1, price: 29.99 }
{ _id: 2, price: "29.99" }   // string instead of number
```

### Shard key is hard to change later

Once a collection is sharded, the **shard key cannot be changed** without dumping, dropping, and reloading the data with a new key. A poor initial choice (low cardinality, monotonically increasing, or not matching query patterns) is very costly to fix.

### Transactions have overhead

Even with 4.0+ transactions, using them for every operation negates MongoDB's speed advantages. They increase latency, hold locks, and complicate failover. The recommended pattern remains the document model (embedding) to avoid cross-document transactions.

### Disk usage and large indexes

- Compression helps, but BSON documents and multiple indexes can consume significant disk.
- Each additional index slows writes (every insert/update must maintain it) and consumes RAM.
- Wide, deeply nested documents and unbounded arrays inflate storage and degrade performance.

### Limited ad-hoc relational reporting

Complex multi-table joins, heavy `GROUP BY` across many entities, and strict referential integrity are where relational databases shine. While `$lookup` exists, building deep join graphs in MongoDB is awkward and slower than a mature SQL optimizer with foreign keys and constraints.

### Not ideal for strongly relational, regulatory workloads

Workloads requiring strict ACID semantics across many entities, complex constraints, and audit-grade relational integrity (banking ledgers, ERP) are usually better served by a traditional RDBMS unless carefully designed around the document model.

### Operational complexity at scale

Running your own sharded cluster means managing `mongos`, config servers, multiple replica sets, the balancer, and chunk migration — a meaningful operations burden. (Managed Atlas removes much of this, at a cost.)

---

## Quick decision checklist

**Lean toward MongoDB when:** you need a flexible schema, horizontal scale, JSON-like data, fast iteration, and tolerant/eventual consistency for most reads.

**Lean away from MongoDB when:** you need complex multi-entity transactions, heavy relational joins/reporting, rigid schemas with strong referential integrity, or you cannot afford the RAM/operational footprint.

Neither choice is absolute — many systems use MongoDB alongside a relational database (polyglot persistence), playing to each one's strengths.
