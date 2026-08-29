# De-Sentry

A decentralized, fully peer-to-peer database engine. No client-server split:
every node is simultaneously a storage engine, a REST API server for local
applications, and a P2P peer that gossips writes to every other node. There
is no coordinator, no leader, and no single point of failure by design.

Built in C++17 from first principles, the way real database engines are
built: a hand-written paged storage layer with a write-ahead log and a
disk-backed B+Tree (not a wrapper around SQLite/RocksDB/LMDB), a CRDT
document model for conflict-free multi-writer replication, and a from-scratch
encrypted P2P transport — no networking or database framework dependencies.
The only third-party dependency is OpenSSL (for Ed25519/X25519/AES-GCM).

See `docs/ARCHITECTURE.md` for the full design writeup: goals and
non-goals, prior-art comparison, the CRDT consistency model, storage engine
internals, the network protocol, the security model, and the hash-chained
audit ledger. See `docs/COMPARISON.md` for how this design was compared
against a second, independently-developed De-Sentry design (ownership
partitioning + a routing coordinator) and what was adopted from it.

## Quickstart

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
make -j"$(nproc)"
ctest --output-on-failure   # 4 suites: crdt, crypto, storage, network
```

Run a 3-node local cluster and watch a write on one node replicate to the
other two over the real encrypted P2P network:

```bash
cd ..
./scripts/run_cluster.sh 3

curl -s -X PUT http://127.0.0.1:7701/db/users/u1 -d '{"name":"Asha","role":"admin"}'
sleep 1
curl -s http://127.0.0.1:7702/db/users/u1   # -> {"name":"Asha","role":"admin"}
curl -s http://127.0.0.1:7703/db/users/u1   # -> {"name":"Asha","role":"admin"}

./scripts/stop_cluster.sh
```

Or use the CLI instead of curl:

```bash
./build/desentry_cli --api 127.0.0.1:7701 put users u1 '{"name":"Asha"}'
./build/desentry_cli --api 127.0.0.1:7702 get users u1
./build/desentry_cli --api 127.0.0.1:7701 status
```

Or use the zero-dependency Python client:

```python
from clients.python.desentry_client import DesentryClient
node = DesentryClient("http://127.0.0.1:7701")
node.put("users", "u1", {"name": "Asha"})
print(node.get("users", "u1"))
print(node.verify_ledger())   # {"verified": true, "entries_checked": N}
```

Or open `tools/dashboard.html` directly in a browser (no build step, no
server) and point it at a running node's API address to see live node
status, the signed ledger tip, per-collection checksums, known peers, and
a scrolling ledger-entry table, with a one-click ledger verification
button.

## REST API (served locally by every node)

| Method | Path                       | Purpose                              |
|--------|----------------------------|---------------------------------------|
| PUT    | `/db/:collection/:key`     | Upsert a document (JSON body)        |
| GET    | `/db/:collection/:key`     | Fetch a document                     |
| DELETE | `/db/:collection/:key`     | Delete a document (tombstone)        |
| GET    | `/db/:collection`          | List/scan a collection               |
| PUT    | `/_schema/:collection`     | Set a JSON-Schema-subset validator   |
| GET    | `/_schema/:collection`     | Read a collection's schema           |
| GET    | `/_collections`            | List known collections               |
| GET    | `/_peers`                  | List known P2P peers                 |
| GET    | `/_status`                 | Node identity, uptime, peer count    |
| GET    | `/_brain`                  | Compact snapshot: signed ledger tip, per-collection checksums, peers |
| GET    | `/_ledger/tip`              | Current hash-chain tip + Ed25519 signature over it |
| GET    | `/_ledger/entries?from=&to=`| A bounded page of ledger entries, for replay/audit |
| POST   | `/_ledger/verify`           | Full re-verification of the hash chain; reports the first broken entry, if any |

Every write accepted on any node's `/db/...` endpoint is broadcast to that
node's connected peers and merged with a CRDT so all nodes converge to the
same value without coordination, even under concurrent conflicting writes.
Every write is also appended to a SHA-256 hash-chained, Ed25519-signed
audit ledger (`docs/ARCHITECTURE.md` §7.6) — a tamper-evident history of
every mutation the node has ever made.

The API is CORS-enabled (loopback-only by default, no session auth to
leak) so browser-based tools — including `tools/dashboard.html` below —
can call it directly from a `file://` page with no proxy.

## Repo layout

```
include/desentry/   public headers, organized by layer (common, storage,
                     crdt, security, net, engine, api)
src/                 implementation, mirrors include/
apps/desentry_node/  the desentryd daemon entrypoint
apps/desentry_cli/   thin CLI client
clients/python/      zero-dependency Python client over the REST API
tools/dashboard.html dependency-free browser dashboard (status, ledger, peers)
tests/               assert-based test suites (crdt, crypto, storage, network)
docs/ARCHITECTURE.md full architecture and design-rationale document
docs/COMPARISON.md   comparison against an alternate consortium/routing design
config/              example node config
scripts/             run_cluster.sh / stop_cluster.sh for local demos
```

## Status

MVP scope, built for a course project deadline as a local multi-node
simulation (real OS processes, real sockets, one host). All four test
suites pass with live assertions (`-UNDEBUG`, see `CMakeLists.txt`), and the
full stack has been verified end-to-end with real compiled binaries, not
just unit tests. Known limitations and the reasoning behind every major
design decision are documented in `docs/ARCHITECTURE.md`, including what is
explicitly out of scope for the MVP (e.g. multi-hop relay beyond one gossip
hop, page-level torn-write detection during B+Tree splits).
