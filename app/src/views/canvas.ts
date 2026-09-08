/**
 * The centre canvas: the same mesh, drawn two ways.
 *
 * Tree is the inventory view -- every node this device knows about, with its
 * ledger position, grouped the way the sidebar groups them. Mesh is the
 * topology view -- who can actually reach whom, drawn from each node's
 * `/_peers`.
 *
 * Features pan & zoom navigation, edge telemetry hover tooltips (latency,
 * fitness, packet loss), and live node card indicators.
 */

import { convergenceOf, meshTip, store, type Convergence, type NodeView } from "../state.js";
import { count, engineLabel, percent, shortHash, shortNode } from "../util/format.js";
import { el, icon, Icons, on, replace, svg } from "../util/dom.js";

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

interface EdgeData {
  a: Placed;
  b: Placed;
  live: boolean;
  fitness: number;
  latencyMs: number;
  successRate: number;
}

/** Pan and zoom state, persistent across renders. */
let panX = 0;
let panY = 0;
let zoomScale = 1.0;

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

function edgesOf(placed: Placed[]): EdgeData[] {
  const byId = new Map(placed.map((p) => [p.node.process.node_id, p]));
  const seen = new Set<string>();
  const edges: EdgeData[] = [];

  for (const p of placed) {
    for (const peer of p.node.peers) {
      const other = byId.get(peer.node_id);
      if (other === undefined) continue;
      const key = [p.node.process.node_id, peer.node_id].sort().join("|");
      if (seen.has(key)) continue;
      seen.add(key);
      const live = peer.state === "running" && Date.now() - peer.last_seen_ms < 30_000;
      const fitness = Math.max(0.25, Math.min(1.0, peer.fitness?.score || 0.5));
      const latencyMs = peer.fitness?.latency_ms || 0;
      const successRate = peer.fitness?.success_rate ?? 1.0;
      edges.push({ a: p, b: other, live, fitness, latencyMs, successRate });
    }
  }
  return edges;
}

const CARD_W = 156;
const CARD_H = 58;

