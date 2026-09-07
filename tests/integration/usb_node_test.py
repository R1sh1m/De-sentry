"""Acceptance test: a node on removable media, unplugged and plugged back in.

A USB node is just a data directory that stops existing for a while and then
reappears -- possibly somewhere else. That is what this test simulates: the
node is stopped, its directory is moved (a different mount point, exactly as a
stick plugged into another port would be), and it is started again from the new
path.

Three properties have to hold, and each has a way of failing that would be
invisible without a test:

  * **Identity travels with the directory.** The node must come back as the
    same node, not as a new one. A node that re-generated its identity would
    silently orphan everything the mesh knows about it.
  * **Writes made while it was away are waiting.** Same transit mechanism as a
    crashed node -- but reached by a different route, so worth checking
    separately.
  * **Nothing was left behind at the old path.** A node whose data partly
    lived outside its data directory would work on the machine that made it
    and break everywhere else, which is the specific failure a portable node
    exists to avoid.

    python3 tests/integration/usb_node_test.py
"""

from __future__ import annotations

import json
import os
import shutil
import sys
import time

from harness import Cluster, Report
from desentry_client import DesentryError

COLLECTION = "field-data"


def main() -> int:
    report = Report("usb_node_test")

    with Cluster() as cluster:
        supervisor = cluster.add("supervisor", supervisor=True)
        host = cluster.add("workstation")
        # The "stick": an ordinary node whose directory happens to be somewhere
        # that will move.
        stick_first_path = os.path.join(cluster.root, "mount-a", "desentry-node")
        stick = cluster.add("portable", data_dir=stick_first_path)
        cluster.start_all()

        print("[1] a node on removable media joins the mesh")
        report.check(stick.running(), "the portable node started")
        stick_id = stick.node_id
        report.check(bool(stick_id), "it has an identity")

        stick.client.put(COLLECTION, "sample-1", {"reading": 21.5, "where": "site A"})
        cluster.wait_visible(COLLECTION, "sample-1")
        report.check(True, "a write made on the stick reaches the workstation")

        host.client.put(COLLECTION, "sample-2", {"reading": 19.0, "where": "lab"})
        cluster.wait_visible(COLLECTION, "sample-2", nodes=[stick])
        report.check(True, "a write made on the workstation reaches the stick")

        # A manifest is what makes the directory recognisable on another
        # machine. The app writes one at creation; here it is written directly,
        # since this test drives the engine rather than the app.
        manifest_path = os.path.join(stick.data_dir, "manifest.json")
        with open(manifest_path, "w", encoding="utf-8") as handle:
            json.dump({
                "version": 2,
                "node_id": stick_id,
                "node_name": "portable",
                "removable": True,
                "encrypted": False,
            }, handle, indent=2)
        report.check(os.path.exists(manifest_path), "the stick carries a manifest")

        print("[2] unplugging")
        stick.stop()
        report.check(not stick.running(), "the portable node is gone")
        time.sleep(2.0)

        print("[3] writing while it is unplugged")
        host.client.put(COLLECTION, "sample-3", {"reading": 30.1, "where": "roof"})
        report.check(True, "the mesh keeps working without it")

        def held_for_stick() -> bool:
            transit = host.client.transit()
            return any(owner.get("owner_node") == stick_id and owner.get("documents", 0) > 0
                       for owner in transit.get("holding_for", []))

        try:
            cluster.wait_for(held_for_stick, 30.0, "the workstation to hold a write for the stick")
            report.check(True, "the workstation is holding the write for the absent node")
        except TimeoutError:
            # With only two data nodes the placement ring may not select the
            # stick as a replica for this key at all, in which case there is
            # legitimately nothing to hold. Say which happened rather than
            # failing a property that was never engaged.
            placement = host.client.placement_for(COLLECTION, "sample-3")
            report.check(
                stick_id not in placement.get("replicas", []),
                "either a write was held for the stick, or the stick was not a replica for it "
                f"(replicas: {placement.get('replicas')})",
            )

        print("[4] plugging into a different mount point")
        second_path = os.path.join(cluster.root, "mount-b", "desentry-node")
        os.makedirs(os.path.dirname(second_path), exist_ok=True)
        shutil.move(stick.data_dir, second_path)
        report.check(not os.path.exists(stick.data_dir), "the old path is empty")

        stick.data_dir = second_path
        stick.log_path = os.path.join(second_path, "node.log")
        stick.start([host.address(), supervisor.address()])

        report.check(stick.running(), "the node starts from its new path")
        report.equal(stick.node_id, stick_id,
                     "it is the same node -- identity travelled with the directory")

        print("[5] it still has everything, and catches up")
        report.equal(stick.client.get(COLLECTION, "sample-1").get("reading"), 21.5,
                     "its own writes survived the move")
        report.equal(stick.client.get(COLLECTION, "sample-2").get("reading"), 19.0,
                     "replicated writes survived the move")

        stick.client.claim_transit()

        def has_missed_write() -> bool:
            try:
                stick.client.get(COLLECTION, "sample-3")
                return True
            except DesentryError:
                return False

        cluster.wait_for(has_missed_write, 45.0, "the stick to catch up on what it missed")
        report.equal(stick.client.get(COLLECTION, "sample-3").get("reading"), 30.1,
                     "the write made while it was unplugged arrived")

        report.check(stick.client.verify_ledger().get("verified", False),
                     "its hash chain verifies after the move")
        report.check(stick.client.verify().get("ok", False),
                     "its storage backends verify after the move")

        cluster.wait_converged(90.0)
        report.check(True, "the mesh reconverges with the node back")

        print("[6] a write from the stick still reaches the mesh")
        stick.client.put(COLLECTION, "sample-4", {"reading": 12.0, "where": "site B"})
        cluster.wait_visible(COLLECTION, "sample-4", nodes=[host])
        report.check(True, "the moved node is a full participant again")

    return report.finish()


if __name__ == "__main__":
    sys.exit(main())
