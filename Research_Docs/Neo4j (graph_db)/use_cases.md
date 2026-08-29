# Use Cases — Neo4j

> DBMS: **Neo4j** — Native graph database (Cypher)
> Research documentation for the Database Project.

---

## Ideal

Neo4j shines when the **relationships are the data**. If your questions are of the
form "what is connected to what, how, and how far," a native graph database is
almost always the right tool.

### 1. Social networks

Modeling people, their friendships, follows, groups, and interactions is the
canonical graph problem. Friend-of-friend recommendations, influence analysis,
and community detection are natural fits.

```cypher
// Mutual friends between two users
MATCH (a:User {id:1})-[:FRIENDS_WITH]-(mutual:User)-[:FRIENDS_WITH]-(b:User {id:2})
RETURN mutual.name, count(*) AS shared
```

### 2. Fraud detection

Fraud rarely happens in isolation — it lives in **rings** and **collusion
networks**. Detecting shared identifiers, circular money flows, and anomalous
connectivity is exactly what graph traversal does well. (Example scenario below.)

### 3. Knowledge graphs

Representing entities and the relationships between them (people, places,
concepts, products) enables semantic search, question answering, and
explainability. Neo4j is widely used to build and query enterprise knowledge
graphs.

```cypher
MATCH (d:Disease)-[:CAUSED_BY]->(p:Pathogen)-[:TREATED_BY]->(m:Medication)
RETURN d.name, p.name, collect(m.name)
```

### 4. Recommendation engines

"Users who bought this also bought that," content similarity via shared
attributes, and graph-based collaborative filtering all map to traversals over
purchase/interaction graphs.

```cypher
// Products liked by people who liked the same product as you
MATCH (me:User {id:42})-[:LIKED]->(:Product)<-[:LIKED]-(peer:User)
      -[:LIKED]->(rec:Product)
WHERE NOT (me)-[:LIKED]->(rec)
RETURN rec.name, count(*) AS score
ORDER BY score DESC LIMIT 10
```

### 5. Network / IT operations (ITOps)

Dependency graphs, service meshes, infrastructure topology, and root-cause
analysis. "Which services depend on this failing host?" is a traversal, not a
join.

```cypher
MATCH (host:Host {name:'web-01'})<-[:RUNS_ON]-(svc:Service)
      <-[:DEPENDS_ON*]-(impacted:Service)
RETURN impacted.name
```

### 6. Authorization / identity graphs

Fine-grained access control expressed as a graph: users belong to groups, groups
have roles, roles grant permissions on resources. Path queries answer "does user
X have access to resource Y?" including indirect membership.

```cypher
MATCH (u:User {id:7})-[:MEMBER_OF*0..]->(:Group)-[:HAS_ROLE]->(:Role)
      -[:CAN]->(perm:Permission {resource:'invoice', action:'read'})
RETURN count(*) > 0 AS allowed
```

---

## When NOT to use

Neo4j is the wrong tool for workloads where connectivity provides no value.

### 1. Flat time-series data

High-frequency metrics, logs, and sensor readings are append-only, wide, and
aggregated by time windows. A time-series database (InfluxDB, TimescaleDB) or
column store handles this far better than a graph.

```text
Better:  INSERT timestamp, metric, value  → TSDB / column store
Worse:   (metric)-[:AT]->(timestamp)-[:HAS]->(value)  → graph overhead
```

### 2. Heavy aggregation / reporting (OLAP)

Month-over-month rollups, giant GROUP BYs, and cross-table aggregates over
billions of rows are the domain of warehouses and column stores, not a graph DB.

### 3. Simple key-value

If you only ever do `get(key)` / `put(key, value)` with no relationships, use a
key-value store (Redis, DynamoDB) which is simpler and faster for that narrow
pattern.

### 4. Large binary blobs

Storing images, videos, and large documents *inside* Neo4j properties is
discouraged. Keep blobs in object storage (S3) and store only references/metadata
as node properties.

### 5. Write-only high-ingest streams

Massive firehose ingest (billions of events/day) without relationship queries is
better served by log/streaming stores (Kafka, Cassandra, column stores).

---

## Example scenario: fraud ring detection

### Domain

A bank wants to detect **synthetic identity fraud** where multiple accounts share
contact information (phone/address/device) and move money in circular patterns to
appear legitimate.

### Model

```cypher
(:Account {id, opened, balance})
(:Person  {id, name, ssn})
(:Phone   {number})
(:Device  {deviceId})
(:Address {line, city})

(:Account)-[:OWNED_BY]->(:Person)
(:Person)-[:USES_PHONE]->(:Phone)
(:Person)-[:USES_DEVICE]->(:Device)
(:Person)-[:LIVES_AT]->(:Address)
(:Account)-[:TRANSFERRED {amount, at}]->(:Account)
```

### Find shared identifiers (collusion signal)

```cypher
// Accounts sharing a phone, device, or address with another account
MATCH (a:Account)-[:OWNED_BY]->(:Person)-[:USES_PHONE]->(:Phone)
      <-[:USES_PHONE]-(:Person)-[:OWNED_BY]->(b:Account)
WHERE a <> b
RETURN a.id AS acctA, b.id AS acctB, 'shared_phone' AS signal
```

### Find circular money flows

```cypher
// Money that returns to its origin within 3 hops (layering / laundering)
MATCH path = (a:Account)-[:TRANSFERRED*2..3]->(a)
RETURN a.id AS ringLeader,
       [n IN nodes(path) | n.id] AS accountsInRing,
       reduce(s = 0, r IN relationships(path) | s + r.amount) AS totalMoved
```

### Score a suspect cluster

```cypher
// Accounts connected by shared identifiers AND in a transfer loop
MATCH (seed:Account {id:'ACC-1001'})
MATCH (seed)-[:OWNED_BY]->(:Person)-[:USES_PHONE|USES_DEVICE|LIVES_AT]->
      (:Phone|Device|Address)<-[:USES_PHONE|USES_DEVICE|LIVES_AT]-
      (:Person)-[:OWNED_BY]->(neighbor:Account)
MATCH (neighbor)-[:TRANSFERRED*1..3]->(seed)
RETURN neighbor.id, count(*) AS riskScore
ORDER BY riskScore DESC
```

### Why the graph model wins here

- The **same shared contact** across accounts is a multi-hop pattern that would
  require many self-joins in SQL and degrade badly as data grows.
- **Circular transfers** are a cycle-detection problem — trivial in Cypher with
  variable-length paths, painful in SQL.
- Investigators can **visualize** the ring in Neo4j Browser and explore
  interactively, pivoting on any node.

---

*End of Neo4j use cases documentation.*