function meshView(nodes: NodeView[], width: number, height: number, container: HTMLElement): HTMLElement {
  const placed = layout(nodes, width, height);
  const edges = edgesOf(placed);
  const selectedId = store.state.selection.nodeId;

  const wrapper = el("div", { style: "position: relative; width: 100%; height: 100%; min-height: 520px; overflow: hidden;" });

  const tooltip = el("div", { class: "mesh__tooltip", hidden: true });
  wrapper.appendChild(tooltip);

  const root = svg("svg", {
    class: "mesh",
    viewBox: `0 0 ${width} ${height}`,
    preserveAspectRatio: "xMidYMid meet",
    role: "img",
    "aria-label": `Mesh topology, ${nodes.length} nodes`,
    style: "width: 100%; height: 100%; display: block; cursor: grab;",
  });

  const viewport = svg("g", {
    class: "mesh__viewport",
    transform: `translate(${panX} ${panY}) scale(${zoomScale})`,
    "transform-origin": `${width / 2} ${height / 2}`,
  });
  root.appendChild(viewport);

  // Pan & Zoom interaction
  let isDragging = false;
  let startX = 0;
  let startY = 0;

  const updateTransform = () => {
    viewport.setAttribute("transform", `translate(${panX} ${panY}) scale(${zoomScale})`);
  };

  root.addEventListener("mousedown", (e) => {
    const target = e.target as SVGElement;
    if (target.closest(".mesh__node")) return;
    isDragging = true;
    startX = e.clientX - panX;
    startY = e.clientY - panY;
    root.style.cursor = "grabbing";
  });

  window.addEventListener("mousemove", (e) => {
    if (!isDragging) return;
    panX = e.clientX - startX;
    panY = e.clientY - startY;
    updateTransform();
  });

  window.addEventListener("mouseup", () => {
    if (isDragging) {
      isDragging = false;
      root.style.cursor = "grab";
    }
  });

  root.addEventListener("wheel", (e) => {
    e.preventDefault();
    const factor = e.deltaY < 0 ? 1.08 : 0.92;
    zoomScale = Math.max(0.35, Math.min(2.8, zoomScale * factor));
    updateTransform();
  });

  // Edge layer
  const edgeLayer = svg("g", { class: "mesh__edges" });
  for (const edge of edges) {
    const edgeLine = svg("line", {
      class: "mesh__edge",
      x1: edge.a.x,
      y1: edge.a.y,
      x2: edge.b.x,
      y2: edge.b.y,
      "stroke-dasharray": edge.live ? null : "4 4",
      opacity: edge.live ? edge.fitness : 0.35,
    });

    edgeLine.addEventListener("mouseenter", (e: MouseEvent) => {
      const rect = container.getBoundingClientRect();
      tooltip.style.left = `${e.clientX - rect.left}px`;
      tooltip.style.top = `${e.clientY - rect.top}px`;
      tooltip.textContent = `${edge.latencyMs.toFixed(1)}ms · ${percent(edge.successRate)} success · fitness ${edge.fitness.toFixed(2)}`;
      tooltip.hidden = false;
    });

    edgeLine.addEventListener("mouseleave", () => {
      tooltip.hidden = true;
    });

    edgeLayer.appendChild(edgeLine);
  }
  viewport.appendChild(edgeLayer);

  // Nodes layer
  const nodesLayer = svg("g", { class: "mesh__nodes" });
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
        rx: 12,
        stroke: "var(--color-hairline)",
      }),
    );

    // 4px Health Strip
    group.appendChild(
      svg("rect", { width: 4.5, height: CARD_H, rx: 2, fill: STATUS_VAR[p.status] }),
    );

    // Pulse dot
    group.appendChild(
      svg("circle", { cx: 16, cy: 22, r: 4, fill: STATUS_VAR[p.status] }),
    );

    group.appendChild(
      svg("text", { class: "mesh__node-label", x: 26, y: 26 }, name),
    );

    group.appendChild(
      svg(
        "text",
        { class: "mesh__node-meta", x: 16, y: 45 },
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
    nodesLayer.appendChild(group);
  }
  viewport.appendChild(nodesLayer);
  wrapper.appendChild(root);

  // Floating Zoom Controls
  const zoomInBtn = el("button", { class: "btn btn--sm btn--ghost", type: "button", title: "Zoom in" }, icon(Icons.zoomIn, 14));
  on(zoomInBtn, "click", () => {
    zoomScale = Math.min(2.8, zoomScale * 1.25);
    updateTransform();
  });

  const zoomOutBtn = el("button", { class: "btn btn--sm btn--ghost", type: "button", title: "Zoom out" }, icon(Icons.zoomOut, 14));
  on(zoomOutBtn, "click", () => {
    zoomScale = Math.max(0.35, zoomScale * 0.8);
    updateTransform();
  });

  const resetBtn = el("button", { class: "btn btn--sm btn--ghost", type: "button", title: "Reset View" }, icon(Icons.zoomReset, 14));
  on(resetBtn, "click", () => {
    panX = 0;
    panY = 0;
    zoomScale = 1.0;
    updateTransform();
  });

  const controls = el("div", { class: "canvas__controls" }, zoomInBtn, zoomOutBtn, resetBtn);
  wrapper.appendChild(controls);

  return wrapper;
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
          { class: "row", style: "margin-top: var(--space-xs); flex-wrap: wrap;" },
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
  const body = el("div", { style: "width: 100%; height: 100%;" });
  const summary = el("span", { class: "muted", style: "font: var(--text-fine);" });
  const toolbar = el("div", { class: "canvas__toolbar" }, summary);
  const element = el("main", { class: "canvas" }, toolbar, body);

  function render(): void {
    const nodes = store.dataNodes();
    const mode = store.state.canvasMode;

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
      const side = Math.max(760, 260 + Math.ceil(Math.sqrt(nodes.length)) * 200);
      replace(body, meshView(nodes, side, Math.round(side * 0.65), element));
    } else {
      replace(body, treeView(nodes));
    }
  }

  return { render, element };
}

export function statusText(status: Convergence): string {
  return STATUS_TEXT[status];
}
