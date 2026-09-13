# Deployment — Pinecone

> DBMS: **Pinecone** (fully managed vector database, SaaS, cloud-only).
> This document covers (A) using Pinecone from a local dev machine, and (B) a
> true self-hosted alternative (Qdrant) for on-prem / air-gapped needs.

---

## Important: SaaS-only (no local / self-hosted Pinecone)

Pinecone is a **proprietary managed cloud service**. There is:

- **No Docker image** for Pinecone (`pinecone/pinecone` does not exist).
- **No binary / Helm chart / on-prem build** you can run yourself.
- **No air-gapped deployment** — the data plane always runs in Pinecone's cloud
  (AWS/GCP). Enterprise plans may offer private networking/VPC peering, but the
  vectors still reside in Pinecone-operated infrastructure.

Therefore "deploying Pinecone locally" is impossible by design. The two
practical options are:

- **Option A** — *use* Pinecone's cloud service **from** your local machine
  (your code runs locally; data lives in Pinecone's cloud).
- **Option B** — run a **self-hosted open-source vector DB (Qdrant)** locally
  via Docker as a true on-prem substitute.

---

## Option A — Use Pinecone from local

### Prerequisites
- Python **3.10+**.
- A free Pinecone account and API key from <https://app.pinecone.io>.
- The `pinecone-client` SDK.

```bash
python -m venv .venv && .\.venv\Scripts\Activate.ps1   # Windows (pwsh)
# source .venv/bin/activate                            # macOS/Linux
pip install pinecone-client
```

Set your API key as an environment variable:

```bash
# Windows (pwsh)
$env:PINECONE_API_KEY = "pcsk-xxxxxxxxxxxxxxxxxxxx"
# macOS/Linux
export PINECONE_API_KEY="pcsk-xxxxxxxxxxxxxxxxxxxx"
```

> Get the key from the Pinecone console → **API Keys**. Keep it secret; do not
> commit it to git.

### Full runnable Python script

```python
# file: pinecone_demo.py
import os
from pinecone import Pinecone, ServerlessSpec

API_KEY = os.environ["PINECONE_API_KEY"]
INDEX = "local-demo"
DIM = 8  # tiny dimension for the demo

pc = Pinecone(api_key=API_KEY)

# 1. Create the index (serverless, cosine)
if INDEX not in pc.list_indexes().names():
    pc.create_index(
        name=INDEX,
        dimension=DIM,
        metric="cosine",
        spec=ServerlessSpec(cloud="aws", region="us-east-1"),
    )
    print(f"Created index '{INDEX}'")
else:
    print(f"Index '{INDEX}' already exists")

index = pc.Index(INDEX)

# 2. Upsert 3 vectors WITH metadata (scoped to a namespace)
vectors = [
    ("vec1", [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8],
     {"title": "Apple fruit",     "category": "food",    "score": 9}),
    ("vec2", [0.15, 0.25, 0.35, 0.45, 0.55, 0.65, 0.75, 0.85],
     {"title": "Banana fruit",    "category": "food",    "score": 7}),
    ("vec3", [0.9, 0.8, 0.7, 0.6, 0.1, 0.2, 0.3, 0.4],
     {"title": "Tesla car",       "category": "vehicle", "score": 8}),
]
index.upsert(vectors=[(i, v, m) for i, v, m in vectors], namespace="demo")
print("Upserted", len(vectors), "vectors")

# 3. Query: most similar to vec1, but only within category 'food'
q = [0.12, 0.22, 0.32, 0.42, 0.52, 0.62, 0.72, 0.82]
res = index.query(
    vector=q,
    top_k=2,
    namespace="demo",
    filter={"category": {"$eq": "food"}},
    include_metadata=True,
)
print("\nQuery results (top-2 food):")
for m in res["matches"]:
    print(f"  {m['id']}  score={m['score']:.4f}  meta={m['metadata']}")

# 4. (Optional) cleanup
# pc.delete_index(INDEX)
```

Run it:

```bash
python pinecone_demo.py
```

Expected output: `vec1` and `vec2` returned (both `category=food`), with
`vec1` highest; `vec3` excluded by the metadata filter.

---

## Option B — Self-hosted alternative (Qdrant)

When you need **true on-prem / air-gapped** vector search, use **Qdrant**, an
open-source vector database that you run yourself. This is the self-hosted
substitute for Pinecone — same mental model (collections, vectors + payloads,
ANN search) but runs in *your* environment.

### docker-compose.yml

```yaml
# file: docker-compose.yml
services:
  qdrant:
    image: qdrant/qdrant:latest
    container_name: qdrant
    ports:
      - "6333:6333"   # REST API
      - "6334:6334"   # gRPC API
    volumes:
      - ./qdrant_storage:/qdrant/storage
    restart: unless-stopped
```

Bring it up:

```bash
docker compose up -d
```

This exposes Qdrant on `localhost:6333` (REST) and `localhost:6334` (gRPC),
persisting data to `./qdrant_storage` on the host.

### Python client

```bash
pip install qdrant-client
```

### Full runnable Python script (mirrors the Pinecone example)

```python
# file: qdrant_demo.py
from qdrant_client import QdrantClient
from qdrant_client.models import Distance, VectorParams, PointStruct

client = QdrantClient(host="localhost", port=6333)

COLLECTION = "local-demo"
DIM = 8

# 1. Create the collection (vector size + cosine)
if not client.collection_exists(COLLECTION):
    client.create_collection(
        collection_name=COLLECTION,
        vectors_config=VectorParams(size=DIM, distance=Distance.COSINE),
    )
    print(f"Created collection '{COLLECTION}'")

# 2. Upsert points WITH payload (mirrors metadata)
points = [
    PointStruct(id=1, vector=[0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8],
                payload={"title": "Apple fruit",  "category": "food",    "score": 9}),
    PointStruct(id=2, vector=[0.15, 0.25, 0.35, 0.45, 0.55, 0.65, 0.75, 0.85],
                payload={"title": "Banana fruit", "category": "food",    "score": 7}),
    PointStruct(id=3, vector=[0.9, 0.8, 0.7, 0.6, 0.1, 0.2, 0.3, 0.4],
                payload={"title": "Tesla car",    "category": "vehicle", "score": 8}),
]
client.upsert(collection_name=COLLECTION, points=points)
print("Upserted", len(points), "points")

# 3. Search: most similar to vec1, filtered to category 'food'
q = [0.12, 0.22, 0.32, 0.42, 0.52, 0.62, 0.72, 0.82]
hits = client.search(
    collection_name=COLLECTION,
    query_vector=q,
    limit=2,
    query_filter={"must": [{"key": "category", "match": {"value": "food"}}]},
    with_payload=True,
)
print("\nSearch results (top-2 food):")
for h in hits:
    print(f"  id={h.id}  score={h.score:.4f}  payload={h.payload}")
```

Run it:

```bash
python qdrant_demo.py
```

---

## Verify

### Pinecone (Option A)
- Run `python pinecone_demo.py` and confirm it prints `Upserted 3 vectors` and
  two query results (`vec1`, `vec2`) with `category=food`.
- In the Pinecone console you should see the `local-demo` index listed.
- Confirm the API key is set: `echo $env:PINECONE_API_KEY` (pwsh) or
  `echo $PINECONE_API_KEY` (bash) returns the key.

### Qdrant (Option B)
- Check the service is up:

  ```bash
  curl http://localhost:6333/collections
  ```

  Returns JSON listing the `local-demo` collection (after running the script).
- Run `python qdrant_demo.py` and confirm it prints the two `food` hits.
- Data persists across container restarts in `./qdrant_storage`.

---

## Limitations

- **Pinecone data resides in the vendor cloud.** Option A keeps your vectors and
  metadata inside Pinecone's AWS/GCP infrastructure. It is *not* on-prem and not
  air-gapped. Use it only where data egress is permitted.
- **Qdrant is a separate OSS product, not Pinecone.** Option B is a *substitute*,
  not Pinecone itself. APIs, SDKs, indexing internals, and operational
  characteristics differ (e.g. Qdrant uses collections + payloads; Pinecone uses
  indexes + namespaces + metadata). Migrating between them requires rewriting
  ingestion/query code and re-uploading vectors.
- **No offline Pinecone.** There is no way to run Pinecone's engine locally for
  development parity; the local machine only *calls* the cloud.
- **Network required for Option A.** Pinecone calls need outbound internet
  access to `*.pinecone.io`.
- **Docker required for Option B.** Qdrant self-hosting needs a container
  runtime (Docker/Podman) and sufficient host resources for your vector volume.

---

*End of Pinecone deployment notes.*
