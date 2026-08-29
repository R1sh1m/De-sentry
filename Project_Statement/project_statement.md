# De-Sentry: Project Statement

## Course Project Brief

### Problem Statement

When multiple autonomous AI agents simultaneously read and write to a shared central database, they can cause **network congestion** and **data-locking issues**. We propose a decentralized database architecture that eliminates this central point of failure by giving each agent its own independent, sandboxed database node.

These nodes form a small, cooperating network (a **"consortium"**) that maintains data integrity without funneling every operation through one instance.

---

### Abstract

When multiple autonomous AI agents simultaneously read and write to a shared central database, they can cause network congestion and data locking issues. We plan to use a decentralized database architecture that eliminates this central point of failure. Instead of forcing agents to hammer a single database instance, our architecture would provide each agent with its own independent, sandboxed database node. Together, this network of nodes shall form a truly decentralized consortium that grows as newer nodes are created.

To maintain data integrity without slowing down the network, the system shall combines three core innovations:

1. Data mutations are tracked using cryptographically secure, Git-style "diff" change ledgers, providing a complete audit trail akin to a blockchain.
2. Since each node would contain entirely unique files, cross-node conflicts are non-existent. Multi-node coordination can be handled via an isolated, temporary memory-locking mechanism (Mutex).
3. Rather than broadcasting massive data payloads across the entire network, nodes maintain a localized "brain file" that can be snapshot of the node and sync only their latest changes with neighbors. These files can later be used by other agents to access things as needed without having to execute a lot of search queries.

We shall also cache these brain files in each node, so that each node has a context of not just itself but also about other nodes as well.

---

### System Design

To maintain integrity without sacrificing performance, the system combines three core ideas:

1. **Change ledger** — every mutation to a node's data is recorded as a hash-chained, append-only "diff" entry, giving a tamper-evident audit trail of how the data reached its current state (similar in spirit to a Git commit history).

2. **Conflict handling** — since each node owns its own primary data, conflicts are minimized by design; where coordination is still needed (e.g., updating shared "brain file" summaries), a lightweight, simplified coordination rule is used instead of full distributed consensus.

3. **Brain files** — each node maintains a compact, periodically-refreshed summary snapshot of its own state ("brain file"). Nodes exchange and cache each other's brain files so that any agent can get a quick picture of the whole network without running expensive queries against every other node directly.

For this course project, the system is scoped to **3 nodes** (one per team member), connected in a **full mesh**, running locally for demonstration purposes.

---

### Assumptions and Limitations

Being explicit about what this project does not solve, and why those choices are reasonable for the scope of a course project:

1. **No true distributed mutual exclusion.** A real distributed lock (e.g., Raft- or Paxos-based) is a research-level problem on its own and out of scope for our time budget. Instead, we use a simplified coordination rule (e.g., last-write-wins by timestamp, or a single lightweight coordinator node) for the rare cases where brain-file updates need ordering. We acknowledge this reintroduces a soft dependency on one node, which trades off against the "no central point of failure" goal — a real production version would replace this with proper distributed consensus.

2. **Not a blockchain.** Our change ledger provides tamper-evidence via hash chaining, not blockchain-grade distributed consensus among untrusted parties. We describe it plainly as a hash-chained append-only log rather than a blockchain.

3. **No network partition or node-failure handling.** The system assumes all 3 nodes are reachable during a sync cycle. Behavior during a dropped connection or offline node is not implemented; this would need explicit retry/reconciliation logic in a production system.

4. **No general scalability guarantee.** The design is validated at exactly 3 nodes in a full mesh. We do not implement dynamic peer discovery or bootstrapping for new nodes joining after the fact, and we do not claim the caching strategy for brain files scales past a small, fixed set of nodes (caching every other node's brain file grows linearly with node count).

5. **No adversarial trust model.** Nodes are assumed to be honest/trusted (a "consortium" of known participants), not defended against malicious or Byzantine nodes. Hashing detects accidental corruption/tampering after the fact, not malicious participation in real time.
