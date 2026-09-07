/**
 * The centre canvas: the same mesh, drawn two ways.
 *
 * Tree is the inventory view -- every node this device knows about, with its
 * ledger position, grouped the way the sidebar groups them. Mesh is the
 * topology view -- who can actually reach whom, drawn from each node's
 * `/_peers`.
 *
 * Both are read-only. Nothing on this canvas writes; selecting a node moves
 * the inspector, and that is all. A control room where a stray drag can
 * repartition a cluster is a control room people are afraid to touch.
 *
 * Layout is deterministic: positions come from a hash of the node id, not from
 * a force simulation. The same mesh therefore draws identically on every
 * refresh and on every machine, so "the node on the left" means the same node
 * five minutes later. A simulation would look livelier and be useless for
 * that.
 */

import { convergenceOf, meshTip, store, type Convergence, type NodeView } from "../state.js";
import { count, engineLabel, shortHash, shortNode } from "../util/format.js";
import { el, on, replace, svg } from "../util/dom.js";

const STATUS_VAR: Record<Convergence, string> = {
  converged: "var(--color-status-converged)",
  lagging: "var(--color-status-lagging)",
  diverged: "var(--color-status-offline)",
  offline: "var(--color-status-offline)",
  supervisor: "var(--color-status-supervisor)",
};

const STATUS_TEXT: Record<Convergence, string> = {
  converged: "In sync",
  lagging: "Catching up",
  diverged: "Diverged",
  offline: "Offline",
  supervisor: "Supervisor",
};

/**
 * FNV-1a over the node id. Any stable hash would do; this one is four lines
 * and needs no imports.
 */
function hash32(text: string): number {
  let h = 0x811c9dc5;
  for (let i = 0; i < text.length; i++) {
    h ^= text.charCodeAt(i);
    h = Math.imul(h, 0x01000193) >>> 0;
  }
  return h >>> 0;
}

interface Placed {
  node: NodeView;
  status: Convergence;
  x: number;
  y: number;
}

/**
 * Places nodes on concentric rings.
 *
 * Ring assignment is by index so the layout stays legible as the mesh grows to
 * the fifty nodes the spec targets; the angle within a ring is seeded by the
 * node id, so a node keeps its bearing when its neighbours come and go. A
 * small id-derived jitter on the radius stops nodes at the same angle on
 * adjacent rings from lining up into false spokes.
 */
function layout(nodes: NodeView[], width: number, height: number): Placed[] {
  const tip = meshTip();
  const cx = width / 2;
  const cy = height / 2;
  const maxRadius = Math.min(width, height) / 2 - 90;
  const placed: Placed[] = [];

  const ordered = [...nodes].sort((a, b) => a.process.node_id.localeCompare(b.process.node_id));
  if (ordered.length === 1) {
    return [{ node: ordered[0], status: convergenceOf(ordered[0], tip), x: cx, y: cy }];
  }

  // Ring capacities grow outward: 6, 12, 18, ... which keeps spacing even.
  const rings: NodeView[][] = [];
  let index = 0;
  for (let ring = 0; index < ordered.length; ring++) {
    const capacity = ring === 0 ? Math.min(6, ordered.length) : 6 * (ring + 1);
    rings.push(ordered.slice(index, index + capacity));
    index += capacity;
  }

  rings.forEach((members, ring) => {
    const radius = rings.length === 1 ? maxRadius * 0.55 : (maxRadius * (ring + 1)) / rings.length;
    members.forEach((node, i) => {
      const seed = hash32(node.process.node_id);
      // Even spacing, nudged by the id so identical meshes are not perfectly
      // symmetric (which reads as a diagram rather than a topology).
      const jitterAngle = ((seed & 0xff) / 255 - 0.5) * ((Math.PI * 2) / members.length) * 0.35;
      const angle = (i / members.length) * Math.PI * 2 - Math.PI / 2 + jitterAngle;
      const jitterRadius = 1 + (((seed >>> 8) & 0x3f) / 63 - 0.5) * 0.08;
      placed.push({
        node,
        status: convergenceOf(node, tip),
        x: cx + Math.cos(angle) * radius * jitterRadius,
        y: cy + Math.sin(angle) * radius * jitterRadius,
      });
    });
  });
  return placed;
}

