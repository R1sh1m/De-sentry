# De-Sentry: Project Statement

## Project Brief

### Problem Statement

Autonomous AI agents that share one central database contend for the same
connections, locks, and I/O. De-Sentry removes the central instance: each agent
gets its own independent, sandboxed database node, and those nodes form a
cooperating peer-to-peer mesh that preserves integrity without funneling every
operation through one machine.

---

### Abstract

When multiple autonomous AI agents read and write a shared central database at
the same time, they create network congestion and lock contention. De-Sentry
removes the central instance entirely: every agent gets its own independent,
sandboxed database node, and those nodes form a cooperating peer-to-peer mesh
that preserves integrity without funneling operations through one place.

The design combines three ideas:

1. Every mutation is recorded in a cryptographically chained, Git-style change
   ledger, producing a tamper-evident audit trail.
2. Conflict handling is local and deterministic: field-level CRDTs over hybrid
   logical clocks converge without locks, leaders, or a coordinator.
3. Nodes maintain a compact "brain" snapshot of their state, exchange it with
   peers, and cache what they receive, so agents can inspect the mesh without
   querying every node.

---

### System Design

The system rests on three mechanisms:

1. **Change ledger** — every mutation is recorded as a hash-chained,
   append-only entry with an Ed25519 signature, giving a tamper-evident audit
   trail of how the data reached its current state (similar in spirit to a Git
   commit history).
2. **Conflict handling** — every replicated document is a state-based CRDT:
   scalars are last-writer-wins registers ordered by hybrid logical clocks,
   arrays are OR-Sets, and objects merge recursively. Concurrent writes
   converge without locks, leaders, or a coordinator.
3. **Brain files** — each node maintains a compact, periodically refreshed
   summary of its state (ledger tip, per-collection checksums, peer liveness).
   Nodes exchange and cache these summaries so an agent can inspect the mesh
   without running expensive queries against every other node.

A mesh is not fixed at three nodes: the data plane is flat, LAN discovery is
automatic, and static bootstrap peers let nodes join across L3 boundaries.

---

### Assumptions and Limitations

Being explicit about what this project does not solve, and why those choices are reasonable for the current scope:

1. **No mutual exclusion, and no coordinator.** Ordering comes from hybrid logical clocks plus CRDT merges, so concurrent writes converge without any lock, leader, or coordinator node -- including for brain-file updates, which converge through gossip like all other state. There is no "soft dependency on one node" anywhere on the data path (see docs/comparison.md §2). A production version needing linearizable transactions would add opt-in per-collection consensus; that is explicitly out of scope, not half-implemented.

2. **Not a blockchain.** Our change ledger provides tamper-evidence via hash chaining, not blockchain-grade distributed consensus among untrusted parties. We describe it plainly as a hash-chained append-only log rather than a blockchain.

3. **Temporary loss is handled; partitions are not guaranteed.** A dropped connection or offline node IS implemented: writes for an unreachable owner are held in the transit store, gossip anti-entropy converges state when it returns, and it claims what was held for it on startup. What is NOT guaranteed: bounded convergence time, survival of arbitrary/extended partitions, or Byzantine behavior -- those need explicit work (see limitation 5) and are not claimed.

4. **Validated past 3 nodes, but no unbounded-scalability claim.** The mesh is validated at 3 nodes for local demos and up to 50 nodes in soak tests, with UDP-broadcast discovery plus static bootstrap peers for nodes joining later (including across L3 boundaries where broadcast does not reach). We do not claim linear scaling to very large meshes, and there is no DHT-based routing past the LAN; brain-file caching cost also grows with node count.

5. **No adversarial trust model.** Nodes are assumed to be honest/trusted (a "consortium" of known participants), not defended against malicious or Byzantine nodes. Hashing detects accidental corruption/tampering after the fact, not malicious participation in real time.
