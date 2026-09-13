# Pinecone — Architecture & Schema

> DBMS: **Pinecone** — a fully managed vector database delivered as a cloud
> service (SaaS). There is no self-hosted or on-premise deployment of Pinecone;
> the software runs entirely inside Pinecone's cloud (multi-tenant on AWS/GCP).

---

## Overview

Pinecone is a purpose-built **vector database** designed to store, index, and
retrieve high-dimensional embeddings produced by machine-learning models. Unlike
general-purpose relational or document databases, Pinecone is optimized for
**approximate nearest-neighbor (ANN) search** over vectors, with optional
structured metadata filtering.

Key ideas:

- **Embeddings, not rows.** You store *vectors* (arrays of floats) — typically
  the output of an embedding model such as OpenAI `text-embedding-3`, Cohere
  `embed-v3`, or an open-source model like `all-MiniLM-L6-v2`.
- **Similarity search at scale.** Queries return the `k` most similar vectors
  according to a distance metric (cosine, dot product, or Euclidean).
- **Metadata filtering.** Vectors carry a JSON `metadata` object; queries can
  restrict the ANN search to subsets matching filters (e.g. `genre == "sci-fi"`).
- **Namespaces.** Each index can be partitioned into logical namespaces to
  isolate tenants or use cases without extra indexes.
- **Fully managed.** Sharding, replication, failover, upgrades, and capacity
  planning are handled by Pinecone. You interact with it only through its API
  and client SDKs.

Pinecone is most commonly used as the retrieval layer in **Retrieval-Augmented
Generation (RAG)** pipelines and semantic search systems.

---

## Managed architecture

Because Pinecone is SaaS, its internal architecture is not something you deploy;
it is the service you consume. Conceptually it is split into two logical planes.

### Control plane

The **control plane** handles the management surface:

- **API gateway / auth** — every request is authenticated with an API key
  (or OAuth token) and routed.
- **Index management** — creating, configuring, scaling, and deleting indexes;
  choosing dimension, metric, and pod/serverless type.
- **Billing, quotas, and observability** — usage metering and metrics.

You never touch the control plane's internals; you call REST/gRPC endpoints such
as `POST /indexes` (create index) or `GET /indexes` (list indexes).

### Data plane

The **data plane** is where vectors live and where search happens:

- An **index** is the unit of deployment. Internally an index is a sharded
  *deployment* (a set of pods or, in serverless, a disaggregated
  storage/compute split).
- Each shard holds a partition of the vectors and builds an **HNSW (Hierarchical
  Navigable Small World) graph** — a proximity graph that enables fast ANN
  traversal.
- A query vector is routed to the relevant shards; each shard runs ANN search
  over its local graph, optionally applying **metadata filtering**, and returns
  its local top-k. Results are merged and re-ranked before the global top-k is
  returned to the client.
- **Namespaces** are virtual partitions *within* an index — they do not add
  shards but scope the keyspace so the same vector IDs can be reused across
  namespaces.

### Pod-based vs. Serverless

| Aspect | Pod-based (classic) | Serverless (Starter/Standard) |
|---|---|---|
| Capacity model | You pick pod type/size and replica count | Capacity auto-scales with usage |
| Scaling | Manual (change replicas/pods) | Automatic, elastic |
| Billing | Provisioned (pay for pods) | Usage-based (pay for reads/writes/storage) |
| Storage/compute | Co-located in pods | Separated (object storage + stateless compute) |
| Best for | Predictable, steady workloads | Bursty, unpredictable, dev/test |

Pod types include `p1` (starter), `p2` (improved performance), `s1`
(storage-optimized), and `x1` (performance-optimized). Each pod holds a fixed
amount of vector capacity; adding **replicas** increases read throughput and
availability, while **additional pods** increase total vector capacity.

### Sharding, replication, HA

- **Sharding** splits an index across pods so a large index fits and parallelizes
  search.
- **Replication** (pod-based) creates read replicas of each shard for higher
  availability and throughput.
- **High availability and durability** are provided by the vendor: Pinecone
  replicates data and runs failover inside its cloud. You do not operate backup
  jobs — though you should still export critical data periodically for your own
  compliance needs.

---

## Why no self-host

Pinecone is a **closed, proprietary, managed cloud service**. There is:

