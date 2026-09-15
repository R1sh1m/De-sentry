#!/usr/bin/env python3
"""
End-to-end soak test: durability=3 with node restarts.

This test verifies that writes with durability=3 (writer + 2 replicas)
survive node restarts and continue to converge correctly.

Usage:
    python3 durability_soak_test.py --nodes 5 --writes 100 --restarts 3 --timeout-ms 5000
"""

import argparse
import json
import os
import random
import signal
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import urllib.error
from concurrent.futures import ThreadPoolExecutor, as_completed

# Default paths
DEFAULT_DESENTRYD = os.environ.get("DESENTRYD_PATH", "./build/desentryd.exe")


def parse_args():
    p = argparse.ArgumentParser(description="Durability soak test with node restarts")
    p.add_argument("--desentryd", default=DEFAULT_DESENTRYD, help="Path to desentryd binary")
    p.add_argument("--nodes", type=int, default=5, help="Number of nodes in the mesh")
    p.add_argument("--writes", type=int, default=100, help="Total writes per node")
    p.add_argument("--restarts", type=int, default=3, help="Number of restart cycles per node")
    p.add_argument("--timeout-ms", type=int, default=5000, help="Durability timeout in ms")
    p.add_argument("--durability", type=int, default=3, help="Durability level (default 3)")
    p.add_argument("--seed", type=int, default=42, help="Random seed for reproducibility")
    p.add_argument("--verbose", action="store_true", help="Verbose output")
    return p.parse_args()


class Node:
    def __init__(self, idx, base_port, data_dir, desentryd_path, verbose=False):
        self.idx = idx
        self.api_port = base_port + idx
        self.p2p_port = base_port + 1000 + idx
        self.data_dir = os.path.join(data_dir, f"node_{idx}")
        self.desentryd_path = desentryd_path
        self.proc = None
        self.node_id = None
        self.verbose = verbose
        self.lock = threading.Lock()

    def start(self):
        os.makedirs(self.data_dir, exist_ok=True)
        cmd = [
            self.desentryd_path,
            "--data-dir", self.data_dir,
            "--api-port", str(self.api_port),
            "--p2p-port", str(self.p2p_port),
        ]
        if self.verbose:
            print(f"  Starting node {self.idx}: {' '.join(cmd)}")
        self.proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if sys.platform == "win32" else 0,
        )
        # Wait for node to be ready
        for _ in range(30):
            time.sleep(0.1)
            if self._check_health():
                return True
        raise RuntimeError(f"Node {self.idx} failed to start")

    def _check_health(self):
        try:
            resp = urllib.request.urlopen(f"http://127.0.0.1:{self.api_port}/_status", timeout=1)
            data = json.load(resp)
            self.node_id = data.get("node_id")
            return True
        except Exception:
            return False

    def stop(self):
        if self.proc:
            if self.verbose:
                print(f"  Stopping node {self.idx} (pid={self.proc.pid})")
            if sys.platform == "win32":
                self.proc.send_signal(signal.CTRL_BREAK_EVENT)
            else:
                self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
            self.proc = None

    def restart(self):
        self.stop()
        time.sleep(0.5)
        self.start()

    def write(self, collection, key, value):
        """Write with durability=3, return (success, report)"""
        url = f"http://127.0.0.1:{self.api_port}/db/{collection}/{key}"
        body = json.dumps(value).encode()
        full_url = f"{url}?durability=3&timeout_ms=5000"
        req = urllib.request.Request(
            full_url,
            data=body,
            method="PUT",
            headers={"Content-Type": "application/json"}
        )
        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                data = json.load(resp)
                return resp.status == 200, data.get("durability", {})
        except urllib.error.HTTPError as e:
            if e.code == 202:
                data = json.load(e)
                return True, data.get("durability", {})  # 202 = partial but accepted
            return False, {}
        except Exception as e:
            return False, {"error": str(e)}

    def read(self, collection, key):
        url = f"http://127.0.0.1:{self.api_port}/db/{collection}/{key}"
        try:
            with urllib.request.urlopen(url, timeout=5) as resp:
                return True, json.load(resp)
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return True, None
            return False, None
        except Exception:
            return False, None

    def get_peers(self):
        url = f"http://127.0.0.1:{self.api_port}/_peers"
        try:
            with urllib.request.urlopen(url, timeout=5) as resp:
                return json.load(resp)
        except Exception:
            return []


