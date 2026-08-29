# De-Sentry — Architecture

**A true peer-to-peer, decentralized DBMS engine. No client-server model: every node is both a full database server and a client of every other node.**

Status: MVP design + reference implementation. Course project scaffold that is built the way a startup would build v0.1 of a real product.

> **Related doc**: [`docs/COMPARISON.md`](COMPARISON.md) compares this design against a second, independently-developed De-Sentry design produced in parallel (ownership-partitioned nodes + a routing coordinator), and records which of its ideas were adopted here (a hash-chained audit ledger, a brain-file status endpoint, a Python client) versus deliberately not adopted, and why.

---

## 1. Goals and non-goals

### Goals (MVP)
1. **No client-server split.** Every process running De-Sentry (a "peer" or "node") is a complete database engine with its own durable local storage, and simultaneously a full member of the P2P replication mesh. There is no coordinator, no primary, no metadata service.
2. **Works offline-first.** A peer can be fully partitioned from the network and still read and write locally. When connectivity returns, it reconciles automatically with no manual conflict resolution.
3. **One data model, two shapes.** The same document store handles rigid, schema-validated "structured" records (rows) and free-form nested JSON ("unstructured" documents) — because at the storage layer they are the same thing: a set of typed, timestamped fields.
4. **Real durability.** Every write survives a crash: write-ahead logging + redo recovery, exactly like InnoDB/Postgres heap storage, not an in-memory toy.
5. **Deterministic, mathematically-grounded convergence.** Any two peers that have seen the same set of writes end up in the *same* state, regardless of the order they received them in — this is the CRDT guarantee, not a "usually works" heuristic.
6. **Authenticated, encrypted mesh.** Every peer has a cryptographic identity. Every replicated write is signed by its origin and verified by every recipient. Every wire connection is encrypted.
7. **Simplest thing that is still real.** No consensus protocol, no leader election, no sharding coordinator in the MVP — those are the "how we scale it" section (§9), not blockers to shipping.

### Non-goals (MVP)
- Byzantine fault tolerance / adversarial peers (BigchainDB-style voting is a v2 concern, see §9.3).
- Cross-shard distributed transactions or SQL joins across peers.
- Automatic sharding / partitioning of a collection across peers (v1 replicates full collections; v2 shards).
- A query planner/optimizer beyond point lookups, range scans and simple predicate filters.

---

## 2. Why this shape (prior art and the decision)

| System | Model | Consistency | Storage | Takeaway we borrow |
|---|---|---|---|---|
| GunDB | P2P graph DB | CRDT (LWW graph) | in-memory + adapter | Field-level LWW is simple and composes well for JSON-shaped data |
| OrbitDB | P2P over IPFS | CRDT (op-log / OR-Set) | LevelDB | Append-only per-peer op log + Merkle-linked sync is a clean anti-entropy primitive |
| BigchainDB | Federated, Tendermint BFT | Consensus (BFT) | MongoDB | Strong consistency needs voting and a bounded, known validator set — too heavy for an open P2P MVP |
| Bluzelle | Sharded, Raft per shard | Consensus (Raft) | LevelDB/custom | Per-shard consensus is a good v2 scaling path once we need linearizable writes on a subset of data |

**Decision: CRDT + Hybrid Logical Clocks (HLC) as the MVP consistency model**, in the spirit of GunDB/OrbitDB rather than BigchainDB/Bluzelle. Rationale:
- It is the only model that keeps **every peer independently writable with no coordination**, which is the actual product requirement ("no client-server").
- It has a formally proven convergence property (state-based CRDTs form a join-semilattice; merge is commutative, associative, idempotent) — this is what "bulletproof" means for a distributed data structure, not a marketing word.
- Consensus (Raft/BFT) is strictly a superset of complexity we can layer in later per-collection (§9.3) once the core engine is proven.

**Decision: build the storage engine from scratch in C/C++.** The team explicitly wants the InnoDB/Postgres-heap engineering experience: a disk manager, a buffer pool with a real eviction policy, a write-ahead log with crash recovery, and a B+Tree index, all hand-built. This is the "novel/academic" core of the project; the P2P and CRDT layers are the "product" layer on top of it.

---

## 3. Node anatomy

Every peer is the *same binary* (`desentryd`). There is no special "seed node" binary, though a peer can be configured with a list of known bootstrap addresses to make joining a network easier (this is a discovery convenience, not a privileged role).

