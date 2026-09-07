"""Soak test: fifty nodes on one LAN, with chaos.

The spec's scale target is 50 nodes per user on a single WiFi network. This
starts that many on one machine, writes across all of them concurrently, kills
and restarts a rotating subset while the writes are still going, and then
requires that every survivor converges on the same ledger tip and the same
per-collection checksums.

Two honest caveats, stated rather than buried:

  * **Fifty processes on one machine is not fifty machines on one WiFi.** It
    shares a kernel, a loopback interface and a disk. What it does exercise is
    the parts most likely to break at scale -- fan-out bounded by the worker
    pool, message-ID dedup, gossip peer selection, the peer table under churn,
    and convergence with nodes coming and going. Real radio behaviour needs
    real radios.
  * **Packet loss is simulated by killing processes, not by dropping frames.**
    Injecting loss on loopback needs root and a platform-specific traffic
    shaper. Process churn is the harsher test of the same recovery paths: a
    dropped packet is retried, whereas a killed node has to rebuild its peer
    table, re-handshake, and claim whatever was held for it.

    python3 tests/integration/soak_test.py [--nodes 50] [--writes 500]
                                           [--chaos 8] [--settle 180]

This is slow (several minutes) and resource-hungry by design. It is not part of
`ctest`; it is the thing to run before believing a release.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import random
import sys
import time
from typing import Dict, List

from harness import Cluster, Node, Report
from desentry_client import DesentryError

COLLECTION = "soak"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nodes", type=int, default=50, help="node count (default 50)")
    parser.add_argument("--writes", type=int, default=500, help="documents to write")
    parser.add_argument("--chaos", type=int, default=8,
                        help="nodes killed and restarted during the write phase")
    parser.add_argument("--settle", type=float, default=180.0,
                        help="seconds allowed for convergence afterwards")
    parser.add_argument("--engine", default=None, help="path to desentryd")
    return parser.parse_args()


def bootstrap_set(nodes: List[Node], index: int, fanout: int = 4) -> List[str]:
    """A few neighbours each, not all forty-nine.

    Handing every node the full list would build a complete graph and make the
    test prove nothing about gossip -- the whole question at this scale is
    whether a sparse membership graph still converges. Neighbours are chosen by
    index so the graph is connected (a ring) plus a couple of chords for
    diameter.
    """
    count = len(nodes)
    peers = {(index + 1) % count, (index - 1) % count}
    for step in (count // 3, count // 7 + 1):
        peers.add((index + step) % count)
    peers.discard(index)
    return [nodes[i].address() for i in list(peers)[:fanout]]


def main() -> int:
    args = parse_args()
    report = Report("soak_test")
    random.seed(20260906)  # reproducible chaos

    print(f"[1] starting {args.nodes} nodes (this takes a while)")
    with Cluster(engine=args.engine) as cluster:
        for i in range(args.nodes):
            cluster.add(
                f"n{i:02d}",
                config_overrides={
                    # Small buffer pools: fifty default pools would be 200 MiB
                    # of page cache for a test that writes a few megabytes.
                    "buffer_pool_pages": 64,
                    "gossip_interval_ms": 700,
                    "discovery_interval_ms": 1000,
                    # The caps that exist precisely for this scale.
                    "max_peer_threads": 4,
                    "peer_rate_limit_per_sec": 500,
                    "peer_rate_burst": 1000,
                },
            )

        started = 0
        for index, node in enumerate(cluster.nodes):
            try:
                node.start(bootstrap_set(cluster.nodes, index))
                started += 1
            except Exception as error:
                print(f"      {node.name} did not start: {error}")
        report.check(started == args.nodes, f"all {args.nodes} nodes started ({started} did)")
        if started < 2:
            return report.finish()

        print("[2] letting the mesh discover itself")
        def most_nodes_have_peers() -> bool:
            with_peers = sum(
                1 for node in cluster.running_nodes()
                if node.client.status().get("known_peers", 0) >= 2
            )
            return with_peers >= int(0.9 * len(cluster.running_nodes()))

        cluster.wait_for(most_nodes_have_peers, 120.0, "nodes to find peers")
        peer_counts = [node.client.status()["known_peers"] for node in cluster.running_nodes()]
        print(f"      peers known per node: min={min(peer_counts)} "
              f"median={sorted(peer_counts)[len(peer_counts) // 2]} max={max(peer_counts)}")
        report.check(min(peer_counts) >= 1, "every node found at least one peer")

        print(f"[3] writing {args.writes} documents from every node, with {args.chaos} kills")
        chaos_targets = random.sample(range(1, args.nodes), min(args.chaos, args.nodes - 1))
        written: Dict[str, int] = {}
        write_failures = 0

        def write_one(index: int) -> str:
            node = cluster.nodes[index % len(cluster.nodes)]
            key = f"doc-{index:05d}"
            node.client.put(COLLECTION, key, {"index": index, "by": node.name})
            return key

        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
            futures = {}
            for i in range(args.writes):
                futures[pool.submit(write_one, i)] = i

                # Chaos, interleaved with the writes rather than after them:
                # the interesting case is a node dying with writes in flight.
                if chaos_targets and i > 0 and i % max(1, args.writes // (len(chaos_targets) + 1)) == 0:
                    victim = cluster.nodes[chaos_targets.pop()]
                    print(f"      killing {victim.name} mid-write")
                    victim.kill()

            for future, index in futures.items():
                try:
                    key = future.result(timeout=60)
                    written[key] = index
                except Exception:
                    # A write to a node that was killed a moment ago fails, and
                    # that is correct behaviour -- the client sees the node is
                    # gone. It is counted rather than ignored.
                    write_failures += 1

        print(f"      {len(written)} writes accepted, {write_failures} refused by dead nodes")
        report.check(len(written) > args.writes * 0.7,
                     f"most writes were accepted despite the chaos ({len(written)}/{args.writes})")

        print("[4] restarting the killed nodes")
        restarted = 0
        for index, node in enumerate(cluster.nodes):
            if node.running():
                continue
            try:
                node.start(bootstrap_set(cluster.nodes, index))
                node.client.claim_transit()
                restarted += 1
            except Exception as error:
                print(f"      {node.name} did not come back: {error}")
        print(f"      {restarted} nodes restarted")

        print(f"[5] settling (up to {args.settle:.0f}s)")
        start = time.time()
        try:
            cluster.wait_converged(args.settle)
            print(f"      converged in {time.time() - start:.1f}s")
            report.check(True, "every node converged on the same ledger tip")
        except TimeoutError:
            tips = {}
            for node in cluster.running_nodes():
                tip = node.client.ledger_tip()
                tips.setdefault((tip["entry_id"], tip["entry_hash"][:12]), []).append(node.name)
            print("      tips still differ:")
            for (entry_id, prefix), names in sorted(tips.items()):
                print(f"        #{entry_id} {prefix}… : {len(names)} nodes")
            report.check(False, f"convergence within {args.settle:.0f}s")

        print("[6] checking what everyone actually holds")
        checksums: Dict[str, List[str]] = {}
        for node in cluster.running_nodes():
            try:
                checksum = node.client.collection(COLLECTION).get("checksum", "")
            except DesentryError:
                checksum = "(unreadable)"
            checksums.setdefault(checksum, []).append(node.name)

        for checksum, names in checksums.items():
            print(f"      {checksum[:16]}… : {len(names)} nodes")
        report.check(len(checksums) == 1,
                     f"every node holds identical data ({len(checksums)} distinct checksums)")

        # Convergence is not enough on its own: a mesh where everybody agrees
        # on an empty collection would also converge.
        sample_node = cluster.running_nodes()[0]
        document_count = sample_node.client.collection(COLLECTION).get("document_count", 0)
        print(f"      {document_count} documents per node")
        report.check(document_count >= len(written) * 0.95,
                     f"the converged state contains the writes ({document_count} of {len(written)})")

        print("[7] every ledger still verifies")
        bad = []
        for node in cluster.running_nodes():
            if not node.client.verify_ledger().get("verified", False):
                bad.append(node.name)
        report.check(not bad, f"every hash chain verifies after the chaos (failed: {bad})")

        print("[8] admission control did its job")
        # The token bucket and the dedup cache exist so a fifty-node fan-out
        # cannot swamp a node. Non-zero counters here are the system working,
        # not a fault -- what would be worrying is a node that dropped
        # everything.
        totals = {"sent": 0, "dropped": 0, "duplicates_suppressed": 0, "rate_limited": 0}
        for node in cluster.running_nodes():
            stats = node.client.status().get("broadcast", {})
            for key in totals:
                totals[key] += stats.get(key, 0)
        print(f"      {totals}")
        report.check(totals["sent"] > 0, "broadcasts were sent")
        report.check(totals["dropped"] < totals["sent"],
                     "the worker pool dropped less than it sent")

    return report.finish()


if __name__ == "__main__":
    sys.exit(main())
