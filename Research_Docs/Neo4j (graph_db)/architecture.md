# Neo4j — Architecture & Schema

> DBMS: **Neo4j** — Native graph database (Cypher)
> Research documentation for the Database Project.

---

## Overview

Neo4j is a **native graph database** designed from the ground up to store,
manage, and query highly connected data. Unlike relational databases that model
relationships as foreign-key joins, or document stores that embed relationships
inside documents, Neo4j stores relationships as **first-class, physical
entities** in the data model. This is the single most important architectural
distinction of a native graph system.

The defining advantage of this design is **index-free adjacency**. Every node
physically points to its adjacent relationships, and every relationship points
to its source and target nodes. There are no join tables, no index lookups, and
no pointer-chasing through intermediate structures to traverse a connection. As a
result, the cost of traversing one more hop in a graph is **O(1) per
relationship**, regardless of the total size of the graph. Query latency grows
with the *size of the answer*, not the *size of the dataset* — the opposite of
what happens in an RDBMS join or a document-store self-join.

Neo4j exposes a declarative, SQL-inspired query language called **Cypher**, which
expresses graph patterns directly: nodes, relationships, and the patterns
between them. Cypher is now an open standard (ISO/IEC 39075:2024) and has been
implemented by other graph databases via the openCypher project.

Key characteristics:

- **ACID transactions** — fully durable, serializable transaction isolation.
- **Native graph storage engine** — relationships are stored as pointers, not
  computed at query time.
- **Property graph model** — nodes and relationships can both carry key/value
  properties and labels / types.
- **Causal clustering** — for high availability and horizontal read scaling
  (Enterprise Edition).
- **Mature tooling** — Neo4j Browser, Cypher Shell, drivers for all major
  languages, and the Graph Data Science (GDS) library.

---

## Native graph storage

Neo4j's storage engine is purpose-built for graphs. The on-disk format (the
"native graph store") organizes data into a set of store files, each responsible
for one kind of record.

### Nodes

A **node** is a record that:

- Has a set of **labels** (`Person`, `Product`, `Account`, …) describing its
  role in the domain.
- Carries **properties** as key/value pairs (`{name:"Alice", age:34}`).
- Holds **direct pointers** to its first incoming and first outgoing
  relationship (the "relationship chain"). From there, each relationship record
  links to the next relationship of that node, forming a singly linked list per
  node.

### Relationships

A **relationship** is also a physical record (not a derived join). It:

- Has a **type** (`KNOWS`, `PURCHASED`, `BELONGS_TO`, …).
- Has a **source node** and a **target node**, stored as direct pointers.
- Carries its own **properties** (`{since:2019, weight:0.8}`).
- Contains **prev/next pointers** on both the source side and the target side, so
  that a node can efficiently walk all of its relationships in either direction.

Because relationships are stored as records with node pointers, traversal is
nothing more than following a memory pointer from one record to the next. This is
the essence of **index-free adjacency**.

### Properties

Properties are stored in a separate **property store**. Each node/relationship
record holds a pointer to its property chain (a linked list of property
records). Property values that are too large to fit inline (e.g. big strings or
arrays) are spilled into a **dynamic store** (string store / array store).

### Labels and the schema

Labels are recorded in a dedicated **label store**. Optionally, the user can
declare **indexes** and **constraints** that introduce auxiliary structures on
top of the native store (see below).

### Indexes (auxiliary structures)

While traversals use index-free adjacency, queries still need a way to find an
*entry point* (an "anchor") into the graph. Neo4j provides several index types:

| Index type      | Purpose                                                        |
|-----------------|---------------------------------------------------------------|
| B-tree (default)| Equality and range lookups on node/relationship properties.    |
| Range           | Optimized range scans.                                         |
| Full-text       | Tokenized text search (Lucene-backed).                        |
| Token lookup    | Fast `MATCH (n:Label)` — find all nodes by label.             |
| Text / point    | Specialized equality / spatial lookups.                       |

Indexes do **not** replace adjacency — they are only used to locate the starting
node(s) of a traversal efficiently. Once the anchor is found, traversal proceeds
pointer-to-pointer with no further index access.

