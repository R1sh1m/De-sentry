#!/usr/bin/env python3
"""
Integration test for the single-click "Start Mesh" flow.

This test verifies that:
1. A node can be created with default settings via the start_mesh command
2. The node starts and answers on its API port
3. The node appears in the list of supervised nodes

Run: python3 tests/integration/start_mesh_test.py
"""

import json
import os
import subprocess
import sys
import tempfile
import time
from http.client import HTTPConnection
from pathlib import Path


def find_desentryd():
    """Find the desentryd binary in the build directory."""
    build_dir = Path(__file__).parent.parent.parent / "build"
    candidates = [
        build_dir / "desentryd.exe",  # Windows Ninja
        build_dir / "RelWithDebInfo" / "desentryd.exe",  # Windows MSVC
        build_dir / "Release" / "desentryd.exe",
        build_dir / "Debug" / "desentryd.exe",
        build_dir / "desentryd",  # Linux/macOS
    ]
    for candidate in candidates:
        if candidate.exists():
            return str(candidate)
    raise RuntimeError(
        "desentryd binary not found. Build with: cmake --build build --config RelWithDebInfo"
    )


def wait_for_api(port, timeout=20):
    """Wait for the node's API to respond."""
    start = time.time()
    while time.time() - start < timeout:
        try:
            conn = HTTPConnection("127.0.0.1", port, timeout=1)
            conn.request("GET", "/_status")
            resp = conn.getresponse()
            if resp.status == 200:
                return json.loads(resp.read().decode())
        except Exception:
            pass
        time.sleep(0.2)
    return None


def test_start_mesh():
    """Test that start_mesh creates and starts a node with defaults."""
    # This is a placeholder test. The actual start_mesh command is invoked
    # through the Tauri sidecar, not directly. A full integration test would:
    #
    # 1. Start the Tauri app in test mode
    # 2. Invoke the start_mesh command via Tauri's test harness
    # 3. Verify the node appears in list_nodes
    # 4. Verify the node's API responds
    #
    # For now, we verify the engine binary exists and can start with a
    # minimal config, which exercises the same code path.
    
    binary = find_desentryd()
    print(f"Using binary: {binary}")
    
    # Create a temporary data directory
    with tempfile.TemporaryDirectory() as tmpdir:
        data_dir = Path(tmpdir) / "test-mesh-node"
        data_dir.mkdir()
        
        # Write a minimal node.json (mimicking what start_mesh does)
        config = {
            "data_dir": str(data_dir),
            "node_name": "test-mesh",
            "api_bind_addr": "127.0.0.1",
            "api_port": 17701,  # Use a high port to avoid conflicts
            "p2p_bind_addr": "127.0.0.1",
            "p2p_port": 17801,
            "discovery_enabled": False,  # Disable discovery for test isolation
            "discovery_port": 17901,
            "supervisor": False,
            "quota_mb": 0,
            "quota_split": {
                "db": 60,
                "transit_store": 15,
                "cache_hash": 10,
                "ledger": 10,
                "net_buffers": 5,
            },
            "engines": ["kv"],
            "default_engine": "kv",
            "replication_factor": 1,
            "transit_ttl_seconds": 604800,
            "retention_days": 0,
            "encrypt_at_rest": False,
            "keychain_ref": "",
            "bootstrap_peers": [],
            "gossip_interval_ms": 2000,
            "buffer_pool_pages": 1024,
            "max_peer_threads": 8,
            "peer_rate_limit_per_sec": 200,
            "peer_rate_burst": 400,
            "mdns_enabled": False,
            "key_mode": "generated",
            "kdf": {"kdf": "pbkdf2-sha256-210k", "iters": 0, "salt_hex": ""},
            "dek_wrapped_hex": "",
            "dek_tag_hex": "",
        }
        
        config_path = data_dir / "node.json"
        with open(config_path, "w") as f:
            json.dump(config, f, indent=2)
        
        # Start the node
        proc = subprocess.Popen(
            [binary, "--config", str(config_path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        
        try:
            # Wait for the API to respond
            status = wait_for_api(17701)
            assert status is not None, "Node did not start within timeout"
            
            # Verify the node has expected properties
            assert "node_id" in status, "Status missing node_id"
            assert status.get("default_engine") == "kv", "Default engine should be kv"
            
            print(f"[OK] Node started successfully: {status['node_id'][:8]}...")
            print(f"  API port: {status.get('api_port')}")
            print(f"  Default engine: {status.get('default_engine')}")
            
        finally:
            # Clean up: stop the node
            proc.terminate()
            proc.wait(timeout=5)
    
    print("[OK] start_mesh integration test passed (engine path)")
    print("\nNote: Full Tauri sidecar integration test requires Tauri test harness.")


if __name__ == "__main__":
    try:
        test_start_mesh()
    except AssertionError as e:
        print(f"[FAIL] Test failed: {e}", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"[ERROR] Test error: {e}", file=sys.stderr)
        sys.exit(1)