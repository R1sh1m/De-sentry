"""End-to-end integration test for a running De-Sentry cluster.

Drives a live 3-node P2P cluster through the public REST API (via the
zero-dependency Python client) and asserts the behaviours that actually
matter for a decentralized database:

  1. Cross-node replication  -- a write on one node shows up on the others.
  2. CRDT convergence        -- concurrent conflicting writes on two nodes
                               settle to the *same* document on every node.
  3. Tamper-evident ledger   -- every node's hash-chained audit ledger
                               verifies cleanly.
  4. Peer membership         -- each node is aware of its siblings.
  5. Tombstone propagation    -- a delete replicates and is observed everywhere.

Run against a cluster:
    python3 cluster_integration_test.py
        [--node-a URL] [--node-b URL] [--node-c URL]

Or via docker compose:
    docker compose run tester

Stdlib only -- same zero-dependency philosophy as desentry_client.py.
"""

from __future__ import annotations

import argparse
import os
import sys
import time

# Make the bundled client importable regardless of CWD.
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "clients", "python"))

from desentry_client import DesentryClient, DesentryError  # noqa: E402

POLL_INTERVAL_S = 0.5
READY_TIMEOUT_S = 40.0
SETTLE_TIMEOUT_S = 30.0


def wait_until(predicate, timeout_s: float, label: str):
    """Poll predicate until true; raise on timeout."""
    deadline = time.time() + timeout_s
    last_err = None
    while time.time() < deadline:
        try:
            if predicate():
                return
        except Exception as exc:  # network not up yet, etc.
            last_err = exc
        time.sleep(POLL_INTERVAL_S)
    raise TimeoutError(f"timed out waiting for: {label} (last error: {last_err})")


def assert_eq(a, b, label: str):
    if a != b:
        raise AssertionError(f"{label}: expected {b!r}, got {a!r}")
    print(f"  ok: {label}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--node-a", default=os.environ.get("NODE_A_URL", "http://127.0.0.1:7701"))
    ap.add_argument("--node-b", default=os.environ.get("NODE_B_URL", "http://127.0.0.1:7702"))
    ap.add_argument("--node-c", default=os.environ.get("NODE_C_URL", "http://127.0.0.1:7703"))
    args = ap.parse_args()

    a = DesentryClient(args.node_a)
    b = DesentryClient(args.node_b)
    c = DesentryClient(args.node_c)
    nodes = {"A": a, "B": b, "C": c}

    print("[integration] waiting for all 3 nodes to be reachable ...")
    for name, node in nodes.items():
        wait_until(lambda: node.status() is not None, READY_TIMEOUT_S,
                   f"node {name} /_status")
        print(f"  ok: node {name} is up")

    # --- 1. Cross-node replication -------------------------------------------
    print("[integration] test 1: write on A replicates to B and C")
    a.put("users", "u1", {"name": "Asha", "role": "admin"})
    wait_until(lambda: _get(b, "users", "u1") == {"name": "Asha", "role": "admin"},
               SETTLE_TIMEOUT_S, "B has u1")
    wait_until(lambda: _get(c, "users", "u1") == {"name": "Asha", "role": "admin"},
               SETTLE_TIMEOUT_S, "C has u1")
    assert_eq(_get(b, "users", "u1"), {"name": "Asha", "role": "admin"}, "B replica of u1")
    assert_eq(_get(c, "users", "u1"), {"name": "Asha", "role": "admin"}, "C replica of u1")

    # --- 2. CRDT convergence under concurrent divergent writes --------------
    print("[integration] test 2: concurrent writes on B and C converge everywhere")
    b.put("users", "u1", {"name": "Asha", "role": "admin", "dept": "eng"})
    c.put("users", "u1", {"name": "Asha Khan", "role": "admin"})

    def converged():
        va = _get(a, "users", "u1")
        vb = _get(b, "users", "u1")
        vc = _get(c, "users", "u1")
        return va is not None and vb is not None and vc is not None and va == vb == vc

    wait_until(converged, SETTLE_TIMEOUT_S, "all nodes agree on u1")
    va = _get(a, "users", "u1")
    assert_eq(_get(b, "users", "u1"), va, "B converged value")
    assert_eq(_get(c, "users", "u1"), va, "C converged value")
    print(f"  converged document: {va}")

    # --- 3. Tamper-evident ledger --------------------------------------------
    print("[integration] test 3: every node's audit ledger verifies")
    for name, node in nodes.items():
        result = node.verify_ledger()
        assert_eq(result.get("verified"), True, f"node {name} ledger verified")
        assert isinstance(result.get("entries_checked"), int) and result["entries_checked"] > 0, \
            f"node {name} ledger non-empty"

    # --- 4. Peer membership --------------------------------------------------
    print("[integration] test 4: each node knows its peers")
    for name, node in nodes.items():
        peers = node.peers()
        assert isinstance(peers, list) and len(peers) >= 1, f"node {name} has peers"
        print(f"  ok: node {name} sees {len(peers)} peer(s)")

    # --- 5. Tombstone propagation --------------------------------------------
    print("[integration] test 5: delete on A is observed on B and C")
    a.delete("users", "u1")
    wait_until(lambda: _get(b, "users", "u1") is None, SETTLE_TIMEOUT_S, "B tombstoned u1")
    wait_until(lambda: _get(c, "users", "u1") is None, SETTLE_TIMEOUT_S, "C tombstoned u1")
    assert_eq(_get(b, "users", "u1"), None, "B sees delete")
    assert_eq(_get(c, "users", "u1"), None, "C sees delete")

    print("\n[integration] ALL INTEGRATION TESTS PASSED")
    return 0


def _get(node: DesentryClient, collection: str, key: str):
    """Return the document dict, or None if 404 (deleted/never written)."""
    try:
        return node.get(collection, key)
    except DesentryError as exc:
        if exc.status == 404:
            return None
        raise


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, TimeoutError) as exc:
        print(f"\n[integration] FAILED: {exc}")
        sys.exit(1)
