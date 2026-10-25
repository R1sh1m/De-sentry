# De-Sentry Node Creation Wizard Flow

**Purpose:** Document the end-to-end node creation wizard for installer E2E manual verification.  
**Source of truth:** `app/src/views/wizard.ts` (UI), `app/src-tauri/src/commands.rs` (`create_node` Tauri command), `src/supervisor/supervisor_routes.cpp` (supervisor API), `app/src/bridge.ts` (Tauri boundary), `app/src/api.ts` (HTTP client).

---

## Overview

The wizard is the **only way a node comes into existence** in the De-Sentry desktop app. It has **five steps**:

| # | Step | Title | Purpose |
|---|------|-------|---------|
| 1 | Placement | "Where should this node live?" | Choose the data directory (folder or removable drive) |
| 2 | Shape | "How big, and how private?" | Name, quota, encryption, key mode, mesh vs standalone, description |
| 3 | Proposal | "What shape should this node be?" | AI sizing proposal for engines, quota split, collections (manual override always available) |
| 4 | Review | "Ready to create" | Confirm all settings, then trigger node creation |
| 5 | Recovery Key | "Take the recovery key" | Export/save the recovery key (or confirm passphrase); non-skippable gate |

**Step names (stepper UI):** `["Placement", "Shape", "Proposal", "Review", "Recovery Key"]`

**Key design constraints (from the file header):**
- The recovery key step is **not skippable**. Nothing in the app, supervisor, or keychain can reproduce a recovery key once the wizard closes.
- The wizard refuses to finish until the user has actually exported the key (or confirmed the passphrase).
- Supervisors are never created through this wizard (`supervisor: false` is hardcoded in the create request).

---

## Step-by-Step Flow

### Step 1 — Placement

**What the user sees:** "Where should this node live?"  
A list of automatically discovered storage candidates (folders on this device), plus a "Choose a folder…" button and a "Rescan" button. Each candidate shows its path, free bytes, and badges for `removable`, `encrypted`, or `existing node`. Selecting one highlights it as the chosen storage location.

**User inputs:**
- Pick a folder via the OS file picker (`sidecar.pickDirectory`)
- Or select an auto-discovered candidate
- Optional: manual peer addresses (`bootstrapPeerText`) and mesh secret (`meshSecret`) for joining an existing mesh — validated on advance (see below)

**Backend / API calls:**
1. `refreshNodeList()` — refreshes the sidecar's node registry so the supervisor port is current
2. `api.scan()` → `GET /_supervisor/scan` — discovers storage candidates on the device
3. `api.mounts()` → `GET /_supervisor/mounts` — lists mount points
4. `api.inspect(path)` → `GET /_supervisor/inspect?path=...` — inspects a user-picked folder (convenience; failure is non-fatal)

**Validation (on advancing from step 1):**
- `draft.dataDir !== ""` — a folder must be selected (`canAdvance()`)
- If manual peer addresses are entered: each must match `host:port` with port 1–65535
- Peer addresses and mesh secret must go together (both or neither)

**State transitions:**
- `draft.dataDir`, `draft.removable`, `draft.adopt`, `draft.nodeName` are set when a candidate is chosen
- On advance: `draft.step = 2`; if `draft.description` is non-empty, `runSizing()` is kicked off early (prefetch for step 3)

---

### Step 2 — Shape

**What the user sees:** "How big, and how private?"  
Fields for node name, architecture (isolated standalone vs LAN mesh replica), storage quota, preallocation, encryption at rest, keychain storage, key mode (generated recovery key vs custom passphrase), and a free-text description with suggestions.

**User inputs:**
| Field | Default | Notes |
|-------|---------|-------|
| Name | (from folder name) | Must not be empty; must not look like a path |
| Architecture | Isolated Standalone | "LAN Mesh Replica" enables UDP discovery on port 7901 |
| Storage Quota Cap (MiB) | 2048 | Min 64, step 64; cannot exceed free bytes on the volume |
| Reserve disk space upfront | off | Pre-allocates `storage.reserved` file (Steam-style) |
| Encrypt at rest | on | On removable media: strongly recommended; unlock uses passphrase/recovery key |
| Store key in keychain | on | Disabled for removable media |
| Recovery key or passphrase | Generated | Generated = 256-bit key shown once; Passphrase = your own secret |
| Custom passphrase | (empty) | Min 12 chars; must confirm; weak passphrases need explicit ack |
| Description | (empty) | Feeds the AI sizing proposal in step 3 |

