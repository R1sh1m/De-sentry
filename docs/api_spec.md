# De-Sentry: Inter-Node API Specification

## 1. Overview

The De-Sentry inter-node API is a **TCP/HTTP-hybrid** protocol:

- **Control plane**: HTTP/1.1 REST for management, brain file exchange, and ledger operations (human-readable, easy to debug).
- **Data plane**: length-prefixed binary frames over persistent TCP for high-frequency ledger notifications and heartbeats.

All inter-node communication is over **loopback** (`127.0.0.1`) for the course demo. Ports are configurable per node.

---

## 2. HTTP REST Endpoints

Base URL: `http://127.0.0.1:{node_port}/api/v1`

### 2.1 Health

#### `GET /health`

Returns node liveness status.

**Response 200:**
```json
{
  "node_id": "A",
  "status": "ok",
  "uptime_us": 86400000000,
  "ledger_tip_entry_id": 1034,
  "peers": {
    "B": { "reachable": true, "last_ping_us": 1735000000000000 },
    "C": { "reachable": true, "last_ping_us": 1735000000001000 }
  }
}
```

---

### 2.2 Brain File

#### `GET /brain-file`

Returns this node's current (freshly generated) brain file.

**Response 200:**
```json
{
  "node_id": "A",
  "schema_version": 1,
  "generated_at_us": 1735000000000000,
  "generation": 42,
  "ttl_seconds": 30,
  "ledger_tip": {
    "entry_id": 1034,
    "entry_hash": "a3f9c2d1e4b5f6a7..."
  },
  "table_summaries": [
    {
      "table": "agent_records",
      "row_count": 512,
      "last_modified_us": 1734999990000000,
      "checksum": "d4e28f0190ab..."
    }
  ],
  "node_uptime_us": 86400000000,
  "custom_metadata": {}
}
```

#### `POST /brain-file/push`

Accepts a brain file pushed by a peer. The receiving node caches the file.

**Request body:**
```json
{
  "sender_node_id": "B",
  "brain_file": { ... }   // same schema as GET /brain-file response
}
```

**Response 200:**
```json
{ "accepted": true, "cached_generation": 17 }
```

**Response 409 Conflict** (stale push — generation ≤ currently cached):
```json
{ "accepted": false, "reason": "stale", "current_generation": 18 }
```

---

### 2.3 Change Ledger

#### `GET /ledger/tip`

Returns the latest ledger entry's metadata (lightweight).

**Response 200:**
```json
{
  "node_id": "A",
  "entry_id": 1034,
  "entry_hash": "a3f9c2d1e4b5f6a7...",
  "timestamp_us": 1735000000000000
}
```

#### `GET /ledger/entries?from={from_id}&to={to_id}`

Returns a range of ledger entries for replay/verification.

| Parameter | Type | Description |
|---|---|---|
| `from` | `uint64` | First entry ID to return (inclusive) |
| `to` | `uint64` | Last entry ID to return (inclusive) |

**Response 200:**
```json
{
  "node_id": "A",
  "entries": [
    {
      "entry_id": 100,
      "node_id": "A",
      "timestamp_us": 1735000000000000,
      "operation": "INSERT",
      "table_name": "agent_records",
      "row_key": "agent-001",
      "payload_delta": "...(msgpack base64-encoded)...",
      "prev_hash": "...(hex)...",
      "entry_hash": "...(hex)..."
    }
  ],
  "count": 1
}
```

#### `POST /ledger/verify`

Requests the node to verify its own ledger chain and return the result.

**Response 200:**
```json
{
  "node_id": "A",
  "verified": true,
  "entries_checked": 1035,
  "tip_entry_id": 1034
}
```

**Response 200 (chain failure):**
```json
{
  "node_id": "A",
  "verified": false,
  "failed_at_entry_id": 512,
  "reason": "Hash mismatch"
}
```

---

### 2.4 Data Operations (Local Only — Agent API)

