# De-Sentry: Annotated Research Bibliography

This document catalogs all academic papers and technical references underpinning the De-Sentry architecture. Each entry includes a brief annotation explaining its relevance to the project's design decisions.

---

## Tier 1 — Core References (Directly Cited in Design)

---

### [P01] Distributed Snapshots: Determining Global States of Distributed Systems

| Field | Detail |
|---|---|
| **Authors** | K. Mani Chandy, Leslie Lamport |
| **Venue** | ACM Transactions on Computer Systems (TOCS), Vol. 3, No. 1, pp. 63–75 |
| **Year** | February 1985 |
| **DOI** | [10.1145/214451.214456](https://doi.org/10.1145/214451.214456) |
| **PDF** | [Available via ACM DL](https://dl.acm.org/doi/10.1145/214451.214456) — search "Chandy Lamport 1985 distributed snapshots" |

**Relevance to De-Sentry:**  
The De-Sentry **brain file** is a direct application of the Chandy-Lamport distributed snapshot concept. A brain file is a periodic, consistent snapshot of a node's state that can be exchanged with peers to give them a coherent picture of the distributed system without stopping ongoing operations. The paper's core insight — that you can record a globally consistent state without a global clock or freezing computation — directly justifies our brain-file generation approach. Their marker-message protocol for coordinating snapshot timing is analogous to our `BRAIN_FILE_PUSH` message.

---

### [P02] Dynamo: Amazon's Highly Available Key-Value Store

| Field | Detail |
|---|---|
| **Authors** | Giuseppe DeCandia, Deniz Hastorun, Madan Jampani, Gunavardhan Kakulapati, Avinash Lakshman, Alex Pilchin, Swaminathan Sivasubramanian, Peter Vosshall, Werner Vogels |
| **Venue** | ACM SOSP 2007 (Symposium on Operating Systems Principles) |
| **Year** | 2007 |
| **DOI** | [10.1145/1294261.1294281](https://doi.org/10.1145/1294261.1294281) |
| **PDF (open access)** | [https://www.allthingsdistributed.com/files/amazon-dynamo-sosp2007.pdf](https://www.allthingsdistributed.com/files/amazon-dynamo-sosp2007.pdf) |

**Relevance to De-Sentry:**  
The Dynamo paper is the single closest architectural analog to De-Sentry. Its three foundational choices map almost directly onto ours: (1) **node-owned primary data** to minimize write conflicts, (2) **last-write-wins with timestamp** for conflict resolution, and (3) **gossip-based state exchange** for decentralized awareness. Dynamo also explicitly chooses AP over CP — the same CAP trade-off De-Sentry makes. Our brain-file caching mechanism is a simplified version of Dynamo's gossip membership protocol, and our change ledger is an evolution of Dynamo's per-node version vector tracking.

---

### [P03] In Search of an Understandable Consensus Algorithm (Raft)

| Field | Detail |
|---|---|
| **Authors** | Diego Ongaro, John Ousterhout |
| **Venue** | USENIX ATC 2014 (Annual Technical Conference) |
| **Year** | 2014 |
| **PDF (official)** | [https://raft.github.io/raft.pdf](https://raft.github.io/raft.pdf) |
| **Web** | [https://raft.github.io](https://raft.github.io) |

**Relevance to De-Sentry:**  
Raft is explicitly cited in De-Sentry's **Limitation §1** as the production-grade distributed consensus algorithm we consciously omit. Understanding Raft clarifies *what* we are trading away: leader election, log replication guarantees, and quorum-based safety under node failures. Our lightweight coordinator node (Node A handles shared-state writes) is a drastically simplified, single-node substitute for Raft's leader. Reading Raft also explains why a "proper" solution to distributed mutual exclusion requires a full consensus protocol — context that strengthens our acknowledgment of this limitation.

---

### [P04] Towards Robust Distributed Systems (CAP Theorem — Keynote)

| Field | Detail |
|---|---|
| **Authors** | Eric A. Brewer |
| **Venue** | ACM PODC 2000 (Symposium on Principles of Distributed Computing) |
| **Year** | 2000 |
| **Slides/Talk** | [https://people.eecs.berkeley.edu/~brewer/cs262b-2004/PODC-keynote.pdf](https://people.eecs.berkeley.edu/~brewer/cs262b-2004/PODC-keynote.pdf) |

**Relevance to De-Sentry:**  
Brewer's CAP conjecture (Consistency, Availability, Partition Tolerance — pick 2) is the theoretical framework that frames the entire De-Sentry design. By choosing node-owned primary data and last-write-wins, De-Sentry sits in the **AP** corner of CAP: it prioritizes availability (every node can always serve reads and writes from its own partition) and partition tolerance (the system continues operating if a peer is unreachable), at the cost of strong global consistency. This paper is the essential starting point for understanding *why* our design choices are necessary.

---

### [P05] Brewer's Conjecture and the Feasibility of Consistent, Available, Partition-Tolerant Web Services

| Field | Detail |
|---|---|
| **Authors** | Seth Gilbert, Nancy Lynch |
| **Venue** | ACM SIGACT News, Vol. 33, No. 2, pp. 51–59 |
| **Year** | 2002 |
| **DOI** | [10.1145/564585.564601](https://doi.org/10.1145/564585.564601) |
| **PDF** | [https://users.ece.cmu.edu/~adrian/731-sp04/readings/GL-cap.pdf](https://users.ece.cmu.edu/~adrian/731-sp04/readings/GL-cap.pdf) |

**Relevance to De-Sentry:**  
This paper provides the formal mathematical proof of Brewer's CAP conjecture. While Brewer's keynote was informal, Gilbert and Lynch construct a rigorous proof using an asynchronous network model showing that no distributed system can simultaneously provide all three CAP properties. This formal proof underpins our justification for the AP trade-off: not just a design preference, but a mathematical impossibility to avoid. Pairs directly with [P04] as citation evidence for our limitations section.

---

### [P06] The Part-Time Parliament (Paxos)

| Field | Detail |
|---|---|
| **Authors** | Leslie Lamport |
| **Venue** | ACM Transactions on Computer Systems (TOCS), Vol. 16, No. 2, pp. 133–169 |
| **Year** | 1998 (originally circulated as a technical report in 1989) |
| **DOI** | [10.1145/279227.279229](https://doi.org/10.1145/279227.279229) |
| **PDF** | [https://lamport.azurewebsites.net/pubs/lamport-paxos.pdf](https://lamport.azurewebsites.net/pubs/lamport-paxos.pdf) |

**Relevance to De-Sentry:**  
Paxos is the foundational consensus algorithm, cited alongside Raft in Limitation §1. While Raft [P03] is more readable, Paxos is the historical baseline that Raft improved upon for understandability. Together, both papers define the "proper distributed consensus" category that De-Sentry's lightweight coordinator deliberately avoids implementing. Understanding Paxos's two-phase prepare/promise + accept/accepted cycle reveals why full consensus is expensive (multiple round trips, quorum requirements) — validating our simplification.

---

## Tier 2 — Supporting Theory

---

### [P07] Conflict-free Replicated Data Types: An Overview

| Field | Detail |
|---|---|
| **Authors** | Nuno Preguiça |
| **Venue** | arXiv preprint |
| **Year** | 2018 |
| **arXiv** | [arXiv:1806.10254](https://arxiv.org/abs/1806.10254) |
| **PDF** | [https://arxiv.org/pdf/1806.10254](https://arxiv.org/pdf/1806.10254) |

**Relevance to De-Sentry:**  
CRDTs (Conflict-free Replicated Data Types) represent the theoretical ideal that De-Sentry partially approximates. In a CRDT-based system, each data type is designed so that concurrent updates from different nodes can always be merged without conflicts — no coordination required. De-Sentry's node-ownership model achieves a similar effect through a different mechanism: by ensuring each data item has a single owning node, we sidestep concurrent writes to the same record entirely. This paper provides the theoretical context for understanding *why* ownership eliminates conflicts, and what a more general conflict-free approach would look like.

---

### [P08] A Comprehensive Study of Convergent and Commutative Replicated Data Types

| Field | Detail |
|---|---|
| **Authors** | Marc Shapiro, Nuno Preguiça, Carlos Baquero, Marek Zawirski |
| **Venue** | INRIA Technical Report RR-7506 |
| **Year** | 2011 |
| **PDF** | [https://inria.hal.science/inria-00555588/document](https://inria.hal.science/inria-00555588/document) |

**Relevance to De-Sentry:**  
This is the foundational CRDT paper, introducing both CvRDTs (convergent) and CmRDTs (commutative) formally. It catalogs concrete CRDT implementations (G-Counter, 2P-Set, MV-Register, etc.). In the context of De-Sentry, the MV-Register (Multi-Value Register) is directly relevant — it is the CRDT equivalent of our last-write-wins field, but with the added ability to detect and expose concurrent-write conflicts rather than silently discarding one. This paper grounds the theoretical gap between our simplified LWW approach and a CRDT-correct solution.

---

### [P09] Epidemic Algorithms for Replicated Database Maintenance

| Field | Detail |
|---|---|
| **Authors** | Alan Demers, Dan Greene, Carl Hauser, Wes Irish, John Larson, Scott Shenker, Howard Sturgis, Dan Swinehart, Doug Terry |
| **Venue** | ACM PODC 1987 |
| **Year** | 1987 |
| **DOI** | [10.1145/41840.41841](https://doi.org/10.1145/41840.41841) |
| **PDF** | [https://bitsavers.org/pdf/xerox/parc/techReports/CSL-89-1_Epidemic_Algorithms_for_Replicated_Database_Maintenance.pdf](https://bitsavers.org/pdf/xerox/parc/techReports/CSL-89-1_Epidemic_Algorithms_for_Replicated_Database_Maintenance.pdf) |

**Relevance to De-Sentry:**  
This 1987 paper is the origin of **gossip protocols** (also called epidemic algorithms). The paper's core insight — that information can spread reliably through a network by having each node periodically share updates with a random subset of peers — maps directly onto De-Sentry's brain-file exchange. Our `BRAIN_FILE_PUSH` periodic cycle is a deterministic variant of the epidemic "push" model (push to all peers rather than a random subset, which is correct and efficient at N=3). The paper also describes "anti-entropy" — periodic full state comparisons to catch divergence — which is analogous to our `LEDGER_REPLAY_REQ` verification flow.

---

### [P10] Time, Clocks, and the Ordering of Events in a Distributed System

| Field | Detail |
|---|---|
| **Authors** | Leslie Lamport |
| **Venue** | Communications of the ACM, Vol. 21, No. 7, pp. 558–565 |
| **Year** | July 1978 |
| **DOI** | [10.1145/359545.359563](https://doi.org/10.1145/359545.359563) |
| **PDF** | [https://lamport.azurewebsites.net/pubs/time-clocks.pdf](https://lamport.azurewebsites.net/pubs/time-clocks.pdf) |

**Relevance to De-Sentry:**  
This is the paper that introduced **Lamport timestamps** (logical clocks). It is foundational for understanding the problem De-Sentry's last-write-wins strategy faces: physical clocks on different nodes can disagree, so a write with a later physical timestamp might not actually be causally later. Lamport's logical clocks — where each event's timestamp is `max(local_clock, received_clock) + 1` — provide a consistent ordering without requiring synchronized physical clocks. This paper is essential context for Limitation §1 and for any future work that replaces physical timestamps with logical ones.

---

### [P11] Multi-Agent Systems and Blockchain: Results from a Systematic Literature Review

| Field | Detail |
|---|---|
| **Authors** | Davide Calvaresi, Alevtina Dubovitskaya, Juan Carlos Restrepo-Zea, Scilla Dragoni, Michael Schumacher |
| **Venue** | International Conference on Practical Applications of Agents and Multi-Agent Systems (PAAMS) 2018 |
| **Year** | 2018 |
| **DOI** | [10.1007/978-3-319-94779-2_16](https://doi.org/10.1007/978-3-319-94779-2_16) |
| **PDF** | [https://www.researchgate.net/publication/325854826](https://www.researchgate.net/publication/325854826) (ResearchGate, search by title) |

**Relevance to De-Sentry:**  
This systematic review surveys the intersection of multi-agent systems (MAS) and blockchain/distributed ledger technology — precisely the domain of De-Sentry. It categorizes why MAS benefit from hash-chained ledgers (accountability, auditability, non-repudiation), and what limitations a pure blockchain imposes (throughput, finality latency). This paper validates the De-Sentry architectural choice to use a hash-chained log (ledger) *without* full blockchain consensus: we capture the auditability benefits while avoiding the consensus overhead. It also surfaces related work in decentralized MAS coordination.

---

### [P12] Paxos vs Raft: Have we reached consensus on distributed consensus?

| Field | Detail |
|---|---|
| **Authors** | Heidi Howard, Richard Mortier |
| **Venue** | arXiv preprint |
| **Year** | 2020 |
| **arXiv** | [arXiv:2004.05074](https://arxiv.org/abs/2004.05074) |
| **PDF** | [https://arxiv.org/pdf/2004.05074](https://arxiv.org/pdf/2004.05074) |

**Relevance to De-Sentry:**  
An accessible comparative analysis of Paxos and Raft that clarifies their equivalence and trade-offs. Useful for grounding De-Sentry's Limitation §1 discussion with concrete detail about what "proper distributed consensus" entails. The paper's findings — that Raft's understandability comes at the cost of some flexibility — also hint at why even Raft might be overkill for a 3-node fixed-topology consortium, further justifying our simplified coordinator approach.

---

## Tier 3 — Extended Background

---

### [P13] PACELC: An Impossibility Theorem for Partitioned-Networked Databases

| Field | Detail |
|---|---|
| **Authors** | Daniel J. Abadi |
| **Venue** | IEEE Data Engineering Bulletin, Vol. 35, No. 4 |
| **Year** | 2012 |
| **PDF** | [http://cs-www.cs.yale.edu/homes/dna/papers/abadi-pacelc.pdf](http://cs-www.cs.yale.edu/homes/dna/papers/abadi-pacelc.pdf) |

**Relevance to De-Sentry:**  
PACELC extends CAP by noting that even *without* a partition, there is a trade-off between **latency (L)** and **consistency (C)**. De-Sentry's brain-file cache-and-TTL approach is a direct PACELC decision: we favor low latency (serve from cache, don't block on a fresh network fetch) at the cost of potentially stale (slightly inconsistent) data. This paper provides a more nuanced framework than CAP alone for describing what De-Sentry actually optimizes.

---

### [P14] Decentralized Multi-Agent System with Trust-Aware Communication

| Field | Detail |
|---|---|
| **Authors** | Ding et al. |
| **Venue** | arXiv preprint |
| **Year** | 2025 |
| **arXiv** | [arXiv search: "Decentralized Multi-Agent System Trust-Aware Communication Ding 2025"](https://arxiv.org/search/?searchtype=all&query=decentralized+multi-agent+trust-aware+communication) |

**Relevance to De-Sentry:**  
A recent (2025) paper proposing a decentralized MAS architecture using blockchain-based ledgers for agent discovery and communication integrity. Directly relevant to De-Sentry's problem domain (autonomous AI agents + decentralized coordination). Provides a contemporary academic framing of the challenges De-Sentry addresses, and is useful for situating the project within current research trends in the introduction/related-work section.

---

## Citation Summary Table

| ID | Paper | Authors | Year | Mapped Component |
|---|---|---|---|---|
| P01 | Distributed Snapshots | Chandy, Lamport | 1985 | Brain files |
| P02 | Dynamo | DeCandia et al. | 2007 | Overall architecture |
| P03 | Raft | Ongaro, Ousterhout | 2014 | Limitation §1 |
| P04 | CAP Theorem (keynote) | Brewer | 2000 | AP trade-off |
| P05 | CAP Proof | Gilbert, Lynch | 2002 | AP trade-off (formal) |
| P06 | Paxos | Lamport | 1998 | Limitation §1 |
| P07 | CRDTs Overview | Preguiça | 2018 | Conflict handling theory |
| P08 | CRDTs Comprehensive | Shapiro et al. | 2011 | Conflict handling (deeper) |
| P09 | Epidemic Algorithms | Demers et al. | 1987 | Brain file gossip |
| P10 | Time, Clocks | Lamport | 1978 | LWW timestamp limitation |
| P11 | MAS + Blockchain Survey | Calvaresi et al. | 2018 | Ledger in MAS context |
| P12 | Paxos vs Raft | Howard, Mortier | 2020 | Consensus comparison |
| P13 | PACELC | Abadi | 2012 | Latency-consistency trade-off |
| P14 | DMAS Trust-Aware | Ding et al. | 2025 | Contemporary MAS framing |