```
                    ┌─────────────────────────────────────────────────────────┐
                    │                        desentryd (one peer)              │
                    │                                                          │
   local apps       │   ┌────────────┐        ┌────────────────────────────┐  │
   (curl, your   ───┼──▶│  API Layer │        │        Engine Core         │  │
   app's HTTP        │   │ HTTP/1.1   │◀──────▶│  (storage + CRDT + catalog)│  │
   client)           │   │ REST + WS  │        └──────────────┬─────────────┘  │
                    │   └────────────┘                       │                │
                    │                                        ▼                │
                    │                          ┌──────────────────────────┐   │
                    │                          │      Storage Engine       │   │
                    │                          │ DiskMgr / BufferPool /    │   │
                    │                          │ WAL / B+Tree / Catalog    │   │
                    │                          │      (local *.dsf file)   │   │
                    │                          └──────────────────────────┘   │
                    │                                        ▲                │
                    │   ┌────────────────────────────────────┘                │
                    │   │                                                     │
                    │   ▼                                                     │
                    │  ┌────────────────────────────────────────────────┐     │
                    │  │                Network Layer                    │     │
                    │  │  Identity (Ed25519+X25519) · Secure channel      │     │
                    │  │  TCP wire protocol · UDP LAN discovery          │     │
                    │  │  Gossip / anti-entropy replicator               │     │
                    │  └────────────────────────────────────────────────┘     │
                    └───────────────────────────┬──────────────────────────────┘
                                                  │  TCP (encrypted, signed frames)
                                                  ▼
                                      other peers, symmetric, same shape
```

Each peer thus plays **both roles simultaneously**:
- **Server**: accepts inbound TCP connections from other peers (replication) and inbound HTTP connections from local applications (API).
- **Client**: dials out to other peers to gossip, and is itself the "database client" for whatever application is colocated with it.

This is the concrete meaning of "no client-server model": the client/server *roles* still exist (someone has to accept a connection and someone has to initiate one), but every peer holds both roles for the P2P mesh, and the notion of a distinguished "the server" that owns the data does not exist — data lives on every replica that subscribed to it.

### 3.1 Applications talk to their *local* peer only
An application never connects directly to a remote peer's data. It talks to the HTTP API of the peer running on its own machine/process (`localhost:7701` by default). That local peer is responsible for propagating the write into the mesh. This mirrors how GunDB and OrbitDB are actually consumed (an embedded/local node, not a remote RPC to someone else's server) and keeps the trust boundary simple: your peer is the only one your application must trust with plaintext.

---

## 4. Data model

### 4.1 Unifying structured and unstructured data
Everything stored in De-Sentry is a **Document**: a UTF-8 key plus a set of named fields. A field's value is one of: `null, bool, int64, double, string, bytes, array, object`. There is no separate "table" storage engine and "document" storage engine — a "structured" collection is simply a document collection with an attached **JSON-Schema-like validator** that the API layer enforces on write; an "unstructured" collection has no validator. Both are stored, indexed, replicated and merged identically. This is the single most important simplification in the whole design — it is why one storage/CRDT engine can serve both of the dataset shapes the team asked for.

```
Collection "users"           (structured: schema attached)
  { _key: "u1", name: "Asha", age: 21, tags: ["admin"] }

Collection "sensor_events"   (unstructured: no schema)
  { _key: "e1", ts: 1699999, payload: { any: "shape", nested: [1,2,3] } }
```

### 4.2 Every field carries a Hybrid Logical Clock timestamp
On disk (and on the wire) a document is not just `{key: value}` — it is `{key: (value, HLC, origin_node_id, tombstone)}` per field, recursively for nested objects. This per-field metadata is what makes merging two divergent copies of a document deterministic (§6).

### 4.3 Physical storage
- The whole peer's data lives in one paged file, default `data/desentry.dsf`, cut into fixed 4 KiB pages — same page size discipline as Postgres/InnoDB defaults, for balance between disk IOP efficiency and memory granularity.
- Documents are encoded with a compact binary codec (`docs/CODEC.md` in the header comments of `document_codec.h`) and stored in **slotted pages**: each page has a header + a slot directory growing from the front and record bytes growing from the back, so variable-length documents (an unstructured JSON blob and a tiny structured row live side by side).
- A **B+Tree** per collection maps primary key → `RID` (page id, slot id) for O(log n) point lookups and ordered range scans.
- A **system catalog** (page 0 of the file, plus overflow pages) records: collection names, their B+Tree root page ids, attached JSON-Schema (if any), and replication scope.
- A **Write-Ahead Log** (`data/desentry.wal`) is appended before any page mutation is applied to the buffer pool, with per-record LSNs and CRC32 checksums; on startup the engine replays the WAL to redo any operations that were durable but not yet checkpointed to the data file. This is the same physical-logging discipline every production RDBMS uses to guarantee the "D" in ACID for a single node.

