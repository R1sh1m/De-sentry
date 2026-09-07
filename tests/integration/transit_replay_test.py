"""Acceptance test: writes addressed to an offline node survive and are claimed.

This is the flow the transit store exists for, exactly as the spec describes it:

    A write is made for node C while C is dead.
    C's replicas store the bytes and broadcast a TRANSIT_INTENT.
    C comes back, replays the ledger, pulls the bytes, applies them,
    and emits TRANSIT_CLAIMED.
    A supervisor checkpoints and garbage-collects the settled pair --
    but only after a quorum agrees on the tip.

The node is killed, not stopped: `Popen.kill()` is SIGKILL on POSIX and
TerminateProcess on Windows, so C really does vanish mid-flight with no
opportunity to flush. A test that shut C down politely would prove much less.

    python3 tests/integration/transit_replay_test.py

Requires a built `desentryd` (see harness.find_engine for where it looks).
"""

from __future__ import annotations

import sys
import time

from harness import Cluster, Report
from desentry_client import DesentryError

COLLECTION = "orders"
KEY_WHILE_AWAY = "o-written-while-c-was-away"
KEY_BEFORE = "o-written-before"


def main() -> int:
    report = Report("transit_replay_test")

    with Cluster() as cluster:
        supervisor = cluster.add("supervisor", supervisor=True)
        a = cluster.add("node-a")
        b = cluster.add("node-b")
        c = cluster.add("node-c")
        cluster.start_all()

        print("[1] three data nodes and a supervisor are up")
        report.check(len(cluster.running_nodes()) == 4, "all four processes answering")
        report.check(supervisor.client.status()["supervisor"] is True,
                     "the supervisor identifies itself as one")
        report.check(a.client.status()["supervisor"] is False,
                     "a data node does not")

        # A baseline write, so C has something before it dies -- otherwise a
        # bug that simply lost C's whole data directory would pass.
        a.client.put(COLLECTION, KEY_BEFORE, {"state": "placed", "total": 10})
        cluster.wait_visible(COLLECTION, KEY_BEFORE)
        report.check(True, "a write before the outage reaches every node")

        print("[2] killing node-c without a chance to flush")
        c_node_id = c.node_id
        c.kill()
        report.check(not c.running(), "node-c is gone")

        # Give the survivors a moment to notice; the peer table marks a node
        # unreachable on failed probes, not instantly.
        time.sleep(2.0)

        print("[3] writing while node-c is away")
        a.client.put(COLLECTION, KEY_WHILE_AWAY, {"state": "placed", "total": 42})
        cluster.wait_visible(COLLECTION, KEY_WHILE_AWAY, nodes=[a, b])
        report.check(True, "the write lands on the nodes that are up")

        # A replica should now be holding bytes on C's behalf, and should have
        # said so on the ledger. Which replica depends on placement, so either
        # survivor counts.
        def someone_holds_for_c() -> bool:
            for node in (a, b):
                transit = node.client.transit()
                for owner in transit.get("holding_for", []):
                    if owner.get("owner_node") == c_node_id and owner.get("documents", 0) > 0:
                        return True
            return False

        cluster.wait_for(someone_holds_for_c, 30.0, "a replica to hold bytes for node-c")
        report.check(True, "a replica is holding the write for node-c")

        def intent_on_ledger() -> bool:
            for node in (a, b):
                entries = node.client.ledger_entries(0, -1)
                if any(entry["operation"] == "TRANSIT_INTENT" for entry in entries):
                    return True
            return False

        cluster.wait_for(intent_on_ledger, 30.0, "a TRANSIT_INTENT on the ledger")
        report.check(True, "the intent is recorded on the hash-chained ledger")

        print("[4] restarting node-c")
        c.start([a.address(), b.address(), supervisor.address()])
        report.check(c.running(), "node-c is back")
        report.check(c.node_id == c_node_id,
                     "node-c kept its identity across the kill (identity.key survived)")

        # Claiming runs at startup; asking again is idempotent and removes the
        # timing dependency from the test.
        c.client.claim_transit()

        def c_has_the_write() -> bool:
            try:
                c.client.get(COLLECTION, KEY_WHILE_AWAY)
                return True
            except DesentryError:
                return False

        cluster.wait_for(c_has_the_write, 45.0, "node-c to pull the held write")
        document = c.client.get(COLLECTION, KEY_WHILE_AWAY)
        report.equal(document.get("total"), 42, "node-c has the document it missed")

        # ...and the write it already had is still there. A replay that
        # replaced C's state rather than merging into it would lose this.
        report.equal(c.client.get(COLLECTION, KEY_BEFORE).get("total"), 10,
                     "node-c still has what it held before the outage")

        def claimed_on_ledger() -> bool:
            entries = c.client.ledger_entries(0, -1)
            return any(entry["operation"] == "TRANSIT_CLAIMED" for entry in entries)

        cluster.wait_for(claimed_on_ledger, 30.0, "a TRANSIT_CLAIMED entry")
        report.check(True, "node-c emitted TRANSIT_CLAIMED")

        print("[5] the mesh reconverges")
        cluster.wait_converged(90.0)
        report.check(True, "every node agrees on the ledger tip again")

        for node in cluster.running_nodes():
            report.check(node.client.verify_ledger().get("verified", False),
                         f"{node.name}: the hash chain verifies after the outage")

        print("[6] checkpoint and garbage collection")
        # The claim has settled, so the intent/claimed pair is now collectable.
        # The gate still has to agree: a checkpoint that proceeded without
        # quorum would be deleting history the mesh has not confirmed.
        result = supervisor.client.checkpoint()
        decision = result["decision"]
        print(f"      decision: proceeded={decision['proceeded']} "
              f"agreeing={decision['agreeing']}/{decision['required']} "
              f"conflicting={decision['conflicting']} "
              f"reason={decision.get('reason', '')}")

        report.check(decision["conflicting"] == 0,
                     "no replica reported a conflicting tip")
        if decision["proceeded"]:
            report.check(result["entries_pruned"] >= 0, "the prune reported what it removed")
            report.check(result["new_tip_hash"] != "", "a CHECKPOINT entry was written")
            # Pruning must not break the audit -- that is the whole point of
            # checkpointing rather than truncating.
            report.check(supervisor.client.verify_ledger().get("verified", False),
                         "the chain still verifies after pruning")
        else:
            # Refusing is a legitimate, safe outcome, and the reason has to be
            # legible rather than a silent no-op.
            report.check(bool(decision.get("reason")),
                         "the checkpoint was refused with a stated reason")

        # A data node must not be able to checkpoint on its own.
        try:
            a.client.checkpoint()
            report.check(False, "a data node was allowed to checkpoint")
        except DesentryError as error:
            report.equal(error.status, 403, "a data node is refused a checkpoint (403)")

        # The document survives the checkpoint. Garbage collection that
        # collected live data would be the worst possible bug here.
        report.equal(c.client.get(COLLECTION, KEY_WHILE_AWAY).get("total"), 42,
                     "the claimed document survives the checkpoint")

    return report.finish()


if __name__ == "__main__":
    sys.exit(main())
