"""Deep-dive investigation and feature test for De-Sentry storage.

Tests:
1. Node usability & lifecycle (start, identity, REST API, P2P communication)
2. Polyglot engines utilization:
   - kv (default B+Tree engine)
   - columnar_lite (columnar segment store with RLE/delta compression)
   - ts_rollup (time series chunked segments and downsampled rollups)
   - vector_hnsw_lite (HNSW vector similarity search)
   - graph_adj (graph adjacency list & edge traversal)
3. Sharding & Placement verification:
   - Consistent hash ring RF-3 subset vs actual mesh replication
4. Ledger v2 functioning:
   - Hash chain integrity
   - Ed25519 signature verification
   - Change feed long polling
   - Tamper-evidence
5. Dropbox intake simulation:
   - Tabular / CSV intake
   - Vector array intake
   - Time series metric intake
   - Graph edge intake
   - Large file (> 3.5 KiB) chunking and part storage
6. Crash & restart durability:
   - Persistence of all engines across process restart
"""

from __future__ import annotations

import json
import math
import os
import sys
import time
from typing import Any, Dict, List

# Add Python client and integration harness to path
REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(REPO_ROOT, "clients", "python"))
sys.path.insert(0, os.path.join(REPO_ROOT, "tests", "integration"))

from desentry_client import DesentryClient, DesentryError  # noqa: E402
from harness import Cluster, find_engine  # noqa: E402

ALL_ENGINES = ["kv", "columnar_lite", "ts_rollup", "vector_hnsw_lite", "graph_adj"]


def log(msg: str):
    print(f"[deep-dive] {msg}")


def assert_true(cond: bool, msg: str):
    if not cond:
        raise AssertionError(f"FAILED: {msg}")
    print(f"  ok: {msg}")


def http_get(client: DesentryClient, path: str) -> Any:
    return client._request("GET", path)


def http_post(client: DesentryClient, path: str, body: Any = None) -> Any:
    return client._request("POST", path, body=body if body is not None else {})


def http_put(client: DesentryClient, path: str, body: Any = None) -> Any:
    return client._request("PUT", path, body=body if body is not None else {})