These endpoints are exposed **only on loopback** and are for the agent/Python layer to write local data. They are **not** peer-facing.

#### `POST /data/{table}`

Insert a new record.

**Request body:**
```json
{
  "key": "agent-001",
  "payload": { "name": "Ava", "status": "idle" }
}
```

**Response 201:**
```json
{
  "key": "agent-001",
  "ledger_entry_id": 1035,
  "entry_hash": "..."
}
```

#### `PATCH /data/{table}/{key}`

Update an existing record (sparse patch).

**Request body:**
```json
{
  "patch": { "status": "active" }
}
```

**Response 200:**
```json
{
  "key": "agent-001",
  "ledger_entry_id": 1036,
  "entry_hash": "..."
}
```

#### `DELETE /data/{table}/{key}`

Delete a record. Creates a DELETE ledger entry.

**Response 200:**
```json
{
  "key": "agent-001",
  "ledger_entry_id": 1037,
  "entry_hash": "..."
}
```

#### `GET /data/{table}/{key}`

Read a record from local storage.

**Response 200:**
```json
{
  "key": "agent-001",
  "payload": { "name": "Ava", "status": "active" },
  "last_ledger_entry_id": 1036
}
```

---

## 3. Binary Frame Protocol (TCP Data Plane)

For high-frequency messages (ledger notifications, heartbeats), a lightweight binary protocol is used over persistent TCP.

### 3.1 Frame Format

```
┌──────────┬──────────┬─────────────────────────────┐
│ 1 byte   │ 4 bytes  │ N bytes                     │
│ msg_type │ length   │ payload (msgpack)            │
│ (uint8)  │ (uint32) │                             │
└──────────┴──────────┴─────────────────────────────┘
```

### 3.2 Message Types

| Code | Name | Payload |
|---|---|---|
| `0x01` | `BRAIN_PUSH` | Full brain file (msgpack) |
| `0x02` | `BRAIN_PULL_REQ` | `{ requesting_node: string }` |
| `0x03` | `BRAIN_PULL_RESP` | Full brain file (msgpack) |
| `0x04` | `LEDGER_NOTIFY` | `{ node_id, entry_id, entry_hash }` |
| `0x05` | `LEDGER_REPLAY_REQ` | `{ from_entry_id, to_entry_id }` |
| `0x06` | `LEDGER_REPLAY_RESP` | `{ entries: [...] }` |
| `0x07` | `HEALTH_PING` | `{ timestamp_us }` |
| `0x08` | `HEALTH_PONG` | `{ timestamp_us }` |

### 3.3 LEDGER_NOTIFY Payload

```json
{
  "node_id": "A",
  "entry_id": 1035,
  "entry_hash": "a3f9c2d1..."
}
```

Peers store this in `peer_ledger_tips` and use it to know whether their cached brain file is likely stale without issuing a full pull.

---

## 4. Error Codes

| HTTP Status | Meaning |
|---|---|
| `200 OK` | Success |
| `201 Created` | Record inserted |
| `400 Bad Request` | Malformed request body |
| `404 Not Found` | Key does not exist |
| `409 Conflict` | Stale brain file push; version conflict |
| `503 Service Unavailable` | Node starting up or DB locked |

---

## 5. Electron/UI Integration

The Electron frontend communicates with the C++ engine via the **N-API bridge** (not direct HTTP). The bridge exposes:

```typescript
// Electron renderer process (via preload.js IPC)
import { desentry } from './bridge';

// Get live health for all nodes
const health = await desentry.getAllNodeHealth();

// Stream ledger entries for display
desentry.onLedgerEntry((entry: LedgerEntry) => {
  updateLedgerTable(entry);
});

// Request brain file for visualization
const brainA = await desentry.getBrainFile('A');
```

The N-API bridge calls into the C++ engine directly for same-process performance, falling back to the HTTP API for cross-node data (peer data).
