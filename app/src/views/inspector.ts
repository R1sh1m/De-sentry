/**
 * The right-hand inspector: everything about the selected node.
 *
 * Ordered by how often it is needed, not by how the API happens to group it:
 * identity and health first, then the signed ledger tip with its Verify
 * action, then collections, then peers, then the things that are usually fine
 * (quota, transit, process). The panel scrolls; the first screenful is the
 * part someone checks every time.
 *
 * Verify is a button rather than something that runs on refresh. Verifying
 * re-derives the whole hash chain and checks every signature -- it is not free,
 * and a number that quietly recomputes itself is a number nobody trusts. When
 * it has run, the panel says when.
 */

import { apiFor, ApiError, type CollectionDetail, type Peer } from "../api.js";
import { sidecar } from "../bridge.js";
import { convergenceOf, meshTip, refreshNode, refreshTopology, store, type NodeView } from "../state.js";
import {
  ago,
  bytes,
  count,
  duration,
  engineLabel,
  millis,
  percent,
  shortHash,
  shortNode,
  timestamp,
} from "../util/format.js";
import { el, icon, Icons, on, replace } from "../util/dom.js";
import { statusText } from "./canvas.js";

function describeError(error: unknown): string {
  if (error instanceof ApiError) return error.message;
  if (error instanceof Error) return error.message;
  return String(error);
}

function card(title: string, ...children: (Node | string | false | null | undefined)[]): HTMLElement {
  return el("section", { class: "card" }, el("h3", { class: "card__title", text: title }), ...children);
}

function kv(...pairs: [string, Node | string][]): HTMLElement {
  const list = el("dl", { class: "kv" });
  for (const [term, value] of pairs) {
    list.appendChild(el("dt", { text: term }));
    list.appendChild(typeof value === "string" ? el("dd", { text: value }) : el("dd", {}, value));
  }
  return list;
}

function mono(text: string, title?: string): HTMLElement {
  return el("span", { class: "mono", text, title: title ?? text });
}

/** A copy-to-clipboard affordance for hashes and ids, which are unretypeable. */
function copyable(text: string, label: string): HTMLElement {
  const button = el(
    "button",
    { class: "btn btn--sm btn--ghost", type: "button", title: `Copy ${label}`, "aria-label": `Copy ${label}` },
    icon(Icons.copy, 13),
  );
  on(button, "click", async () => {
    try {
      await navigator.clipboard.writeText(text);
      store.toast("success", `${label} copied`, "", 2000);
    } catch (error) {
      store.toast("error", `Could not copy the ${label.toLowerCase()}`, describeError(error));
    }
  });
  return el("span", { class: "row" }, mono(shortHash(text, 10, 8), text), button);
}

// -- sections ----------------------------------------------------------------

function identityCard(node: NodeView): HTMLElement {
  const status = convergenceOf(node, meshTip());
  const s = node.status;

  return card(
    "Node",
    el(
      "div",
      { class: "row row--between" },
      el(
        "div",
        { class: "row" },
        el("span", { class: "dot", "data-status": status }),
        el("strong", { text: node.process.node_name || shortNode(node.process.node_id) }),
      ),
      el("span", { class: "badge", "data-tone": status, text: statusText(status) }),
    ),
    kv(
      ["Node ID", copyable(node.process.node_id, "Node ID")],
      ["Uptime", s ? duration(s.uptime_seconds) : "—"],
      ["API port", mono(String(node.process.api_port))],
      ["P2P port", mono(String(node.process.p2p_port))],
      ["Data directory", mono(node.process.data_dir)],
      ["Default engine", s ? engineLabel(s.default_engine) : "—"],
      ["Replication factor", s ? String(s.replication_factor) : "—"],
      ["At rest", node.process.encrypted ? "Encrypted" : "Not encrypted"],
    ),
    !node.reachable &&
      el(
        "p",
        { class: "error-note" },
        `This node is not answering on 127.0.0.1:${node.process.api_port}. ${node.unreachableReason}`,
      ),
  );
}

