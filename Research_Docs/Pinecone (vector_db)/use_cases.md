# Use Cases — Pinecone

> DBMS: **Pinecone** (managed vector database, SaaS). This document describes
> where Pinecone excels, where it should be avoided, and a worked RAG scenario.

---

## Ideal use cases

### 1. Semantic / natural-language search
Replace keyword search with meaning-based search. Embed documents or queries
with an embedding model, store the vectors in Pinecone, and retrieve passages
that are *semantically* similar even when they share no keywords.

- Support tickets → find similar past resolutions.
- Knowledge base → "how do I reset MFA?" returns the right article.
- E-commerce → search by description rather than exact SKU terms.

### 2. Retrieval-Augmented Generation (RAG) for LLMs
Pinecone is the canonical retrieval store behind RAG chatbots. At query time you
embed the user question, pull the top-k relevant chunks, and inject them into the
LLM prompt as grounding context. This reduces hallucination and keeps answers
fresh without retraining the model.

### 3. Recommendations by embedding
Represent users and items as vectors (from collaborative filtering, two-tower
models, or content features). "Users like this" → query the item's vector for
nearest neighbors. Pinecone serves real-time neighbor lookups for
personalization feeds, "you may also like", and similar-product carousels.

### 4. Image / multimodal similarity search
Embed images (CLIP, ResNet embeddings) or audio into vectors and find visually
or acoustically similar items — used in duplicate detection, content
moderation, stock-photo search, and IP protection.

### 5. Deduplication & near-duplicate detection
Store document/record embeddings; query each new record against the index to find
near-duplicates above a similarity threshold. Common in crawling, plagiarism
detection, and record linkage.

### 6. Anomaly detection on vectors
Points far from any cluster centroid (low max-similarity to neighbors) are
outliers. Useful for fraud, intrusion, and quality monitoring where "normal"
behavior is densely clustered in embedding space.

### 7. Question answering over private docs
Combine RAG with metadata filtering: restrict retrieval to a department, date
range, or document classification while still ranking by semantic similarity.

### 8. Hybrid search (lexical + semantic)
Use `sparse_values` together with dense vectors so exact-match keywords (product
codes, names) and semantic meaning both contribute to ranking.

---

## When NOT to use Pinecone

### 1. Relational / transactional data
If your workload is orders, invoices, and joins with ACID guarantees, use
PostgreSQL/MySQL/Spanner. Pinecone has no transactions, no SQL, no joins.

### 2. Data must stay on-prem / air-gapped
Pinecone is SaaS-only; vectors leave your perimeter. Regulated or classified
environments that forbid external data transfer cannot use it. Use Qdrant /
Weaviate / Milvus self-hosted instead (see `deployment.md`, Option B).

### 3. Tiny datasets where a local library suffices
If you have <100k vectors and run on a single machine, **FAISS**, **Annoy**, or
**scikit-learn** `NearestNeighbors` are free, in-process, and simpler. Pinecone's
managed value (scaling, HA) is unnecessary overhead at that scale.

### 4. Structured analytics / OLAP
Aggregations, group-bys, and BI over tabular data belong in a warehouse
(Snowflake/BigQuery/DuckDB). Pinecone answers "nearest neighbors", not "SUM by
region".

### 5. Strong consistency required
If every read must immediately reflect the latest write (e.g. financial ledger),
Pinecone's eventual/near-real-time consistency is unsuitable.

### 6. Cost-sensitive, steady, high-volume workloads
At very large scale with predictable traffic, self-hosted OSS on your own
hardware is typically far cheaper than a managed per-operation bill.

---

## Example scenario — RAG chatbot

**Goal:** Answer employee questions using an internal HR policy wiki, grounded in
the most relevant passages.

### Step A — Ingest (build the index)

```python
# pip install pinecone-client openai
import os, pinecone, openai

pc = pinecone.Pinecone(api_key=os.environ["PINECONE_API_KEY"])
INDEX = "hr-policies"
if INDEX not in pc.list_indexes().names():
    pc.create_index(INDEX, dimension=1536, metric="cosine",
                    spec=pinecone.ServerlessSpec(cloud="aws", region="us-east-1"))
idx = pc.Index(INDEX)

def embed(text: str) -> list[float]:
    return openai.Embedding.create(model="text-embedding-3-small",
                                   input=text)["data"][0]["embedding"]

# chunks = [(id, text, {"source":..., "dept":...})]
for cid, text, meta in chunks:
    idx.upsert([(cid, embed(text), meta)])
```

### Step B — Retrieve (query at ask time)

```python
question = "How many vacation days do I get after 3 years?"
qvec = embed(question)
res = idx.query(vector=qvec, top_k=5,
                filter={"dept": {"$in": ["HR", "General"]}},
                include_metadata=True)
context = "\n\n".join(m["metadata"]["text"] for m in res["matches"])
```

### Step C — Generate (feed LLM)

```python
prompt = f"Context:\n{context}\n\nQuestion: {question}\nAnswer:"
answer = openai.ChatCompletion.create(
    model="gpt-4o-mini",
    messages=[{"role": "user", "content": prompt}])["choices"][0]["message"]["content"]
print(answer)
```

The chatbot returns a grounded answer whose sources are the top-k HR passages —
the classic Pinecone + LLM RAG loop.

---

## Decision checklist

Use Pinecone if **all** of these are true:
- Your data can legally/operationally live in a vendor cloud.
- You need low-latency similarity search without running infrastructure.
- Your workload is embeddings/ANN-dominated (search, RAG, recs).
- Speed-to-market and managed HA beat cost/control concerns.

Otherwise consider a self-hosted vector DB or an in-process library.

---

*End of Pinecone use cases.*