- No Docker image (`pinecone/pinecone` does not exist).
- No open-source distribution or Helm chart.
- No "run it on my own hardware / air-gapped VPC" option (aside from limited
  private-network/VPC-peering arrangements on enterprise plans, the data plane
  still runs in Pinecone's cloud).

This is a deliberate product decision: Pinecone sells *operational simplicity*
rather than software you run yourself. The trade-off is that your vectors —
which may contain sensitive or regulated data — reside in Pinecone's multi-tenant
cloud. If you require on-prem or air-gapped vectors, use a self-hosted alternative
such as **Qdrant**, **Weaviate**, **Milvus**, or **FAISS** (see `deployment.md`,
Option B).

---

## Write path

1. **Client (app / SDK / LangChain)** builds an embedding for the document and
   calls `upsert` with `(id, values, metadata, optional sparse_values)`,
   optionally scoped to a `namespace`.
2. Request hits the **API gateway**, is authenticated, and routed to the index's
   **control/data routing layer**.
3. The vector is assigned to the correct **shard** (by hash of the ID, or
   namespace routing).
4. The shard inserts the vector into its **HNSW graph** and writes the vector +
   metadata to **durable storage** (replicated within the cloud).
5. On success, an acknowledgement (`upserted_count`) is returned to the client.

```
app → embedding model → upsert(id, values, metadata)
     → API gateway / auth
     → index router → shard (HNSW insert) → durable storage (replicated)
```

---

## Read path (similarity search + filtering)

1. **Client** creates a query embedding and calls `query(vector, top_k,
   filter, namespace, include_metadata)`.
2. Request is authenticated and routed to the index.
3. The router fans the query out to the relevant **shards** in parallel.
4. Each shard runs **ANN traversal over its HNSW graph**, applying the
   **metadata filter** to prune candidates, returning its local top-k.
5. Results are merged, globally re-ranked by score, and the final **top-k** is
   returned (with metadata if requested).

```
query(vector, filter) → API gateway
     → index router → [shard₁ ANN, shard₂ ANN, …] (metadata filter applied)
     → merge + rerank → top-k results
```

---

## Data / schema model

Pinecone has a deliberately small schema. There are no tables or collections —
only **indexes**, **namespaces**, and **vectors**.

### Index (top-level object)

| Field | Description |
|---|---|
| `name` | Unique index name (DNS-safe string). |
| `dimension` | Fixed vector length, e.g. `1536` (OpenAI) or `384` (MiniLM). |
| `metric` | `cosine` (default), `dotproduct`, or `euclidean`. |
| `pod_type` / `serverless` | Capacity model selection. |
| `metadata_config` | Optional index-level metadata field type hints. |

All vectors in one index must share the same `dimension` and `metric`.

### Vector (stored record)

| Field | Type | Notes |
|---|---|---|
| `id` | string | Unique within a namespace. |
| `values` | `float[]` | The dense embedding; length == index dimension. |
| `metadata` | `object` | JSON of filterable fields (string/number/bool/arrays). |
| `sparse_values` | `{indices[], values[]}` | Optional lexical/hybrid component. |

### Namespace

A namespace is a logical partition inside an index. Vector IDs are unique per
namespace, so the same ID can exist in `ns-a` and `ns-b`. Namespaces are free
and do not add shards.

### Concrete Python SDK example

```python
# pip install pinecone-client
import os
from pinecone import Pinecone, ServerlessSpec

# 1. Initialize the client with your API key
pc = Pinecone(api_key=os.environ["PINECONE_API_KEY"])

INDEX_NAME = "research-docs"

# 2. Create the index (serverless, cosine, 1536-dim)
if INDEX_NAME not in pc.list_indexes().names():
    pc.create_index(
        name=INDEX_NAME,
        dimension=1536,
        metric="cosine",
        spec=ServerlessSpec(cloud="aws", region="us-east-1"),
    )

index = pc.Index(INDEX_NAME)

# 3. Upsert 3 vectors WITH metadata (use namespace to isolate)
docs = [
    ("doc1", [0.1] * 1536, {"title": "Pinecone intro",   "genre": "tech", "year": 2023}),
    ("doc2", [0.2] * 1536, {"title": "Vector search 101","genre": "tech", "year": 2022}),
    ("doc3", [0.3] * 1536, {"title": "Sci-fi stories",   "genre": "fiction", "year": 2021}),
]
index.upsert(
    vectors=[(id_, vec, meta) for id_, vec, meta in docs],
    namespace="demo",
)

# 4. Query: top-2 tech docs most similar to doc1
res = index.query(
    vector=[0.1] * 1536,
    top_k=2,
    namespace="demo",
    filter={"genre": {"$eq": "tech"}},
    include_metadata=True,
)
for m in res["matches"]:
    print(m["id"], round(m["score"], 4), m["metadata"])

# 5. Cleanup (optional)
# pc.delete_index(INDEX_NAME)
```

---

## Scaling

- **Pod-based:** add pods to grow capacity; add replicas to grow read
  throughput/HA. Sizes `p1/s1` (dev), `p2/s1`, `s1` (storage), `x1` (perf).
- **Serverless:** scales automatically with write/read/storage; no capacity
  planning. Best for spiky or unknown workloads.
- **Dimensions & shards:** total vector count is bounded by `pods × capacity
  per pod`; serverless removes this explicit ceiling.
- **Throughput:** writes and queries are parallelized across shards/replicas.
  Rate limits depend on plan tier.

---

## Consistency & limits

- **Consistency:** Pinecone provides **eventual consistency** for reads after
  writes in serverless, and near-real-time visibility in pod-based indexes.
  Upserts are acknowledged only after durable persistence; immediate
  strongly-consistent reads are not guaranteed across all replicas.
- **Index limits:** one dimension/metric per index; dimension fixed at creation.
- **Metadata:** filterable fields are indexed; very high-cardinality or deeply
  nested metadata can affect performance.
- **Vector size:** practical limits depend on plan (dimension and count quotas).
- **IDs:** string IDs, max length ~512 chars; must be unique per namespace.
- **Quotas:** requests-per-second, indexes-per-account, and total vectors are
  plan-limited (Free, Starter, Standard, Enterprise).
- **No transactions / joins:** Pinecone is not a general-purpose database — it
  stores vectors + metadata only, with no multi-document ACID transactions.

---

*End of Pinecone architecture & schema notes.*
