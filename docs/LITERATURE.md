<p align="center">
  <img src="assets/banner-slim.svg" width="100%" alt="De-Sentry">
</p>

# Research Foundation & Literature

De-Sentry is built on decades of foundational research in distributed systems, database theory, and peer-to-peer architectures. This document catalogs the key research and industry works that directly influenced the design decisions in this system.

---

## Core Database Systems

### Textbooks & Foundational Works

**Elmasri & Navathe — _Fundamentals of Database Systems_ (7th ed., 2016)**
- The canonical reference for relational database theory, normalization, and transaction processing
- Direct influence on: B+Tree implementation, WAL design, ACID properties, normalization guidelines
- Key concepts adopted: relational model constraints, functional dependencies, query optimization fundamentals

**Silberschatz, Korth & Sudarshan — _Database System Concepts_ (7th ed., 2019)**
- Comprehensive treatment of storage architectures, indexing, and concurrency control
- Direct influence on: buffer pool management and recovery algorithms; its locking chapters are the reference for what we deliberately leave out of the data plane
- Key concepts adopted: buffer pool design, recovery. Two-phase and multi-granularity locking are studied but not used — see the transaction table below.

**Özsu & Valduriez — _Principles of Distributed Database Systems_ (4th ed., 2020)**
- The definitive reference for distributed database architectures
- Direct influence on: replication and allocation framing, distributed catalog management, distributed query concerns
- Key concepts adopted: replica allocation and distributed catalog design. De-Sentry deliberately departs from fragmentation-based placement: the data plane broadcasts eagerly and converges by gossip rather than partitioning rows into disjoint fragments.

---

## Distributed Systems & Consistency

### CAP Theorem & Trade-offs

**Brewer (2000) — "Towards Robust Distributed Systems" (PODC Keynote)**
- Introduced the CAP conjecture: Consistency, Availability, Partition tolerance — pick two
- De-Sentry's stance: **AP with eventual consistency** for the data plane; supervisors are app-local control processes and never participate in replication or coordination
- Justification: the mesh must remain writable during network partitions; convergence is guaranteed via CRDTs

**Gilbert & Lynch (2002) — "Brewer's Conjecture and the Feasibility of Consistent, Available, Partition-Tolerant Web Services" (ACM SIGACT News)**
- Formal proof of the CAP theorem
- Reinforces our design choice: linearizable consistency would require a coordinator, violating the "no client-server split" principle

### CRDTs & Conflict-Free Replication

**Preguiça (2018) — "Conflict-free Replicated Data Types: An Overview" (arXiv)**
- Comprehensive survey of CRDT theory and implementations
- Direct influence on: field-level LWW (Last-Writer-Wins) semantics, OR-Set for multi-value fields, HLC-based causality tracking
- Key insight: CRDTs provide **mathematical convergence guarantees** without coordination

**Shapiro et al. (2011) — "Conflict-free Replicated Data Types" (SSS Symposium)**
- Introduced the formal framework: join-semilattices, merge functions, convergence proofs
- De-Sentry's document model implements state-based CRDTs with HLC timestamps

### Hybrid Logical Clocks

**Lamport (1978) — "Time, Clocks, and the Ordering of Events in a Distributed System" (CACM)**
- Introduced logical clocks for causal ordering
- Foundation for: our HLC implementation in `src/crdt/hlc.cpp`

**Kulkarni et al. (2014) — "Logical Physical Clocks and Consistent Snapshots in Globally Distributed Databases" (OPODIS)**
- The hybrid logical clock: physical time plus a logical counter, bounded drift
- Direct influence on: `HLCTimestamp` encoding, causality tracking without global synchronization

---

## Peer-to-Peer Architectures

### Dynamo & Amazon's Architecture

