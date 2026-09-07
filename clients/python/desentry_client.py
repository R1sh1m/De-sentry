"""De-Sentry Python client -- a thin wrapper around one peer's local REST
API, for AI agents and scripts that want to read/write a node without
speaking raw HTTP.

Deliberately NOT a native/pybind11 binding into the C++ engine. Every node
already exposes a full REST API on loopback (ARCHITECTURE.md Sec 8;
README.md's endpoint table) specifically so applications -- including
Python-based AI agents -- never need to link against the engine directly.
A pure-stdlib HTTP client:
  * has zero build step (no CMake/pybind11/compiler needed on the agent's
    machine, which may not even be the same machine/OS as the node),
  * works identically against a node running in-process, on localhost, or
    across the network (the native-binding approach only ever works
    same-process),
  * and can't crash the calling Python process on a native/ABI mismatch.
Only Python's standard library is used (urllib, json) -- no extra
dependency to install, matching the rest of this project's
zero-fetched-dependencies philosophy.

Example:
    from desentry_client import DesentryClient

    node = DesentryClient("http://127.0.0.1:7701")
    node.put("users", "u1", {"name": "Asha", "role": "admin"})
    doc = node.get("users", "u1")
    for key, doc in node.list("users"):
        ...
    node.delete("users", "u1")

    tip = node.ledger_tip()               # {"entry_id", "entry_hash", "signature", ...}
    result = node.verify_ledger()         # {"verified": true, "entries_checked": N}
    brain = node.brain()                  # compact whole-node snapshot
    peers = node.peers()

v2 adds the storage router, per-collection ACLs, quotas, the transit store
and the long-poll change feed:

    node.bind_engine("readings", "ts_rollup")
    node.set_acl("journal", private=True, readers=[friend_node_id])
    node.set_placement("readings", shard_key="series", replication_factor=3)

    for change in node.follow():          # a follower, with no polling timer
        handle(change)
"""

from __future__ import annotations

import json
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Dict, Iterable, Iterator, List, Optional, Tuple


class DesentryError(Exception):
    """Raised for any non-2xx response from a node's REST API."""

    def __init__(self, status: int, message: str):
        super().__init__(f"HTTP {status}: {message}")
        self.status = status
        self.message = message