---

## 5. Storage engine internals

```
Application write
        │
        ▼
 ┌──────────────┐   append   ┌────────────┐
 │   WAL Writer  │──────────▶│  desentry  │   fsync'd before ack
 └──────┬───────┘            │    .wal    │
        │ apply                └────────────┘
        ▼
 ┌────────────────────┐   evict (LRU)   ┌───────────────┐
 │  Buffer Pool Mgr    │────────────────▶│  desentry.dsf │
 │  (pages in RAM,     │◀────────────────│  (on disk)    │
 │   pin/dirty bits)   │   fetch on miss  └───────────────┘
 └─────────┬───────────┘
           │
   ┌───────┴────────┐
   ▼                ▼
 B+Tree index   Slotted page
 (per collection) record storage
```

- **DiskManager**: raw `pread`/`pwrite` on the paged file, tracks the next free page id, does the actual `fsync`.
- **BufferPoolManager**: fixed-size pool of in-memory page frames, an **LRU replacer** picks a victim frame on a miss (documented as the first thing to upgrade to LRU-K/clock when we have real access traces — see §9), pin counts prevent evicting a page a caller is actively using, dirty pages are flushed back through the DiskManager before reuse.
- **WAL + recovery**: log record = `{lsn, type, page_id, collection_id, before/after image or logical op, crc32}`. On boot: read the last checkpoint LSN, replay every log record after it (redo-only recovery — simple and correct for the MVP; undo/rollback for multi-statement transactions is a v2 item, §9).
- **B+Tree**: classic on-disk B+Tree, order chosen so a full node fits in one page; leaf nodes are singly-linked for fast range scans; keys are the document's `_key` string (or a composite for secondary indexes later).
- **Catalog**: collection metadata bootstraps a B+Tree the same way any other collection does — the catalog is "just another collection" at page 0, which keeps the bootstrap code path tiny.

---

## 6. Consistency model: CRDT + HLC

### 6.1 Hybrid Logical Clock
Every peer keeps an HLC: `(physical_ms, logical_counter, node_id)`. HLCs give every write a timestamp that (a) is close to wall-clock time for humans debugging the system, (b) is strictly monotonic per node, and (c) captures causality — if peer A saw a message from peer B and then wrote, A's next HLC is guaranteed greater than B's, even under clock skew. This is the same primitive CockroachDB uses for its MVCC timestamps.

### 6.2 Per-field Last-Writer-Wins with tombstones
For scalar fields (and whole documents on delete) we use **LWW-Register** semantics: when two peers have different values for the same field, the value with the higher `(HLC, node_id)` tuple wins; `node_id` is the tiebreaker for the vanishingly rare exact-timestamp collision. Deletes are tombstones (`{tombstone: true, HLC}`) rather than physical removal, so a delete can correctly "beat" a concurrent update and vice versa — physically removing rows on delete is what makes naive replicated deletes non-convergent, and is a classic distributed-systems bug we design out from day one.

### 6.3 Arrays and sets: OR-Set
Where a field is semantically a *set* (e.g. `tags`), a plain LWW-Register would let a concurrent add and remove on two peers "flicker" depending on merge order. We use an **Observed-Remove Set**: each element carries a unique add-tag `(node_id, HLC)`; removing an element removes only the tags that peer has *observed*, so a concurrent add on another peer always survives a remove issued before the add was seen. This is the standard construction from Shapiro et al.'s CRDT paper and is what GunDB and Automerge both build on for collection-like fields.

### 6.4 Document merge = recursive field-level CRDT merge
Merging two versions of a document is: for every field present in either version, apply LWW-Register (scalars) or OR-Set (arrays) or recurse (nested objects). This is proven to be a valid CRDT itself (a product of CRDTs is a CRDT), so whole-document merge inherits the same commutative/associative/idempotent guarantees — **any peer can merge any two states in any order and land on the same result.** That is the formal meaning of "eventual consistency" here, not a hand-wave.