**DeCandia et al. (2007) — "Dynamo: Amazon's Highly Available Key-Value Store" (SOSP)**
- Introduced: consistent hashing, vector clocks, gossip-based membership, quorum-based reads/writes
- Direct influence on: our consistent hashing ring (160 virtual nodes per peer), RF=3 replica sets, gossip anti-entropy
- Key adoption: availability over strict quorum semantics. Durability acknowledgements and the transit store play the role of Dynamo's hinted handoff for an unreachable holder.

### Chord & DHTs

**Stoica et al. (2001) — "Chord: A Scalable Peer-to-Peer Lookup Service" (SIGCOMM)**
- Introduced distributed hash tables with O(log N) lookup
- Influence: our placement algorithm uses consistent hashing (simplified from full Chord for LAN-scale meshes)

### Gossip Protocols

**Demers et al. (1987) — "Epidemic Algorithms for Replicated Database Maintenance" (PODC)**
- Introduced gossip/epidemic propagation for eventual consistency
- Direct influence on: `src/net/gossip.cpp` anti-entropy mechanism, digest exchange, repair

**Jelasity et al. (2007) — "Gossip-Based Peer Sampling" (ACM TOCS)**
- Analyzed gossip convergence properties
- Validates our choice: gossip ensures all reachable nodes converge, even with multi-hop topologies

---

## Storage Engine Design

### B+Trees & Indexing

**Comer (1979) — "The Ubiquitous B-Tree" (ACM Computing Surveys)**
- Comprehensive analysis of B-Tree variants
- Direct influence on: our disk-backed B+Tree in `src/storage/bplus_tree.cpp`

**Graefe (1993) — "Query Evaluation Techniques for Large Databases" (ACM Computing Surveys)**
- Survey of query evaluation and access-path selection
- Influence: the router's scan/filter path and the deliberate decision to ship no general SQL planner in v1

### Write-Ahead Logging

**Gray & Reuter (1993) — "Transaction Processing: Concepts and Techniques"**
- WAL fundamentals: write-ahead, redo/undo, ARIES algorithm
- Direct influence on: `src/storage/wal.cpp`, crash recovery in `StorageEngine::Open()`

**Mohan et al. (1992) — "ARIES: A Transaction Recovery Method Supporting Fine-Granularity Locking and Partial Rollbacks Using Write-Ahead Logging" (ACM TODS)**
- Three-pass recovery: analysis, redo, undo
- De-Sentry adopts the redo half only: WAL replay on open, with no analysis/undo pass, because committed writes are immutable CRDT deltas

---

## Security & Cryptography

### Transport Security

**Rescorla (2000) — "SSL and TLS: Designing and Building Secure Systems"**
- Foundation for: our X25519 ECDH key exchange, AES-256-GCM authenticated encryption
- Direct influence on: `src/security/crypto.cpp`, secure channel establishment

**IETF RFC 8446 (TLS 1.3), RFC 7748 (X25519), RFC 8032 (Ed25519)**
- The modern primitive choices and handshake shape the transport borrows from
- Direct influence on: key exchange, signatures and transcript binding in the secure channel

### Hash Chains & Audit Logs

**Schneier & Kelsey (1999) — "Secure Audit Logs to Support Computer Forensics" (ACM TOCS)**
- Tamper-evident, append-only audit logs built from cryptographic chaining
- Direct influence on: our hash-chained ledger in `src/ledger/`, Ed25519 signatures on every entry

**Crosby & Wallach (2009) — "Efficient Data Structures for Tamper-Evident Logging" (USENIX Security)**
- History trees: Merkle-tree-based tamper-evident logs with efficient inclusion proofs
- Inspiration for: future checkpoint Merkle proofs (current implementation uses a simple hash chain)

---

## NoSQL & Modern Storage

### NoSQL Categories

**Han et al. (2011) — "Survey on NoSQL Database" (IEEE ICPCA)**
- Classification: key-value, document, column-family, graph
- Direct influence on: our multi-engine router supporting KV, columnar, time-series, vector, graph backends

