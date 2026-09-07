"""Spawns and supervises real `desentryd` processes for the v2 acceptance tests.

The v1 integration test drives a cluster somebody else started (docker compose,
or run_cluster.sh). The v2 acceptance tests cannot: they need to kill a node
mid-flight, move a data directory between machines, and start fifty nodes with
no network reachable. So this harness owns the processes.

Everything here is stdlib only, and works on Windows, macOS and Linux -- the
three platforms the app ships for. That rules out `kill -9` as a literal
command and `/tmp` as a literal path; `Popen.kill()` and `tempfile` are the
portable forms of both, and `kill()` really is SIGKILL / TerminateProcess, so
"the owner was killed without a chance to flush" is honestly reproduced.
"""

from __future__ import annotations

import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from typing import Any, Dict, List, Optional

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "clients", "python"))

from desentry_client import DesentryClient, DesentryError  # noqa: E402

READY_TIMEOUT_S = 30.0
POLL_S = 0.2


def find_engine(explicit: Optional[str] = None) -> str:
    """Locates the `desentryd` binary.

    Checked in order: an explicit path, $DESENTRY_ENGINE, PATH, then the usual
    CMake output directories. Failing with the build command in the message
    beats failing with 'No such file or directory'.
    """
    exe = "desentryd.exe" if os.name == "nt" else "desentryd"
    candidates: List[str] = []
    if explicit:
        candidates.append(explicit)
    if os.environ.get("DESENTRY_ENGINE"):
        candidates.append(os.environ["DESENTRY_ENGINE"])

    found = shutil.which("desentryd")
    if found:
        candidates.append(found)

    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    for relative in ("build", "build/RelWithDebInfo", "build/Release", "build/Debug"):
        candidates.append(os.path.join(repo, relative, exe))

    for candidate in candidates:
        if candidate and os.path.isfile(candidate):
            return candidate

    raise FileNotFoundError(
        f"could not find {exe}. Build it first:\n"
        "  cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo\n"
        "  cmake --build build --config RelWithDebInfo\n"
        "or set DESENTRY_ENGINE to its path."
    )


def free_port(start: int) -> int:
    """First port at or above `start` that nothing is listening on.

    Binding is the only test that means anything here; reading a list of
    listeners and picking a gap races with every other process on the machine.
    """
    for port in range(start, start + 500):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            try:
                probe.bind(("127.0.0.1", port))
                return port
            except OSError:
                continue
    raise RuntimeError(f"no free port in {start}..{start + 500}")


class Node:
    """One `desentryd` process, its config, and a client for it."""

    def __init__(self, name: str, data_dir: str, api_port: int, p2p_port: int,
                 discovery_port: int, engine: str, *, supervisor: bool = False,
                 config_overrides: Optional[Dict[str, Any]] = None):
        self.name = name
        self.data_dir = data_dir
        self.api_port = api_port
        self.p2p_port = p2p_port
        self.discovery_port = discovery_port
        self.engine = engine
        self.supervisor = supervisor
        self.config_overrides = config_overrides or {}
        self.process: Optional[subprocess.Popen] = None
        self.log_path = os.path.join(data_dir, "node.log")
        self.node_id: str = ""
        self.client = DesentryClient(f"http://127.0.0.1:{api_port}")

    # -- configuration -------------------------------------------------------
    def config(self, bootstrap_peers: Optional[List[str]] = None) -> Dict[str, Any]:
        config: Dict[str, Any] = {
            "data_dir": self.data_dir,
            "node_name": self.name,
            "api_bind_addr": "127.0.0.1",
            "api_port": self.api_port,
            # A supervisor binds P2P to loopback too: it holds no replicated
            # data and must not be reachable from the LAN.
            "p2p_bind_addr": "127.0.0.1" if self.supervisor else "0.0.0.0",
            "p2p_port": self.p2p_port,
            "discovery_enabled": not self.supervisor,
            "discovery_port": self.discovery_port,
            "discovery_interval_ms": 500,
            "gossip_interval_ms": 400,
            "bootstrap_peers": bootstrap_peers or [],
            "buffer_pool_pages": 256,
            "supervisor": self.supervisor,
            "quota_mb": 0,
            "engines": ["kv"],
            "default_engine": "kv",
            "replication_factor": 3,
            "transit_ttl_seconds": 3600,
            "mdns_enabled": False,
        }
        config.update(self.config_overrides)
        return config

    def write_config(self, bootstrap_peers: Optional[List[str]] = None) -> str:
        os.makedirs(self.data_dir, exist_ok=True)
        path = os.path.join(self.data_dir, "node.json")
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(self.config(bootstrap_peers), handle, indent=2)
        return path

    # -- lifecycle -----------------------------------------------------------
    def start(self, bootstrap_peers: Optional[List[str]] = None,
              wait: bool = True) -> "Node":
        config_path = self.write_config(bootstrap_peers)
        log = open(self.log_path, "ab")
        self.process = subprocess.Popen(
            [self.engine, "--config", config_path],
            stdout=log,
            stderr=subprocess.STDOUT,
            # A new process group on POSIX so a Ctrl-C in the test runner does
            # not take the nodes with it before their assertions run.
            start_new_session=(os.name != "nt"),
        )
        if wait:
            self.wait_ready()
        return self

    def wait_ready(self, timeout_s: float = READY_TIMEOUT_S) -> None:
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            if self.process is not None and self.process.poll() is not None:
                raise RuntimeError(
                    f"{self.name} exited with code {self.process.returncode} before answering.\n"
                    f"{self.tail_log()}"
                )
            try:
                status = self.client.status()
                self.node_id = status.get("node_id", "")
                if self.node_id:
                    return
            except Exception:
                pass
            time.sleep(POLL_S)
        raise TimeoutError(f"{self.name} did not answer on :{self.api_port}\n{self.tail_log()}")

    def kill(self) -> None:
        """Terminates without a chance to flush -- SIGKILL, or its Windows
        equivalent. This is what the transit test needs: a node that vanished
        rather than one that shut down.
        """
        if self.process is None:
            return
        self.process.kill()
        self.process.wait(timeout=10)
        self.process = None

    def stop(self) -> None:
        """Asks the node to exit, then insists."""
        if self.process is None:
            return
        self.process.terminate()
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=10)
        self.process = None

    def restart(self, bootstrap_peers: Optional[List[str]] = None) -> "Node":
        self.stop()
        return self.start(bootstrap_peers)

    def running(self) -> bool:
        return self.process is not None and self.process.poll() is None

    def tail_log(self, lines: int = 25) -> str:
        try:
            with open(self.log_path, "r", encoding="utf-8", errors="replace") as handle:
                tail = handle.readlines()[-lines:]
            return "".join(f"    {self.name}| {line}" for line in tail)
        except OSError:
            return f"    {self.name}| (no log)"

    def address(self) -> str:
        return f"127.0.0.1:{self.p2p_port}"


