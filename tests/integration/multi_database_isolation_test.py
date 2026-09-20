"""Multi-Database Isolation Integration Test (Architecture 1).

Verifies that each node can operate as a specialized standalone database
holding its own unique schema, collections, and data with NO replication
or data leakage across nodes.

Nodes under test:
- Node A: "vector-db" (Specialized in vector embeddings & similarity search)
- Node B: "metrics-db" (Specialized in time-series telemetry & bucket rollups)
- Node C: "documents-db" (Specialized in document & tabular records)
"""

from __future__ import annotations

import json
import os
import sys
import time
from typing import Any, Dict

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(REPO_ROOT, "clients", "python"))
sys.path.insert(0, os.path.join(REPO_ROOT, "tests", "integration"))

from desentry_client import DesentryClient, DesentryError  # noqa: E402
from harness import Cluster, find_engine  # noqa: E402


def log(msg: str):
    print(f"[isolation-test] {msg}")


def assert_true(cond: bool, msg: str):
    if not cond:
        raise AssertionError(f"FAILED: {msg}")
    print(f"  ok: {msg}")


def http_get(client: DesentryClient, path: str) -> Any:
    return client._request("GET", path)


def http_put(client: DesentryClient, path: str, body: Dict[str, Any]) -> Any:
    return client._request("PUT", path, body)


def http_post(client: DesentryClient, path: str, body: Dict[str, Any]) -> Any:
    return client._request("POST", path, body)


