"""End-to-end integration test for a running De-Sentry cluster.

Spawns its own 3-node P2P cluster and asserts the behaviours that actually
matter for a decentralized database:

  1. Cross-node replication  -- a write on one node shows up on the others.
  2. CRDT convergence        -- concurrent conflicting writes on two nodes
                               settle to the *same* document on every node.
  3. Tamper-evident ledger   -- every node's hash-chained audit ledger
                               verifies cleanly.
  4. Peer membership         -- each node is aware of its siblings.
  5. Tombstone propagation    -- a delete on A is observed on B and C.

Run it standalone:
    python3 tests/integration/cluster_integration_test.py

Requires a built `desentryd` (see harness.find_engine for where it looks).
"""

from __future__ import annotations

import sys
import time

from harness import Cluster, Report  # noqa: E402

COLLECTION = "users"


def main() -> int:
    report = Report("cluster_integration_test")

    with Cluster() as cluster:
        # Spawn 3 data nodes
        a = cluster.add("node-a")
        b = cluster.add("node-b")
        c = cluster.add("node-c")
        cluster.start_all()

        print("[1] waiting for all 3 nodes to be reachable")
        for name, node in [("A", a), ("B", b), ("C", c)]:
            report.check(node.running(), f"node {name} is up")

        # --- 1. Cross-node replication -------------------------------------------
        print("[2] test 1: write on A replicates to B and C")
        a.client.put(COLLECTION, "u1", {"name": "Asha", "role": "admin"})
        cluster.wait_visible(COLLECTION, "u1", nodes=[b, c])
        report.check(b.client.get(COLLECTION, "u1") == {"name": "Asha", "role": "admin"},
                     "B replica of u1")
        report.check(c.client.get(COLLECTION, "u1") == {"name": "Asha", "role": "admin"},
                     "C replica of u1")

        # --- 2. CRDT convergence under concurrent divergent writes --------------
        print("[3] test 2: concurrent writes on B and C converge everywhere")
        b.client.put(COLLECTION, "u1", {"name": "Asha", "role": "admin", "dept": "eng"})
        c.client.put(COLLECTION, "u1", {"name": "Asha Khan", "role": "admin"})

        cluster.wait_converged()
        # Allow a brief settling period for CRDT merges to propagate after checksum convergence
        time.sleep(2.0)
        va = a.client.get(COLLECTION, "u1")
        vb = b.client.get(COLLECTION, "u1")
        vc = c.client.get(COLLECTION, "u1")
        if not (va == vb == vc):
            print(f"  A: {va}")
            print(f"  B: {vb}")
            print(f"  C: {vc}")
        report.check(va == vb == vc, "all nodes converged to the same document")
        print(f"  converged document: {va}")

        # --- 3. Tamper-evident ledger --------------------------------------------
        print("[4] test 3: every node's audit ledger verifies")
        for name, node in [("A", a), ("B", b), ("C", c)]:
            result = node.client.verify_ledger()
            report.check(result.get("verified") is True, f"node {name} ledger verified")
            report.check(isinstance(result.get("entries_checked"), int) and result["entries_checked"] > 0,
                         f"node {name} ledger non-empty")

        # --- 4. Peer membership --------------------------------------------------
        print("[5] test 4: each node knows its peers")
        for name, node in [("A", a), ("B", b), ("C", c)]:
            peers = node.client.peers()
            report.check(isinstance(peers, list) and len(peers) >= 1, f"node {name} has peers")
            print(f"  ok: node {name} sees {len(peers)} peer(s)")

        # --- 5. Tombstone propagation --------------------------------------------
        print("[6] test 5: delete on A is observed on B and C")
        a.client.delete(COLLECTION, "u1")
        cluster.wait_for(lambda: _get(b.client, COLLECTION, "u1") is None, 30.0, "B tombstoned u1")
        cluster.wait_for(lambda: _get(c.client, COLLECTION, "u1") is None, 30.0, "C tombstoned u1")
        report.check(_get(b.client, COLLECTION, "u1") is None, "B sees delete")
        report.check(_get(c.client, COLLECTION, "u1") is None, "C sees delete")

    print("\n[integration] ALL INTEGRATION TESTS PASSED")
    return report.finish()


def _get(client, collection: str, key: str):
    """Return the document dict, or None if 404 (deleted/never written)."""
    try:
        return client.get(collection, key)
    except Exception as exc:  # noqa: E722
        # Check if it's a 404 error by looking at the exception
        if hasattr(exc, 'status') and exc.status == 404:
            return None
        raise


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, TimeoutError) as exc:
        print(f"\n[integration] FAILED: {exc}")
        sys.exit(1)
