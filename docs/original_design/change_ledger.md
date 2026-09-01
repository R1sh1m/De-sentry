# De-Sentry: Change Ledger Specification

## 1. Purpose

The Change Ledger is an **append-only, hash-chained log** of every mutation applied to a node's local database. Its goals are:

1. **Tamper-evidence**: any post-hoc alteration of any entry is detectable by recomputing and comparing hashes.
2. **Audit trail**: a complete history of how the database reached its current state.
3. **Peer verification**: any consortium peer can replay the full chain and confirm integrity.

The ledger is analogous to a **Git commit graph** applied to individual database operations — each entry is like a commit, with the `prev_hash` acting as the parent pointer.

---

## 2. Entry Structure

### 2.1 Fields

| Field | Type | Size | Description |
|---|---|---|---|
| `entry_id` | `uint64` | 8 B | Monotonically increasing, node-local counter. Starts at 0. |
| `node_id` | `string` | ≤ 4 B | Identifying string for the owning node ("A", "B", "C"). |
| `timestamp_us` | `int64` | 8 B | Unix epoch in **microseconds** (local clock — not globally synchronized). |
| `operation` | `uint8` | 1 B | Enum: `0=INSERT`, `1=UPDATE`, `2=DELETE`, `3=SCHEMA` |
| `table_name` | `string` | variable | Name of the affected table. |
| `row_key` | `string` | variable | Primary key of the affected row. |
| `payload_delta` | `bytes` | variable | msgpack-serialized delta. For INSERT/SCHEMA: full new value. For UPDATE: `{field: new_value, ...}`. For DELETE: empty. |
| `prev_hash` | `bytes[32]` | 32 B | SHA-256 hash of the immediately preceding entry. Genesis entry uses `0x00 × 32`. |
| `entry_hash` | `bytes[32]` | 32 B | SHA-256 of all fields above concatenated in canonical order (see §3). |

### 2.2 Wire Format (Binary)

Entries are serialized to disk and network using **msgpack** with the field order above. String fields are length-prefixed (msgpack standard). Hash fields are raw bytes (not hex-encoded) in the wire format.

---

## 3. Hash Chain Algorithm

### 3.1 Canonical Serialization for Hashing

```
hash_input = concat(
    uint64_to_bytes_BE(entry_id),
    uint8(len(node_id)) || node_id_bytes,
    int64_to_bytes_BE(timestamp_us),
    uint8(operation),
    uint8(len(table_name)) || table_name_bytes,
    uint16_to_bytes_BE(len(row_key)) || row_key_bytes,
    uint32_to_bytes_BE(len(payload_delta)) || payload_delta_bytes,
    prev_hash[32]
)

entry_hash = SHA-256(hash_input)
```

> All integer fields use **big-endian** byte order for deterministic serialization across architectures.

### 3.2 Chain Invariant

```
∀ n > 0:  entry[n].prev_hash == entry[n-1].entry_hash
entry[0].prev_hash == 0x00_00_..._00  (32 zero bytes)
```

Violating this invariant means either:
- An entry was **modified** after insertion, or
- An entry was **deleted** from the middle of the chain, or
- A **new entry was inserted** into the middle of the chain.

All three cases are detectable by chain verification (§4).

---

## 4. Verification Algorithm

### 4.1 Full Chain Verification

```python
def verify_chain(entries: list[LedgerEntry]) -> VerificationResult:
    expected_prev_hash = bytes(32)  # 32 zero bytes for genesis

    for i, entry in enumerate(entries):
        # Check chain link
        if entry.prev_hash != expected_prev_hash:
            return VerificationResult(
                ok=False,
                failed_at=entry.entry_id,
                reason=f"Chain break: expected prev_hash {expected_prev_hash.hex()}, "
                       f"got {entry.prev_hash.hex()}"
            )

        # Recompute and check entry hash
        computed_hash = compute_entry_hash(entry)
        if computed_hash != entry.entry_hash:
            return VerificationResult(
                ok=False,
                failed_at=entry.entry_id,
                reason=f"Hash mismatch at entry {entry.entry_id}"
            )

        # Advance expected prev_hash
        expected_prev_hash = entry.entry_hash

    return VerificationResult(ok=True, tip_entry_id=entries[-1].entry_id)
```