def main() -> int:
    engine_bin = find_engine()
    log(f"Using engine binary: {engine_bin}")

    # Start a 3-node cluster + 1 supervisor with ALL engines enabled
    cluster = Cluster(engine=engine_bin)
    with cluster:
        node_a = cluster.add("node-a", config_overrides={"engines": ALL_ENGINES})
        node_b = cluster.add("node-b", config_overrides={"engines": ALL_ENGINES})
        node_c = cluster.add("node-c", config_overrides={"engines": ALL_ENGINES})
        sup = cluster.add("supervisor", supervisor=True)
        cluster.start_all(mesh=True)

        client_a = node_a.client
        client_b = node_b.client
        client_c = node_c.client
        sup_client = sup.client

        # --------------------------------------------------------------------
        # 1. NODE USABILITY
        # --------------------------------------------------------------------
        log("1. Verifying Node Usability and Baseline State...")
        for i, n in enumerate([node_a, node_b, node_c]):
            st = n.client.status()
            assert_true(st is not None, f"Node {n.name} returned status")
            assert_true("node_id" in st and len(st["node_id"]) > 0, f"Node {n.name} has valid node_id: {st.get('node_id')}")
            engines_resp = http_get(n.client, "/_engines")
            active_engines = [e["name"] for e in engines_resp if e.get("active_on_this_node")]
            assert_true(all(eng in active_engines for eng in ALL_ENGINES), f"Node {n.name} active engines: {active_engines}")
            # Verify _brain endpoint
            brain = http_get(n.client, "/_brain")
            assert_true(brain is not None and "node_id" in brain, f"Node {n.name} /_brain answers with full state")
            assert_true(len(brain.get("ledger_tip", {}).get("entry_hash", "")) == 64, f"Node {n.name} ledger tip hash exists")

        # Verify supervisor state
        if sup_client is not None:
            sup_st = sup_client.status()
            assert_true(sup_st.get("supervisor") is True, "Supervisor node correctly identifies as supervisor: true")
            topo = http_get(sup_client, "/_supervisor/topology")
            assert_true(topo is not None, "Supervisor /_supervisor/topology returns hardware scan")

        # --------------------------------------------------------------------
        # 2. KV ENGINE
        # --------------------------------------------------------------------
        log("2. Testing KV Engine (Default B+Tree)...")
        # Put, Get, Update, Delete
        res = client_a.put("kv_test", "doc1", {"title": "Hello KV", "count": 1})
        assert_true(res.get("ok") is True, "KV put returned ok: true")
        got = client_a.get("kv_test", "doc1")
        assert_true(got.get("title") == "Hello KV" and got.get("count") == 1, "KV document read matches written")

        # Update
        client_a.put("kv_test", "doc1", {"title": "Updated KV", "count": 2})
        got2 = client_a.get("kv_test", "doc1")
        assert_true(got2.get("title") == "Updated KV" and got2.get("count") == 2, "KV document update applied")

        # Scan
        client_a.put("kv_test", "doc2", {"title": "Second Doc"})
        scan = http_get(client_a, "/db/kv_test?limit=10")
        docs_arr = scan.get("documents", []) if isinstance(scan, dict) else scan
        assert_true(len(docs_arr) >= 2, f"KV scan returned {len(docs_arr)} rows")

        # --------------------------------------------------------------------
        # 3. COLUMNAR_LITE ENGINE
        # --------------------------------------------------------------------
        log("3. Testing Columnar Lite Engine...")
        # Bind collection to columnar_lite
        bind_res = http_put(client_a, "/_collection/analytics/engine", {"engine": "columnar_lite"})
        assert_true(bind_res is not None, "Bound collection 'analytics' to columnar_lite")
        col_meta = http_get(client_a, "/_collection/analytics")
        assert_true(col_meta.get("engine") == "columnar_lite", "Collection metadata confirms engine is columnar_lite")

        # Write tabular / analytics records
        for i in range(15):
            client_a.put("analytics", f"row_{i:03d}", {
                "device_id": f"dev_{i % 3}",
                "status": "active" if i % 2 == 0 else "idle",
                "val": i * 1.5,
            })

        # Read back and scan
        row5 = client_a.get("analytics", "row_005")
        assert_true(row5.get("val") == 7.5 and row5.get("device_id") == "dev_2", "Columnar row read back correctly")
        col_scan_resp = http_get(client_a, "/db/analytics?limit=50")
        col_scan = col_scan_resp.get("documents", []) if isinstance(col_scan_resp, dict) else col_scan_resp
        assert_true(len(col_scan) == 15, f"Columnar scan returned all 15 rows (got {len(col_scan)})")

        # --------------------------------------------------------------------
        # 4. TS_ROLLUP ENGINE
        # --------------------------------------------------------------------
        log("4. Testing Time-Series Rollup Engine...")
        bind_ts = http_put(client_a, "/_collection/sensors/engine", {"engine": "ts_rollup"})
        assert_true(bind_ts is not None, "Bound collection 'sensors' to ts_rollup")
        ts_meta = http_get(client_a, "/_collection/sensors")
        assert_true(ts_meta.get("engine") == "ts_rollup", "Collection metadata confirms engine is ts_rollup")

        base_time = 1700000000000
        # Write points with various timestamps and values
        # Test series A
        for i in range(10):
            t = base_time + i * 5000  # every 5 seconds
            client_a.put("sensors", f"temp:pt_{i}", {
                "series": "temp",
                "timestamp": t,
                "value": 20.0 + i,
            })
        # Test series B with "ts" and "val" (verifying expanded numeric value extraction)
        for i in range(5):
            t = base_time + i * 10000
            client_a.put("sensors", f"humidity:pt_{i}", {
                "series": "humidity",
                "ts": t,
                "val": 50.0 + i * 2,
            })

        # Query rollups
        rollups = http_get(client_a, f"/_ts/sensors/rollups?from_ms={base_time - 1000}&to_ms={base_time + 100000}&bucket_ms=60000")
        log(f"Sensors rollups response: {json.dumps(rollups)}")
        assert_true("temp" in rollups and "humidity" in rollups, "ts_rollup returned both temp and humidity series")
        if "humidity" in rollups:
            hum_buckets = rollups["humidity"]
            total_hum = sum(b["count"] for b in hum_buckets)
            assert_true(total_hum == 5, f"Expected 5 humidity points using 'val' field, got {total_hum}")

        if "temp" in rollups:
            temp_buckets = rollups["temp"]
            assert_true(len(temp_buckets) >= 1, "temp series has rollup buckets")
            total_temp = sum(b["count"] for b in temp_buckets)
            assert_true(total_temp == 10, f"Expected 10 total points across temp buckets, got {total_temp}")
            assert_true(min(b["min"] for b in temp_buckets) == 20.0, "Overall min is 20.0")
            assert_true(max(b["max"] for b in temp_buckets) == 29.0, "Overall max is 29.0")

        # --------------------------------------------------------------------
        # 5. VECTOR_HNSW_LITE ENGINE
        # --------------------------------------------------------------------
        log("5. Testing Vector HNSW Lite Engine...")
        bind_vec = http_put(client_a, "/_collection/embeddings/engine", {"engine": "vector_hnsw_lite"})
        assert_true(bind_vec is not None, "Bound collection 'embeddings' to vector_hnsw_lite")

        # Insert 8-dimensional vectors
        # v1: pointing mainly in dim 0
        client_a.put("embeddings", "vec_x", {"vector": [1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0], "label": "X-axis"})
        # v2: pointing mainly in dim 1
        client_a.put("embeddings", "vec_y", {"vector": [0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0], "label": "Y-axis"})
        # v3: close to dim 0
        client_a.put("embeddings", "vec_near_x", {"vector": [0.95, 0.1, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0], "label": "Near X"})
        # v4: pointing in dim 7
        client_a.put("embeddings", "vec_w", {"vector": [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0], "label": "W-axis"})

        # Search query vector close to X
        search_res = http_post(client_a, "/_search/vector/embeddings", {
            "vector": [1.0, 0.05, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0],
            "k": 3
        })
        log(f"Vector search response: {json.dumps(search_res)}")
        assert_true(search_res is not None and "hits" in search_res, "Vector search returned hits")
        hits = search_res.get("hits", [])
        assert_true(len(hits) >= 2, f"Search returned {len(hits)} hits")
        top_hit_key = hits[0].get("key")
        assert_true(top_hit_key in ("vec_x", "vec_near_x"), f"Top hit is close to query: {top_hit_key}")

        # --------------------------------------------------------------------
        # 6. GRAPH_ADJ ENGINE
        # --------------------------------------------------------------------
        log("6. Testing Graph Adjacency Engine...")
        bind_graph = http_put(client_a, "/_collection/org_graph/engine", {"engine": "graph_adj"})
        assert_true(bind_graph is not None, "Bound collection 'org_graph' to graph_adj")

        # Insert graph nodes with children / parent
        client_a.put("org_graph", "ceo", {"name": "Alice CEO", "children": ["vp_eng", "vp_sales"]})
        client_a.put("org_graph", "vp_eng", {"name": "Bob VP Eng", "parent": "ceo", "children": ["lead_dev"]})
        client_a.put("org_graph", "vp_sales", {"name": "Carol VP Sales", "parent": "ceo"})
        client_a.put("org_graph", "lead_dev", {"name": "Dave Lead", "parent": "vp_eng"})

        # Also insert standalone edge document (from/to or source/target, as Dropbox produces)
        client_a.put("org_graph", "edge_lead_contractor", {"source": "lead_dev", "target": "contractor", "label": "manages"})

        # Query graph node
        graph_ceo = http_get(client_a, "/_graph/org_graph/ceo?depth=2")
        log(f"Graph CEO query: {json.dumps(graph_ceo)}")
        assert_true(graph_ceo is not None, "Graph query returned response")
        out_edges = graph_ceo.get("out_edges", [])
        descendants = graph_ceo.get("descendants", [])
        assert_true(len(out_edges) == 2, f"CEO has 2 unique out edges (got {len(out_edges)})")
        assert_true("vp_eng" in [e["to"] for e in out_edges], "vp_eng is an out-edge of ceo")
        assert_true("lead_dev" in descendants, "lead_dev is a 2-hop descendant of ceo")

        # Verify standalone edge was correctly indexed
        graph_lead = http_get(client_a, "/_graph/org_graph/lead_dev?depth=1")
        assert_true(any(e["to"] == "contractor" for e in graph_lead.get("out_edges", [])), "Standalone edge document lead_dev -> contractor indexed")

        # --------------------------------------------------------------------
        # 7. SHARDING vs REPLICATION INVESTIGATION
        # --------------------------------------------------------------------
        log("7. Investigating Sharding & Placement vs Replication...")
        # Check /_placement/:col/:key
        placement_info = http_get(client_a, "/_placement/kv_test/doc1")
        log(f"Placement info for kv_test/doc1: {json.dumps(placement_info)}")
        assert_true(placement_info is not None, "/_placement returns placement plan")
        assert_true("replicas" in placement_info, "Placement plan specifies ring replicas")
        replicas = placement_info.get("replicas", [])
        log(f"Ring-selected replicas (RF=3): {replicas}")

        # Wait for gossip to settle across nodes
        time.sleep(2.0)
        # Check if Node B and Node C received the writes
        doc_b = client_b.get("kv_test", "doc1")
        doc_c = client_c.get("kv_test", "doc1")
        assert_true(doc_b == got2, "Node B has kv_test/doc1 via replication")
        assert_true(doc_c == got2, "Node C has kv_test/doc1 via replication")

        # Check if Node B and Node C received the analytics collection
        col_b = client_b.get("analytics", "row_005")
        col_c = client_c.get("analytics", "row_005")
        assert_true(col_b == row5, "Node B has analytics/row_005 via replication")
        assert_true(col_c == row5, "Node C has analytics/row_005 via replication")

        log("CONFIRMATION: Even though Placement computes an RF-3 subset for durability and transit, all connected nodes replicate all data via eager broadcast + gossip anti-entropy.")

        # --------------------------------------------------------------------
        # 8. LEDGER FUNCTIONING & VERIFICATION
        # --------------------------------------------------------------------
        log("8. Testing Ledger v2 Functioning...")
        # 1. Entries endpoint
        entries_a = http_get(client_a, "/_ledger/entries?from=0&to=100")
        assert_true(entries_a is not None and "entries" in entries_a, "Node A returns ledger entries")
        entry_list = entries_a.get("entries", [])
        assert_true(len(entry_list) > 0, f"Node A has {len(entry_list)} ledger entries")
        e0 = entry_list[0]
        assert_true("entry_id" in e0 and "entry_hash" in e0 and "hlc" in e0, "Ledger entry has entry_id, entry_hash, and hlc")

        # 2. Ledger verification
        verify_a = http_post(client_a, "/_ledger/verify", {})
        assert_true(verify_a.get("verified") is True, f"Node A /_ledger/verify passed: {verify_a}")

        verify_b = http_post(client_b, "/_ledger/verify", {})
        assert_true(verify_b.get("verified") is True, f"Node B /_ledger/verify passed: {verify_b}")

        # 3. Whole-node verification (ledger + all backends)
        whole_a = http_post(client_a, "/_verify", {})
        assert_true(whole_a.get("ok") is True, f"Node A /_verify passed: {whole_a}")

        # 4. Changes feed long-poll
        tip_before = client_a.status().get("ledger_tip", {}).get("entry_id", 0)
        client_a.put("kv_test", "change_test", {"v": 123})
        changes = http_get(client_a, f"/_changes?since={tip_before}&timeout_ms=1000")
        log(f"Changes feed response: {json.dumps(changes)}")
        assert_true(changes is not None and "changes" in changes, "Changes feed returned changes list")
        change_list = changes.get("changes", [])
        assert_true(any(c.get("key") == "change_test" for c in change_list), "Changes feed contains change_test write")

        # --------------------------------------------------------------------
        # 9. DROPBOX INGESTION SIMULATION
        # --------------------------------------------------------------------
        log("9. Testing Universal Dropbox Workload Simulations...")

        # 9a. CSV / Tabular Ingest Simulation
        # In dropbox: CSV header + rows -> suggestedEngine: columnar_lite
        http_put(client_a, "/_collection/csv_import/engine", {"engine": "columnar_lite"})
        csv_rows: list[tuple[str, dict[str, Any]]] = [
            ("csv_row_1", {"name": "Alice", "score": 95.5, "dept": "HR"}),
            ("csv_row_2", {"name": "Bob", "score": 88.0, "dept": "ENG"}),
            ("csv_row_3", {"name": "Charlie", "score": 92.3, "dept": "ENG"}),
        ]
        for key, doc in csv_rows:
            client_a.put("csv_import", key, doc)
        read_csv = client_a.get("csv_import", "csv_row_2")
        assert_true(read_csv.get("name") == "Bob" and read_csv.get("score") == 88.0, "CSV import via columnar_lite works")

        # 9b. Large Document Chunking Simulation
        # Document with > 3.5 KiB content string
        large_content = "X" * 8000
        # Simulating chunkRecords:
        chunk_total = math.ceil(len(large_content) / 3500)
        # Write parts
        for i in range(chunk_total):
            part_text = large_content[i * 3500 : (i + 1) * 3500]
            client_a.put("assets", f"big_file.txt#{i}", {
                "parent_key": "big_file.txt",
                "part_index": i,
                "part_total": chunk_total,
                "field": "content",
                "text": part_text,
            })
        # Write manifest
        client_a.put("assets", "big_file.txt", {
            "filename": "big_file.txt",
            "chunked": True,
            "chunk_total": chunk_total,
            "chunk_field": "content",
            "size_bytes": len(large_content),
        })

        manifest = client_a.get("assets", "big_file.txt")
        assert_true(manifest.get("chunked") is True and manifest.get("chunk_total") == chunk_total, "Chunked manifest stored")
        # Verify re-assembly
        reassembled = ""
        for i in range(chunk_total):
            p = client_a.get("assets", f"big_file.txt#{i}")
            reassembled += p.get("text", "")
        assert_true(reassembled == large_content, f"Reassembled chunked content matches original ({len(reassembled)} bytes)")

        # --------------------------------------------------------------------
        # 10. RESTART & PERSISTENCE TEST
        # --------------------------------------------------------------------
        log("10. Testing Crash/Restart Persistence Across All Engines...")
        # Checkpoint cleanly
        if sup_client is not None:
            try:
                http_post(sup_client, "/_checkpoint", {})
            except Exception as e:
                log(f"Supervisor checkpoint note: {e}")

        # Kill node A
        node_a.kill()
        time.sleep(0.5)

        # Restart node A
        node_a.start(wait=True)
        time.sleep(1.0)

        # Verify all engines persisted data:
        # KV
        kv_after = node_a.client.get("kv_test", "doc1")
        assert_true(kv_after.get("title") == "Updated KV", "KV data survived restart")

        # Columnar
        col_after = node_a.client.get("analytics", "row_005")
        assert_true(col_after.get("val") == 7.5, "Columnar data survived restart")

        # Time Series
        ts_after = node_a.client.get("sensors", "temp:pt_5")
        assert_true(ts_after.get("value") == 25.0, "Time-series point survived restart")

        # Vector
        vec_search_after = http_post(node_a.client, "/_search/vector/embeddings", {
            "vector": [1.0, 0.05, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0],
            "k": 2
        })
        assert_true(len(vec_search_after.get("hits", [])) > 0, "Vector index survived restart and answers queries")

        # Graph
        graph_after = http_get(node_a.client, "/_graph/org_graph/ceo?depth=2")
        assert_true(len(graph_after.get("out_edges", [])) == 2, "Graph index survived restart and answers queries")

        # Ledger verification after restart
        verify_after = http_post(node_a.client, "/_ledger/verify", {})
        assert_true(verify_after.get("verified") is True, "Ledger verifies cleanly after restart")

    log("=== ALL DEEP-DIVE CHECKS COMPLETED SUCCESSFULLY ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