### 6.5 What this buys, and what it costs
- **Buys**: always-writable, partition-tolerant, no coordinator, no leader election, no downtime for a minority partition.
- **Costs**: no cross-field atomicity (two fields updated "together" can be observed independently mid-merge by a third peer), no global ordering/no double-spend-style guarantees. We document this trade-off explicitly rather than hide it — a collection that genuinely needs strong consistency (e.g. a ledger balance) is exactly the case for the v2 per-collection consensus mode in §9.3.

---

## 7. Network layer

### 7.1 Identity
On first boot a peer generates two Ed25519/X25519 keypairs (via OpenSSL EVP, no third-party crypto library): a **signing** key (Ed25519) and a **key-exchange** key (X25519). `node_id = hex(SHA-256(signing_pubkey))[:32]`. Keys persist in `data/identity.key` (0600 permissions). Node identity is how every other peer recognizes "the same peer" across reconnects, and is the actor recorded in every HLC and every CRDT tag.

### 7.2 Secure channel (peer-to-peer)
Because we are not terminating public HTTPS, we hand-roll a minimal authenticated-encryption handshake instead of embedding a full TLS stack, using only OpenSSL primitives:
1. **HELLO**: each side sends `(node_id, x25519_pubkey, ed25519_signature_over_pubkey)`.
2. Each side verifies the signature (proves the peer controls the signing key claimed by `node_id`), then computes a shared secret via **X25519 ECDH**.
3. Both derive a session key via **HKDF-SHA256** over the shared secret.
4. All subsequent wire messages are sealed with **AES-256-GCM** (authenticated encryption — confidentiality *and* integrity of the transport in one primitive), with a monotonic nonce counter per direction.

This gives every peer link forward-privacy-lite (ephemeral X25519 per connection), transport confidentiality, and mutual authentication of node identity — the actual security properties that matter for the MVP threat model (§8), without vendoring an X.509/TLS implementation.

### 7.3 Application-layer signing
Independently of the transport, **every replicated write is signed by its origin node's Ed25519 key** and the signature travels *with the data*, not just the connection. This matters because gossip is multi-hop: peer C may receive a document that originated at peer A via peer B. Transport encryption only proves "B sent me this," origin signing proves "A actually wrote this, unmodified" — the same reason BigchainDB and blockchain-style systems sign the payload, not just the pipe. Signatures are verified before a merge is ever applied to local storage.