class DesentryClient:
    def __init__(self, base_url: str, timeout_s: float = 10.0):
        self.base_url = base_url.rstrip("/")
        self.timeout_s = timeout_s

    # -- document CRUD -------------------------------------------------------
    def put(self, collection: str, key: str, document: Dict[str, Any]) -> Dict[str, Any]:
        return self._request("PUT", f"/db/{_q(collection)}/{_q(key)}", body=document)

    def get(self, collection: str, key: str) -> Dict[str, Any]:
        return self._request("GET", f"/db/{_q(collection)}/{_q(key)}")

    def delete(self, collection: str, key: str) -> Dict[str, Any]:
        return self._request("DELETE", f"/db/{_q(collection)}/{_q(key)}")

    def list(self, collection: str, start_key: str = "", limit: int = 100
             ) -> List[Tuple[str, Dict[str, Any]]]:
        # `start` is the v2 name; the node still accepts `start_key`, and both
        # are sent so this client works unchanged against a v1 node.
        params = {"start": start_key, "start_key": start_key, "limit": str(limit)}
        resp = self._request("GET", f"/db/{_q(collection)}", query=params)
        return [(item["key"], item["document"]) for item in resp.get("documents", [])]

    def scan(self, collection: str, page_size: int = 200
             ) -> Iterator[Tuple[str, Dict[str, Any]]]:
        """Every document in a collection, paged.

        The node's scan is a lower bound (inclusive), so each page asks from
        the last key of the previous one and drops the repeat. Nudging the key
        instead -- appending a byte, incrementing the last character -- would
        silently skip anything sorting between the two.
        """
        start = ""
        first = True
        while True:
            page = self.list(collection, start_key=start, limit=page_size)
            if not first and page and page[0][0] == start:
                page = page[1:]
            if not page:
                return
            for key, document in page:
                yield key, document
            first = False
            start = page[-1][0]

    # -- schema ---------------------------------------------------------------
    def set_schema(self, collection: str, schema: Dict[str, Any]) -> Dict[str, Any]:
        return self._request("PUT", f"/_schema/{_q(collection)}", body=schema)

    def get_schema(self, collection: str) -> Optional[Dict[str, Any]]:
        return self._request("GET", f"/_schema/{_q(collection)}")

    # -- collection metadata: engine, ACL, placement (v2) ----------------------
    def collection(self, collection: str) -> Dict[str, Any]:
        """Everything the node knows about one collection: engine, checksum,
        document count, effective ACL, shard key, retention."""
        return self._request("GET", f"/_collection/{_q(collection)}")

    def bind_engine(self, collection: str, engine: str) -> Dict[str, Any]:
        """Binds a collection to a storage engine.

        Refused once the collection has rows: the data is already laid out for
        the engine it has, and a silent rebind would strand it.
        """
        return self._request("PUT", f"/_collection/{_q(collection)}/engine",
                             body={"engine": engine})

    def set_acl(self, collection: str, *, owner_node: Optional[str] = None,
                private: Optional[bool] = None, readers: Optional[List[str]] = None,
                parent: Optional[str] = None) -> Dict[str, Any]:
        """Sets a collection's access control.

        A collection with no ACL is shared with the mesh. Making it private
        restricts reads to the owner plus `readers`, and writes to the owner
        alone -- enforced on the API path *and* in the gossip byte filter, so
        a peer that is not a reader receives hashes rather than bytes.
        """
        body: Dict[str, Any] = {}
        if owner_node is not None:
            body["owner_node"] = owner_node
        if private is not None:
            body["private"] = private
        if readers is not None:
            body["readers"] = readers
        if parent is not None:
            body["parent"] = parent
        return self._request("PUT", f"/_collection/{_q(collection)}/acl", body=body)

    def set_placement(self, collection: str, *, shard_key: Optional[str] = None,
                      replication_factor: Optional[int] = None,
                      retention_days: Optional[int] = None) -> Dict[str, Any]:
        body: Dict[str, Any] = {}
        if shard_key is not None:
            body["shard_key"] = shard_key
        if replication_factor is not None:
            body["replication_factor"] = replication_factor
        if retention_days is not None:
            body["retention_days"] = retention_days
        return self._request("PUT", f"/_collection/{_q(collection)}/placement", body=body)

    def placement_for(self, collection: str, key: str) -> Dict[str, Any]:
        """Which nodes hold this key, and which candidates were skipped."""
        return self._request("GET", f"/_placement/{_q(collection)}/{_q(key)}")

    def engines(self) -> List[Dict[str, Any]]:
        """Engines this build has, which are active on this node, and the CMake
        option that would add the rest. The binary is the authority here, not a
        table in the documentation."""
        return self._request("GET", "/_engines")

    # -- cluster / node introspection ------------------------------------------
    def collections(self) -> List[str]:
        return self._request("GET", "/_collections")

    def peers(self) -> List[Dict[str, Any]]:
        return self._request("GET", "/_peers")

    def status(self) -> Dict[str, Any]:
        return self._request("GET", "/_status")

    def config(self) -> Dict[str, Any]:
        """The config as the node actually parsed it, not as the file was
        written -- which is what "why isn't my setting taking effect?" needs."""
        return self._request("GET", "/_config")

    def quota(self) -> Dict[str, Any]:
        """Node budget, current usage, and the per-engine breakdown."""
        return self._request("GET", "/_quota")

    # -- brain file: compact whole-node snapshot (collections, ledger tip,
    #    peers, uptime) -- the fast, single-call way for an agent to get
    #    situational awareness of one node without walking every endpoint.
    def brain(self) -> Dict[str, Any]:
        return self._request("GET", "/_brain")

    # -- hash-chained audit ledger --------------------------------------------
    def ledger_tip(self) -> Dict[str, Any]:
        return self._request("GET", "/_ledger/tip")

    def ledger_entries(self, from_id: int = 0, to_id: int = -1) -> List[Dict[str, Any]]:
        resp = self._request("GET", "/_ledger/entries",
                             query={"from": str(from_id), "to": str(to_id)})
        return resp.get("entries", [])

    def verify_ledger(self) -> Dict[str, Any]:
        return self._request("POST", "/_ledger/verify", timeout_s=60.0)

    def verify(self) -> Dict[str, Any]:
        """Ledger chain *and* every storage backend's own structural check.

        The ledger proves history was not rewritten; the backends prove the
        materialised state is actually readable. Reporting only the first
        would be misleading, so this reports both.
        """
        return self._request("POST", "/_verify", timeout_s=120.0)

    # -- live change feed (v2) --------------------------------------------------
    def changes(self, since: int = -1, timeout_ms: int = 25000, limit: int = 256
                ) -> Dict[str, Any]:
        """Long-polls for ledger changes after `since`.

        Blocks up to `timeout_ms` and returns as soon as anything lands, so a
        follower needs no polling timer. `truncated` in the response means the
        cursor predates a checkpoint prune: drop it and re-read from `tip`
        rather than assuming continuity across a gap that is really there.
        """
        return self._request(
            "GET",
            "/_changes",
            query={"since": str(since), "timeout_ms": str(timeout_ms), "limit": str(limit)},
            # The read timeout must outlast the server's poll window, or every
            # quiet interval looks like a disconnection.
            timeout_s=(timeout_ms / 1000.0) + 10.0,
        )

    def follow(self, since: int = -1, timeout_ms: int = 25000) -> Iterator[Dict[str, Any]]:
        """Yields ledger entries as they land, indefinitely, handling gaps."""
        cursor = since
        while True:
            batch = self.changes(cursor, timeout_ms=timeout_ms)
            cursor = batch.get("tip", cursor)
            if batch.get("truncated"):
                continue
            for change in batch.get("changes", []):
                yield change

    # -- transit: writes held for offline owners (v2) ---------------------------
    def transit(self) -> Dict[str, Any]:
        """What this node is holding on behalf of nodes that were offline."""
        return self._request("GET", "/_transit")

    def claim_transit(self) -> Dict[str, Any]:
        """Asks peers for writes they are holding for this node.

        Runs automatically at startup; exposed so a test -- or an operator who
        has just plugged a drive back in -- does not have to wait for it.
        """
        return self._request("POST", "/_transit/claim", timeout_s=60.0)

    def expire_transit(self) -> Dict[str, Any]:
        return self._request("POST", "/_transit/expire", timeout_s=30.0)

    def checkpoint(self) -> Dict[str, Any]:
        """Runs the quorum-gated checkpoint.

        Supervisor nodes only; a data node answers 403, which is the point --
        no single node gets to decide on its own to delete history.
        """
        return self._request("POST", "/_checkpoint", timeout_s=120.0)

    # -- engine-specific query surfaces (v2) ------------------------------------
    def vector_search(self, collection: str, vector: List[float], k: int = 10
                      ) -> Dict[str, Any]:
        return self._request("POST", f"/_search/vector/{_q(collection)}",
                             body={"vector": vector, "k": k})

    def ts_rollups(self, collection: str, series: str = "", from_ms: int = 0,
                   to_ms: Optional[int] = None, bucket_ms: int = 60000) -> Dict[str, Any]:
        query = {"series": series, "from_ms": str(from_ms), "bucket_ms": str(bucket_ms)}
        if to_ms is not None:
            query["to_ms"] = str(to_ms)
        return self._request("GET", f"/_ts/{_q(collection)}/rollups", query=query)

    def graph(self, collection: str, key: str, depth: int = 1) -> Dict[str, Any]:
        return self._request("GET", f"/_graph/{_q(collection)}/{_q(key)}",
                             query={"depth": str(depth)})

    # -- internals --------------------------------------------------------------
    def _request(self, method: str, path: str, body: Optional[Dict[str, Any]] = None,
                 query: Optional[Dict[str, str]] = None,
                 timeout_s: Optional[float] = None) -> Any:
        url = self.base_url + path
        if query:
            url += "?" + urllib.parse.urlencode(query)
        data = json.dumps(body).encode("utf-8") if body is not None else None
        req = urllib.request.Request(url, data=data, method=method,
                                      headers={"Content-Type": "application/json"} if data else {})
        try:
            with urllib.request.urlopen(req, timeout=timeout_s or self.timeout_s) as resp:
                raw = resp.read()
                return json.loads(raw) if raw else None
        except urllib.error.HTTPError as e:
            raw = e.read()
            message = raw.decode("utf-8", errors="replace")
            try:
                message = json.loads(message).get("error", message)
            except Exception:
                pass
            raise DesentryError(e.code, message) from None


