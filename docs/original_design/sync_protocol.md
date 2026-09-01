# De-Sentry: Brain File Sync Protocol

## 1. Overview

The sync protocol defines how nodes exchange and cache each other's brain files. It has three operating modes:

| Mode | Trigger | Direction |
|---|---|---|
| **Periodic Push** | Timer fires (`SYNC_INTERVAL_S`) | Originator → all peers |
| **Cache-Miss Pull** | Cached copy is stale | Requester → originator |
| **Ledger Tip Notify** | Write commits to local ledger | Originator → all peers |

---

## 2. Message Flows

### 2.1 Periodic Push (Happy Path)

```
Node A                     Node B                     Node C
  │                           │                           │
  │── BRAIN_FILE_PUSH ────────►│                           │
  │   { brain_A.json }         │                           │
  │                            │                           │
  │── BRAIN_FILE_PUSH ─────────┼──────────────────────────►│
  │   { brain_A.json }         │                           │
  │                            │ (Node B caches brain_A)   │
  │                            │                (Node C caches brain_A)
```

Each node runs this cycle independently on its own timer. All three nodes push concurrently.

### 2.2 Cache-Miss Pull

```
Node B                     Node A
  │                           │
  │  [needs brain_A.json,      │
  │   cache is stale]          │
  │                            │
  │── BRAIN_FILE_PULL_REQ ────►│
  │   { requesting_node: "B" } │
  │                            │
  │◄── BRAIN_FILE_PULL_RESP ───│
  │   { brain_A.json }         │
  │                            │
  │  [cache updated, TTL reset]│
```

### 2.3 Ledger Tip Notification

After every write, the originating node sends a lightweight notification to peers. This is **not** the brain file — it is a minimal record so peers know the originator's ledger has advanced.

```
Node A                     Node B                     Node C
  │   [write committed]       │                           │
  │                           │                           │
  │── LEDGER_NOTIFY ──────────►│                           │
  │── LEDGER_NOTIFY ───────────┼──────────────────────────►│
  │   {                        │                           │
  │     node_id: "A",          │                           │
  │     entry_id: 1035,        │                           │
  │     entry_hash: "a3f9..."  │                           │
  │   }                        │                           │
  │                            │                           │
  │         (peers update peer_ledger_tips table)          │
```

---

## 3. Freshness and TTL Rules

A brain file cached by a remote node is considered **fresh** if:

```
now_us  ≤  brain_file.generated_at_us + (brain_file.ttl_seconds × 1_000_000)
```

A brain file is considered **stale** when the above condition is false. On staleness:
1. If the originating node is reachable → issue a PULL_REQ.
2. If the originating node is **not** reachable → serve the stale copy with a `stale=true` flag and log a warning. (No partition recovery logic is implemented; this is an acknowledged limitation.)

---

## 4. Conflict Resolution

### 4.1 Brain File Version Conflict

If a node receives two copies of the same peer's brain file with different content:

| Scenario | Resolution |
|---|---|
| Different `generation` values | Keep the higher-generation copy |
| Same `generation`, different `generated_at_us` | Keep the more recently generated copy |
| Same `generation`, same `generated_at_us`, different content | Log hash collision warning; keep the copy received from the originating node directly (not a relay) |

This situation should not arise in practice because each node is the sole writer of its own brain file.

### 4.2 Last-Write-Wins (Shared-State)

For the coordinator-managed shared-state namespace, conflict resolution uses **last-write-wins by timestamp**:

```
winner = argmax{ timestamp_us } over conflicting entries
```

> ⚠️ **Known limitation**: physical clocks drift. In production, Lamport timestamps or vector clocks would be used instead. A write with a slightly earlier physical clock but causally later in the system could incorrectly lose. Acknowledged as out-of-scope for course demonstration.

---

## 5. Timing Parameters (Configurable)

| Parameter | Default | Description |
|---|---|---|
| `SYNC_INTERVAL_S` | 10 s | How often each node pushes its own brain file to peers |
| `BRAIN_FILE_TTL_S` | 30 s | How long a cached brain file is considered fresh |
| `LEDGER_NOTIFY_DEBOUNCE_MS` | 100 ms | Coalesce rapid writes into a single notify (burst protection) |
| `HEALTH_PING_INTERVAL_S` | 5 s | Heartbeat frequency |
| `PULL_TIMEOUT_MS` | 2000 ms | How long to wait for a PULL_RESP before marking peer unreachable |

---

## 6. Full Sync Cycle State Machine

```
                    ┌─────────────────┐
                    │   CONNECTED     │◄───────────────────┐
                    └────────┬────────┘                    │
                             │                             │
                    Timer fires (SYNC_INTERVAL_S)          │
                             │                             │
                    ┌────────▼────────┐                    │
                    │ Generate fresh  │                    │
                    │ brain file      │                    │
                    └────────┬────────┘                    │
                             │                             │
                    ┌────────▼────────┐                    │
                    │ Push to all     │                    │
                    │ reachable peers │                    │
                    └────────┬────────┘                    │
                             │                             │
              ┌──────────────▼──────────────┐             │
              │ Check all cached peer files │             │
              └──────────────┬──────────────┘             │
                             │                             │
              ┌──────────────▼──────────────┐             │
              │ For each peer:              │             │
              │   Is cached copy stale?     │             │
              └──────────────┬──────────────┘             │
                             │                             │
              ┌─────── Yes ──┤── No ───────────────────────┘
              │              │
   ┌──────────▼──────┐       │ (all fresh, loop back)
   │ Issue PULL_REQ  │
   └──────────┬──────┘
              │
   ┌──────────▼──────┐
   │ Received RESP?  │
   └──────────┬──────┘
              │
        ┌─Yes─┤─No────────────────────────────────────────┐
        │     │                                           │
   ┌────▼─┐  ┌▼─────────────────────────────────────┐   │
   │Update│  │ Mark peer as unreachable             │   │
   │cache │  │ Serve stale copy with stale=true flag│   │
   └──────┘  └──────────────────────────────────────┘   │
        │                                                │
        └────────────────────────────────────────────────┘
```

---

## 7. Security Notes

- All messages are transmitted in plaintext over loopback (`127.0.0.1`) for the course demonstration.
- A production system would require **TLS mutual authentication** between nodes and **signed brain files** (e.g., Ed25519 signatures) to prevent a rogue node from injecting a false brain file for a peer.
- Brain file hashes are **not** currently signed — integrity against accidental corruption is provided by SHA-256 in the ledger; brain file content integrity relies on TCP delivery correctness.