### 7.4 Discovery
MVP scope is a single LAN/host demo cluster (per the team's own answer on deployment scope), so discovery is:
- **UDP broadcast** ("who's out there?" / "I'm `node_id` at `ip:port`") on a well-known port, peers maintain a live membership table with liveness timestamps.
- **Static bootstrap list** in config, for the (future) WAN case where broadcast doesn't reach — this is the same fallback IPFS/libp2p use.

### 7.5 Replication: gossip anti-entropy
Each peer periodically (default 2s) picks a random known peer and exchanges a **digest**: per collection, a version-vector summarizing "the highest HLC I've seen from each node_id." Comparing digests tells both sides exactly which documents are missing or stale on which side, and only those documents (deltas) are transferred — not a full dump. New local writes are additionally **eagerly broadcast** to connected peers for low-latency propagation; gossip anti-entropy is the convergence backstop that guarantees eventual consistency even if an eager broadcast is dropped, a peer was offline, or the network partitions and heals.

### 7.6 Hash-chained audit ledger and brain-file status
Independent of the WAL's durability/recovery role (§5), every WAL record also carries a SHA-256 `entry_hash` chained from the previous record's hash (`prev_hash`; the genesis record chains from 32 zero bytes) — the same Git-commit-style construction a blockchain's block-hash linkage uses, without the distributed-consensus part, because this ledger has exactly one writer: the local node. Altering, reordering, or deleting a past entry breaks the chain from that point forward, detectable by a full independent re-verification (`WriteAheadLog::VerifyChain()`), not a cached "trust the tip" shortcut. This turns the durability log into a tamper-evident audit trail of every mutation a node has ever made, for free, on the same storage substrate.

A hash chain alone only proves *internal self-consistency* — nothing stops a party from fabricating an entirely different but equally self-consistent chain from scratch. So the current chain tip is additionally signed with the node's persistent Ed25519 identity key (`NodeEngine::SignLedgerTip()`, the same key from §7.1) before being exposed. A peer that already trusts a given `node_id` can verify the signature and know that *this specific node* attests to *this exact* ledger state — origin authenticity for the ledger, composing with the application-layer write-signing in §7.3 rather than duplicating it.

Exposed over the local REST API (see `README.md`'s endpoint table):
- `GET /_ledger/tip` — current `entry_id`, `entry_hash`, and the Ed25519 `signature` over them.
- `GET /_ledger/entries?from=&to=` — a bounded page of ledger entries for replay/audit (capped per call; a full replay pages through it).
- `POST /_ledger/verify` — triggers a full chain re-verification and reports the result, including the first broken `entry_id` if the chain is not intact.
- `GET /_brain` — a compact, single-call, human- or agent-readable snapshot of the whole node: the signed ledger tip, a per-collection summary (live document count plus a deterministic checksum over every live key's HLC timestamp — two converged nodes produce an identical checksum for the same collection), and known peers. Meant for exactly the audience this project targets: an AI agent (or operator) that wants situational awareness of one node, or of several via `clients/python/desentry_client.py`'s `Consortium` helper, without walking every endpoint by hand.

---

## 8. Security model

| Concern | Mechanism |
|---|---|
| Peer impersonation | Ed25519 identity keys; `node_id` is derived from the pubkey, so it can't be spoofed without the private key |
| Data tampering in transit or in multi-hop gossip | Origin signs every write; signature travels with the data and is verified before merge |
| Eavesdropping on the wire | X25519 ECDH + AES-256-GCM authenticated encryption per connection |
| Replay of an old signed write | HLC + per-field version check — a replayed old write cannot "win" a merge against a newer one; explicit nonce/session counters additionally prevent transport-level replay |
| Malicious/buggy peer flooding writes | (v2, see §9) rate limiting per peer + reputation scoring; out of scope for MVP correctness but flagged as a hard requirement before any public deployment |
| Local API abuse (an app on the same host over-privileged) | API bind defaults to `127.0.0.1` only; optional bearer-token config for the local HTTP API |
| Data at rest | (v2) page-level encryption in the DiskManager; MVP assumes the host filesystem is trusted, documented explicitly rather than silently assumed |
| Post-hoc tampering with a node's own committed history | SHA-256 hash-chained WAL (§7.6): any alteration, reordering, or deletion of a past entry breaks the chain and is detectable via `POST /_ledger/verify`; the chain tip is Ed25519-signed so tampering can also be attributed to (or ruled out for) a specific `node_id` |

**Explicit MVP threat model**: we defend against a network attacker (on-path or off-path) and against non-colluding peers trying to corrupt each other's data. We do **not** yet defend against a majority-colluding set of malicious peers rewriting history (that requires the BFT path in §9.3), and we do not yet defend against a compromised host (that requires at-rest encryption + secure enclaves, out of MVP scope and stated as such rather than hand-waved).

---

## 9. Roadmap: how this scales to a real product

1. **Sharding.** Once a collection is too large for one peer, partition its key space (consistent hashing, à la Bluzelle) across a subset of peers instead of full replication; the gossip/digest protocol already generalizes to "digest of my shard" with no redesign.
2. **Replacing LRU with LRU-K/clock-sweep** in the buffer pool once we have real workload traces to tune against.
3. **Optional per-collection consensus mode.** For collections that need linearizability (e.g. a balance ledger), run Raft (simplest, CP, needs a majority) or a BFT protocol (needs 3f+1, tolerates malicious minority, closer to BigchainDB/Tendermint) *scoped to that collection's replica set* — the CRDT engine stays the default, consensus is opt-in, matching the "hybrid" option the team considered.
4. **Undo logging / multi-statement transactions** beyond the current single-document-write atomicity.
5. **At-rest page encryption** and a proper cert/PKI story if the mesh ever crosses an untrusted WAN.
6. **Query layer**: secondary indexes, a real predicate pushdown/optimizer, and eventually a small subset of SQL/JSONPath over the document model.
7. **Membership churn at scale**: replace flat UDP broadcast discovery with a Kademlia-style DHT (as IPFS/libp2p/BitTorrent use) once cluster size exceeds a LAN broadcast domain.

---

## 10. Repository layout

See `README.md` for the build instructions; the directory structure is documented inline there and mirrors this document's component boundaries 1:1 (`storage/`, `crdt/`, `net/`, `security/`, `api/`). `clients/python/` and `tools/dashboard.html` (§7.6, `docs/COMPARISON.md` §3.3–§4) are consumers of the REST API, not part of the engine itself.