/** Edges are drawn once per pair, from whichever side reports the other. */
function edgesOf(placed: Placed[]): { a: Placed; b: Placed; live: boolean }[] {
  const byId = new Map(placed.map((p) => [p.node.process.node_id, p]));
  const seen = new Set<string>();
  const edges: { a: Placed; b: Placed; live: boolean }[] = [];

  for (const p of placed) {
    for (const peer of p.node.peers) {
      const other = byId.get(peer.node_id);
      if (other === undefined) continue;
      const key = [p.node.process.node_id, peer.node_id].sort().join("|");
      if (seen.has(key)) continue;
      seen.add(key);
      // "Live" means the peer was heard from recently. A stale edge is still
      // drawn -- the fact that two nodes used to talk and no longer do is
      // exactly what an operator is looking for -- but drawn faintly.
      const live = peer.state === "running" && Date.now() - peer.last_seen_ms < 30_000;
      edges.push({ a: p, b: other, live });
    }
  }
  return edges;
}

const CARD_W = 148;
const CARD_H = 54;

function meshView(nodes: NodeView[], width: number, height: number): SVGElement {
  const placed = layout(nodes, width, height);
  const edges = edgesOf(placed);
  const selectedId = store.state.selection.nodeId;

  const root = svg("svg", {
    class: "mesh",
    viewBox: `0 0 ${width} ${height}`,
    preserveAspectRatio: "xMidYMid meet",
    role: "img",
    "aria-label": `Mesh topology, ${nodes.length} nodes`,
  });

  const edgeLayer = svg("g", {});
  for (const edge of edges) {
    edgeLayer.appendChild(
      svg("line", {
        class: "mesh__edge",
        x1: edge.a.x,
        y1: edge.a.y,
        x2: edge.b.x,
        y2: edge.b.y,
        "stroke-dasharray": edge.live ? null : "4 4",
        opacity: edge.live ? 1 : 0.45,
      }),
    );
  }
  root.appendChild(edgeLayer);

  for (const p of placed) {
    const tipId = p.node.tip?.entry_id ?? p.node.brain?.ledger_tip.entry_id ?? 0;
    const name = p.node.process.node_name || shortNode(p.node.process.node_id, 8);

    const group = svg("g", {
      class: "mesh__node",
      "data-selected": String(selectedId === p.node.process.node_id),
      role: "button",
      tabindex: "0",
      "aria-label": `${name}, ${STATUS_TEXT[p.status]}, ledger entry ${tipId}`,
      transform: `translate(${p.x - CARD_W / 2} ${p.y - CARD_H / 2})`,
    });

    group.appendChild(
      svg("rect", {
        class: "mesh__node-card",
        width: CARD_W,
        height: CARD_H,
        rx: 11,
        stroke: "var(--color-hairline)",
      }),
    );
    // The status is a filled bar down the card's leading edge rather than a
    // dot: at this size a dot disappears, and the bar reads at a glance across
    // a fifty-node canvas.
    group.appendChild(
      svg("rect", { width: 4, height: CARD_H, rx: 2, fill: STATUS_VAR[p.status] }),
    );
    group.appendChild(
      svg("text", { class: "mesh__node-label", x: 16, y: 22 }, name),
    );
    group.appendChild(
      svg(
        "text",
        { class: "mesh__node-meta", x: 16, y: 40 },
        p.status === "offline" ? STATUS_TEXT[p.status] : `#${tipId} · ${shortHash(p.node.tip?.entry_hash, 6, 0)}`,
      ),
    );

    const select = () => store.select({ kind: "node", nodeId: p.node.process.node_id });
    group.addEventListener("click", select);
    group.addEventListener("keydown", (event) => {
      const key = (event as KeyboardEvent).key;
      if (key === "Enter" || key === " ") {
        event.preventDefault();
        select();
      }
    });
    root.appendChild(group);
  }

  return root;
}