def _q(path_segment: str) -> str:
    return urllib.parse.quote(path_segment, safe="")


class Consortium:
    """Thin fan-out convenience over several DesentryClient instances -- for
    an agent orchestrating or auditing more than one node at once (e.g. "do
    all N nodes' ledger tips currently agree after settling?"). This never
    talks node-to-node itself; it just calls each node's own REST API, the
    same as a human operator with N terminals would.
    """

    def __init__(self, base_urls: Iterable[str]):
        self.nodes = {url: DesentryClient(url) for url in base_urls}

    def all_status(self) -> Dict[str, Dict[str, Any]]:
        return {url: c.status() for url, c in self.nodes.items()}

    def all_verified(self) -> Dict[str, bool]:
        return {url: c.verify_ledger().get("verified", False) for url, c in self.nodes.items()}

    def all_tips(self) -> Dict[str, Dict[str, Any]]:
        return {url: c.ledger_tip() for url, c in self.nodes.items()}

    def converged(self) -> bool:
        """True when every node reports the same ledger tip -- id *and* hash.

        Comparing ids alone would call two nodes converged while they held
        different histories of the same length, which is the one disagreement
        actually worth catching.
        """
        tips = [(t.get("entry_id"), t.get("entry_hash")) for t in self.all_tips().values()]
        return len(set(tips)) == 1 if tips else False

    def checksums(self, collection: str) -> Dict[str, str]:
        """Each node's checksum for one collection.

        Two converged nodes must agree regardless of which engine each has the
        collection bound to -- that is the router's central promise, and this
        is how to check it from outside.
        """
        out: Dict[str, str] = {}
        for url, client in self.nodes.items():
            try:
                out[url] = client.collection(collection).get("checksum", "")
            except DesentryError:
                out[url] = ""
        return out