### Storage layout summary

```
+-----------+     +----------------+     +------------------+
| Node store| --> | Relationship   | <-- | Property store   |
| (labels,  |     | store (type,   |     | (key/value,      |
| rel ptrs) |     | src/tgt ptrs,  |     | dynamic strings/ |
+-----------+     | props ptr)     |     | arrays)          |
                  +----------------+
```

---

## Query layer

### Cypher

Cypher is a declarative, pattern-matching query language. A typical query
describes the shape of the graph to find:

```cypher
MATCH (p:Person)-[:KNOWS]->(friend:Person)
WHERE p.name = 'Alice' AND friend.age > 30
RETURN friend.name, friend.age
```

The pattern `(:Person)-[:KNOWS]->(:Person)` is matched directly against the
native store using index-free adjacency. Cypher is the primary interface for:

- Ad-hoc exploration (Neo4j Browser).
- Application queries (drivers).
- Administrative tasks (schema, users, procedures).

### Bolt — the binary protocol (:7687)

**Bolt** is Neo4j's primary client-server protocol. It is a **binary**,
stateful, request/response protocol optimized for graph workloads. Key features:

- Efficient binary encoding of graph types (nodes, relationships, paths).
- Connection pooling and pipelining from client drivers.
- Default port **7687**.

All official drivers (Java, Python, JavaScript, .NET, Go) communicate over Bolt.

### HTTP — transactional REST endpoint (:7474)

Neo4j also exposes an **HTTP API** (default port **7474**) for transactional
Cypher execution and for non-driver clients. The HTTP endpoint is convenient for
tooling and integrations but is generally slower than Bolt for high-throughput
workloads.

### Neo4j Browser & Cypher Shell

- **Neo4j Browser** — a web UI served on `:7474` for writing Cypher, visualizing
  graphs, and managing the database.
- **Cypher Shell** (`cypher-shell`) — a command-line client for executing Cypher
  over Bolt.

```
Client tier:
  Neo4j Browser ─┐
  Drivers       ─┼─> Bolt :7687  ──> Neo4j kernel
  Cypher Shell  ─┘               (HTTP :7474 also available)
```

---

## Causal clustering

For high availability and horizontal read scaling, Neo4j offers **Causal
Clustering** (Enterprise Edition). It is built on the **Raft consensus
protocol**.

### Core nodes (Raft consensus)

- A cluster is composed of **Core nodes** that jointly store the data.
- Core nodes use **Raft** to agree on the transaction log order, guaranteeing
  that committed transactions are durably replicated to a majority of cores.
- Core nodes provide **automatic failover**: if the leader fails, a new leader is
  elected without human intervention.
- Core nodes serve both reads and writes.
- Safety property: a write is safe once a **majority quorum** of cores has
  acknowledged it (typically 3 or 5 cores).

### Read Replicas

- **Read Replicas** are read-only members that asynchronously copy the transaction
  log from a Core node.
- They provide **horizontal read scaling** — offloading read traffic from cores.
- They are *eventually consistent* (lag is usually milliseconds).
- Replicas do not participate in Raft voting and cannot become leaders.

### Failover & routing

- The **Bolt routing protocol** allows a driver to discover cluster topology and
  route writes to the leader and reads to followers/replicas automatically.
- A failed Core is replaced by promoting/electing within the surviving majority.

```
        +---------------- Causal Cluster ------------------+
        |  Core (leader, Raft)   Core (follower, Raft)     |
        |     |      \             /                       |
        |     |       \           /                        |
        |  Read Replica      Read Replica                 |
        +--------------------------------------------------+
```

> Note: Causal clustering and HA are **Enterprise Edition** features. The
> Community Edition runs as a **single instance** only (see Deployment & Scaling
> sections).

---

## Memory management

Neo4j uses two primary memory regions that must be sized carefully for
performance.

### Page cache (native store cache)

- The **page cache** holds portions of the native graph store (nodes,
  relationships, properties, indexes) mapped from disk into memory.
