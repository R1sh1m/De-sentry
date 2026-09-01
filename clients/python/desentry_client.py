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
"""

from __future__ import annotations

import json
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Dict, Iterable, List, Optional, Tuple


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

    def list(self, collection: str, start_key: str = "", limit: int = 100) -> List[Tuple[str, Dict[str, Any]]]:
        params = {"start_key": start_key, "limit": str(limit)}
        resp = self._request("GET", f"/db/{_q(collection)}", query=params)
        return [(item["key"], item["document"]) for item in resp.get("documents", [])]

    # -- schema ---------------------------------------------------------------
    def set_schema(self, collection: str, schema: Dict[str, Any]) -> Dict[str, Any]:
        return self._request("PUT", f"/_schema/{_q(collection)}", body=schema)

    def get_schema(self, collection: str) -> Optional[Dict[str, Any]]:
        return self._request("GET", f"/_schema/{_q(collection)}")

    # -- cluster / node introspection ------------------------------------------
    def collections(self) -> List[str]:
        return self._request("GET", "/_collections")

    def peers(self) -> List[Dict[str, Any]]:
        return self._request("GET", "/_peers")

    def status(self) -> Dict[str, Any]:
        return self._request("GET", "/_status")

    # -- brain file: compact whole-node snapshot (collections, ledger tip,
    #    peers, uptime) -- the fast, single-call way for an agent to get
    #    situational awareness of one node without walking every endpoint.
    def brain(self) -> Dict[str, Any]:
        return self._request("GET", "/_brain")

    # -- hash-chained audit ledger --------------------------------------------
    def ledger_tip(self) -> Dict[str, Any]:
        return self._request("GET", "/_ledger/tip")

    def ledger_entries(self, from_id: int = 0, to_id: int = -1) -> List[Dict[str, Any]]:
        resp = self._request("GET", "/_ledger/entries", query={"from": str(from_id), "to": str(to_id)})
        return resp.get("entries", [])

    def verify_ledger(self) -> Dict[str, Any]:
        return self._request("POST", "/_ledger/verify")

    # -- internals --------------------------------------------------------------
    def _request(self, method: str, path: str, body: Optional[Dict[str, Any]] = None,
                 query: Optional[Dict[str, str]] = None) -> Any:
        url = self.base_url + path
        if query:
            url += "?" + urllib.parse.urlencode(query)
        data = json.dumps(body).encode("utf-8") if body is not None else None
        req = urllib.request.Request(url, data=data, method=method,
                                      headers={"Content-Type": "application/json"} if data else {})
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