**Sadalage & Fowler (2012) — "NoSQL Distilled"**
- Practical guide to NoSQL trade-offs
- Validates our approach: polyglot persistence via storage router, not a one-size-fits-all engine

### Vector Databases

**Malkov & Yashunin (2020) — "Efficient and Robust Approximate Nearest Neighbor Search Using Hierarchical Navigable Small World Graphs" (IEEE TPAMI)**
- The HNSW graph index for approximate nearest-neighbour search
- Direct influence on: the graph structure and search parameters in `src/storage/engines/vector_hnsw_lite.cpp`

**Johnson et al. (2019) — "Billion-Scale Similarity Search with GPUs" (IEEE Transactions on Big Data)**
- GPU implementations of k-selection and similarity search at scale
- Influence on: the distance/kernel layout used by the vector engine

### Time-Series Databases

**Jensen, Pedersen & Thomsen (2017) — "Time Series Management Systems: A Survey" (IEEE TKDE)**
- Survey of time-series models, compression, and query patterns
- Direct influence on: rollup strategies and retention policy design in `src/storage/engines/ts_rollup.cpp`

---

## File Systems & Distributed Storage

### Google File System

**Ghemawat et al. (2003) — "The Google File System" (SOSP)**
- Chunk-based storage and chunk replication
- Direct influence on: transit-store chunking for large documents and holder selection. The single-master metadata design was deliberately not adopted.

### HDFS

**Shvachko et al. (2010) — "The Hadoop Distributed File System" (IEEE MSST)**
- Block replication and heartbeat-based failure detection
- Influence on: liveness tracking via heartbeats; rack-awareness was not carried over (the mesh has no rack model)

---

## Distributed Snapshots & Global State

**Chandy & Lamport (1985) — "Distributed Snapshots: Determining Global States of Distributed Systems" (ACM TOCS)**
- Recording a globally consistent state without a global clock or freezing computation
- Direct influence on: `GET /_brain`, the compact mesh snapshot (ledger tip, per-collection checksums, peers) that peers exchange instead of querying every node
- The marker-message idea maps onto our snapshot push/pull cycle

### Latency vs Consistency

**Abadi (2012) — "Consistency Tradeoffs in Modern Distributed Database System Design: CAP is Only Part of the Story" (PACELC, IEEE Computer)**
- Extends CAP: even without partitions, there is a latency/consistency trade-off
- Direct influence on: the `/_brain` cache-and-TTL design — serve a slightly stale snapshot fast rather than block on a fresh network round trip

### Consensus Comparisons

**Howard & Mortier (2020) — "Paxos vs Raft: Have We Reached Consensus on Distributed Consensus?" (arXiv)**
- Comparative analysis of the two dominant consensus algorithms
- Used to justify why neither is on the De-Sentry data path

---

## Consensus & Coordination (What We Avoid)

### Raft

**Ongaro & Ousterhout (2014) — "In Search of an Understandable Consensus Algorithm" (USENIX ATC)**
- Leader-based consensus for replicated state machines
- **Deliberately not adopted**: Raft requires a leader, violating our "no coordinator" principle
- Documented in: `docs/comparison.md` §2

### Paxos

**Lamport (1998) — "The Part-Time Parliament" (ACM TOCS)**
- Consensus with crash failures
- **Not adopted**: extra round trips and a distinguished-proposer role the mesh does not need

### Tendermint / BFT

**Buchman (2016) — "Tendermint: Byzantine Fault Tolerance in the Age of Blockchains" (PhD Thesis)**
- BFT consensus for untrusted networks
- **Future consideration**: v3 could add per-collection BFT for adversarial settings (currently out of scope)

---

## Related Systems

### GunDB

**GunDB Project** — P2P graph database with CRDT sync
- Similarities: field-level LWW, offline-first, no coordinator
- Differences: GunDB is a sync layer over pluggable storage; we ship a disk-first pager with WAL