**Validation (on advancing from step 2):**
- Node name not empty, not a path, quota > 0 (HTML5 constraint validation via `setCustomValidity`/`reportValidity`)
- If passphrase mode: client-side `passphraseScore()` meter; server validates authoritatively later
- Weak passphrase (score ≤ 1) requires the "I understand this passphrase is weak" checkbox

**Backend / API calls:** None directly. Step 2 is pure UI state collection.

**State transitions:**
- `draft.spec = null` and `draft.sizingError = ""` are reset (forces fresh sizing on step 3)
- `draft.step = 3`; if description non-empty, `runSizing()` fires automatically

---

### Step 3 — Proposal (Sizing)

**What the user sees:** "What shape should this node be?"  
Three possible UI states:
1. **Idle:** "Propose a shape with AI" button + manual engine picker
2. **Loading:** skeleton spinner ("Reading the description…")
3. **Result:** confidence meter, workload label, clarifying questions (if low confidence), engine picker, quota split bars, collections list

**User inputs:**
- Click "Propose a shape with AI" to run sizing
- Answer clarifying questions (when confidence < 0.35 floor) — each answer re-runs sizing with appended context
- Toggle engines on/off in the manual picker (built-in engines only; opt-in engines show a rebuild hint)
- Edit collections (names are derived from engine defaults)

**Backend / API calls:**
- `sidecar.sizeWorkload(description, quotaMb)` → Tauri command `size_workload`
  - Calls `ai::get()` (bundled MiniLM ONNX model) → compares description to 7 prototype embeddings
  - Falls back to deterministic keyword heuristic if model failed to load (`decision.method = "keyword"`, `fallback_reason` set)
  - 15-second timeout; on timeout the user is told to choose engines manually
  - Generation guard prevents a stale sizing result from overwriting a newer run

**Validation:** None — manual engine selection is always allowed and overrides the proposal.

**State transitions:**
- `draft.spec` populated with engines, default_engine, quota_split, collections, decision
- `draft.manualEngines` synced from spec
- `draft.step = 4` on advance

---

### Step 4 — Review (Confirm)

**What the user sees:** "Ready to create"  
A summary key/value list: Name, Folder, Budget, Engines, Default engine, Database mode, Replication factor, LAN Discovery, At rest, Adopting. Plus a note about what happens next (recovery key or passphrase).

**User inputs:** Click "Create node" (disabled while creating).

**Backend / API calls:**
- `sidecar.createNode(request)` → Tauri command `create_node` (see detailed breakdown below)
- On success: `refreshNodeList()` and `refreshTopology()` (outside the try block — node exists even if refresh fails)
- `store.select({ kind: "node", nodeId })` — selects the new node in the sidebar

**Client-side validation (before the Tauri call):**
- If encrypt + passphrase mode:
  - Passphrase ≥ 12 characters
  - Passphrase and confirmation match
  - Weak passphrase (score ≤ 1) requires the weak-passphrase acknowledgement

**Server-side validation (in `create_node` Rust command):**
- Node name not empty
- Mesh secret (if provided) must be exactly 64 hex characters
- Mesh secret must match current `DESENTRY_CLUSTER_SECRET` or be stored in keychain + env var
- Bootstrap peers require a mesh secret (and vice versa)
- Passphrase validation via `passphrase::validate()` (min length, match)
- Port allocation, config write, process start, keychain storage — all with rollback on failure

**State transitions:**
- On success: `draft.created = result.node`, `draft.recoveryKey = result.recovery_key`, `draft.step = 5`
- On failure: `draft.createError` set, `draft.step = 4` (stays on review, error shown)

---

### Step 5 — Recovery Key

**What the user sees:** "Take the recovery key"  
Three possible screens depending on key mode:

#### 5a — Encrypted, generated key (default)
The recovery key is displayed **once**: as plain mono text, a QR code, and a "constellation" (decorative star groups). Buttons: "Save to a file…", "Print", "Copy". An optional "verify constellation" mini-game lets the user tap the stars back in order (self-check only, does not gate Done).