function ledgerCard(node: NodeView, onBusy: (label: string) => void): HTMLElement {
  const tip = node.tip;
  const mesh = meshTip();
  const behind = tip !== null && mesh !== null ? mesh.entry_id - tip.entry_id : 0;
  const status = convergenceOf(node, mesh);

  const verifyButton = el("button", { class: "btn btn--sm", type: "button", text: "Verify" });
  on(verifyButton, "click", async () => {
    onBusy("Verifying the ledger…");
    try {
      const result = await apiFor(node.process.api_port).verifyLedger();
      node.lastVerify = result;
      node.lastVerifyMs = Date.now();
      if (result.verified) {
        store.toast(
          "success",
          "Ledger verified",
          `${count(result.entries_checked)} entries, ${count(result.signed_entries)} signed, chain intact.`,
        );
      } else {
        store.toast(
          "error",
          "Ledger verification failed",
          `${result.reason ?? "chain broken"} at entry ${result.failed_at_entry_id ?? "?"}.`,
          0,
        );
      }
    } catch (error) {
      store.toast("error", "Could not verify the ledger", describeError(error));
    } finally {
      onBusy("");
      store.notify();
    }
  });

  const verdict = node.lastVerify;
  return card(
    "Ledger",
    kv(
      ["Tip entry", tip ? mono(`#${tip.entry_id}`) : "—"],
      ["Tip hash", tip ? copyable(tip.entry_hash, "Tip hash") : "—"],
      ["Signature", tip ? mono(shortHash(tip.signature, 10, 6), tip.signature) : "—"],
      [
        "Against the mesh",
        status === "diverged"
          ? "Same height, different hash — histories do not match"
          : behind > 0
            ? `${count(behind)} entries behind the furthest-ahead node`
            : "At the furthest-ahead tip",
      ],
      ...(node.resyncs > 0
        ? ([["Re-syncs", `${node.resyncs} — its cursor predated a checkpoint`]] as [string, string][])
        : []),
    ),
    el(
      "div",
      { class: "row row--between" },
      verdict === null
        ? el("span", { class: "muted", text: "Not verified this session" })
        : el("span", {
            class: verdict.verified ? "muted" : "error-note",
            text: verdict.verified
              ? `Verified ${ago(node.lastVerifyMs)} · ${count(verdict.entries_checked)} entries, ${count(verdict.unsigned_entries)} unsigned`
              : `Failed ${ago(node.lastVerifyMs)} · ${verdict.reason ?? "chain broken"}`,
          }),
      verifyButton,
    ),
  );
}

function collectionsCard(node: NodeView): HTMLElement {
  const collections = node.brain?.collections ?? [];
  if (collections.length === 0) {
    return card(
      "Collections",
      el("p", { class: "muted", text: "No collections yet. Writing a document creates one." }),
    );
  }

  const table = el(
    "table",
    { class: "table" },
    el(
      "thead",
      {},
      el(
        "tr",
        {},
        el("th", { text: "Name" }),
        el("th", { text: "Engine" }),
        el("th", { text: "Docs" }),
        el("th", { text: "Checksum" }),
      ),
    ),
  );
  const body = el("tbody", {});
  for (const collection of collections) {
    const row = el(
      "tr",
      { tabindex: "0", role: "button", title: `Open ${collection.name}` },
      el(
        "td",
        {},
        collection.name,
        collection.private && el("span", { class: "badge", text: "private", title: "Readable only by its ACL" }),
      ),
      el("td", { text: engineLabel(collection.engine) }),
      el("td", { text: count(collection.document_count) }),
      // The checksum is what a parity check across replicas compares, so it is
      // shown in full on hover rather than only as a truncation.
      el("td", { class: "mono", text: shortHash(collection.checksum, 8, 4), title: collection.checksum }),
    );
    const open = () =>
      store.select({ kind: "collection", nodeId: node.process.node_id, collection: collection.name });
    on(row, "click", open);
    on(row, "keydown", (event) => {
      if (event.key === "Enter") open();
    });
    body.appendChild(row);
  }
  table.appendChild(body);
  return card("Collections", table);
}