function treeView(nodes: NodeView[]): HTMLElement {
  const tip = meshTip();
  const container = el("div", { class: "tree-canvas" });

  for (const node of nodes) {
    const status = convergenceOf(node, tip);
    const selected = store.state.selection.nodeId === node.process.node_id;
    const tipId = node.tip?.entry_id ?? node.brain?.ledger_tip.entry_id ?? 0;
    const behind = tip !== null ? tip.entry_id - tipId : 0;

    const collections = node.brain?.collections ?? [];
    const card = el(
      "article",
      { class: selected ? "card card--elevated" : "card", tabindex: "0", role: "button" },
      el(
        "div",
        { class: "row row--between" },
        el(
          "div",
          { class: "row" },
          el("span", { class: "dot", "data-status": status }),
          el("h3", { class: "card__title", text: node.process.node_name || shortNode(node.process.node_id) }),
        ),
        el("span", { class: "badge", "data-tone": status, text: STATUS_TEXT[status] }),
      ),
      el(
        "dl",
        { class: "kv" },
        el("dt", { text: "Ledger" }),
        el("dd", { class: "mono", text: `#${tipId}${behind > 0 ? ` (${behind} behind)` : ""}` }),
        el("dt", { text: "Collections" }),
        el("dd", { text: String(collections.length) }),
        el("dt", { text: "Documents" }),
        el("dd", { text: count(collections.reduce((sum, c) => sum + c.document_count, 0)) }),
        el("dt", { text: "Free quota" }),
        el("dd", { text: node.brain ? `${node.brain.free_quota_mb} MiB` : "—" }),
        el("dt", { text: "Data directory" }),
        el("dd", { class: "mono", text: node.process.data_dir }),
      ),
      collections.length > 0 &&
        el(
          "div",
          { class: "row" },
          ...collections
            .slice(0, 6)
            .map((c) =>
              el("span", {
                class: "chip",
                title: `${c.document_count} documents · checksum ${shortHash(c.checksum)}`,
                text: `${c.name} · ${engineLabel(c.engine)}`,
              }),
            ),
          collections.length > 6 && el("span", { class: "muted", text: `+${collections.length - 6} more` }),
        ),
    );

    const select = () => store.select({ kind: "node", nodeId: node.process.node_id });
    on(card, "click", select);
    on(card, "keydown", (event) => {
      if (event.key === "Enter" || event.key === " ") {
        event.preventDefault();
        select();
      }
    });
    container.appendChild(card);
  }
  return container;
}

function emptyCanvas(onNewNode: () => void): HTMLElement {
  const button = el("button", { class: "btn btn--primary", type: "button", text: "Create a node" });
  on(button, "click", onNewNode);
  return el(
    "div",
    { class: "empty" },
    el("p", { class: "empty__title", text: "Nothing to chart yet" }),
    el("p", {
      class: "empty__body",
      text: "A node is a place to keep data — a folder on this machine, or a drive you can carry. Create one and it will appear here, along with every peer it finds.",
    }),
    button,
  );
}

export interface CanvasHandles {
  render(): void;
  element: HTMLElement;
}

export function createCanvas(onNewNode: () => void): CanvasHandles {
  const body = el("div", {});

  const modeButton = (mode: "tree" | "mesh", label: string): HTMLElement => {
    const button = el("button", { type: "button", text: label, "aria-pressed": "false" });
    on(button, "click", () => store.setCanvasMode(mode));
    button.dataset.mode = mode;
    return button;
  };

  const segmented = el("div", { class: "segmented", role: "group", "aria-label": "Canvas view" },
    modeButton("tree", "Tree"),
    modeButton("mesh", "Mesh"),
  );

  const summary = el("span", { class: "muted" });
  const toolbar = el("div", { class: "canvas__toolbar" }, segmented, el("span", { class: "header__spacer" }), summary);
  const element = el("main", { class: "canvas" }, toolbar, body);

  function render(): void {
    const nodes = store.dataNodes();
    const mode = store.state.canvasMode;

    for (const button of segmented.querySelectorAll("button")) {
      button.setAttribute("aria-pressed", String(button.dataset.mode === mode));
    }

    const reachable = nodes.filter((n) => n.reachable).length;
    const held = nodes.reduce((sum, n) => sum + (n.brain?.transit_documents_held ?? 0), 0);
    summary.textContent =
      nodes.length === 0
        ? ""
        : `${reachable}/${nodes.length} answering${held > 0 ? ` · ${count(held)} documents held in transit` : ""}`;

    if (nodes.length === 0) {
      replace(body, emptyCanvas(onNewNode));
      return;
    }

    if (mode === "mesh") {
      // The viewBox grows with the node count so a large mesh does not compress
      // into an unreadable knot; the SVG scales to the pane either way.
      const side = Math.max(720, 260 + Math.ceil(Math.sqrt(nodes.length)) * 190);
      replace(body, meshView(nodes, side, Math.round(side * 0.62)));
    } else {
      replace(body, treeView(nodes));
    }
  }

  return { render, element };
}

/** Exported for the inspector's storage summary, which uses the same wording. */
export function statusText(status: Convergence): string {
  return STATUS_TEXT[status];
}
