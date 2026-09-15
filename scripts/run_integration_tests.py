#!/usr/bin/env python3
"""Unified runner for De-Sentry's Python integration test suites.

Executes real-process acceptance tests driven via the standard library HTTP client:
  - transit_replay_test.py (offline write hold, claim, and reconvergence)
  - airplane_mode_test.py (zero-egress, offline operation verification)
  - usb_node_test.py (removable drive data-directory relocation & catchup)
  - soak_test.py (bounded or full cluster soak under chaotic kills)

Usage:
  python scripts/run_integration_tests.py
  python scripts/run_integration_tests.py --smoke
  python scripts/run_integration_tests.py --full-soak
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from typing import List, Tuple

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
TESTS_DIR = os.path.join(REPO_ROOT, "tests", "integration")

sys.path.insert(0, TESTS_DIR)
try:
    from harness import find_engine
except ImportError:
    find_engine = None


def run_suite(cmd: List[str], name: str, timeout: float = 300.0) -> Tuple[bool, float, str]:
    print(f"\n{'='*20} RUNNING: {name} {'='*20}")
    start = time.monotonic()
    try:
        proc = subprocess.run(
            cmd,
            cwd=REPO_ROOT,
            stdout=sys.stdout,
            stderr=sys.stderr,
            timeout=timeout,
        )
        duration = time.monotonic() - start
        return (proc.returncode == 0, duration, "OK" if proc.returncode == 0 else f"exit {proc.returncode}")
    except subprocess.TimeoutExpired:
        duration = time.monotonic() - start
        print(f"\n[ERROR] {name} timed out after {timeout}s", file=sys.stderr)
        return (False, duration, "TIMEOUT")
    except Exception as e:
        duration = time.monotonic() - start
        print(f"\n[ERROR] {name} failed with exception: {e}", file=sys.stderr)
        return (False, duration, str(e))


def main() -> int:
    parser = argparse.ArgumentParser(description="Run De-Sentry Python integration suites.")
    parser.add_argument("--smoke", action="store_true", default=True,
                        help="Run bounded soak (12 nodes, 150 writes, 3 chaos kills) [default]")
    parser.add_argument("--full-soak", action="store_true",
                        help="Run full soak (50 nodes, 500 writes, 8 chaos kills)")
    parser.add_argument("--engine", default=None,
                        help="Explicit path to desentryd binary")
    args = parser.parse_args()

    engine_bin = find_engine(args.engine) if find_engine else None
    if not engine_bin or not os.path.isfile(engine_bin):
        print(f"[ERROR] Could not find desentryd binary. Build it first with 'cmake --build build'",
              file=sys.stderr)
        return 1

    print(f"Using engine binary: {engine_bin}")
    os.environ["DESENTRY_ENGINE"] = engine_bin

    py_exe = sys.executable
    suites: List[Tuple[str, List[str], float]] = [
        ("Transit Replay", [py_exe, os.path.join(TESTS_DIR, "transit_replay_test.py")], 120.0),
        ("Airplane Mode", [py_exe, os.path.join(TESTS_DIR, "airplane_mode_test.py")], 120.0),
        ("USB Removable Node", [py_exe, os.path.join(TESTS_DIR, "usb_node_test.py")], 120.0),
    ]

    if args.full_soak:
        suites.append((
            "50-Node Soak",
            [py_exe, os.path.join(TESTS_DIR, "soak_test.py"), "--nodes", "50", "--writes", "500", "--chaos", "8", "--settle", "180"],
            400.0,
        ))
    else:
        suites.append((
            "12-Node Bounded Soak",
            [py_exe, os.path.join(TESTS_DIR, "soak_test.py"), "--nodes", "12", "--writes", "150", "--chaos", "3"],
            180.0,
        ))

    results = []
    overall_start = time.monotonic()

    for name, cmd, timeout in suites:
        ok, dur, status_msg = run_suite(cmd, name, timeout=timeout)
        results.append((name, ok, dur, status_msg))
        if not ok:
            print(f"[FAIL] Suite {name} failed; aborting remaining suites.")
            break

    total_time = time.monotonic() - overall_start

    print("\n" + "=" * 60)
    print(f"{'Integration Suite Summary':^60}")
    print("=" * 60)
    all_ok = True
    for name, ok, dur, status_msg in results:
        status_str = "PASS" if ok else "FAIL"
        print(f"  {name:<30} {status_str:>6}  ({dur:>5.1f}s)  [{status_msg}]")
        if not ok:
            all_ok = False

    print("-" * 60)
    print(f"Total time: {total_time:.1f}s — Result: {'ALL PASSED' if all_ok else 'SOME FAILED'}\n")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