### OrbitDB

**OrbitDB Project** — P2P database over IPFS
- Similarities: CRDT op-logs, append-only ledgers
- Differences: OrbitDB depends on IPFS; we have native P2P transport

### CockroachDB / TiDB

- Similarities: distributed, replicated storage with production operations tooling
- Differences: they use Raft per range; we use CRDTs for coordination-free convergence

---

## Contemporary Issues

### Edge Computing & Offline-First

**Satyanarayanan (2017) — "The Emergence of Edge Computing" (IEEE Computer)**
- Arguments for local processing, intermittent connectivity
- Validates our offline-first design: nodes function fully without network access

### Data Privacy & Sovereignty

**GDPR (2018) — General Data Protection Regulation**
- Right to data portability, local storage requirements
- Influences: our per-node recovery keys, no escrow policy, encrypted at-rest storage

---

## Theory → Implementation Map

The canonical database-systems literature describes a stable set of
abstractions. De-Sentry implements them concretely rather than delegating to a
library. This table is the shortest path from a textbook chapter to the code
that embodies it.

### Data models and design

| Concept (standard reference) | Where it lives in De-Sentry | Notes |
|---|---|---|
| Relational model, domains, keys, entity/referential integrity | `include/desentry/storage/document_codec.h`, `src/api/routes.cpp` (`/_schema/:collection`) | Collections are documents; schema validators enforce the subset of constraints the API accepts |
| ER / EER modelling, weak entities, generalization | `docs/original_design/`, `app/src/views/wizard.ts` | Conceptual modelling informs the workload prototypes in `app/resources/prototypes.json` |
| Functional dependencies, normalization (1NF–BCNF, 4NF, 5NF) | `docs/project_statement.md` §Assumptions, `app/resources/prototypes.json` | Schema design guidance; the engine stores validated documents rather than decomposed relations |
| Three-schema architecture, data independence | `include/desentry/storage/router.h`, `include/desentry/storage/catalog.h` | Catalog = logical schema; router = physical independence across engines |

### Physical design and indexing

| Concept | Where it lives | Notes |
|---|---|---|
| Disk manager, pages, slotted page layout | `src/storage/disk_manager.cpp`, `src/storage/slotted_page.cpp` | 4 KiB pages, checksummed |
| Buffer pool and replacement policy | `src/storage/buffer_pool_manager.cpp` | Sizing threaded from config through the router |
| Heap / ordered / hashed file organizations | `src/storage/engines/kv_bplus.cpp`, `src/storage/segment_store.cpp` | B+Tree primary access path; segment store for columnar/append data |
| B+Tree and multilevel indexing | `src/storage/bplus_tree.cpp` | Split/merge, bounded descent, persisted roots validated on open |
| LSM-tree family | `src/storage/engines/columnar_lite.cpp`, `ts_rollup` | Append-oriented, sealed segments with an explicit `Compact()` pass — LSM-adjacent in shape, not a full leveled LSM tree |
| Static and dynamic hashing (file organizations) | Not implemented as a file organization | The only hashing on the data path is the consistent-hash ring in `src/net/placement.cpp`, used for replica placement; `kv` stores rows in the B+Tree |
| Query translation, relational algebra, heuristic optimisation | `src/api/routes.cpp` (scan/filter paths), `src/storage/router.cpp` | Point lookups, range scans, predicate filters; no general SQL planner |

### Transactions, concurrency, recovery