class Cluster:
    """A set of nodes sharing a discovery port and a temporary root."""

    def __init__(self, engine: Optional[str] = None, root: Optional[str] = None,
                 api_base: int = 17700, p2p_base: int = 17800):
        self.engine = find_engine(engine)
        self.owns_root = root is None
        self.root = root or tempfile.mkdtemp(prefix="desentry-acceptance-")
        self.api_base = api_base
        self.p2p_base = p2p_base
        self.discovery_port = free_port(17900)
        self.nodes: List[Node] = []

    def add(self, name: str, *, supervisor: bool = False,
            data_dir: Optional[str] = None,
            config_overrides: Optional[Dict[str, Any]] = None) -> Node:
        api_port = free_port(self.api_base + len(self.nodes) * 2)
        p2p_port = free_port(self.p2p_base + len(self.nodes) * 2)
        node = Node(
            name=name,
            data_dir=data_dir or os.path.join(self.root, name),
            api_port=api_port,
            p2p_port=p2p_port,
            discovery_port=self.discovery_port,
            engine=self.engine,
            supervisor=supervisor,
            config_overrides=config_overrides,
        )
        self.nodes.append(node)
        return node

    def start_all(self, *, mesh: bool = True) -> "Cluster":
        """Starts every node.

        With `mesh`, each node is given every other as a bootstrap peer.
        Broadcast discovery would usually find them, but a test that depends on
        UDP broadcast reaching loopback is a test that fails on somebody's
        laptop for reasons unrelated to the code.
        """
        for node in self.nodes:
            peers = [other.address() for other in self.nodes if other is not node] if mesh else []
            node.start(peers)
        return self

    def stop_all(self) -> None:
        for node in self.nodes:
            try:
                node.stop()
            except Exception:
                pass

    def cleanup(self) -> None:
        self.stop_all()
        if self.owns_root:
            shutil.rmtree(self.root, ignore_errors=True)

    def __enter__(self) -> "Cluster":
        return self

    def __exit__(self, *_exc: Any) -> None:
        self.cleanup()

    # -- assertions ----------------------------------------------------------
    def running_nodes(self) -> List[Node]:
        return [node for node in self.nodes if node.running()]

    def wait_for(self, predicate, timeout_s: float, label: str) -> None:
        deadline = time.time() + timeout_s
        last: Optional[Exception] = None
        while time.time() < deadline:
            try:
                if predicate():
                    return
            except Exception as exc:
                last = exc
            time.sleep(POLL_S)
        logs = "\n".join(node.tail_log(10) for node in self.nodes)
        raise TimeoutError(f"timed out waiting for {label} (last error: {last})\n{logs}")

    def wait_converged(self, timeout_s: float = 60.0) -> None:
        """Waits until every running node reports the same ledger tip.

        Compares the hash as well as the entry id: two nodes at the same height
        with different hashes are diverged, not converged, and that is the one
        disagreement worth catching.
        """
        def same_tip() -> bool:
            tips = set()
            for node in self.running_nodes():
                tip = node.client.ledger_tip()
                tips.add((tip["entry_id"], tip["entry_hash"]))
            return len(tips) == 1

        self.wait_for(same_tip, timeout_s, "every node to agree on the ledger tip")

    def wait_visible(self, collection: str, key: str, timeout_s: float = 45.0,
                     nodes: Optional[List[Node]] = None) -> None:
        targets = nodes if nodes is not None else self.running_nodes()

        def visible() -> bool:
            for node in targets:
                try:
                    node.client.get(collection, key)
                except DesentryError:
                    return False
            return True

        self.wait_for(visible, timeout_s, f"{collection}/{key} on every node")


# -- test-report plumbing ----------------------------------------------------

class Report:
    """Counts checks so a test prints one honest summary line at the end."""

    def __init__(self, name: str):
        self.name = name
        self.passed = 0
        self.failures: List[str] = []

    def check(self, condition: bool, label: str) -> bool:
        if condition:
            self.passed += 1
            print(f"  ok: {label}")
        else:
            self.failures.append(label)
            print(f"  FAIL: {label}")
        return condition

    def equal(self, actual: Any, expected: Any, label: str) -> bool:
        return self.check(actual == expected, f"{label} (expected {expected!r}, got {actual!r})"
                          if actual != expected else label)

    def finish(self) -> int:
        print()
        if self.failures:
            print(f"{self.name}: {len(self.failures)} FAILED, {self.passed} passed")
            for failure in self.failures:
                print(f"  - {failure}")
            return 1
        print(f"{self.name}: ALL {self.passed} CHECKS PASSED")
        return 0