### 4.2 Incremental Verification

For continuous operation, nodes verify each new entry at append time:

```cpp
bool LedgerStore::append(LedgerEntry& entry) {
    entry.prev_hash = current_tip_hash_;
    entry.entry_hash = compute_entry_hash(entry);

    // Verify before committing to DB
    if (entry.entry_hash != compute_entry_hash(entry)) {
        log_error("Hash computation inconsistency — aborting append");
        return false;
    }

    db_.execute_insert(entry);
    current_tip_hash_ = entry.entry_hash;
    current_tip_id_ = entry.entry_id;
    return true;
}
```

---

## 5. Operation Types

### 5.1 INSERT

```json
{
  "operation": "INSERT",
  "table_name": "agent_records",
  "row_key": "agent-001",
  "payload_delta": {
    "id": "agent-001",
    "node_owner": "A",
    "created_at_us": 1735000000000000,
    "updated_at_us": 1735000000000000,
    "payload": { "name": "Ava", "status": "idle" }
  }
}
```

`payload_delta` contains the **full new row** so the state can be reconstructed from the ledger alone.

### 5.2 UPDATE

```json
{
  "operation": "UPDATE",
  "table_name": "agent_records",
  "row_key": "agent-001",
  "payload_delta": {
    "updated_at_us": 1735000001000000,
    "payload.status": "active"
  }
}
```

`payload_delta` contains only the **changed fields** (sparse update). Full state reconstruction requires replaying all preceding entries for this key.

### 5.3 DELETE

```json
{
  "operation": "DELETE",
  "table_name": "agent_records",
  "row_key": "agent-001",
  "payload_delta": {}
}
```

Empty delta — the key alone identifies the deleted row.

### 5.4 SCHEMA

```json
{
  "operation": "SCHEMA",
  "table_name": "agent_records",
  "row_key": "__schema__",
  "payload_delta": {
    "ddl": "ALTER TABLE agent_records ADD COLUMN priority INTEGER DEFAULT 0"
  }
}
```

Schema changes are also ledger entries, making the schema evolution history auditable.

---

## 6. Ledger Storage

The ledger is stored in the node's SQLite database as the `change_ledger` table (see [`docs/node_design.md`](node_design.md)). The `entry_hash` and `prev_hash` columns store raw BLOB (32 bytes each).

**No DELETE or UPDATE statements are ever issued on the `change_ledger` table.** The C++ ledger store enforces this at the API level — there is no `delete_entry()` or `update_entry()` method.

---

## 7. Peer Verification Flow

Any node can request a full ledger replay from a peer:

```
Node B                          Node A
  │                                │
  │── LEDGER_REPLAY_REQ ──────────►│
  │   { from_entry: 0,             │
  │     to_entry: 1034 }           │
  │                                │
  │◄── LEDGER_REPLAY_RESP ─────────│
  │   [ entry_0, entry_1, ...,     │
  │     entry_1034 ]               │
  │                                │
  │  [B runs verify_chain()]       │
  │                                │
  │  [B reports: ✓ or ✗ + which    │
  │   entry_id failed]             │
```

This is used during **initial join**, **manual audit**, and **post-incident investigation**.

---

## 8. Limitations and Acknowledged Gaps

| Gap | Impact | Production Fix |
|---|---|---|
| Local clock for timestamps | Clock drift between nodes can cause misordering in LWW conflict resolution | Use Lamport timestamps or HLC (Hybrid Logical Clocks) |
| No entry signing | A node could regenerate a full chain from scratch and substitute it | Sign each entry with the node's private key (Ed25519) |
| No sparse-delta reconstruction index | Full state reconstruction requires full replay | Add a compaction / materialized snapshot step |
| SQLite row-level locking | Concurrent write throughput bounded by SQLite's writer lock | Use RocksDB or a custom B-Tree for the ledger |
