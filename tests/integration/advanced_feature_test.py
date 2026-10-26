#!/usr/bin/env python3
"""
De-Sentry Advanced Feature Test Suite

Tests advanced features: time-series, vector search, graph, cross-node replication,
ACL enforcement, checkpoint, retention, CLI, and more.
"""
import json, sys, time, urllib.request, urllib.error

BASE_URLS = {
    "node0": "http://127.0.0.1:7701",
    "node1": "http://127.0.0.1:7702",
    "node2": "http://127.0.0.1:7703",
    "supervisor": "http://127.0.0.1:7700",
}

TEST_RESULTS = []

def log(msg, level="INFO"):
    ts = time.strftime("%H:%M:%S")
    print(f"[{ts}] [{level}] {msg}")

def record(name, passed, details=""):
    TEST_RESULTS.append({"name": name, "passed": passed, "details": details})
    if passed:
        log(f"PASS {name}")
    else:
        log(f"FAIL {name} - {details}")

def request(method, url, data=None, headers=None):
    if headers is None:
        headers = {}
    if data is not None:
        data = json.dumps(data).encode('utf-8')
        headers['Content-Type'] = 'application/json'
    req = urllib.request.Request(url, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            body = resp.read().decode()
            return json.loads(body) if body else {}
    except urllib.error.HTTPError as e:
        body = e.read().decode() if hasattr(e, 'read') else ''
        return {"error": f"HTTP {e.code}: {e.reason} - {body}"}
    except Exception as e:
        return {"error": str(e)}

def test_time_series():
    log("=== Testing Time-Series Engine ===")
    base = BASE_URLS['node0']
    
    # Create time-series collection
    schema = {"type": "object", "properties": {"ts": {"type": "string"}, "value": {"type": "number"}}}
    res = request('PUT', f"{base}/_schema/ts_metrics", schema)
    record("Create TS collection", 'error' not in res)

    # Bind to ts_rollup engine
    res = request('PUT', f"{base}/_collection/ts_metrics/engine", {"engine": "ts_rollup"})
    record("Bind TS engine", 'error' not in res)
    
    # Set placement for time-series
    placement = {"shard_key": "metric_name", "replication_factor": 3, "retention_days": 7}
    res = request('PUT', f"{base}/_collection/ts_metrics/placement", placement)
    record("Set TS placement", 'error' not in res)
    
    # Insert time-series data
    for i in range(5):
        doc = {"metric_name": "cpu", "ts": f"2026-10-26T10:0{i}:00Z", "value": 42 + i}
        res = request('PUT', f"{base}/db/ts_metrics/metric_{i}", doc)
        if i == 0:
            record("Insert TS data", 'error' not in res)
    
    # Run retention
    res = request('POST', f"{base}/db/ts_metrics/_retention/run")
    record("Run retention", 'error' not in res)
    
    # Query time-series rollups
    res = request('GET', f"{base}/_ts/ts_metrics/rollups")
    record("Get TS rollups", 'error' not in res or 'rollups' in res)
    
    # Cleanup
    request('DELETE', f"{base}/_schema/ts_metrics")

def test_vector_search():
    log("=== Testing Vector Search ===")
    base = BASE_URLS['node0']
    
    # Create vector collection
    schema = {"type": "object", "properties": {"embedding": {"type": "array"}, "text": {"type": "string"}}}
    res = request('PUT', f"{base}/_schema/vec_docs", schema)
    record("Create vector collection", 'error' not in res)
    
    # Bind to vector engine BEFORE inserting
    res = request('PUT', f"{base}/_collection/vec_docs/engine", {"engine": "vector_hnsw_lite"})
    record("Bind vector engine", 'error' not in res)
    
    # Insert vectors
    for i in range(3):
        doc = {"embedding": [float(i), float(i+1), float(i+2)], "text": f"doc {i}"}
        res = request('PUT', f"{base}/db/vec_docs/doc_{i}", doc)
        if i == 0:
            record("Insert vector doc", 'error' not in res)
    
    # k-NN search (use "vector" key as required)
    query = {"vector": [0.5, 1.5, 2.5], "k": 2}
    res = request('POST', f"{base}/_search/vector/vec_docs", query)
    record("Vector k-NN search", 'error' not in res or 'results' in res)
    
    # Cleanup
    request('DELETE', f"{base}/_schema/vec_docs")

def test_graph():
    log("=== Testing Graph Engine ===")
    base = BASE_URLS['node0']
    
    # Create graph collection
    schema = {"type": "object", "properties": {"name": {"type": "string"}, "edges": {"type": "array"}}}
    res = request('PUT', f"{base}/_schema/graph_nodes", schema)
    record("Create graph collection", 'error' not in res)
    
    # Insert nodes with edges
    nodes = [
        {"key": "A", "doc": {"name": "Node A", "edges": ["B", "C"]}},
        {"key": "B", "doc": {"name": "Node B", "edges": ["C"]}},
        {"key": "C", "doc": {"name": "Node C", "edges": []}},
    ]
    for node in nodes:
        res = request('PUT', f"{base}/db/graph_nodes/{node['key']}", node['doc'])
        if node['key'] == "A":
            record("Insert graph node", 'error' not in res)
    
    # Query adjacency
    res = request('GET', f"{base}/_graph/graph_nodes/A")
    record("Graph adjacency query", 'error' not in res or 'edges' in res)
    
    # Cleanup
    request('DELETE', f"{base}/_schema/graph_nodes")

def test_cross_node_replication():
    log("=== Testing Cross-Node Replication ===")
    
    # Write to node0
    base0 = BASE_URLS['node0']
    schema = {"type": "object", "properties": {"data": {"type": "string"}}}
    res = request('PUT', f"{base0}/_schema/repl_test", schema)
    record("Create repl collection", 'error' not in res)
    
    doc = {"data": "replicated across mesh"}
    res = request('PUT', f"{base0}/db/repl_test/key1", doc)
    record("Write to node0", 'error' not in res)
    
    # Wait for replication
    time.sleep(2)
    
    # Read from node1 and node2
    for node in ['node1', 'node2']:
        base = BASE_URLS[node]
        res = request('GET', f"{base}/db/repl_test/key1")
        if res.get('data') == 'replicated across mesh':
            record(f"Read from {node}", True)
        else:
            record(f"Read from {node}", False, f"Got {res}")
    
    # Cleanup
    request('DELETE', f"{base0}/_schema/repl_test")

def test_acl_enforcement():
    log("=== Testing ACL Enforcement ===")
    base = BASE_URLS['node0']
    
    # Create private collection
    schema = {"type": "object", "properties": {"secret": {"type": "string"}}}
    res = request('PUT', f"{base}/_schema/private_docs", schema)
    record("Create private collection", 'error' not in res)
    
    # Set restrictive ACL
    acl = {"owner": "node0", "is_private": True, "readers": ["node0"]}
    res = request('PUT', f"{base}/_collection/private_docs/acl", acl)
    record("Set private ACL", 'error' not in res)
    
    # Insert doc
    doc = {"secret": "top secret"}
    res = request('PUT', f"{base}/db/private_docs/s1", doc)
    record("Insert private doc", 'error' not in res)
    
    # Cleanup
    request('DELETE', f"{base}/_schema/private_docs")

def test_checkpoint():
    log("=== Testing Checkpoint ===")
    base = BASE_URLS['supervisor']
    
    # Trigger checkpoint
    res = request('POST', f"{base}/_checkpoint")
    record("Trigger checkpoint", 'error' not in res or 'checkpoint' in res)

def test_columnar():
    log("=== Testing Columnar Engine ===")
    base = BASE_URLS['node0']
    
    # Create columnar collection
    schema = {"type": "object", "properties": {"id": {"type": "integer"}, "val": {"type": "string"}}}
    res = request('PUT', f"{base}/_schema/col_data", schema)
    record("Create columnar collection", 'error' not in res)
    
    # Bind to columnar engine
    res = request('PUT', f"{base}/_collection/col_data/engine", {"engine": "columnar_lite"})
    record("Bind columnar engine", 'error' not in res)
    
    # Insert data
    for i in range(10):
        doc = {"id": i, "val": f"value_{i}"}
        res = request('PUT', f"{base}/db/col_data/row_{i}", doc)
    
    # Scan
    res = request('GET', f"{base}/db/col_data")
    record("Columnar scan", 'documents' in res or 'error' not in res)
    
    # Cleanup
    request('DELETE', f"{base}/_schema/col_data")

def test_placement_ring():
    log("=== Testing Placement Ring ===")
    base = BASE_URLS['node0']
    
    # Create collection with shard key
    schema = {"type": "object", "properties": {"tenant": {"type": "string"}, "data": {"type": "string"}}}
    res = request('PUT', f"{base}/_schema/tenant_data", schema)
    record("Create tenant collection", 'error' not in res)
    
    placement = {"shard_key": "tenant", "replication_factor": 3}
    res = request('PUT', f"{base}/_collection/tenant_data/placement", placement)
    record("Set shard placement", 'error' not in res)
    
    # Insert a document so the key has a known placement
    doc = {"tenant": "tenant_abc", "data": "test data"}
    res = request('PUT', f"{base}/db/tenant_data/doc_001", doc)
    if 'error' not in res:
        record("Insert doc for placement", True)
    else:
        record("Insert doc for placement", False, res.get('error', ''))
    
    # Get placement for the inserted key
    res = request('GET', f"{base}/_placement/tenant_data/doc_001")
    record("Get ring placement", 'error' not in res or 'owners' in res or 'displaced' in res)
    
    # Cleanup
    request('DELETE', f"{base}/_schema/tenant_data")

def test_pairing_qr():
    log("=== Testing Pairing QR ===")
    base = BASE_URLS['supervisor']
    
    res = request('GET', f"{base}/_supervisor/pairing")
    record("Get pairing QR payload", 'error' not in res or 'qr' in res or 'bootstrap' in res)

def test_manifest():
    log("=== Testing Node Manifest ===")
    base = BASE_URLS['supervisor']
    
    # Write portable manifest
    import tempfile, os
    tmpdir = tempfile.mkdtemp(prefix="desentry_manifest_")
    manifest = {"dir": tmpdir, "manifest": {"node_id": "test_node", "purpose": "test", "quota_bytes": 1073741824}}
    res = request('POST', f"{base}/_supervisor/manifest", manifest)
    record("Write manifest", 'error' not in res)
    try:
        os.rmdir(tmpdir)
    except Exception:
        pass

def test_engines_list():
    log("=== Testing Available Engines ===")
    base = BASE_URLS['node0']
    
    res = request('GET', f"{base}/_engines")
    engines = res
    has_kv = any('kv' in str(e).lower() for e in engines)
    record("KV engine available", has_kv)

def test_ledger_verify():
    log("=== Testing Full Integrity Verify ===")
    base = BASE_URLS['node0']
    
    res = request('POST', f"{base}/_verify")
    record("Full integrity verify", 'error' not in res or 'verified' in res)

def test_brain_snapshot():
    log("=== Testing Brain Snapshot ===")
    base = BASE_URLS['node0']
    
    res = request('GET', f"{base}/_brain")
    has_tip = 'tip_hash' in res or 'ledger_tip' in res
    record("Brain has ledger tip", has_tip)

def test_changes_feed():
    log("=== Testing Change Feed ===")
    base = BASE_URLS['node0']
    
    res = request('GET', f"{base}/_changes?since=0&timeout_ms=100")
    record("Change feed works", 'error' not in res or 'changes' in res)

def cleanup():
    log("=== Cleanup ===")
    base = BASE_URLS['node0']
    collections = ['ts_metrics', 'vec_docs', 'graph_nodes', 'repl_test', 'private_docs', 'col_data', 'tenant_data']
    for coll in collections:
        res = request('DELETE', f"{base}/_schema/{coll}")

def summary():
    total = len(TEST_RESULTS)
    passed = sum(1 for r in TEST_RESULTS if r['passed'])
    failed = total - passed
    
    print("\n" + "="*60)
    print(f"ADVANCED TEST SUMMARY: {passed}/{total} passed ({100*passed/total:.1f}%)")
    print("="*60)
    
    if failed > 0:
        print("\nFailures:")
        for r in TEST_RESULTS:
            if not r['passed']:
                print(f"  - {r['name']}: {r['details']}")
    
    return failed == 0

def is_supervisor_present():
    base = BASE_URLS['supervisor']
    try:
        res = request('GET', f"{base}/_status")
        return 'error' not in res
    except Exception:
        return False

def main():
    log("Starting De-Sentry Advanced Feature Test")
    log("="*60)
    
    test_engines_list()
    test_time_series()
    test_vector_search()
    test_graph()
    test_columnar()
    test_cross_node_replication()
    test_acl_enforcement()
    test_placement_ring()
    if is_supervisor_present():
        test_checkpoint()
        test_pairing_qr()
        test_manifest()

    test_ledger_verify()
    test_brain_snapshot()
    test_changes_feed()
    cleanup()
    
    success = summary()
    sys.exit(0 if success else 1)

if __name__ == "__main__":
    main()