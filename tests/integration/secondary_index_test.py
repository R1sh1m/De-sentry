#!/usr/bin/env python3
"""Integration test for secondary index equality lookup.

Tests the GET /_lookup/:collection/:index?value=... endpoint.
This is a minimal implementation:
- Equality queries only (no range queries)
- Index built lazily on each query (full collection scan)
- No persistence between restarts
- Returns matching keys with their document values
"""

import json
import sys
import urllib.error
import urllib.request
from typing import Any, Dict, List, Optional


class DesentryClient:
    """Minimal client for testing secondary index lookup."""

    def __init__(self, base_url: str, timeout_s: float = 10.0):
        self.base_url = base_url.rstrip("/")
        self.timeout_s = timeout_s

    def _request(self, method: str, path: str,
                 query: Optional[Dict[str, str]] = None,
                 body: Optional[Dict[str, Any]] = None) -> Any:
        url = self.base_url + path
        if query:
            url += "?" + urllib.parse.urlencode(query)
        data = json.dumps(body).encode("utf-8") if body else None
        req = urllib.request.Request(
            url, data=data, method=method,
            headers={"Content-Type": "application/json"} if data else {}
        )
        try:
            with urllib.request.urlopen(req, timeout=self.timeout_s) as resp:
                raw = resp.read()
                return json.loads(raw) if raw else None
        except urllib.error.HTTPError as e:
            raw = e.read()
            message = raw.decode("utf-8", errors="replace")
            try:
                message = json.loads(message).get("error", message)
            except Exception:
                pass
            raise Exception(f"HTTP {e.code}: {message}") from None

    def put(self, collection: str, key: str, document: Dict[str, Any]) -> Dict[str, Any]:
        return self._request("PUT", f"/db/{collection}/{key}", body=document)

    def get(self, collection: str, key: str) -> Dict[str, Any]:
        return self._request("GET", f"/db/{collection}/{key}")

    def delete(self, collection: str, key: str) -> Dict[str, Any]:
        return self._request("DELETE", f"/db/{collection}/{key}")

    def lookup_index(self, collection: str, index_field: str, value: Any,
                     limit: int = 100) -> Dict[str, Any]:
        """Query secondary index for equality match."""
        if isinstance(value, str):
            value_str = value
        else:
            value_str = json.dumps(value)
        return self._request(
            "GET", f"/_lookup/{collection}/{index_field}",
            query={"value": value_str, "limit": str(limit)}
        )

    def ping(self) -> bool:
        try:
            resp = self._request("GET", "/_ping")
            return resp.get("ok") is True
        except Exception:
            return False


def test_secondary_index_basic(client: DesentryClient) -> bool:
    """Test basic equality lookup on email field."""
    collection = "test_users_index"
    
    # Clean up from previous runs
    for key in ["user1", "user2", "user3", "user4"]:
        try:
            client.delete(collection, key)
        except Exception:
            pass

    # Insert test documents
    client.put(collection, "user1", {"email": "alice@example.com", "name": "Alice", "age": 30})
    client.put(collection, "user2", {"email": "bob@example.com", "name": "Bob", "age": 25})
    client.put(collection, "user3", {"email": "alice@example.com", "name": "Alice2", "age": 35})
    client.put(collection, "user4", {"email": "charlie@example.com", "name": "Charlie", "age": 40})

    # Query by email (equality)
    result = client.lookup_index(collection, "email", "alice@example.com")
    
    assert "results" in result, "Response should have 'results' field"
    assert "count" in result, "Response should have 'count' field"
    assert result["count"] == 2, f"Expected 2 results, got {result['count']}"
    
    keys = {r["key"] for r in result["results"]}
    assert keys == {"user1", "user3"}, f"Expected user1 and user3, got {keys}"
    
    # Verify values are included
    for r in result["results"]:
        assert "value" in r, "Each result should have 'value' field"
        assert r["value"]["email"] == "alice@example.com"
    
    print("[PASS] Basic equality lookup test passed")
    return True