| Concept | Where it lives | Notes |
|---|---|---|
| ACID properties | `src/storage/wal.cpp`, `src/crdt/document.cpp`, `src/ledger/` | Durability via WAL; atomicity per document; isolation via CRDT merge rather than locks; consistency by convergence |
| Transaction states, schedules, serializability | `tests/crdt_test.cpp`, `tests/ledger_v2_test.cpp` | Convergence replaces serializable scheduling on the data plane |
| Two-phase locking, timestamp ordering, deadlock handling | Not implemented on the data plane — deliberate | See `docs/comparison.md` §2: locks require a coordinator |
| Multiversion storage and per-field versioning | `src/crdt/hlc.cpp`, per-field HLC timestamps | No snapshot isolation; every field keeps its own version, and merge is a join over versions |
| Deferred-update and immediate-update recovery | `src/storage/wal.cpp`, `StorageEngine::Open()` | WAL replay with CRC32 validation; torn tail vs mid-file corruption distinguished |
| ARIES-style analysis/redo/undo | `src/storage/wal.cpp` | Redo replay on open; no undo pass, because committed writes are immutable CRDT deltas |
| Shadow paging | Not used | WAL + rebuild-from-replay chosen instead (a stored root page id is never trusted) |

### Distributed databases and NoSQL

| Concept | Where it lives | Notes |
|---|---|---|
| Distributed architectures, client/server vs peer-to-peer | `docs/architecture.md` §3, `src/engine/node_engine.cpp` | Every node is both server and client; no distinguished owner |
| Fragmentation (horizontal/vertical) | Not used as a partitioning strategy | `src/net/placement.cpp` computes an RF=3 replica subset per key; eager broadcast + gossip means reachable holders converge on the full document set |
| Replication and allocation | `src/net/gossip.cpp`, `src/ledger/transit_store.cpp` | Anti-entropy repair + transit holding for unreachable owners |
| Distributed concurrency control and recovery | `src/crdt/`, `src/ledger/checkpoint.cpp` | CRDT join semantics; quorum-gated checkpoint GC |
| Distributed query processing and optimisation | `src/storage/router.cpp` | Engine selection per collection; no cross-node joins |
| Distributed catalog management | `src/storage/catalog.cpp`, `GET /_brain` | Per-node catalog plus a compact mesh snapshot |
| CAP theorem; ACID vs BASE | `docs/architecture.md` §2, `docs/comparison.md` | AP by design; convergence is eventual and provable, not best-effort |
| Key-value, document, column-family, graph, vector, time-series stores | `src/storage/engines/` | Five native engines plus four opt-in vendored backends behind one `EngineBackend` contract |
| MapReduce / batch processing | Out of scope | The mesh is an OLTP/OLAP-lite store, not a compute framework |

### Concurrency and liveness

| Concept | Where it lives | Notes |
|---|---|---|
| Heartbeats, failure detection, liveness tiers | `src/net/receipt_tracker.cpp`, `tests/liveness_test.cpp` | Silence tiers upgrade to probes; supervisors act only on real reports |
| Gossip / epidemic propagation | `src/net/gossip.cpp` | Digest exchange with content fingerprints so divergence is repairable |
| Consistent hashing and virtual nodes | `src/net/placement.cpp` | Deterministic across machines; supervisors excluded from the ring |

---

## Reading List by Topic

**If you are new to database engines:** Elmasri & Navathe, chapters 1–2
(concepts and architecture), 16–17 (storage and indexing), 20–22
(transactions, concurrency, recovery), then 23–24 (distributed and NoSQL).

**If you are new to distributed systems:** Brewer (2000), Gilbert & Lynch
(2002), Lamport (1978), then DeCandia et al. (2007) and Shapiro et al. (2011).
Read Özsu & Valduriez chapters 1, 5, and 6 for the distributed-database
framing.

**If you are here for the storage engine:** Comer (1979), Gray & Reuter
(1993), Mohan et al. (1992), and the WAL/`StorageEngine` sources side by side.

**If you are here for CRDTs:** Shapiro et al. (2011), then Preguiça (2018),
then `src/crdt/document.cpp` and `tests/crdt_test.cpp`.

---

## Bibliography (BibTeX)

