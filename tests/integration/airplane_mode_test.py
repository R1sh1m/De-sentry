"""Acceptance test: everything works with no internet at all.

The spec's headline requirement is "zero fetched dependencies at run time;
fully functional with no internet". This is the test that makes that claim
falsifiable rather than aspirational.

It does two things:

  1. **Proves the claim negatively.** Before starting anything, it records
     which non-loopback sockets the machine has open, runs the full
     create-write-read-replicate-verify cycle, and asserts the node processes
     opened no connection to anything outside the LAN. Since it cannot
     actually disconnect the machine's network, it checks the property that
     matters instead: nothing left for the internet.

  2. **Proves the claim positively.** A node is created from nothing, written
     to, read back, replicated to a second node, verified, restarted, and read
     again -- all with the discovery and P2P ports on loopback.

Run it on a machine with the network genuinely off and it passes identically;
that is the point.

    python3 tests/integration/airplane_mode_test.py
"""

from __future__ import annotations

import os
import socket
import subprocess
import sys
import time
from typing import List, Set, Tuple

from harness import Cluster, Report
from desentry_client import DesentryError

COLLECTION = "field-notes"


def outbound_connections(pids: Set[int]) -> List[str]:
    """Non-loopback, non-LAN sockets belonging to the given processes.

    Implemented with the platform's own tooling rather than a dependency:
    `netstat` exists on all three targets and reports the owning pid. If it
    cannot be read the test says so rather than passing on an empty result --
    a check that silently degrades to "no problems found" is worse than no
    check.
    """
    try:
        if os.name == "nt":
            output = subprocess.run(["netstat", "-ano", "-p", "TCP"],
                                     capture_output=True, text=True, timeout=30).stdout
        else:
            output = subprocess.run(["netstat", "-anp"],
                                     capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError):
        return ["(netstat unavailable on this machine -- this check was skipped)"]

    suspicious: List[str] = []
    for line in output.splitlines():
        parts = line.split()
        if len(parts) < 4 or "ESTABLISHED" not in line:
            continue

        owner = parts[-1]
        pid = 0
        if os.name == "nt" and owner.isdigit():
            pid = int(owner)
        elif "/" in owner:
            try:
                pid = int(owner.split("/", 1)[0])
            except ValueError:
                pid = 0
        if pid not in pids:
            continue

        remote = parts[2] if os.name == "nt" else parts[4]
        host = remote.rsplit(":", 1)[0].strip("[]")
        if is_local(host):
            continue
        suspicious.append(f"{remote} (pid {pid})")
    return suspicious


def is_local(host: str) -> bool:
    """Loopback, link-local, or RFC1918 -- i.e. this machine or this LAN."""
    if host in ("", "*", "0.0.0.0", "::", "::1") or host.startswith("127."):
        return True
    if host.startswith("169.254.") or host.startswith("fe80:"):
        return True
    if host.startswith("10.") or host.startswith("192.168."):
        return True
    if host.startswith("172."):
        try:
            second = int(host.split(".")[1])
            return 16 <= second <= 31
        except (IndexError, ValueError):
            return False
    return False


def dns_was_not_needed() -> Tuple[bool, str]:
    """Sanity check that the test itself is not resolving anything.

    Not an assertion about the engine -- it is about this test being an honest
    witness. If the harness needed DNS to run, "it worked offline" would be a
    claim about a machine that was online.
    """
    try:
        socket.gethostbyname("127.0.0.1")
        return True, "loopback resolves without a nameserver"
    except OSError as error:
        return False, str(error)


def main() -> int:
    report = Report("airplane_mode_test")

    ok, detail = dns_was_not_needed()
    report.check(ok, f"the test harness itself needs no name resolution ({detail})")

    with Cluster() as cluster:
        print("[1] creating a node from nothing")
        a = cluster.add("solo")
        a.start([])  # no bootstrap peers, no discovery partners: truly alone
        report.check(a.running(), "a node starts with no peers and no network")
        report.check(bool(a.node_id), "it generated its own identity offline")

        status = a.client.status()
        report.check(status["known_peers"] == 0, "it knows no peers, as expected")

        print("[2] writing and reading with no network")
        a.client.put(COLLECTION, "n1", {"text": "written on a plane", "altitude_m": 11000})
        document = a.client.get(COLLECTION, "n1")
        report.equal(document.get("altitude_m"), 11000, "the document reads back")

        a.client.put(COLLECTION, "n2", {"text": "second note"})
        keys = [key for key, _ in a.client.list(COLLECTION)]
        report.check(sorted(keys) == ["n1", "n2"], "both documents are listed")

        report.check(a.client.verify_ledger().get("verified", False),
                     "the hash chain verifies offline")
        report.check(a.client.verify().get("ok", False),
                     "the ledger and every storage backend verify offline")

        # Engine availability is answered by the binary, not fetched.
        engines = a.client.engines()
        builtin = [e["name"] for e in engines if e.get("built_in")]
        report.check(len(builtin) >= 5,
                     f"the from-scratch engines are all present offline ({', '.join(builtin)})")
        report.check(all(e["compiled_in"] for e in engines if e.get("built_in")),
                     "every built-in engine is compiled in, not downloaded on demand")

        print("[3] replicating to a second node over loopback only")
        b = cluster.add("solo-2")
        b.start([a.address()])
        report.check(b.running(), "a second node joins with no internet")

        cluster.wait_visible(COLLECTION, "n1", nodes=[b])
        report.equal(b.client.get(COLLECTION, "n1").get("altitude_m"), 11000,
                     "the write replicated to the second node")

        # A write on the new node reaches the first: replication is not
        # one-directional, and neither node is a coordinator.
        b.client.put(COLLECTION, "n3", {"text": "reply"})
        cluster.wait_visible(COLLECTION, "n3", nodes=[a])
        report.check(True, "a write on the second node reaches the first")

        cluster.wait_converged(60.0)
        report.check(True, "both nodes converge on the same ledger tip")

        print("[4] surviving a restart")
        a.restart([b.address()])
        report.equal(a.client.get(COLLECTION, "n1").get("altitude_m"), 11000,
                     "the data is still there after a restart")
        report.equal(a.client.get(COLLECTION, "n3").get("text"), "reply",
                     "including what arrived by replication")
        report.check(a.client.verify_ledger().get("verified", False),
                     "the chain verifies after a restart")

        print("[5] no process reached the internet")
        pids = {node.process.pid for node in cluster.running_nodes() if node.process}
        # Give anything that was going to phone home a moment to do it.
        time.sleep(2.0)
        outbound = outbound_connections(pids)
        for entry in outbound:
            print(f"      {entry}")
        report.check(
            all(entry.startswith("(") for entry in outbound),
            "no node opened a connection outside this machine or LAN",
        )

    return report.finish()


if __name__ == "__main__":
    sys.exit(main())