**User actions:**
- Save to file → `sidecar.pickSaveFile()` then `sidecar.exportRecoveryKey(nodeId, path)` → `GET /_supervisor/nodes/:id/recovery-key-exported` (records that export happened, never the key)
- Print → `window.print()`, then confirm "Did the key print correctly?"
- Copy → `navigator.clipboard.writeText(key)`
- Checkbox: "I have safely stored this recovery key"

#### 5b — Encrypted, passphrase mode
No key is displayed. The user already knows the passphrase. Checkbox: "I have written down my passphrase somewhere safe." Note about keychain auto-unlock on this machine vs passphrase required elsewhere.

#### 5c — Unencrypted
No key, no checkbox needed. Node starts finding peers immediately.

**Validation (`canAdvance()` for step 5):**
- Generated key: `recoveryExported && recoveryConfirmed` (must save/print/copy AND check the box)
- Passphrase: `recoveryConfirmed` (must check the box)
- Unencrypted: `recoveryExported` (set to true at creation time since `result.recovery_key === null`)

**Backend / API calls:**
- `sidecar.exportRecoveryKey(nodeId, path)` → Tauri command `export_recovery_key` — writes key to user-chosen file, then drops it from memory (`take_recovery_key`)
- `api.recordRecoveryKeyExport(nodeId)` → `POST /_supervisor/nodes/:node_id/recovery-key-exported` — records custody fact in supervisor

**State transitions:**
- `draft.recoveryExported = true` after save/print/copy
- `draft.recoveryConfirmed = true` after checkbox
- Clicking "Done" closes the wizard (`close()`)
- If closing without export: warning modal ("Recovery key not exported" / "Passphrase not confirmed") — user can still close, but is warned the node cannot be recovered

---

## Backend: `create_node` Tauri Command

**File:** `app/src-tauri/src/commands.rs` (lines 377–709)  
**Command name:** `create_node`  
**Request type:** `CreateNodeRequest` (lines 322–357)  
**Result type:** `CreateNodeResult` (lines 367–375)

### Flow

1. **Adopt shortcut:** If `adopt == true`, delegates to `start_existing_node()` (reads existing `node.json`, re-allocates ports, starts the process). Returns with `recovery_key: None`.

2. **Mesh secret validation:** If provided, must be 64 hex chars. If different from current `DESENTRY_CLUSTER_SECRET`, stores it in the OS keychain and restarts all running data nodes onto the new mesh.

3. **Bootstrap peers require mesh secret:** Error if peers are given without a mesh code.

4. **Port allocation:** `state.allocate_ports()` — reserves api/p2p/discovery port triple.

5. **Key mode resolution:**
   - `encrypt_at_rest && key_mode == "passphrase"` → `"passphrase"`
   - Everything else → `"generated"`

6. **Key material:**
   - **Passphrase mode:** `passphrase::validate()` (min length, match), derive KEK via PBKDF2, generate random DEK, wrap DEK with KEK (AES-GCM), store salt + wrapped DEK + tag in config. Recovery key = DEK encoded as Crockford base32 (shown to user as the unlock secret, but `create_node` returns `recovery_key: None` for passphrase mode so the wizard shows the passphrase screen instead).
   - **Generated mode:** `recovery::generate()` creates a 256-bit recovery key. Stored in keychain (if not removable and keychain requested) and held in memory for one-time export.

7. **Config write:** `NodeConfigSpec` written to `node.json` in the data directory. Includes quota split, engines, replication factor (1 for standalone, else from spec), encryption, key mode, KDF params, bootstrap peers.

8. **Preallocation:** If `preallocate && quota_mb > 0`, creates `storage.reserved` file of `quota_mb * 1024 * 1024` bytes.

9. **Process start:** `state.start_node(LaunchSpec)` spawns `desentryd` with the config.

10. **Keychain storage:** For generated keys on non-removable nodes with keychain requested: `keychain::store(keychain_ref, key)`. Config updated with `keychain_ref`. Recovery key held in memory via `state.hold_recovery_key(node_id, key)`.