def main() -> int:
    engine_path = find_engine()
    log(f"Using engine binary: {engine_path}")

    with Cluster(engine=engine_path) as cluster:
        node_a = cluster.add(
            "vector-db",
            config_overrides={
                "default_engine": "vector_hnsw_lite",
                "engines": ["kv", "vector_hnsw_lite"],
                "discovery_enabled": False,
                "bootstrap_peers": [],
                "replication_factor": 1,
            },
        )
        node_b = cluster.add(
            "metrics-db",
            config_overrides={
                "default_engine": "ts_rollup",
                "engines": ["kv", "ts_rollup"],
                "discovery_enabled": False,
                "bootstrap_peers": [],
                "replication_factor": 1,
            },
        )
        node_c = cluster.add(
            "documents-db",
            config_overrides={
                "default_engine": "columnar_lite",
                "engines": ["kv", "columnar_lite"],
                "discovery_enabled": False,
                "bootstrap_peers": [],
                "replication_factor": 1,
            },
        )

        cluster.start_all(mesh=False)
        node_a = cluster.nodes[0]
        node_b = cluster.nodes[1]
        node_c = cluster.nodes[2]

        client_a = node_a.client
        client_b = node_b.client
        client_c = node_c.client

        # --------------------------------------------------------------------
        # 1. Verify Isolation Baseline (0 peers across all nodes)
        # --------------------------------------------------------------------
        log("1. Verifying zero peer discovery (complete network isolation)...")
        peers_a = client_a.peers()
        peers_b = client_b.peers()
        peers_c = client_c.peers()

        assert_true(len(peers_a) == 0, f"Node A has 0 peers (isolated standalone database)")
        assert_true(len(peers_b) == 0, f"Node B has 0 peers (isolated standalone database)")
        assert_true(len(peers_c) == 0, f"Node C has 0 peers (isolated standalone database)")

        # --------------------------------------------------------------------
        # 2. Populate Node A with Vector Embeddings
        # --------------------------------------------------------------------
        log("2. Setting up Node A as a specialized Vector Database...")
        http_put(client_a, "/_collection/embeddings/engine", {"engine": "vector_hnsw_lite"})

        vectors = [
            ("doc_sports", [0.9, 0.1, 0.0, 0.0], "sports news"),
            ("doc_football", [0.85, 0.15, 0.05, 0.0], "football match"),
            ("doc_politics", [0.05, 0.05, 0.9, 0.1], "election results"),
            ("doc_finance", [0.0, 0.1, 0.2, 0.85], "stock market"),
        ]
        for key, vec, text in vectors:
            client_a.put("embeddings", key, {"vector": vec, "text": text})

        # Run vector search on Node A
        search_res = http_post(client_a, "/_search/vector/embeddings", {
            "vector": [0.88, 0.12, 0.0, 0.0],
            "k": 2
        })
        hits = search_res.get("hits", [])
        assert_true(len(hits) == 2, f"Node A vector search returned 2 nearest neighbors")
        assert_true(hits[0]["key"] == "doc_sports", "Node A top hit matches query")

        # --------------------------------------------------------------------
        # 3. Populate Node B with Time-Series Telemetry
        # --------------------------------------------------------------------
        log("3. Setting up Node B as a specialized Time-Series Database...")
        http_put(client_b, "/_collection/telemetry/engine", {"engine": "ts_rollup"})

        base_time = 1700000000000
        for i in range(12):
            client_b.put("telemetry", f"cpu_load:pt_{i}", {
                "series": "cpu_load",
                "ts": base_time + i * 5000,
                "val": 15.0 + i * 3.5,
            })

        rollups = http_get(client_b, f"/_ts/telemetry/rollups?from_ms={base_time - 1000}&to_ms={base_time + 100000}&bucket_ms=60000")
        assert_true("cpu_load" in rollups, "Node B rollup query returns cpu_load series")
        total_pts = sum(b["count"] for b in rollups["cpu_load"])
        assert_true(total_pts == 12, f"Node B has 12 time-series points aggregated")

        # --------------------------------------------------------------------
        # 4. Populate Node C with Document / Tabular Records
        # --------------------------------------------------------------------
        log("4. Setting up Node C as a specialized Document / Columnar Database...")
        http_put(client_c, "/_collection/analytics/engine", {"engine": "columnar_lite"})

        for i in range(20):
            client_c.put("analytics", f"user_{i:03d}", {
                "user_id": f"u_{i}",
                "tier": "enterprise" if i % 4 == 0 else "free",
                "logins": i * 10,
            })

        user_row = client_c.get("analytics", "user_004")
        assert_true(user_row.get("tier") == "enterprise" and user_row.get("logins") == 40, "Node C read back tabular row")

        # --------------------------------------------------------------------
        # 5. Strict Cross-Node Data Leakage Audit
        # --------------------------------------------------------------------
        log("5. Auditing Cross-Node Data Leakage (Strict Zero-Replication)...")
        # Give time to ensure no background thread or gossip leaked anything
        time.sleep(1.5)

        # Node A collections: must only have 'embeddings' (and default 'kv_test' if touched)
        colls_a = client_a.collections()
        assert_true("embeddings" in colls_a, "Node A has 'embeddings'")
        assert_true("telemetry" not in colls_a, "Node A DOES NOT have Node B's 'telemetry' collection")
        assert_true("analytics" not in colls_a, "Node A DOES NOT have Node C's 'analytics' collection")

        # Node B collections: must only have 'telemetry'
        colls_b = client_b.collections()
        assert_true("telemetry" in colls_b, "Node B has 'telemetry'")
        assert_true("embeddings" not in colls_b, "Node B DOES NOT have Node A's 'embeddings' collection")
        assert_true("analytics" not in colls_b, "Node B DOES NOT have Node C's 'analytics' collection")

        # Node C collections: must only have 'analytics'
        colls_c = client_c.collections()
        assert_true("analytics" in colls_c, "Node C has 'analytics'")
        assert_true("embeddings" not in colls_c, "Node C DOES NOT have Node A's 'embeddings' collection")
        assert_true("telemetry" not in colls_c, "Node C DOES NOT have Node B's 'telemetry' collection")

        # Check /_brain reports on each node
        brain_a = http_get(client_a, "/_brain")
        brain_b = http_get(client_b, "/_brain")
        brain_c = http_get(client_c, "/_brain")

        names_a = [c["name"] for c in brain_a.get("collections", [])]
        names_b = [c["name"] for c in brain_b.get("collections", [])]
        names_c = [c["name"] for c in brain_c.get("collections", [])]

        assert_true(names_a == ["embeddings"], f"Node A /_brain shows ONLY its own collection: {names_a}")
        assert_true(names_b == ["telemetry"], f"Node B /_brain shows ONLY its own collection: {names_b}")
        assert_true(names_c == ["analytics"], f"Node C /_brain shows ONLY its own collection: {names_c}")

        # Verify Ledgers: Each node has its own distinct ledger tip and entry count
        tip_a = client_a.ledger_tip().get("entry_id", 0)
        tip_b = client_b.ledger_tip().get("entry_id", 0)
        tip_c = client_c.ledger_tip().get("entry_id", 0)

        assert_true(tip_a == 3, f"Node A ledger tip matches its 4 vector writes (0-indexed tip=3, got {tip_a})")
        assert_true(tip_b == 11, f"Node B ledger tip matches its 12 telemetry writes (0-indexed tip=11, got {tip_b})")
        assert_true(tip_c == 19, f"Node C ledger tip matches its 20 tabular writes (0-indexed tip=19, got {tip_c})")

        # Whole node verification
        verify_a = http_post(client_a, "/_verify", {})
        verify_b = http_post(client_b, "/_verify", {})
        verify_c = http_post(client_c, "/_verify", {})
        assert_true(verify_a.get("ok") is True, "Node A whole-node verify passed")
        assert_true(verify_b.get("ok") is True, "Node B whole-node verify passed")
        assert_true(verify_c.get("ok") is True, "Node C whole-node verify passed")

        # --------------------------------------------------------------------
        # 6. Restart Persistence Under Standalone Mode
        # --------------------------------------------------------------------
        log("6. Testing independent restart persistence for standalone databases...")
        node_a.kill()
        node_b.kill()
        time.sleep(0.5)

        node_a.start(wait=True)
        node_b.start(wait=True)
        time.sleep(0.5)

        # After restart, verify each still only has its own data
        search_after = http_post(node_a.client, "/_search/vector/embeddings", {
            "vector": [0.88, 0.12, 0.0, 0.0],
            "k": 1
        })
        assert_true(search_after.get("hits", [])[0]["key"] == "doc_sports", "Node A vector search intact after restart")
        assert_true("telemetry" not in node_a.client.collections(), "Node A still has 0 collections from Node B after restart")

        rollups_after = http_get(node_b.client, f"/_ts/telemetry/rollups?from_ms={base_time - 1000}&to_ms={base_time + 100000}&bucket_ms=60000")
        assert_true("cpu_load" in rollups_after, "Node B rollups intact after restart")
        assert_true("embeddings" not in node_b.client.collections(), "Node B still has 0 collections from Node A after restart")

    log("=== MULTI-DATABASE ISOLATION TEST PASSED (ZERO REPLICATION VERIFIED) ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