def run_soak_test(args):
    random.seed(args.seed)
    base_port = 17700 + (args.seed % 1000) * 10  # Avoid port conflicts
    temp_dir = tempfile.mkdtemp(prefix="desentry_durability_soak_")
    print(f"Test directory: {temp_dir}")

    nodes = []
    for i in range(args.nodes):
        node = Node(i, base_port, temp_dir, args.desentryd, args.verbose)
        nodes.append(node)

    try:
        # Start all nodes
        print(f"Starting {args.nodes} nodes...")
        for node in nodes:
            node.start()
        print("All nodes started")

        # Wait for mesh to form
        time.sleep(3)

        # Verify mesh connectivity
        for node in nodes:
            peers = node.get_peers()
            if args.verbose:
                print(f"Node {node.idx} ({node.node_id[:8]}) sees {len(peers)} peers")

        collection = "durability_test"
        expected_keys = []

        # Write phase with durability=3
        print(f"\nPhase 1: Writing {args.writes} keys with durability={args.durability}...")
        write_errors = 0
        achieved_counts = {3: 0, 2: 0, 1: 0}
        for w in range(args.writes):
            key = f"key_{w:06d}"
            value = {"seq": w, "data": random.randint(0, 1000000), "node": args.seed}
            # Pick a random node to write from
            writer = random.choice(nodes)
            ok, durability = writer.write(collection, key, value)
            if not ok:
                write_errors += 1
                if args.verbose:
                    print(f"  Write {key} FAILED: {durability}")
                continue
            expected_keys.append(key)
            achieved = durability.get("achieved", 0)
            achieved_counts[achieved] = achieved_counts.get(achieved, 0) + 1
            if args.verbose and w % 10 == 0:
                print(f"  Write {key}: achieved={durability.get('achieved', '?')}, timed_out={durability.get('timed_out', '?')}")

        print(f"  Write errors: {write_errors}/{args.writes}")
        print(f"  Durability achieved: {achieved_counts}")

        # Restart cycles
        print(f"\nPhase 2: {args.restarts} restart cycles...")
        for cycle in range(args.restarts):
            print(f"  Cycle {cycle + 1}/{args.restarts}")
            # Restart random subset of nodes
            restart_nodes = random.sample(nodes, max(1, args.nodes // 2))
            for node in restart_nodes:
                if args.verbose:
                    print(f"    Restarting node {node.idx}...")
                node.restart()
            # Wait for mesh to re-form
            time.sleep(2)

            # Verify data integrity after restarts
            print(f"    Verifying {len(expected_keys)} keys...")
            for key in random.sample(expected_keys, min(20, len(expected_keys))):
                values = []
                for node in nodes:
                    ok, val = node.read(collection, key)
                    if ok and val is not None:
                        values.append(val)
                if len(set(json.dumps(v, sort_keys=True) for v in values)) > 1:
                    print(f"    ERROR: Divergence detected on {key}: {values}")
                    return False
            if args.verbose:
                print(f"    All sampled keys consistent")

        # Final convergence check
        print("\nPhase 3: Final convergence check...")
        time.sleep(3)  # Let gossip settle
        all_consistent = True
        for key in expected_keys:
            values = []
            for node in nodes:
                ok, val = node.read(collection, key)
                if not ok or val is None:
                    all_consistent = False
                    break
                values.append(val)
            if len(set(json.dumps(v, sort_keys=True) for v in values)) > 1:
                print(f"ERROR: Divergence on {key}: {values}")
                all_consistent = False
                break

        if all_consistent:
            print("SUCCESS: All keys consistent across all nodes after restarts!")
            return True
        else:
            print("FAILURE: Data divergence detected")
            return False

    finally:
        print("\nCleaning up...")
        for node in nodes:
            node.stop()
        # Clean up temp dir
        import shutil
        shutil.rmtree(temp_dir, ignore_errors=True)


def main():
    args = parse_args()
    print(f"Durability Soak Test")
    print(f"  Nodes: {args.nodes}, Writes: {args.writes}, Restarts: {args.restarts}")
    print(f"  Durability: {args.durability}, Timeout: {args.timeout_ms}ms")
    print(f"  Binary: {args.desentryd}")

    success = run_soak_test(args)
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()