11. **Manifest write:** `manifest.json` written with version 2, node id, name, engines, quota, description, sizing decision. Non-fatal if it fails (logged as warning).

12. **Event emission:** `SidecarEvent::NodeState` emitted so the UI sidebar updates.

13. **Return:** `CreateNodeResult { node, recovery_key, keychain_ref, key_mode }`.

### Rollback

`rollback_create()` (lines 797–837) undoes any partial creation: stops the node, releases ports, forgets keychain entries, removes `node.json`, removes `storage.reserved`, removes `manifest.json`, and removes the data directory **only if it was created by this attempt and is empty** (ghost-directory rule — never deletes a user's pre-existing directory).

### Error conditions

| Condition | Error message |
|-----------|---------------|
| Empty node name | "a node needs a name" |
| Bad mesh secret format | "the mesh code must be the 64-character code shown on the device you are joining" |
| Mesh secret mismatch | (stored in keychain + env, nodes restarted) |
| Peers without mesh secret | "enter the mesh code from the device you are joining as well as its peer address" |
| Port allocation failure | from `state.allocate_ports()` |
| Config write failure | "could not write …node.json: …" |
| Process start failure | from `state.start_node()` |
| Keychain store failure | from `keychain::store()` |
| Passphrase too short | "choose a passphrase with at least N characters, then confirm it matches" |
| Passphrase mismatch | "the two passphrases do not match — retype both" |

---

## Cross-Reference: Supervisor API Endpoints

The wizard drives node creation through a mix of **Tauri sidecar commands** (local machine operations) and **supervisor HTTP API calls** (control plane). The supervisor API endpoints verified in installer E2E (`STATUS-installer-e2e.md`) are:

| Endpoint | Method | Wizard usage | E2E verified |
|----------|--------|--------------|--------------|
| `/_supervisor/scan` | GET | Step 1: discover storage candidates | Not in E2E table |
| `/_supervisor/mounts` | GET | Step 1: list mount points | Not in E2E table |
| `/_supervisor/inspect` | GET | Step 1: inspect a user-picked folder | Not in E2E table |
| `/_supervisor/nodes` | PUT | Not called by wizard directly (create_node is a Tauri command, not an HTTP call) | ✅ 200 — creates node entry |
| `/_supervisor/nodes/:id/transition` | POST | Not called by wizard directly | ✅ 200 — lifecycle transitions |
| `/_supervisor/nodes/:id/recovery-key-exported` | POST | Step 5: record that the user exported the key | Not in E2E table |
| `/_supervisor/nodes/:id` | DELETE | Not called by wizard | ✅ 200 — removes node entry |
| `/_supervisor/topology` | GET | Post-creation refresh (`refreshTopology()`) | ✅ 200 |
| `/_engines` | GET | Step 3: engine list for sizing (via `engine_names()` Rust helper) | Not in E2E table |

**Note on API auth:** All supervisor endpoints require `Authorization: Bearer <token>` (per-boot token, C-8 security control). The Tauri sidecar attaches this automatically on direct `desentryd` calls; the wizard's supervisor HTTP calls go through `apiFor(port)` which handles the token.

### How the wizard maps to the verified lifecycle

The E2E verification tested the supervisor lifecycle manually:
```
PUT /_supervisor/nodes/e2e-node-002 -> 200 (allocated)
POST /_supervisor/nodes/e2e-node-002/transition -> 200 (provisioned)
POST /_supervisor/nodes/e2e-node-002/transition -> 200 (running)
POST /_supervisor/nodes/e2e-node-002/transition -> 200 (degraded)
POST /_supervisor/nodes/e2e-node-002/transition -> 200 (reclaimed)
DELETE /_supervisor/nodes/e2e-node-002 -> 200
```

The wizard's `create_node` command performs the equivalent of **allocate + provision + start** in a single Tauri call (ports allocated, config written, process spawned). The supervisor transition endpoints are available for operational lifecycle management after creation but are not part of the wizard flow itself.

---

## Manual Verification Checklist

Use this to verify the wizard works correctly after installation:

### Prerequisites
- [ ] De-Sentry app installed and launched
- [ ] Supervisor running (API on 127.0.0.1:7701)
- [ ] Airplane mode (no network dependencies)

### Step 1 — Placement
- [ ] "Rescan" discovers folders on the device
- [ ] "Choose a folder…" opens the OS picker and selects a folder
- [ ] Selected folder shows as highlighted with free bytes
- [ ] Cannot advance without selecting a folder (Next disabled)

### Step 2 — Shape
- [ ] Node name defaults to folder name; editable
- [ ] Standalone vs Mesh toggle works; Mesh shows "Replicated" badge
- [ ] Quota min 64 MiB, step 64; cannot exceed free space
- [ ] Preallocate disabled when quota exceeds free space
- [ ] Encrypt toggle shows/hides keychain and key-mode options
- [ ] Keychain checkbox disabled for removable media
- [ ] Generated vs Passphrase radio switches UI
- [ ] Passphrase meter shows weak/fair/strong/excellent
- [ ] Weak passphrase requires acknowledgement checkbox
- [ ] Description suggestions insert text when clicked

### Step 3 — Proposal
- [ ] "Propose a shape with AI" runs sizing (15s timeout)
- [ ] Confidence bar shows; below 0.35 shows warning
- [ ] Keyword fallback noted when ONNX model unavailable
- [ ] Clarifying questions appear on low confidence; answering re-runs sizing
- [ ] Engine picker toggles engines; opt-in engines show rebuild hint
- [ ] Quota split bars show percentages and byte amounts
- [ ] Collections list matches selected engines

### Step 4 — Review
- [ ] Summary shows all settings correctly
- [ ] "Create node" button disabled during creation
- [ ] Passphrase validation errors return to step 2 (not step 4)
- [ ] On success: advances to step 5; node appears in sidebar
- [ ] On failure: error shown, stays on step 4

### Step 5 — Recovery Key
- [ ] **Generated:** Key shown once (text + QR + constellation)
- [ ] **Generated:** Save to file works; key exported; checkbox enables Done
- [ ] **Generated:** Print works; "Did it print?" confirmation sets exported
- [ ] **Generated:** Copy works; sets exported
- [ ] **Generated:** Verify constellation game works (optional)
- [ ] **Generated:** Done disabled until exported + confirmed
- [ ] **Passphrase:** No key shown; checkbox "I have written down my passphrase" enables Done
- [ ] **Unencrypted:** No key, no checkbox; Done enabled immediately
- [ ] Closing without export shows warning modal but allows exit

### Post-Creation
- [ ] Node appears in sidebar with correct name and status
- [ ] Node data directory exists on disk with `node.json` and `manifest.json`
- [ ] Supervisor topology reflects the new node
- [ ] Recovery key export recorded by supervisor (if applicable)

---

## Files Referenced

| File | Role |
|------|------|
| `app/src/views/wizard.ts` | Wizard UI: 5 steps, state machine, validation, rendering |
| `app/src/bridge.ts` | Tauri invoke boundary; `CreateNodeRequest`, `CreateNodeResult`, `NodeSpec` types |
| `app/src/api.ts` | HTTP client; `apiFor()`, supervisor endpoints (`scan`, `mounts`, `inspect`, `recordRecoveryKeyExport`, `topology`) |
| `app/src-tauri/src/commands.rs` | `create_node` command (lines 377–709), `rollback_create`, key generation, keychain storage |
| `app/src-tauri/src/ai.rs` | `size_workload` command, ONNX model, keyword fallback, `CONFIDENCE_FLOOR = 0.35` |
| `app/src-tauri/src/passphrase.rs` | Passphrase validation, KEK derivation, DEK wrapping |
| `app/src-tauri/src/recovery.rs` | Recovery key generation, encoding, export |
| `app/src-tauri/src/keychain.rs` | OS keychain storage (Windows Credential Manager / macOS Keychain / Linux Secret Service) |
| `src/supervisor/supervisor_routes.cpp` | Supervisor HTTP endpoints: `/_supervisor/scan`, `/_supervisor/mounts`, `/_supervisor/inspect`, `/_supervisor/nodes`, `/_supervisor/nodes/:id/transition`, `/_supervisor/nodes/:id/recovery-key-exported`, `/_supervisor/topology` |
| `STATUS-installer-e2e.md` | Installer E2E verification results (supervisor API endpoints verified) |
