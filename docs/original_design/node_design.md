# De-Sentry: Node Design Specification

## 1. Overview

Each De-Sentry **node** is a self-contained C++ process that:
- Owns and manages a local database partition
- Maintains a hash-chained append-only change ledger (WAL)
- Generates and caches brain files
- Communicates with peer nodes over TCP

---

## 2. Local Database Schema

The local database stores the node's **primary data** (owned exclusively) and **cached summaries** (read-only copies of peer brain files).

### 2.1 Primary Tables

```sql
-- Primary agent data (owned by this node)
CREATE TABLE agent_records (
    id            TEXT PRIMARY KEY,
    node_owner    TEXT NOT NULL,        -- must equal this node's ID
    created_at_us INTEGER NOT NULL,
    updated_at_us INTEGER NOT NULL,
    payload       BLOB NOT NULL,        -- application-defined JSON/msgpack
    ledger_entry  INTEGER REFERENCES change_ledger(entry_id)
);

-- The append-only change ledger
CREATE TABLE change_ledger (
    entry_id      INTEGER PRIMARY KEY AUTOINCREMENT,
    node_id       TEXT    NOT NULL,
    timestamp_us  INTEGER NOT NULL,
    operation     TEXT    NOT NULL CHECK(operation IN ('INSERT','UPDATE','DELETE','SCHEMA')),
    tbl           TEXT    NOT NULL,
    row_key       TEXT    NOT NULL,
    payload_delta BLOB,                 -- serialized diff or full new value
    prev_hash     BLOB    NOT NULL,     -- SHA-256 (32 bytes)
    entry_hash    BLOB    NOT NULL      -- SHA-256 (32 bytes)
);

-- Cached brain files from peers
CREATE TABLE brain_file_cache (
    node_id       TEXT    PRIMARY KEY,
    generation    INTEGER NOT NULL,
    generated_at_us INTEGER NOT NULL,
    ttl_seconds   INTEGER NOT NULL,
    raw_json      TEXT    NOT NULL,
    cached_at_us  INTEGER NOT NULL
);

-- Peer ledger tips (lightweight notification cache)
CREATE TABLE peer_ledger_tips (
    node_id       TEXT    PRIMARY KEY,
    entry_id      INTEGER NOT NULL,
    entry_hash    BLOB    NOT NULL,
    observed_at_us INTEGER NOT NULL
);
```

### 2.2 Schema Invariants

- `change_ledger.node_id` must always equal this node's ID (no remote entries stored locally)
- `agent_records.node_owner` must equal this node's ID (enforced in the C++ write path, not just SQL)
- `change_ledger` rows are **never** deleted or updated after insert

---

## 3. Change Ledger Entry Format

### 3.1 C++ Struct

```cpp
struct LedgerEntry {
    uint64_t    entry_id;          // monotonic, node-local counter
    std::string node_id;           // "A", "B", or "C"
    int64_t     timestamp_us;      // Unix epoch microseconds (local clock)
    Operation   operation;         // INSERT | UPDATE | DELETE | SCHEMA
    std::string table_name;
    std::string row_key;
    std::vector<uint8_t> payload_delta;   // msgpack-serialized delta
    std::array<uint8_t, 32> prev_hash;    // SHA-256 of previous entry
    std::array<uint8_t, 32> entry_hash;   // SHA-256 of this entry's fields
};
```

### 3.2 Hash Computation

```cpp
// Pseudocode — implemented in src/core/ledger.cpp
std::array<uint8_t, 32> compute_entry_hash(const LedgerEntry& e) {
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, &e.entry_id,       sizeof(e.entry_id));
    SHA256_Update(&ctx, e.node_id.data(),  e.node_id.size());
    SHA256_Update(&ctx, &e.timestamp_us,   sizeof(e.timestamp_us));
    SHA256_Update(&ctx, &e.operation,      sizeof(e.operation));
    SHA256_Update(&ctx, e.table_name.data(),  e.table_name.size());
    SHA256_Update(&ctx, e.row_key.data(),  e.row_key.size());
    SHA256_Update(&ctx, e.payload_delta.data(), e.payload_delta.size());
    SHA256_Update(&ctx, e.prev_hash.data(), 32);  // ← chain link
    std::array<uint8_t, 32> hash;
    SHA256_Final(hash.data(), &ctx);
    return hash;
}
```

### 3.3 Genesis Entry

The very first entry (entry_id = 0) uses `prev_hash = {0x00 × 32}`. All subsequent entries link to the previous entry's `entry_hash`.

---

## 4. Brain File