- It is the most important cache for graph traversal performance: the more of the
  graph that fits in the page cache, the fewer disk reads are required.
- Sized via `server.memory.pagecache.size`. Ideally large enough to hold the
  *working set* of the graph (or the whole graph).

### Heap

- The **heap** is the JVM heap used for:
  - Query execution (runtime state, intermediate result sets).
  - Transaction state and locking.
  - Cypher query planning.
  - Management of connections and caches.
- Sized via `server.memory.heap.initial_size` and
  `server.memory.heap.max_size`.

### Rule of thumb

- Page cache should be sized for the graph data footprint; heap for query/transaction
  working memory.
- Total memory used ≈ page cache + heap + OS overhead. Under-provisioning the
  page cache causes disk-bound traversal; under-provisioning the heap causes GC
  pressure and `OutOfMemoryError`.

```
        +----------------------- Neo4j process -----------------------+
        |  Heap (JVM)            |  Page cache (off-heap mapped)     |
        |  - query runtime       |  - node/rel/property pages        |
        |  - txn state           |  - index pages                    |
        |  - planner             |                                   |
        +------------------------+-----------------------------------+
                                      |
                                   disk (native store + WAL)
```

---

## Write path

A write transaction follows these stages:

1. **Cypher submission** — a client sends a Cypher `CREATE`/`MERGE`/`SET`
   statement over Bolt (or HTTP).
2. **Query processor** — the Cypher engine **parses** the query, builds a
   **logical plan**, and the **cost-based planner** produces an executable
   **physical plan**.
3. **Transaction begin** — a write transaction is opened; locks are acquired as
   needed.
4. **Native store write** — the runtime writes new **node records**,
   **relationship records**, and **property records** into the appropriate store
   files. Relationship records are wired into the node's relationship chain
   (pointer updates enable index-free adjacency).
5. **Index maintenance** — any affected **indexes** (B-tree, full-text, token
   lookup, etc.) are updated synchronously so they stay consistent.
6. **WAL for durability** — every change is appended to the **write-ahead log**
   (transaction log) before the transaction is acknowledged, guaranteeing
   durability and crash recovery.
7. **Commit / acknowledge** — once a majority (in a cluster) or the local log
   (single instance) confirms the write, the transaction commits and the client
   receives success.

```text
Cypher WRITE
  -> Cypher planner/optimizer
  -> transaction (locks, txn state)
  -> write node/rel/property records into native store
  -> update indexes (B-tree/full-text/token)
  -> append to WAL (durability)
  -> commit
```

---

## Read path (traversal)

A read query is optimized around pointer-following:

1. **Cypher MATCH** — client submits a pattern query over Bolt.
2. **Planner** decides how to start — typically using an **index** (or token
   lookup) to find the **anchor node(s)** that satisfy the starting constraint
   (e.g. `:Person` with `name = 'Alice'`).
3. **Index lookup** returns the starting node IDs.
4. **Traversal via index-free adjacency** — from each anchor, the runtime follows
   **relationship pointers** stored on the node records to reach adjacent nodes,
   recursively expanding the pattern. No join tables, no extra index lookups per
   hop.
5. **Page cache usage** — node/relationship/property pages are read from the
   **page cache** (disk only on a cache miss).
6. **Subgraph assembly** — matched nodes/relationships are assembled into the
   result (often as paths) and returned to the client over Bolt.

```text
Cypher MATCH
  -> planner uses index to find anchor node
  -> traverse from anchor via relationship pointers (index-free adjacency)
  -> each hop is O(1) pointer follow
  -> pages served from page cache (disk on miss)
  -> return subgraph
```

The performance of this path is proportional to the **size of the returned
subgraph**, not the size of the whole database.

---

## Data / schema model

Neo4j implements the **property graph** model:

- **Nodes** — can have 0..n **labels** and 0..n **properties**.
- **Relationships** — directed (have a start node and end node), have exactly one
  **type**, and can have properties.

### Creating nodes