```bibtex
@book{elmasri2016fundamentals,
  title={Fundamentals of Database Systems},
  author={Elmasri, Ramez and Navathe, Shamkant B},
  edition={7th},
  year={2016},
  publisher={Pearson}
}

@book{silberschatz2019database,
  title={Database System Concepts},
  author={Silberschatz, Abraham and Korth, Henry F and Sudarshan, S},
  edition={7th},
  year={2019},
  publisher={McGraw Hill}
}

@book{ozsu2020principles,
  title={Principles of Distributed Database Systems},
  author={{\"O}zsu, M Tamer and Valduriez, Patrick},
  edition={4th},
  year={2020},
  publisher={Springer Nature}
}

@inproceedings{brewer2000cap,
  title={Towards Robust Distributed Systems},
  author={Brewer, Eric A},
  booktitle={PODC Keynote},
  year={2000}
}

@article{gilbert2002brewer,
  title={Brewer's Conjecture and the Feasibility of Consistent, Available, Partition-Tolerant Web Services},
  author={Gilbert, Seth and Lynch, Nancy},
  journal={ACM SIGACT News},
  volume={33},
  number={2},
  pages={51--59},
  year={2002}
}

@inproceedings{shapiro2011crdt,
  title={Conflict-free Replicated Data Types},
  author={Shapiro, Marc and Pregui{\c{c}}a, Nuno and Baquero, Carlos and Zawirski, Marek},
  booktitle={SSS Symposium},
  year={2011}
}

@inproceedings{decandia2007dynamo,
  title={Dynamo: Amazon's Highly Available Key-Value Store},
  author={DeCandia, Giuseppe and Hastorun, Deniz and Jampani, Madan and others},
  booktitle={SOSP},
  year={2007}
}

@article{lamport1978time,
  title={Time, Clocks, and the Ordering of Events in a Distributed System},
  author={Lamport, Leslie},
  journal={CACM},
  volume={21},
  number={7},
  pages={558--565},
  year={1978}
}

@inproceedings{ghemawat2003gfs,
  title={The Google File System},
  author={Ghemawat, Sanjay and Gobioff, Howard and Leung, Shun-Tak},
  booktitle={SOSP},
  year={2003}
}

@inproceedings{ongaro2014raft,
  title={In Search of an Understandable Consensus Algorithm},
  author={Ongaro, Diego and Ousterhout, John},
  booktitle={USENIX ATC},
  year={2014}
}

@article{chandy1985distributed,
  title={Distributed Snapshots: Determining Global States of Distributed Systems},
  author={Chandy, K Mani and Lamport, Leslie},
  journal={ACM Transactions on Computer Systems},
  volume={3},
  number={1},
  pages={63--75},
  year={1985}
}

@article{abadi2012consistency,
  title={Consistency Tradeoffs in Modern Distributed Database System Design: CAP is Only Part of the Story},
  author={Abadi, Daniel J},
  journal={IEEE Computer},
  volume={45},
  number={2},
  pages={37--42},
  year={2012}
}

@article{howard2020paxos,
  title={Paxos vs Raft: Have We Reached Consensus on Distributed Consensus?},
  author={Howard, Heidi and Mortier, Richard},
  journal={arXiv preprint arXiv:2004.05074},
  year={2020}
}

@inproceedings{demers1987epidemic,
  title={Epidemic Algorithms for Replicated Database Maintenance},
  author={Demers, Alan and Greene, Dan and Hauser, Carl and Irish, Wes and Larson, John and Shenker, Scott and Sturgis, Howard and Swinehart, Dan and Terry, Doug},
  booktitle={PODC},
  year={1987}
}

@article{comer1979btree,
  title={The Ubiquitous B-Tree},
  author={Comer, Douglas},
  journal={ACM Computing Surveys},
  volume={11},
  number={2},
  pages={121--137},
  year={1979}
}

@book{gray1993transaction,
  title={Transaction Processing: Concepts and Techniques},
  author={Gray, Jim and Reuter, Andreas},
  year={1993},
  publisher={Morgan Kaufmann}
}

@article{mohan1992aries,
  title={ARIES: A Transaction Recovery Method Supporting Fine-Granularity Locking and Partial Rollbacks Using Write-Ahead Logging},
  author={Mohan, C and Haderle, Don and Lindsay, Bruce and Pirahesh, Hamid and Schwarz, Peter},
  journal={ACM Transactions on Database Systems},
  volume={17},
  number={1},
  pages={94--162},
  year={1992}
}

@inproceedings{kulkarni2014hlc,
  title={Logical Physical Clocks and Consistent Snapshots in Globally Distributed Databases},
  author={Kulkarni, Sandeep S and Demirbas, Murat and Madappa, Devan and Avva, Bharadwaj and Leone, Marcelo},
  booktitle={OPODIS},
  year={2014}
}

@inproceedings{stoica2001chord,
  title={Chord: A Scalable Peer-to-Peer Lookup Service for Internet Applications},
  author={Stoica, Ion and Morris, Robert and Karger, David and Kaashoek, M Frans and Balakrishnan, Hari},
  booktitle={SIGCOMM},
  year={2001}
}

@inproceedings{shvachko2010hdfs,
  title={The Hadoop Distributed File System},
  author={Shvachko, Konstantin and Kuang, Hairong and Radia, Sanjay and Chansler, Robert},
  booktitle={IEEE MSST},
  year={2010}
}

@article{johnson2019billion,
  title={Billion-scale Similarity Search with GPUs},
  author={Johnson, Jeff and Douze, Matthijs and J{\'e}gou, Herv{\'e}},
  journal={IEEE Transactions on Big Data},
  volume={7},
  number={3},
  pages={535--547},
  year={2019}
}

@article{malkov2020hnsw,
  title={Efficient and Robust Approximate Nearest Neighbor Search Using Hierarchical Navigable Small World Graphs},
  author={Malkov, Yu A and Yashunin, Dmitry A},
  journal={IEEE Transactions on Pattern Analysis and Machine Intelligence},
  volume={42},
  number={4},
  pages={824--836},
  year={2020}
}

@article{jelasity2007gossip,
  title={Gossip-Based Peer Sampling},
  author={Jelasity, M{\'a}rk and Voulgaris, Spyros and Guerraoui, Rachid and Kermarrec, Anne-Marie and van Steen, Maarten},
  journal={ACM Transactions on Computer Systems},
  volume={25},
  number={3},
  pages={8--es},
  year={2007}
}

@article{schneier1999secure,
  title={Secure Audit Logs to Support Computer Forensics},
  author={Schneier, Bruce and Kelsey, John},
  journal={ACM Transactions on Information and System Security},
  volume={2},
  number={2},
  pages={159--176},
  year={1999}
}

@inproceedings{crosby2009efficient,
  title={Efficient Data Structures for Tamper-Evident Logging},
  author={Crosby, Scott A and Wallach, Dan S},
  booktitle={USENIX Security Symposium},
  year={2009}
}

@inproceedings{han2011survey,
  title={Survey on NoSQL Database},
  author={Han, Jing and E, Haihong and Le, Guan and Du, Jian},
  booktitle={IEEE International Conference on Pervasive Computing and Applications (ICPCA)},
  year={2011}
}

@article{jensen2017time,
  title={Time Series Management Systems: A Survey},
  author={Jensen, S{\o}ren Kejser and Pedersen, Torben Bach and Thomsen, Christian},
  journal={IEEE Transactions on Knowledge and Data Engineering},
  volume={29},
  number={11},
  pages={2581--2600},
  year={2017}
}

@article{preguica2018crdt,
  title={Conflict-free Replicated Data Types: An Overview},
  author={Pregui{\c{c}}a, Nuno},
  journal={arXiv preprint arXiv:1806.10254},
  year={2018}
}
```

---

*This document is a living bibliography. As De-Sentry evolves, new influences and comparisons will be added.*