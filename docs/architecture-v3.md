---
title: De-Sentry v3 — local AI, adaptive advisor, control-room overhaul
status: in progress
branch: feat/v3-local-ai-overhaul
---

# Architecture v3

v2 built the engine and the control room. v3 makes the local AI the centre of
the product and removes the friction around it: a sizing step that reasons and
asks instead of giving up, an assistant that can answer and act, logs you can
actually see, passwords that actually gate something, pairing that actually
pairs, and a UI that moves forward on its own whenever it can.

This document is the **contract** the v3 workstreams build against. Types and
command signatures named here already exist in the scaffold commit; a
workstream fills in bodies, it does not change shapes without changing this
document and both sides of the bridge.

---

## 0. What was actually wrong

Found by reading and running the tree, not assumed:

| Problem | Evidence |
| --- | --- |
| Sizing confidence structurally cannot clear its floor on real workloads | `ai.rs` softmaxes one score per *exclusive* prototype. Mixtures ("users, orders, and a revenue dashboard") split probability across shapes, so the top-1 lands under `CONFIDENCE_FLOOR = 0.35`. |
| Clarifying questions do not adapt | Hand-written templates for five fixed prototype pairs (`question_for_pair`). Answers are applied by appending text and re-embedding (`wizard.ts:631`), so an answer can fail to move the score at all. |
| The ONNX runtime never installed on Windows | `fetch-model.mjs` runs `tar`; under Git Bash that is GNU tar, which reads `C:\...` as a remote host. It then prints "Done" and exits 0. The app silently ran on keyword fallback. |
| Unlock rejects passwords | `commands.rs` `unlock_node` runs `recovery::decode(&password)` first; anything that is not a recovery key is refused, while the modal asks for a "passphrase". |
| "Encrypt at rest" protects nothing | `apps/desentry_node/main.cpp` logs `encrypt_at_rest=true is NOT YET ENFORCED`. The secret piped to stdin is never read. |
| App logs are discarded | The sidecar calls `log::warn!`/`log::error!` throughout; no logger is ever installed. |
| Pairing is a dead end | Nothing consumes the pairing code (no scan, paste or join path), and it advertised the *supervisor*, whose P2P binds to loopback. |
| QR not centred | `.qr { display: block }` inside the sheet body with no centring. |

---

## 1. Decisions and trade-offs

**Zero network at run time stays absolute.** The assistant is built-in and
offline. If an Ollama-compatible server is *already running* on 127.0.0.1 it
is used for free-form answers; the app never downloads a model and never
reaches the internet. A cloud LLM was rejected: it breaks the airplane-mode
acceptance test, the loopback-only CSP, and it would ship node logs off the
machine.

**Passwords are verified access control, not encryption.** The engine does not
encrypt at rest, and building page-level AES-GCM across ~14 storage files plus
a migration is its own project. v3 makes passwords *real and honest*: an
Argon2id verifier gates starting a node, the recovery key is an alternative
unlock, and "remember on this device" uses the OS keychain. The UI must never
call a node "encrypted". Engine encryption is tracked in §12.

**The UI proceeds automatically.** Anything harmless happens without a
confirmation: the advisor auto-advances when `ready`, the assistant performs
`auto` actions, a remembered-password node starts itself, the default node
exists on first launch. The user is asked only for (a) information the system
cannot infer and (b) consent to destructive actions.

**Dark mode only.** One palette, no toggle. Printing a recovery key still
forces a light, ink-on-paper page.

**At least one data node always exists.** On startup, if the app manages no
data node, it creates `default` in the app data directory.

---

## 2. Workstreams and ownership

Each workstream runs in its own git worktree off the scaffold commit and owns
its files exclusively. Nothing outside the "owns" column is edited; if a change
is needed elsewhere it goes in the workstream's report for integration.

| Workstream | Owns | Reads / calls |
| --- | --- | --- |
| **advisor** | `src-tauri/src/advisor.rs`, `src-tauri/src/ai.rs`, `resources/facets.json`, `resources/prototypes.json` | `commands::engine_names` |
| **assistant** | `src-tauri/src/assistant.rs`, `src-tauri/src/http.rs`, `resources/knowledge/**` | `advisor::{start,answer}`, `applog::search`, `AppState` |
| **platform** | `applog.rs`, `password.rs`, `pairing.rs`, `commands.rs`, `nodes.rs`, `appstate.rs`, `lib.rs`, `keychain.rs`, `recovery.rs`, `configgen.rs`, `Cargo.toml`, `app/scripts/**` | `http::post_json` |
| **engine** | `src/**`, `include/**`, `tests/*.cpp`, `tests/integration/**`, `apps/**`, `CMakeLists.txt`, `docs/engine-audit.md` | — |
| **frontend** | `app/src/**`, `app/index.html`, `app/tests/**`, `app/package.json`, `DESIGN.md` | `bridge.ts` contract |