function collectionDetailCard(detail: CollectionDetail): HTMLElement {
  const acl = detail.acl;
  return card(
    `Collection · ${detail.name}`,
    kv(
      ["Engine", engineLabel(detail.engine)],
      ["Documents", count(detail.document_count)],
      ["Checksum", copyable(detail.checksum, "Checksum")],
      ["Created", timestamp(detail.created_at_ms)],
      ["Shard key", detail.shard_key || "— (keys hash directly)"],
      ["Replication factor", detail.replication_factor > 0 ? String(detail.replication_factor) : "node default"],
      ["Retention", detail.retention_days > 0 ? `${detail.retention_days} days` : "keep everything"],
      ["Schema", detail.has_schema ? "Enforced on write" : "None"],
      ["Visibility", acl.private ? "Private" : "Shared with the mesh"],
      ["Owner", acl.owner_node ? mono(shortNode(acl.owner_node, 12), acl.owner_node) : "—"],
      [
        "Readers",
        acl.private
          ? acl.readers.length === 0
            ? "owner only"
            : `${acl.readers.length} node${acl.readers.length === 1 ? "" : "s"}`
          : "anyone on the mesh",
      ],
      ...(acl.parent ? ([["Inherits from", acl.parent]] as [string, string][]) : []),
      ...(detail.secondary_indexes.length > 0
        ? ([["Indexes", detail.secondary_indexes.join(", ")]] as [string, string][])
        : []),
    ),
  );
}

function peersCard(node: NodeView): HTMLElement {
  const peers = node.peers;
  if (peers.length === 0) {
    return card(
      "Peers",
      el("p", {
        class: "muted",
        text: "No peers found. On a fresh LAN this is normal until another node starts; across subnets it means broadcast discovery cannot reach and a bootstrap peer is needed.",
      }),
    );
  }

  const rank = (peer: Peer): string =>
    peer.fitness.probes === 0 ? "not probed" : percent(peer.fitness.success_rate);

  const table = el(
    "table",
    { class: "table table--numeric" },
    el(
      "thead",
      {},
      el(
        "tr",
        {},
        el("th", { text: "Peer" }),
        el("th", { text: "Latency" }),
        el("th", { text: "Success" }),
        el("th", { text: "Ledger" }),
        el("th", { text: "Seen" }),
      ),
    ),
  );
  const body = el("tbody", {});
  for (const peer of peers) {
    body.appendChild(
      el(
        "tr",
        { title: `${peer.host}:${peer.p2p_port} · fitness ${peer.fitness.score.toFixed(3)}` },
        el(
          "td",
          {},
          el("span", { class: "mono", text: peer.hostname || shortNode(peer.node_id, 10) }),
          peer.supervisor && el("span", { class: "badge", "data-tone": "supervisor", text: "sup" }),
        ),
        el("td", { text: millis(peer.fitness.latency_ms) }),
        el("td", { text: rank(peer) }),
        el("td", { text: `#${peer.fitness.ledger_freshness_entry_id}` }),
        el("td", { text: ago(peer.last_seen_ms) }),
      ),
    );
  }
  table.appendChild(body);
  return card("Peers", table);
}