### 4.1 JSON Schema

```json
{
  "$schema": "https://desentry.local/brain-file/v1",
  "node_id": "A",
  "schema_version": 1,
  "generated_at_us": 1735000000000000,
  "generation": 42,
  "ttl_seconds": 30,
  "ledger_tip": {
    "entry_id": 1034,
    "entry_hash": "a3f9c2d1...c12b"
  },
  "table_summaries": [
    {
      "table": "agent_records",
      "row_count": 512,
      "last_modified_us": 1734999990000000,
      "checksum": "d4e28f01..."
    }
  ],
  "node_uptime_us": 86400000000,
  "custom_metadata": {}
}
```

### 4.2 Generation Counter

The `generation` field is a monotonically increasing integer incremented every time the brain file is regenerated. Peers comparing two cached copies of the same node's brain file always keep the higher-generation copy.

### 4.3 TTL and Staleness

A brain file is considered **stale** when:

```
now_us > generated_at_us + (ttl_seconds × 1_000_000)
```

A stale brain file triggers a pull from the originating node before use.

---

## 5. Network I/O

### 5.1 Connection Model

Each node maintains **persistent TCP connections** to all peers (2 connections at N=3). Connections use a simple length-prefixed binary framing:

```
┌─────────────────┬───────────────────────────────┐
│  4 bytes        │  N bytes                      │
│  (uint32 BE)    │  (JSON or msgpack payload)    │
│  payload length │                               │
└─────────────────┴───────────────────────────────┘
```

### 5.2 Message Types

```cpp
enum class MessageType : uint8_t {
    BRAIN_FILE_PUSH    = 0x01,  // Originator → peers: full brain file JSON
    BRAIN_FILE_PULL_REQ = 0x02, // Node → peer: request fresh brain file
    BRAIN_FILE_PULL_RESP= 0x03, // Peer → requestor: brain file response
    LEDGER_NOTIFY      = 0x04,  // Node → peers: { node_id, entry_id, hash }
    LEDGER_REPLAY_REQ  = 0x05,  // Node → peer: request entries from_id..to_id
    LEDGER_REPLAY_RESP = 0x06,  // Peer → requestor: array of LedgerEntry
    HEALTH_PING        = 0x07,
    HEALTH_PONG        = 0x08,
};
```

---

## 6. C++ Module Breakdown

```
src/core/
├── db/
│   ├── local_db.h/.cpp        ← SQLite wrapper + schema init
│   └── query_engine.h/.cpp    ← CRUD operations with ledger integration
├── ledger/
│   ├── ledger.h/.cpp          ← LedgerEntry struct, hash computation
│   ├── ledger_store.h/.cpp    ← Append to DB, verify chain
│   └── ledger_verifier.h/.cpp ← Full chain replay + hash check
├── brain/
│   ├── brain_file.h/.cpp      ← JSON generation, serialization
│   └── brain_cache.h/.cpp     ← Cache management, TTL, eviction
├── network/
│   ├── peer_connection.h/.cpp ← Single TCP connection state machine
│   ├── mesh_manager.h/.cpp    ← Manages all peer connections
│   └── message_codec.h/.cpp   ← Frame encode/decode
├── sync/
│   └── sync_engine.h/.cpp     ← Periodic push, pull-on-miss, notify
├── ipc/
│   └── napi_bridge.cpp        ← node-addon-api bindings for Electron
└── main.cpp                   ← Entry point, config parsing, startup
```

---

## 7. Python Binding Surface (pybind11)

```python
import desentry

# Connect to local node
node = desentry.Node(config_path="node_a.toml")
node.start()

# Write (triggers ledger append)
node.insert("agent_records", key="agent-001", payload={"name": "Ava"})
node.update("agent_records", key="agent-001", patch={"status": "active"})

# Read local data
record = node.get("agent_records", key="agent-001")

# Read peer brain file (from cache or network)
brain_b = node.get_brain_file("B")  # returns dict

# Verify local ledger integrity
result = node.verify_ledger()  # returns VerificationResult

# Dump ledger to JSON
entries = node.get_ledger(from_entry=0, to_entry=100)
```

---

## 8. Configuration File (TOML)

```toml
[node]
id = "A"
port = 7001

[peers]
B = "127.0.0.1:7002"
C = "127.0.0.1:7003"

[brain_file]
ttl_seconds = 30
sync_interval_seconds = 10
output_path = "./data/brain_A.json"

[ledger]
db_path = "./data/node_A.db"

[coordinator]
node_id = "A"   # which node handles shared-state writes
```