Integration (merge, cross-cutting fixes, end-to-end verification, docs) is done
after all five land.

---

## 3. Adaptive advisor

`advisor.rs`. Commands: `advisor_start`, `advisor_answer`, `advisor_refine`,
`advisor_set_engines`. Public functions of the same names are used by the
assistant.

### 3.1 Facets

Independent dimensions, each a distribution over values:

| Facet | Values |
| --- | --- |
| `data_model` | relational, document, time_series, vector, graph, events (columnar analytics), blob |
| `access` | point_lookup, range_scan, aggregation, similarity, traversal |
| `writes` | append_heavy, update_heavy, read_mostly, balanced |
| `consistency` | strict (constraints, joins, transactions), eventual_ok |
| `retention` | short (days), medium (months), long (years), forever |
| `scale` | small, medium, large |

`data_model` is **multi-label**: each value holds an independent presence
probability, because a workload can be relational *and* time-series. Every
other facet is a single categorical distribution.

### 3.2 Evidence

1. **Clauses.** The description is split into clauses (sentences, `;`, `and`
   between noun phrases). Each clause is scored separately, so a mixture
   contributes evidence to several values instead of being averaged into mush.
2. **Embeddings.** When the model is loaded, each clause is embedded
   (`Sizer::embed_texts`) and compared by cosine to per-value exemplar centroids
   from `facets.json`. Similarities become likelihoods through a per-facet
   temperature.
3. **Keywords.** Whole-word matches with weights from `facets.json`. Always
   available; the only source when the model is not.
4. **Answers.** A chosen option applies a strong likelihood (calibrated so one
   answer takes a facet from uniform to ≥ 0.85 on the chosen value). A skip
   applies nothing and marks the facet as not worth asking again.
5. **Priors.** Mild, from `facets.json` (e.g. `consistency.eventual_ok` is the
   CRDT default).

Posterior per facet = normalise(prior × Π likelihoods). Evidence strings record
the clause span or answer that moved each value, and feed engine reasons.

### 3.3 Engine utility

`facets.json` holds a capability matrix `capability[engine][facet][value] ∈
[0,1]`. For each available engine:

```
U(engine) = Σ_facet  w_facet · Σ_value  P(facet = value) · capability[engine][facet][value]
```

The **primary** engine is argmax U. **Secondary** engines are added when a
`data_model` value with presence ≥ 0.35 is served markedly better by another
engine than by the primary. Unavailable engines are scored but never selected,
and are reported with `available: false` so the UI can say why.

### 3.4 Decision confidence

The probability that the primary choice is right given current uncertainty,
estimated by sampling: draw N = 256 joint facet assignments from the posteriors
(deterministic seed from the session id, so results are reproducible), compute
the argmax engine for each, report the fraction matching the primary. This is
calibrated in a way a softmax share is not: it is high when every plausible
reading of the workload leads to the same engine, even if the facets
themselves are still uncertain.

### 3.5 Choosing the next question

For each facet not yet answered or skipped, expected confidence after asking:

```
E[conf | ask f] = Σ_value  P(f = value) · confidence(posterior with f fixed to value)
gain(f) = E[conf | ask f] − confidence
```

Ask the facet with the highest gain, if `confidence < target_confidence`, gain
≥ 0.02, and fewer than `MAX_QUESTIONS` were asked. Otherwise `ready = true`.