```cypher
// Single node with label and properties
CREATE (p:Person {name: 'Alice', age: 34, email: 'alice@example.com'});

// Multiple nodes
CREATE (:Person {name: 'Bob', age: 29}),
       (:Person {name: 'Carol', age: 41});
```

### Creating relationships

```cypher
// Create two people and connect them
CREATE (a:Person {name: 'Alice'})
CREATE (b:Person {name: 'Bob'})
CREATE (a)-[:KNOWS {since: 2019, weight: 0.9}]->(b);

// Find existing nodes and relate them
MATCH (a:Person {name: 'Alice'}), (b:Person {name: 'Carol'})
CREATE (a)-[:FRIENDS_WITH {since: 2021}]->(b);
```

### Creating indexes

```cypher
// Range / equality index on a single property
CREATE INDEX FOR (n:Person) ON (n.name);

// Composite index
CREATE INDEX FOR (n:Person) ON (n.firstName, n.lastName);

// Full-text index
CREATE FULLTEXT INDEX personSearch FOR (n:Person) ON EACH [n.name, n.bio];

// Token lookup index (auto-created for labels in modern versions)
CREATE LOOKUP INDEX FOR (n:Person) ON EACH labels(n);
```

### Creating constraints

```cypher
// Uniqueness constraint (also creates a backing index)
CREATE CONSTRAINT FOR (n:Person) REQUIRE n.id IS UNIQUE;

// Node property existence
CREATE CONSTRAINT FOR (n:Person) REQUIRE n.name IS NOT NULL;

// Relationship property existence
CREATE CONSTRAINT FOR ()-[r:KNOWS]-() REQUIRE r.since IS NOT NULL;

// Node key (combination uniquely identifies a node)
CREATE CONSTRAINT FOR (n:Person) REQUIRE (n.firstName, n.lastName) IS NODE KEY;
```

### A MATCH query example

```cypher
// Find friends-of-friends of Alice who are over 30
MATCH (alice:Person {name: 'Alice'})-[r1:KNOWS]->(friend:Person)
      -[r2:KNOWS]->(fof:Person)
WHERE fof.age > 30 AND fof <> alice
RETURN fof.name AS recommendation,
       r1.since AS friendSince,
       r2.since AS fofSince
ORDER BY fof.age DESC
LIMIT 10;
```

### Path / pattern example

```cypher
// Variable-length path: up to 4 hops of KNOWS
MATCH path = (a:Person {name:'Alice'})-[:KNOWS*1..4]->(d:Person)
RETURN path, length(path) AS distance;
```

---

## Scaling & HA

### Vertical scaling

- The primary scaling axis is **vertical**: more RAM (for page cache), faster
  disks (SSD/NVMe for WAL and store), and more CPU cores for concurrent queries.
- A single Neo4j instance can serve very large graphs (billions of nodes/relationships)
  as long as the **working set fits in the page cache**.

### Horizontal scaling — reads

- **Read Replicas** (Enterprise) provide linear horizontal read scaling by
  replicating the transaction log to additional read-only members.

### Horizontal scaling — writes

- Write scaling is limited because Core nodes must reach Raft quorum for each
  write. Adding cores improves *fault tolerance*, not raw single-write
  throughput. For very high write rates, shard at the application level or use a
  different store for ingest.

### High availability

- **Single instance (Community)** — no HA; a node failure means downtime until
  restart. Suitable for dev, small prod, or where external orchestration provides
  restart.
- **Causal Cluster (Enterprise)** — automatic failover via Raft, no data loss on
  core failure (quorum-based durability), and rolling upgrades.

### Super-node consideration

- A **super-node** (a node with millions of relationships, e.g. a "Country" node
  connected to every citizen) can become a hotspot because its relationship chain
  is long. Mitigation strategies: model intermediate nodes, use relationship
  indexes, or restructure the domain to avoid extreme degree.

### Backup & operations

- Online backups (`neo4j-admin database backup`) and restore.
- Metrics via Prometheus/JMX; logs for audit and debugging.
- Operations are typically managed with `neo4j-admin` and the system database
  (`SYSTEM`) in Neo4j 4+.

---

*End of Neo4j architecture & schema documentation.*