function quotaCard(node: NodeView): HTMLElement {
  const quota = node.quota ?? node.status?.quota;
  if (quota === undefined || quota === null) return card("Storage", el("p", { class: "muted", text: "—" }));

  const unlimited = quota.limit_bytes === 0;
  const level = quota.over_limit ? "full" : quota.used_fraction > 0.85 ? "warn" : "ok";

  const bar = el(
    "div",
    { class: "quota-bar", "data-level": level, role: "img", "aria-label": `${percent(quota.used_fraction)} of quota used` },
    el("div", {
      class: "quota-bar__fill",
      style: `width: ${Math.min(100, Math.round(quota.used_fraction * 100))}%`,
    }),
  );

  const perEngine = node.quota?.engines ?? [];
  return card(
    "Storage",
    unlimited
      ? el("p", { class: "muted", text: "No quota set — this node will use whatever the disk allows." })
      : bar,
    kv(
      ["Used", bytes(quota.used_bytes)],
      ["Budget", unlimited ? "unlimited" : bytes(quota.limit_bytes)],
      ["Ledger", bytes(quota.ledger_bytes)],
      ...(quota.over_limit ? ([["State", "Over budget — writes are being refused"]] as [string, string][]) : []),
    ),
    perEngine.length > 0 &&
      el(
        "div",
        { class: "row" },
        ...perEngine.map((engine) =>
          el("span", {
            class: "chip",
            title: `${bytes(engine.used_bytes)} of ${engine.limit_bytes === 0 ? "unlimited" : bytes(engine.limit_bytes)}`,
            text: `${engineLabel(engine.engine)} · ${bytes(engine.used_bytes)}`,
          }),
        ),
      ),
  );
}

function transitCard(node: NodeView, onBusy: (label: string) => void): HTMLElement {
  const held = node.brain?.transit_documents_held ?? 0;

  const claimButton = el("button", { class: "btn btn--sm", type: "button", text: "Claim now" });
  on(claimButton, "click", async () => {
    onBusy("Asking peers for held writes…");
    try {
      const report = await apiFor(node.process.api_port).claimTransit();
      store.toast(
        report.documents_claimed > 0 ? "success" : "info",
        report.documents_claimed > 0
          ? `Claimed ${count(report.documents_claimed)} document${report.documents_claimed === 1 ? "" : "s"}`
          : "Nothing was waiting",
        `${report.peers_asked} peers asked${report.failures > 0 ? `, ${report.failures} did not answer` : ""}.`,
      );
      await refreshNode(node.process.node_id);
    } catch (error) {
      store.toast("error", "Could not claim held writes", describeError(error));
    } finally {
      onBusy("");
    }
  });

  return card(
    "Transit",
    el("p", {
      class: "muted",
      text: "Writes addressed to a node that was offline are held by its replicas until it returns. This node holds writes for others, and claims its own when it starts.",
    }),
    kv([
      "Holding for others",
      held === 0 ? "nothing" : `${count(held)} document${held === 1 ? "" : "s"}`,
    ]),
    el("div", { class: "row row--between" }, el("span", { class: "muted", text: "" }), claimButton),
  );
}

function processCard(node: NodeView, onBusy: (label: string) => void): HTMLElement {
  const restart = el("button", { class: "btn btn--sm", type: "button", text: "Restart" });
  on(restart, "click", async () => {
    onBusy("Restarting…");
    try {
      await sidecar.restartNode(node.process.node_id);
      store.toast("info", "Restarting", node.process.node_name || node.process.node_id);
    } catch (error) {
      store.toast("error", "Could not restart the node", describeError(error));
    } finally {
      onBusy("");
    }
  });

  const stop = el("button", { class: "btn btn--sm btn--ghost", type: "button", text: "Stop" });
  on(stop, "click", async () => {
    try {
      await sidecar.stopNode(node.process.node_id);
      store.toast("info", "Stopped", "Its replicas will hold writes addressed to it until it returns.");
    } catch (error) {
      store.toast("error", "Could not stop the node", describeError(error));
    }
  });

  const reveal = el("button", { class: "btn btn--sm btn--ghost", type: "button", text: "Show files" });
  on(reveal, "click", () => {
    void sidecar.revealPath(node.process.data_dir).catch((error) => {
      store.toast("error", "Could not open the folder", describeError(error));
    });
  });

  return card(
    "Process",
    kv(
      ["State", node.process.process],
      ["PID", node.process.pid === null ? "—" : String(node.process.pid)],
      ["Started", node.process.started_ms ? timestamp(node.process.started_ms) : "—"],
      [
        "Restarts",
        node.process.restarts === 0
          ? "none"
          : `${node.process.restarts} — a climbing count means it is crashing`,
      ],
      ...(node.process.last_error ? ([["Last error", node.process.last_error]] as [string, string][]) : []),
    ),
    el("div", { class: "row" }, restart, stop, reveal),
  );
}

