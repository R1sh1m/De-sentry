#!/usr/bin/env python3
"""
De-Sentry Comprehensive Feature Test Suite

Exercises all major REST API features, mirroring how a human user
would interact via the UI and CLI. Assumes a 3-node cluster with
supervisor is running on localhost:7700-7703.
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

def test_cluster_health():
    log("=== Testing Cluster Health ===")
    for name, base in BASE_URLS.items():
        res = request('GET', f"{base}/_status")
        record(f"{name} _status", 'error' not in res)

def test_engines():
    log("=== Testing Engine Discovery ===")
    res = request('GET', f"{BASE_URLS['node0']}/_engines")
    record("_engines list", 'built_in' in res or 'error' not in res)

def test_collection_crud():
    log("=== Testing Collection CRUD ===")
    base = BASE_URLS['node0']
    
    # Create collection with simple JSON schema
    schema_body = {"type": "object", "properties": {"name": {"type": "string"}, "age": {"type": "integer"}}}
    res = request('PUT', f"{base}/_schema/test_users", schema_body)
    record("Create collection schema", 'error' not in res)
    
    # Insert document
    doc = {"name": "Alice", "age": 30, "email": "alice@example.com"}
    res = request('PUT', f"{base}/db/test_users/alice001", doc)
    record("Insert document", 'error' not in res)
    
    # Get document
    res = request('GET', f"{base}/db/test_users/alice001")
    record("Get document", res.get('name') == 'Alice')
    
    # Update document
    doc['age'] = 31
    res = request('PUT', f"{base}/db/test_users/alice001", doc)
    record("Update document", 'error' not in res)
    
    # Verify update
    res = request('GET', f"{base}/db/test_users/alice001")
    record("Verify update", res.get('age') == 31)
    
    # List collection
    res = request('GET', f"{base}/db/test_users")
    record("List collection", 'documents' in res or 'error' not in res)
    
    # Delete document
    res = request('DELETE', f"{base}/db/test_users/alice001")
    record("Delete document", 'error' not in res)
    
    # Verify delete
    res = request('GET', f"{base}/db/test_users/alice001")
    record("Verify delete", res.get('error') is not None or 'not_found' in str(res).lower())

def test_acl():
    log("=== Testing Access Control ===")
    base = BASE_URLS['node0']
    
    # Set ACL
    acl = {"owner": "node0", "is_private": False, "readers": ["node0", "node1"]}
    res = request('PUT', f"{base}/_collection/test_users/acl", acl)
    record("Set collection ACL", 'error' not in res)
    
    # Get ACL
    res = request('GET', f"{base}/_collection/test_users")
    record("Get collection metadata", 'error' not in res)

def test_placement():
    log("=== Testing Placement ===")
    base = BASE_URLS['node0']
    
    placement = {"shard_key": "tenant_id", "replication_factor": 3}
    res = request('PUT', f"{base}/_collection/test_users/placement", placement)
    record("Set placement", 'error' not in res)
    
    res = request('GET', f"{base}/_placement/test_users/alice001")
    record("Get placement info", 'error' not in res)

def test_ledger():
    log("=== Testing Audit Ledger ===")
    base = BASE_URLS['node0']
    
    res = request('GET', f"{base}/_ledger/tip")
    record("Get ledger tip", 'tip_hash' in res or 'error' not in res)
    
    res = request('GET', f"{base}/_ledger/entries?from=0&to=10")
    record("Get ledger entries", 'entries' in res or 'error' not in res)
    
    res = request('POST', f"{base}/_ledger/verify")
    record("Verify ledger", 'error' not in res)

def test_transit():
    log("=== Testing Transit Store ===")
    base = BASE_URLS['node0']
    
    res = request('GET', f"{base}/_transit")
    record("Get transit status", 'error' not in res)
    
    res = request('POST', f"{base}/_transit/expire")
    record("Expire transit", 'error' not in res)

def test_outbox():
    log("=== Testing Outbox ===")
    base = BASE_URLS['node0']
    
    res = request('GET', f"{base}/_outbox")
    record("Get outbox status", 'error' not in res)
    
    res = request('POST', f"{base}/_outbox/flush")
    record("Flush outbox", 'error' not in res)

def test_supervisor():
    log("=== Testing Supervisor API ===")
    base = BASE_URLS['supervisor']
    
    res = request('GET', f"{base}/_supervisor/nodes")
    record("List supervisor nodes", 'error' not in res)
    
    res = request('GET', f"{base}/_supervisor/topology")
    record("Get topology", 'error' not in res)
    
    res = request('GET', f"{base}/_supervisor/scan")
    record("Hardware scan", 'error' not in res)

def test_peers():
    log("=== Testing Peer Discovery ===")
    base = BASE_URLS['node0']
    
    res = request('GET', f"{base}/_peers")
    record("Get peers", 'peers' in res or 'error' not in res)

def test_quota():
    log("=== Testing Quota ===")
    base = BASE_URLS['node0']
    
    res = request('GET', f"{base}/_quota")
    record("Get quota info", 'error' not in res)

def test_lookup():
    log("=== Testing Universal Lookup ===")
    base = BASE_URLS['node0']
    
    res = request('GET', f"{base}/_lookup/test_users")
    record("Collection lookup", 'error' not in res)

def test_brain():
    log("=== Testing Brain Snapshot ===")
    base = BASE_URLS['node0']
    
    res = request('GET', f"{base}/_brain")
    record("Get brain snapshot", 'error' not in res)

def test_config():
    log("=== Testing Config ===")
    base = BASE_URLS['node0']
    
    res = request('GET', f"{base}/_config")
    record("Get config", 'error' not in res)

def test_changes():
    log("=== Testing Change Feed ===")
    base = BASE_URLS['node0']
    
    res = request('GET', f"{base}/_changes?since=0")
    record("Get changes", 'error' not in res)

def cleanup():
    log("=== Cleanup ===")
    base = BASE_URLS['node0']
    
    res = request('DELETE', f"{base}/_schema/test_users")
    record("Drop test collection", 'error' not in res or 'not_found' not in str(res).lower())

def summary():
    total = len(TEST_RESULTS)
    passed = sum(1 for r in TEST_RESULTS if r['passed'])
    failed = total - passed
    
    print("\n" + "="*60)
    print(f"TEST SUMMARY: {passed}/{total} passed ({100*passed/total:.1f}%)")
    print("="*60)
    
    if failed > 0:
        print("\nFailures:")
        for r in TEST_RESULTS:
            if not r['passed']:
                print(f"  - {r['name']}: {r['details']}")
    
    return failed == 0

def main():
    log("Starting De-Sentry Comprehensive Feature Test")
    log("="*60)
    
    test_cluster_health()
    test_engines()
    test_collection_crud()
    test_acl()
    test_placement()
    test_ledger()
    test_transit()
    test_outbox()
    test_peers()
    test_quota()
    test_lookup()
    test_brain()
    test_config()
    test_changes()
    test_supervisor()
    cleanup()
    
    success = summary()
    sys.exit(0 if success else 1)

if __name__ == "__main__":
    main()