def test_secondary_index_no_matches(client: DesentryClient) -> bool:
    """Test lookup with no matching documents."""
    collection = "test_users_index"
    
    result = client.lookup_index(collection, "email", "nobody@example.com")
    
    assert result["count"] == 0, f"Expected 0 results, got {result['count']}"
    assert result["results"] == [], "Expected empty results list"
    
    print("[PASS] No matches test passed")
    return True


def test_secondary_index_missing_field(client: DesentryClient) -> bool:
    """Test lookup on field that doesn't exist in documents."""
    collection = "test_users_index"
    
    result = client.lookup_index(collection, "nonexistent_field", "any_value")
    
    assert result["count"] == 0, f"Expected 0 results for missing field, got {result['count']}"
    
    print("[PASS] Missing field test passed")
    return True


def test_secondary_index_numeric_value(client: DesentryClient) -> bool:
    """Test lookup on numeric field."""
    collection = "test_users_index"
    
    result = client.lookup_index(collection, "age", 30)
    
    assert result["count"] == 1, f"Expected 1 result for age=30, got {result['count']}"
    assert result["results"][0]["key"] == "user1"
    
    print("[PASS] Numeric value test passed")
    return True


def test_secondary_index_limit(client: DesentryClient) -> bool:
    """Test that limit parameter works."""
    collection = "test_users_index"
    
    # Insert more documents with same email
    for i in range(5, 15):
        client.put(collection, f"user{i}", {"email": "many@example.com", "name": f"User{i}"})
    
    result = client.lookup_index(collection, "email", "many@example.com", limit=5)
    
    assert result["count"] <= 5, f"Expected at most 5 results, got {result['count']}"
    
    print("[PASS] Limit test passed")
    return True


def test_secondary_index_special_characters(client: DesentryClient) -> bool:
    """Test lookup with special characters in value."""
    collection = "test_special_index"
    
    # Clean up
    for key in ["doc1", "doc2"]:
        try:
            client.delete(collection, key)
        except Exception:
            pass
    
    client.put(collection, "doc1", {"tag": "hello world", "data": 1})
    client.put(collection, "doc2", {"tag": "hello+world", "data": 2})
    
    result = client.lookup_index(collection, "tag", "hello world")
    assert result["count"] == 1, f"Expected 1 result, got {result['count']}"
    assert result["results"][0]["key"] == "doc1"
    
    result = client.lookup_index(collection, "tag", "hello+world")
    assert result["count"] == 1, f"Expected 1 result, got {result['count']}"
    assert result["results"][0]["key"] == "doc2"
    
    print("[PASS] Special characters test passed")
    return True


def cleanup_test_data(client: DesentryClient):
    """Clean up test collections."""
    # We can't delete collections, but we can delete the keys we created
    pass  # Test data cleanup is done in each test


def run_tests(base_url: str) -> bool:
    """Run all secondary index tests."""
    client = DesentryClient(base_url)
    
    # Check if node is running
    if not client.ping():
        print(f"[FAIL] Node at {base_url} is not responding")
        return False
    
    print(f"Testing secondary index lookup at {base_url}")
    print("-" * 50)
    
    tests = [
        test_secondary_index_basic,
        test_secondary_index_no_matches,
        test_secondary_index_missing_field,
        test_secondary_index_numeric_value,
        test_secondary_index_limit,
        test_secondary_index_special_characters,
    ]
    
    passed = 0
    failed = 0
    
    for test in tests:
        try:
            if test(client):
                passed += 1
            else:
                failed += 1
                print(f"[FAIL] {test.__name__} failed")
        except AssertionError as e:
            failed += 1
            print(f"[FAIL] {test.__name__} assertion failed: {e}")
        except Exception as e:
            failed += 1
            print(f"[FAIL] {test.__name__} error: {e}")
    
    print("-" * 50)
    print(f"Results: {passed} passed, {failed} failed")
    
    return failed == 0


if __name__ == "__main__":
    base_url = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:7701"
    success = run_tests(base_url)
    sys.exit(0 if success else 1)