function supervisorCard(port: number, onBusy: (label: string) => void): HTMLElement {
  const checkpoint = el("button", { class: "btn btn--sm", type: "button", text: "Checkpoint now" });
  on(checkpoint, "click", async () => {
    onBusy("Collecting replica tips…");
    try {
      const result = await apiFor(port).checkpoint();
      if (result.decision.proceeded) {
        store.toast(
          "success",
          "Checkpoint written",
          `${count(result.entries_pruned)} entries pruned, ${count(result.envelopes_released)} held writes released. ${result.decision.agreeing} of ${result.decision.required} replicas agreed.`,
        );
      } else {
        // Not proceeding is the safe outcome, not a failure -- say which.
        store.toast(
          "warning",
          "Checkpoint held back",
          result.decision.reason ??
            `Only ${result.decision.agreeing} of ${result.decision.required} replicas agreed on the tip.`,
        );
      }
      await refreshTopology();
    } catch (error) {
      store.toast("error", "Could not checkpoint", describeError(error));
    } finally {
      onBusy("");
    }
  });

  const pass = el("button", { class: "btn btn--sm btn--ghost", type: "button", text: "Run housekeeping" });
  on(pass, "click", async () => {
    try {
      const report = await apiFor(port).supervisorPass();
      store.toast(
        "info",
        "Housekeeping pass complete",
        `${report.peers_marked_degraded} peers marked degraded, ${report.transit_expired} held writes expired.`,
      );
      await refreshTopology();
    } catch (error) {
      store.toast("error", "Housekeeping failed", describeError(error));
    }
  });

  return card(
    "Supervisor",
    el("p", {
      class: "muted",
      text: "The supervisor runs discovery, quota checks and the quorum-gated checkpoint. It holds no data and is never on the write path — if it stops, replication carries on without it.",
    }),
    el("div", { class: "row" }, checkpoint, pass),
  );
}

// -- panel -------------------------------------------------------------------

export interface InspectorHandles {
  render(): void;
  element: HTMLElement;
}

export function createInspector(): InspectorHandles {
  const body = el("div", { class: "stack" });
  const element = el("aside", { class: "inspector", "data-open": "false" }, body);

  const setBusy = (label: string) => store.setBusy(label);

  function render(): void {
    const selection = store.state.selection;
    const node = store.selectedNode();

    if (node === undefined) {
      replace(
        body,
        el(
          "div",
          { class: "empty" },
          el("p", { class: "empty__title", text: "Nothing selected" }),
          el("p", {
            class: "empty__body",
            text: "Pick a node in the sidebar or on the canvas to see its identity, ledger tip, collections and peers.",
          }),
        ),
      );
      return;
    }

    const detail =
      selection.kind === "collection" && selection.collection
        ? store.state.collectionDetails.get(`${node.process.node_id}::${selection.collection}`)
        : undefined;

    const refresh = el(
      "button",
      { class: "btn btn--sm btn--ghost", type: "button", title: "Refresh this node" },
      icon(Icons.refresh, 14),
    );
    on(refresh, "click", () => void refreshNode(node.process.node_id));

    replace(
      body,
      el(
        "div",
        { class: "row row--between" },
        el("span", { class: "tree__group-label", text: "Inspector" }),
        refresh,
      ),
      identityCard(node),
      ledgerCard(node, setBusy),
      detail !== undefined && collectionDetailCard(detail),
      collectionsCard(node),
      peersCard(node),
      quotaCard(node),
      transitCard(node, setBusy),
      processCard(node, setBusy),
      node.process.supervisor && supervisorCard(node.process.api_port, setBusy),
    );
  }

  return { render, element };
}