Options are the facet's values with probability ≥ 0.05, most likely first,
2–5 of them. The prompt comes from a per-facet template in `facets.json`,
filled with nouns found in the description ("You mentioned *orders* — will you
mostly fetch one order at a time, or report across many?"). `why` names the
engines the answer decides between.

### 3.6 Output

`AdvisorState` (see `advisor.rs`). `summary` is two or three plain sentences.
`suggested_name` is a slug from the description's head noun phrase.
`spec: NodeSpec` stays a drop-in for `create_node`, and `spec.decision` keeps
the audit record written into the node manifest (method, confidence, facets,
answers).

### 3.7 Acceptance

* Unit tests run without the model (keyword evidence) and, under
  `--features onnx`, with it.
* A fixed suite of at least 25 descriptions — single-shape, mixtures, vague,
  and adversarial wording — each asserts the primary engine and that
  confidence reaches the target within `MAX_QUESTIONS` when answers are chosen
  to match the intended workload.
* Mixtures produce a primary plus the right secondary (e.g. orders + daily
  revenue dashboard → relational primary, `columnar_lite` secondary).
* No question is asked twice; a skipped facet is never re-asked.
* Deterministic: the same inputs produce the same state.

---

## 4. Assistant

`assistant.rs`. Commands: `assistant_send`, `assistant_confirm`,
`assistant_status`, `assistant_reset`.

### 4.1 Intents

Recognised by cosine similarity to intent exemplars (model) or keywords
(fallback), plus entity extraction for node names/ids, levels, time windows.

| Intent | Does |
| --- | --- |
| `status` | Summarise the mesh: nodes, running/stopped/locked, reachability. Card `nodes`. |
| `node.detail` | One node's state, ports, engines, recent errors. |
| `logs.errors` / `logs.search` | Query `applog::search`; summarise by source and pattern; card `logs`. |
| `logs.explain` | Match recent error lines against known patterns (bind failure, quota, WAL, handshake, locked) and explain + suggest the fix. |
| `node.start` / `stop` / `restart` / `lock` / `unlock` / `delete` | Propose a `PendingAction`. Stop, lock and delete are destructive. |
| `node.create` | Start an advisor session from the user's words; card `advisor`; answers continue inline via `answer_advisor`; `create_node` action when ready. |
| `pairing` | `open_view pairing`, `auto: true`. |
| `howto` | Retrieve from `resources/knowledge/` (BM25 over `##` sections, embedding rerank when the model is loaded); card `doc`. |
| `smalltalk` / `unknown` | Short answer and suggestions. With a local LLM, `unknown` goes to it with grounded context. |

### 4.2 Actions

Every state change is proposed, never performed silently. `assistant_confirm`
executes an action through the same `AppState` / command paths the window
uses, then replies with the outcome. `auto: true` is only for navigation.

### 4.3 Local LLM

Probed at most once every 60 s: `GET http://127.0.0.1:11434/api/tags`. When
present, free-form turns call `POST /api/chat` (`stream: false`) with a system
prompt carrying the grounded context (node summary, recent errors, retrieved
knowledge) and a hard instruction to answer only from it. The model can suggest
actions only by naming an intent; it never executes anything directly. Replies
are labelled `backend: "local-llm"`.

### 4.4 Acceptance

Unit tests for intent routing (≥ 40 utterances), entity extraction, action
safety (destructive actions always need confirmation), and knowledge retrieval;
all run without the model and without a local LLM.

---

## 5. Logs

`applog.rs`, with `nodes.rs` log capture.

* `applog::init(log_dir)` installs a `log::Log` writing to a 5000-entry ring
  and to `<app_log_dir>/app.log`, rotated at 5 MiB, three files kept.
* Every node line is stamped with `applog::next_seq()` and parsed from the
  engine's format `HH:MM:SS.mmm [target] LEVEL  message` into level and
  target. Unparseable lines are `info` from `stdout`, `warn` from `stderr`.
* `app_logs(query)` merges the app ring and every node ring, filters, orders by
  `seq`, returns the newest `limit`. The window follows a live log by polling
  with `since_seq`.
* `export_logs(path, query)` writes plain text to a path chosen in the save
  dialog.

---

## 6. Per-node passwords

`password.rs`, with create/unlock in `commands.rs` and startup in `lib.rs`.

* `security.json` in the node directory holds Argon2id verifiers (PHC strings)
  for the password and, when issued, the recovery key. Never a secret.
* **Create** with a password: write the verifier; generate a recovery key and
  its verifier; hold the recovery key for the one-time display; if
  `remember_password`, store the password in the keychain.
* **Startup**: a protected node starts automatically only if its password is
  remembered and verifies; otherwise it is registered as `locked` and not
  spawned.
* **Unlock**: accept the password *or* the recovery key, verified against
  `security.json`; `remember` stores the password.
* **Lock**: stop the node; it stays locked until unlocked.
* **Set / change / remove**: `set_node_password`, verifying `current_secret`
  where a password already exists.
* **Existing v2 nodes** created with `encrypt_at_rest` keep working: their
  keychain-held recovery key unlocks them as before.
* `NodeSecurity.encrypted_at_rest` is always `false` until §12 lands.

**Node view contract** (shared by platform and frontend — both implement it):

* `ProcessState` (Rust, `nodes.rs`) gains a `Locked` variant, serialised exactly
  like the existing variants, i.e. the string `"locked"`. A locked node is
  registered and listed but has no process, no ports reserved, and is skipped
  by the watchdog's restart logic.
* `SupervisedNode` gains `protected: bool` (a password is required to start
  it). `bridge.ts` mirrors both: `state` includes `"locked"`, and
  `protected: boolean` is added to the interface.
* The sidebar and inspector show a locked node with a lock badge and an
  **Unlock** action; they never show it as "failed" or "stopped".

---

## 7. Pairing

`pairing.rs`, engine `POST /_peers`.

* **Code**: `DSN1-` + base64url (no padding) of `PairingPayload` JSON
  (`v, id, pk, a, d, n`). The QR encodes exactly this string.
* **Addresses**: non-loopback, non-link-local IPv4 addresses of this machine,
  best first (private LAN ranges before others), each with the advertised data
  node's P2P port; at most four.
* **Join**: decode, refuse a code for a node this app already manages, append
  addresses to `bootstrap_peers` in every local data node's `node.json`,
  `POST /_peers {"address": "ip:port"}` to each running node, trigger
  discovery. Returns `JoinResult` with warnings rather than failing on a
  partially reachable mesh.
* **Engine route** `POST /_peers`: body `{"address": "host:port"}`; adds the
  peer to the peer table and dials it; responds `{"ok": true, "added": bool}`
  (`added` false when already known). 400 on a malformed address.
* **UI**: QR centred with a quiet zone, the code as selectable text with Copy,
  and a "Join another mesh" field that pastes a code and calls `join_mesh`.

---

## 8. Default node

After restoring nodes at startup: if no data node is managed, create `default`
under the app data root with every built-in engine available, `kv` as default
engine, no password, and a manifest recording that it was created
automatically. Idempotent: an existing `default` directory is adopted instead.

---

## 9. Engine audit

Scope for the engine workstream, reported in `docs/engine-audit.md`:

1. `POST /_peers` (§7), with tests.
2. Gossip digest cost: `NodeEngine::LocalDigest` scans a whole collection and
   SHA-256s every document on every gossip round for every peer. Cache per-key
   content hashes on write, or bucket the digest so unchanged ranges are skipped.
3. B+Tree node search is linear over keys; binary search.
4. Anything else found with a measurable effect, each change covered by a test.

`ctest` must stay green; new behaviour gets new tests.

---

## 10. UI overhaul

* **Dark only.** One token set in `:root`; no `data-theme`, no media query, no
  toggle; `meshGlobe` always dark. Print keeps a light override.
* **Header**, minimal: brand · Tree/Mesh/Logs switch · Assistant · Pair ·
  **New node**. Shortcuts: `Ctrl+K` assistant, `Ctrl+L` logs, `Ctrl+N` new node.
* **Create flow** replaces the five-step wizard with one sheet: a single "What
  will this node store?" field; the advisor's recommendation renders inline
  with engines and reasons; questions appear one at a time as option chips with
  their `why`; when `ready` the name, location and Create button are presented
  focused. Options (folder, size, password, remember, removable) sit behind one
  disclosure with sensible defaults. A recovery key appears only when a
  password was set, with Copy/Save and one acknowledgement.
* **Assistant** drawer: conversation, cards, action buttons (destructive in
  red, confirmation inline), suggestion chips, backend badge.
* **Logs** view: source chips, level filter, search, follow, copy, export.
* **Pairing** sheet per §7.
* **Unlock** modal: password or recovery key, "Remember on this device".
* **Node security** card in the inspector: set/change/remove password, lock now.
* **Browser mock bridge** for `vite dev` in a normal browser (`?mock`), so the
  UI can be exercised without the desktop shell. Excluded from production
  builds.
* The `[hidden] { display: none !important }` reset and `npm run check:css`
  stay; the check must not depend on `wizard.ts` existing.

---

## 11. Verification

Per workstream: its own tests green (`cargo test`, `ctest`, `tsc`, `vite
build`, `check:*`). After integration: all of the above on the merged tree,
the Python integration suites, and the app launched end to end — default node
present, create via advisor, assistant status and actions, logs populated,
password create/lock/unlock, pairing code round-trip through `join_mesh`.

---

## 12. Out of scope, tracked

* **Encryption at rest in the engine**: key from the password (Argon2id in the
  app, 32-byte key over stdin), AES-256-GCM per page and per WAL record,
  catalog and side files, migration of plaintext nodes. When it lands,
  `NodeSecurity.encrypted_at_rest` becomes true and the UI may say "encrypted".
* A bundled generative model.
* Camera QR scanning (desktop devices rarely have one pointed at a screen; the
  pasteable code covers the flow).
