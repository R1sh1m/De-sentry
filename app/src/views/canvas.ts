/**
 * The centre canvas: the same mesh, drawn two ways.
 *
 * Tree is the inventory view -- every node this device knows about, with its
 * ledger position, grouped the way the sidebar groups them. Mesh is the
 * topology view -- who can actually reach whom, drawn from each node's
 * `/_peers`.
 *
 * Features pan & zoom navigation, edge telemetry hover tooltips (latency,
 * fitness, packet loss), and live node card indicators. Node details appear
 * in a small popover anchored just above the selected node -- there is no
 * longer a right-hand inspector column.
 */

import { convergenceOf, meshTip, refreshNodeList, refreshTopology, store, type Convergence, type NodeView } from "../state.js";
import { sidecar, type DiscoveredCandidate } from "../bridge.js";
import { openUnlockModal } from "./unlockModal.js";
import { openChangePassphraseModal } from "./changePassphraseModal.js";
import { apiFor } from "../api.js";
import { bytes, count, displayNodeName, duration, engineLabel, percent, shortHash, shortNode, truncate } from "../util/format.js";
import { el, icon, Icons, on, replace, svg } from "../util/dom.js";
import { emptyState } from "../util/empty.js";

/** Node purpose cache (wizard description in manifest.json), keyed by node id. */
const purposeCache = new Map<string, string | null>();

/** Node physical folder size on disk cache (bytes), keyed by node id. */
const diskUsageCache = new Map<string, number>();

/** Node allocation mode cache (reserved vs dynamic), keyed by node id. */
const allocationModeCache = new Map<string, "reserved" | "dynamic">();

/** Best human name for a card: explicit name first, then folder, then id. */
function displayNameOf(node: NodeView): string {
  return displayNodeName(node.process.node_name, node.process.data_dir, node.process.node_id);
}

/** The unmodified stored name, for tooltips alongside the display name. */
function rawNameOf(node: NodeView): string {
  const raw = (node.process.node_name || "").trim();
  return raw !== "" ? raw : node.process.data_dir || shortNode(node.process.node_id, 8);
}

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
  /**
   * "peer" is a real data-plane link from /_peers. "supervised" is a
   * supervision spoke: every node in the store is managed by this device's
   * sidecar, so a node with no peer edge to the supervisor still gets a
   * visibly distinct spoke instead of floating unconnected. Spokes never
   * claim replication flows anywhere.
   */
  kind: "peer" | "supervised";
}

/** Pan and zoom state, persistent across renders. */
let panX = 0;
let panY = 0;
let zoomScale = 1.0;

/**
 * User-arranged node displacements in layout coordinates, keyed by node id.
 * Applied on top of layout() output so a manual arrangement survives
 * re-renders (refresh polls, resizes, view switches). Session-only: never
 * persisted, never synced — positions are presentation, not mesh state.
 */
const nodeOffsets = new Map<string, { x: number; y: number }>();

/** Fold user displacements into a fresh layout. */
function withOffsets(placed: Placed[]): Placed[] {
  for (const p of placed) {
    const o = nodeOffsets.get(p.node.process.node_id);
    if (o !== undefined) {
      p.x += o.x;
      p.y += o.y;
    }
  }
  return placed;
}

/** Upper zoom bound (Slice 1 guardrail): past 2.2x SVG text raster blurs. */
const ZOOM_MIN = 0.35;
const ZOOM_MAX = 2.2;

/** Single document-level outside-click dismiss, registered once (not per render). */
let popoverOutsideArmed = false;

function armPopoverOutsideClick(): void {
  if (popoverOutsideArmed) return;
  popoverOutsideArmed = true;
  document.addEventListener("pointerdown", (e) => {
    try {
      const sel = store.state.selection;
      if (sel.kind !== "node") return;
      const target = e.target as HTMLElement | null;
      if (target === null || target.closest === undefined) return;
      // Chrome (zoom controls, starmap) and cards/popover itself never dismiss.
      if (
        target.closest(".mesh__popover")
        || target.closest(".mesh__node")
        || target.closest(".canvas__controls")
        || target.closest(".mesh-minimap")
      ) {
        return;
      }
      store.select({ kind: "none" });
    } catch {
      // Dismissal is a nicety; never break the event that triggered it.
    }
  }, true);
}


let meshPlacementMode: "concentric" | "hierarchical" = "concentric";

function layout(nodes: NodeView[], width: number, height: number): Placed[] {
  const tip = meshTip();
  const cx = width / 2;
  const cy = height / 2;
  const maxRadius = Math.min(width, height) / 2 - 90;
  const placed: Placed[] = [];

  const ordered = [...nodes].sort((a, b) => a.process.node_id.localeCompare(b.process.node_id));
  if (ordered.length === 1) {
    return withOffsets([{ node: ordered[0], status: convergenceOf(ordered[0], tip), x: cx, y: cy }]);
  }

  if (meshPlacementMode === "hierarchical") {
    const levels: NodeView[][] = [];
    const assigned = new Set<string>();

    const primary = ordered.find((n) => n.process.supervisor) || ordered[0];
    levels.push([primary]);
    assigned.add(primary.process.node_id);

    const directPeerIds = new Set(primary.peers.map((p) => p.node_id));
    const level1 = ordered.filter((n) => !assigned.has(n.process.node_id) && directPeerIds.has(n.process.node_id));
    if (level1.length > 0) {
      levels.push(level1);
      for (const n of level1) assigned.add(n.process.node_id);
    }

    const remaining = ordered.filter((n) => !assigned.has(n.process.node_id));
    if (remaining.length > 0) {
      for (let i = 0; i < remaining.length; i += 4) {
        levels.push(remaining.slice(i, i + 4));
      }
    }

    const startY = 90;
    const availH = height - 180;
    const rowStep = levels.length > 1 ? availH / (levels.length - 1) : 0;

    levels.forEach((row, rowIdx) => {
      const y = levels.length === 1 ? cy : startY + rowIdx * rowStep;
      const colStep = width / (row.length + 1);
      row.forEach((node, colIdx) => {
        const x = colStep * (colIdx + 1);
        placed.push({
          node,
          status: convergenceOf(node, tip),
          x,
          y,
        });
      });
    });

    return withOffsets(placed);
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
  return withOffsets(placed);
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
      edges.push({ a: p, b: other, live, fitness, latencyMs, successRate, kind: "peer" });
    }
  }

  // Supervision spokes: no supervised node floats alone. A spoke is drawn
  // from each non-supervisor node to a placed supervisor only when no peer
  // edge already joins that pair (peer links win; spokes fill the gaps).
  const supervisors = placed.filter((p) => p.node.process.supervisor);
  if (supervisors.length > 0) {
    for (const p of placed) {
      if (p.node.process.supervisor) continue;
      const hub = supervisors[0];
      const key = [p.node.process.node_id, hub.node.process.node_id].sort().join("|");
      if (seen.has(key)) continue;
      seen.add(key);
      edges.push({
        a: p,
        b: hub,
        live: p.status !== "offline",
        fitness: 0.3,
        latencyMs: 0,
        successRate: 1,
        kind: "supervised",
      });
    }
  }
  return edges;
}

const CARD_W = 200;
const CARD_H = 60;

function meshView(nodes: NodeView[], width: number, height: number, _container: HTMLElement): HTMLElement {
  const placed = layout(nodes, width, height);
  const edges = edgesOf(placed);
  const selectedId = store.state.selection.nodeId;

  const wrapper = el("div", {
    class: zoomScale < 0.6 ? "mesh-zoom-host mesh-canvas--compact" : "mesh-zoom-host",
    style: "position: relative; width: 100%; height: 100%; min-height: 520px; overflow: hidden;",
  });

  const vignette = el("div", { class: "mesh__vignette", "aria-hidden": "true" });
  wrapper.appendChild(vignette);

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

  // Pan & Zoom interaction (mouse drag + wheel + multi-touch pinch + trackpad).
  // Dragging that starts on a node card moves the node (edges follow);
  // dragging that starts on empty water pans the viewport.
  let isDragging = false;
  let startX = 0;
  let startY = 0;
  let downX = 0;
  let downY = 0;
  let draggedFar = false;
  // Node-drag state: downNodeId is the press candidate, dragNodeId the live
  // drag (both null unless the gesture started on a card).
  let downNodeId: string | null = null;
  let dragNodeId: string | null = null;
  let dragLastX = 0;
  let dragLastY = 0;

  /** Move one card and re-anchor its edges, all in the live DOM. */
  const moveNodeDom = (nodeId: string, dx: number, dy: number): void => {
    const card = viewport.querySelector(`.mesh__node[data-node-id="${nodeId}"]`);
    if (card) {
      const cur = card.getAttribute("transform") || "";
      const m = /translate\(\s*([-\d.]+)\s+([-\d.]+)\s*\)/.exec(cur);
      if (m) {
        card.setAttribute(
          "transform",
          `translate(${parseFloat(m[1]) + dx} ${parseFloat(m[2]) + dy})`,
        );
      }
    }
    const lines = edgeLayer.querySelectorAll("line[data-edge]");
    for (let i = 0; i < lines.length; i++) {
      const line = lines[i];
      const parts = (line.getAttribute("data-edge") || "").split("|");
      if (parts.length !== 2 || (parts[0] !== nodeId && parts[1] !== nodeId)) continue;
      const first = parts[0] === nodeId;
      const attr = (name: string): number => parseFloat(line.getAttribute(name) || "0") || 0;
      if (first) {
        line.setAttribute("x1", String(attr("x1") + dx));
        line.setAttribute("y1", String(attr("y1") + dy));
      } else {
        line.setAttribute("x2", String(attr("x2") + dx));
        line.setAttribute("y2", String(attr("y2") + dy));
      }
    }
    const o = nodeOffsets.get(nodeId) || { x: 0, y: 0 };
    nodeOffsets.set(nodeId, { x: o.x + dx, y: o.y + dy });
    positionPopover();
  };

  const nodeIdFromTarget = (target: EventTarget | null): string | null => {
    const card = (target as Element | null)?.closest?.(".mesh__node");
    return card?.getAttribute("data-node-id") || null;
  };

  const popover = el("div", { class: "mesh__popover", hidden: true });
  wrapper.appendChild(popover);

  const positionPopover = (): void => {
    try {
      const selected = wrapper.querySelector(".mesh__node[data-selected=\"true\"]");
      if (!selected || store.state.selection.kind !== "node") {
        popover.hidden = true;
        return;
      }
      const nodeRect = (selected as SVGGElement).getBoundingClientRect();
      const hostRect = wrapper.getBoundingClientRect();
      if (nodeRect.width === 0 && nodeRect.height === 0) {
        popover.hidden = true;
        // Schedule retry once mounted and laid out in the DOM
        requestAnimationFrame(positionPopover);
        window.setTimeout(positionPopover, 20);
        return;
      }
      // Scale proportionally with zoom level (matches zoomScale down to ZOOM_MIN 0.35x, up to 1.25x max)
      const popScale = Math.max(0.35, Math.min(1.25, zoomScale));
      popover.style.setProperty("--popover-scale", String(popScale));
      const halfPopover = 118 * popScale;
      const left = nodeRect.left - hostRect.left + nodeRect.width / 2;
      const top = nodeRect.top - hostRect.top;
      const clampedX = Math.max(halfPopover + 8, Math.min(hostRect.width - halfPopover - 8, left));

      const flipToBottom = top < (180 * popScale) && hostRect.height - (top + nodeRect.height) > (180 * popScale);
      if (flipToBottom) {
        popover.setAttribute("data-placement", "bottom");
        popover.style.top = `${Math.round(top + nodeRect.height)}px`;
      } else {
        popover.removeAttribute("data-placement");
        popover.style.top = `${Math.round(Math.max(8, top))}px`;
      }
      popover.style.left = `${Math.round(clampedX)}px`;
      popover.hidden = false;
    } catch {
      // Geometry unavailable: popover stays hidden rather than misplaced.
      popover.hidden = true;
    }
  };

  const schedulePositionPopover = (): void => {
    positionPopover();
    requestAnimationFrame(positionPopover);
    window.setTimeout(positionPopover, 0);
    window.setTimeout(positionPopover, 60);
  };

  const updateTransform = () => {
    viewport.setAttribute("transform", `translate(${panX} ${panY}) scale(${zoomScale})`);
    wrapper.classList.toggle("mesh-canvas--compact", zoomScale < 0.6);
    wrapper.classList.toggle("mesh-canvas--surface", zoomScale > 1.6);
    minimapRect?.setAttribute("x", String(centerX() - viewW() / 2));
    minimapRect?.setAttribute("y", String(centerY() - viewH() / 2));
    minimapRect?.setAttribute("width", String(viewW()));
    minimapRect?.setAttribute("height", String(viewH()));
    try {
      const zoomPct = Math.round(zoomScale * 100);
      const label = `${zoomPct}%`;
      if (zoomPill.textContent !== label) zoomPill.textContent = label;
      zoomPill.setAttribute("aria-label", `Zoom level ${label}. Activate to reset.`);
      if (zoomSlider && zoomSlider.value !== String(zoomPct)) {
        zoomSlider.value = String(zoomPct);
      }
    } catch {
      // Controls not yet mounted (first updateTransform runs before controls).
    }
    positionPopover();
  };

  // Viewport geometry in layout coords (screen S = P*z + pan).
  const centerX = (): number => (width / 2 - panX) / zoomScale;
  const centerY = (): number => (height / 2 - panY) / zoomScale;
  const viewW = (): number => width / zoomScale;
  const viewH = (): number => height / zoomScale;

  // Starmap minimap: a static miniature of the same map — edges first
  // (peer links plus supervision spokes, same geometry as the main view), then status dots, then the viewport rect. In
  // hierarchical mode the dots already sit on tree rows, so the miniature
  // reads as a tree map; in radial mode it reads as the mesh.
  const MM_W = 96;
  const MM_H = 64;
  const mmSvg = svg("svg", {
    class: "mesh-minimap__svg",
    viewBox: `0 0 ${width} ${height}`,
    width: MM_W,
    height: MM_H,
    "aria-hidden": "true",
  });
  for (const edge of edges) {
    mmSvg.appendChild(
      svg("line", {
        x1: edge.a.x,
        y1: edge.a.y,
        x2: edge.b.x,
        y2: edge.b.y,
        class: edge.live ? "mesh-minimap__edge" : "mesh-minimap__edge--stale",
      }),
    );
  }
  for (const p of placed) {
    const isSelected = selectedId === p.node.process.node_id;
    mmSvg.appendChild(
      svg("circle", {
        cx: p.x,
        cy: p.y,
        r: isSelected ? 11 : 8,
        fill: STATUS_VAR[p.status],
        class: isSelected ? "mesh-minimap__dot--selected" : "mesh-minimap__dot",
      }),
    );
  }
  const minimapRect = svg("rect", {
    class: "mesh-minimap__viewport",
    x: centerX() - viewW() / 2,
    y: centerY() - viewH() / 2,
    width: viewW(),
    height: viewH(),
    rx: 24,
  });
  mmSvg.appendChild(minimapRect);
  const minimap = el(
    "button",
    { class: "mesh-minimap", type: "button", title: "Starmap — click to center", "aria-label": "Starmap overview. Activate, then use arrow keys to pan." },
    mmSvg as unknown as HTMLElement,
  );
  // Click-to-center: clicking a point centers it in the main view.
  mmSvg.addEventListener("click", (e: MouseEvent) => {
    try {
      const r = (mmSvg as unknown as SVGSVGElement).getBoundingClientRect();
      const lx = ((e.clientX - r.left) / Math.max(1, r.width)) * width;
      const ly = ((e.clientY - r.top) / Math.max(1, r.height)) * height;
      panX = width / 2 - lx * zoomScale;
      panY = height / 2 - ly * zoomScale;
      updateTransform();
    } catch {
      // Geometry unavailable: minimap stays informational.
    }
  });
  // Keyboard: arrows pan the main view when the minimap is focused.
  minimap.addEventListener("keydown", (e: KeyboardEvent) => {
    const step = 40;
    if (e.key === "ArrowLeft") {
      e.preventDefault();
      panX += step;
      updateTransform();
    } else if (e.key === "ArrowRight") {
      e.preventDefault();
      panX -= step;
      updateTransform();
    } else if (e.key === "ArrowUp") {
      e.preventDefault();
      panY += step;
      updateTransform();
    } else if (e.key === "ArrowDown") {
      e.preventDefault();
      panY -= step;
      updateTransform();
    }
  });
  updateTransform();

  // Single-pointer drag pans; two pointers pinch-zoom + pan together.
  // Pointer Events unify mouse / pen / touch; touch-action:none (CSS) stops
  // the webview from stealing the gesture for scroll.
  const activePointers = new Map<number, { x: number; y: number; onNode: boolean }>();
  let pinchPrevDist = 0;
  let pinchPrevMidX = 0;
  let pinchPrevMidY = 0;

  const pinchSnapshot = (): void => {
    const pts = [...activePointers.values()];
    if (pts.length !== 2) return;
    const dx = pts[0].x - pts[1].x;
    const dy = pts[0].y - pts[1].y;
    pinchPrevDist = Math.max(1, Math.hypot(dx, dy));
    pinchPrevMidX = (pts[0].x + pts[1].x) / 2;
    pinchPrevMidY = (pts[0].y + pts[1].y) / 2;
  };

  // Every pointer is tracked — even ones starting on a node card — so a
  // two-finger pinch engages wherever the fingers land. Only drag-panning
  // and tap-to-deselect stay gated on "not on a node".
  const onNode = (target: EventTarget | null): boolean => {
    try {
      return (target as SVGElement | null)?.closest?.(".mesh__node") != null;
    } catch {
      return false;
    }
  };

  root.addEventListener("pointerdown", (e) => {
    const startedOnNode = onNode(e.target);
    if (!startedOnNode) {
      try {
        root.setPointerCapture(e.pointerId);
      } catch {
        // Older webviews: fall back to window-level move/up below.
      }
    }
    activePointers.set(e.pointerId, { x: e.clientX, y: e.clientY, onNode: startedOnNode });
    downX = e.clientX;
    downY = e.clientY;
    draggedFar = false;
    dragNodeId = null;
    downNodeId = startedOnNode ? nodeIdFromTarget(e.target) : null;
    dragLastX = e.clientX;
    dragLastY = e.clientY;
    if (activePointers.size === 2) {
      isDragging = false;
      pinchSnapshot();
    } else if (activePointers.size === 1 && !startedOnNode) {
      isDragging = true;
      startX = e.clientX - panX;
      startY = e.clientY - panY;
      root.style.cursor = "grabbing";
    }
  });

  root.addEventListener("pointermove", (e) => {
    const known = activePointers.get(e.pointerId);
    if (!known) return;
    activePointers.set(e.pointerId, { x: e.clientX, y: e.clientY, onNode: known.onNode });
    if (Math.hypot(e.clientX - downX, e.clientY - downY) > 4) draggedFar = true;
    if (activePointers.size === 2) {
      const pts = [...activePointers.values()];
      const dx = pts[0].x - pts[1].x;
      const dy = pts[0].y - pts[1].y;
      const dist = Math.max(1, Math.hypot(dx, dy));
      const midX = (pts[0].x + pts[1].x) / 2;
      const midY = (pts[0].y + pts[1].y) / 2;
      if (pinchPrevDist > 0) {
        const factor = dist / pinchPrevDist;
        zoomScale = Math.max(ZOOM_MIN, Math.min(ZOOM_MAX, zoomScale * factor));
        // Pan with the midpoint so the map follows the fingers.
        panX += midX - pinchPrevMidX;
        panY += midY - pinchPrevMidY;
        updateTransform();
      }
      pinchPrevDist = dist;
      pinchPrevMidX = midX;
      pinchPrevMidY = midY;
      return;
    }
    if (activePointers.size === 1 && downNodeId !== null && dragNodeId === null && draggedFar) {
      // Press started on a card and moved past tap slop: this is a node
      // drag, not a pan. Take pointer capture so fast moves keep tracking.
      dragNodeId = downNodeId;
      try {
        root.setPointerCapture(e.pointerId);
      } catch {
        // Older webviews: window-level move/up still fire.
      }
      root.style.cursor = "grabbing";
    }
    if (dragNodeId !== null && activePointers.size === 1) {
      const dx = (e.clientX - dragLastX) / zoomScale;
      const dy = (e.clientY - dragLastY) / zoomScale;
      dragLastX = e.clientX;
      dragLastY = e.clientY;
      if (dx !== 0 || dy !== 0) moveNodeDom(dragNodeId, dx, dy);
      return;
    }
    if (!isDragging) return;
    panX = e.clientX - startX;
    panY = e.clientY - startY;
    updateTransform();
  });

  const endPointer = (e: PointerEvent): void => {
    const ended = activePointers.get(e.pointerId);
    activePointers.delete(e.pointerId);
    try {
      if (root.hasPointerCapture(e.pointerId)) root.releasePointerCapture(e.pointerId);
    } catch {
      // ignore
    }
    if (activePointers.size === 1) {
      // Pinch ended with one finger left: resume panning from it, unless
      // that finger started on a node card (then it owns tap/click).
      const remaining = [...activePointers.values()][0];
      pinchPrevDist = 0;
      if (remaining.onNode) {
        isDragging = false;
        root.style.cursor = "grab";
      } else {
        isDragging = true;
        startX = remaining.x - panX;
        startY = remaining.y - panY;
      }
    } else if (activePointers.size === 0) {
      const wasTap = !draggedFar && dragNodeId === null;
      isDragging = false;
      dragNodeId = null;
      downNodeId = null;
      pinchPrevDist = 0;
      root.style.cursor = "grab";
      if (wasTap) {
        if (ended?.onNode || onNode(e.target)) {
          // Tap landed on a node: ensure it is selected
          const nodeEl = (e.target as Element | null)?.closest?.(".mesh__node")
            || document.elementFromPoint(e.clientX, e.clientY)?.closest?.(".mesh__node");
          const nodeId = nodeEl?.getAttribute("data-node-id");
          if (nodeId) {
            store.select({ kind: "node", nodeId });
            return;
          }
        } else if (store.state.selection.kind === "node") {
          // Tap on empty water: deselect
          store.select({ kind: "none" });
        }
      }
    }
  };
  root.addEventListener("pointerup", endPointer);
  root.addEventListener("pointercancel", endPointer);

  // Mouse fallback for webviews without pointer capture + legacy mousedown.
  root.addEventListener("mousedown", (e) => {
    if (activePointers.size > 0) return;
    const target = e.target as SVGElement;
    if (target.closest(".mesh__node")) return;
    isDragging = true;
    downX = e.clientX;
    downY = e.clientY;
    draggedFar = false;
    startX = e.clientX - panX;
    startY = e.clientY - panY;
    root.style.cursor = "grabbing";
  });
  window.addEventListener("mousemove", (e) => {
    if (!isDragging || activePointers.size > 0) return;
    if (Math.hypot(e.clientX - downX, e.clientY - downY) > 4) draggedFar = true;
    panX = e.clientX - startX;
    panY = e.clientY - startY;
    updateTransform();
  });
  window.addEventListener("mouseup", (e) => {
    if (!isDragging || activePointers.size > 0) return;
    const wasTap = !draggedFar;
    isDragging = false;
    root.style.cursor = "grab";
    if (wasTap) {
      const target = e.target as SVGElement | null;
      if (target && !target.closest(".mesh__node") && store.state.selection.kind === "node") {
        store.select({ kind: "none" });
      }
    }
  });

  // Wheel zoom listens on both host and SVG root: handles mouse wheel,
  // trackpad precision scroll, and trackpad pinch-to-zoom (ctrlKey).
  const onWheel = (e: WheelEvent): void => {
    try {
      e.preventDefault();
      let factor: number;
      if (e.ctrlKey) {
        // Trackpad pinch gesture (WebView2 / WKWebView emit wheel+ctrl).
        factor = Math.exp(-e.deltaY * 0.015);
      } else if (e.deltaMode === 1) {
        // Line scrolling (standard mouse wheel notch)
        factor = e.deltaY < 0 ? 1.1 : 0.9;
      } else {
        // Pixel scrolling (trackpad 2-finger scroll or continuous wheel)
        const normalized = Math.max(-100, Math.min(100, e.deltaY));
        factor = Math.exp(-normalized * 0.003);
      }
      factor = Math.max(0.75, Math.min(1.35, factor));
      zoomScale = Math.max(ZOOM_MIN, Math.min(ZOOM_MAX, zoomScale * factor));
      updateTransform();
    } catch {
      // A zoom gesture must never break the canvas; buttons/keys remain.
    }
  };
  wrapper.addEventListener("wheel", onWheel as EventListener, { passive: false });
  root.addEventListener("wheel", onWheel as EventListener, { passive: false });

  // Double-click empty water: spring-settle zoom-to-fit.
  root.addEventListener("dblclick", (e) => {
    const target = e.target as SVGElement;
    if (target.closest(".mesh__node")) return;
    try {
      viewport.style.transition = "transform var(--motion-settle)";
      panX = 0;
      panY = 0;
      zoomScale = 1.0;
      updateTransform();
      window.setTimeout(() => {
        viewport.style.transition = "";
      }, 300);
    } catch {
      panX = 0;
      panY = 0;
      zoomScale = 1.0;
      updateTransform();
    }
  });

  // Edge layer
  const edgeLayer = svg("g", { class: "mesh__edges" });
  for (const edge of edges) {
    const supervised = edge.kind === "supervised";
    // data-edge mirrors edgesOf()'s dedupe key so a dragged node can find
    // and re-anchor its own lines without a re-render.
    const edgeKey = [edge.a.node.process.node_id, edge.b.node.process.node_id].sort().join("|");
    const edgeLine = svg("line", {
      class: supervised ? "mesh__edge mesh__edge--supervised" : "mesh__edge",
      "data-edge": edgeKey,
      x1: edge.a.x,
      y1: edge.a.y,
      x2: edge.b.x,
      y2: edge.b.y,
      "stroke-dasharray": edge.live && !supervised ? null : "4 4",
      opacity: supervised ? 0.5 : edge.live ? edge.fitness : 0.35,
    });

    edgeLine.addEventListener("mouseenter", (e: MouseEvent) => {
      const rect = wrapper.getBoundingClientRect();
      tooltip.style.left = `${e.clientX - rect.left}px`;
      tooltip.style.top = `${e.clientY - rect.top}px`;
      tooltip.textContent = supervised
        ? "supervised by this device · no data-plane peer link"
        : `${edge.latencyMs.toFixed(1)}ms · ${percent(edge.successRate)} success · fitness ${edge.fitness.toFixed(2)}`;
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
    const rawTipId = p.node.tip?.entry_id ?? p.node.brain?.ledger_tip.entry_id ?? 0;
    const tipId = rawTipId < 1 ? 1 : rawTipId;
    const rawName = displayNameOf(p.node);
    const fullName = rawNameOf(p.node);
    const hoverName = fullName === rawName ? rawName : `${rawName} — ${fullName}`;
    const displayName = truncate(rawName, 22);
    const hash = p.node.tip?.entry_hash;
    const isZeroOrEmptyHash = !hash || /^0+$/.test(hash);
    const metaText = p.status === "offline"
      ? STATUS_TEXT[p.status]
      : isZeroOrEmptyHash
        ? `#${tipId}`
        : `#${tipId} · ${shortHash(hash, 6, 0)}`;

    const group = svg("g", {
      class: "mesh__node",
      "data-selected": String(selectedId === p.node.process.node_id),
      "data-node-id": p.node.process.node_id,
      role: "button",
      tabindex: "0",
      "aria-label": `${rawName}, ${STATUS_TEXT[p.status]}, ledger entry ${tipId}`,
      transform: `translate(${p.x - CARD_W / 2} ${p.y - CARD_H / 2})`,
    });

    group.appendChild(svg("title", {}, hoverName));

    group.appendChild(
      svg("rect", {
        class: "mesh__node-card",
        width: CARD_W,
        height: CARD_H,
        rx: 12,
        stroke: "var(--color-hairline)",
      }),
    );

    // Health strip sitting flush inside the rectangular box
    group.appendChild(
      svg("rect", {
        x: 4.5,
        y: 6,
        width: 3.5,
        height: CARD_H - 12,
        rx: 1.75,
        fill: STATUS_VAR[p.status],
      }),
    );

    // Pulse dot
    group.appendChild(
      svg("circle", { cx: 16, cy: 22, r: 4, fill: STATUS_VAR[p.status] }),
    );

    group.appendChild(
      svg("text", { class: "mesh__node-label", x: 26, y: 26 }, displayName),
    );

    group.appendChild(
      svg(
        "text",
        { class: "mesh__node-meta", x: 16, y: 45 },
        metaText,
      ),
    );

    const select = () => {
      store.select({ kind: "node", nodeId: p.node.process.node_id });
    };
    group.addEventListener("click", (e) => {
      e.stopPropagation();
      if (!draggedFar) {
        select();
      }
    });
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

  // Node details popover: full name, engine, location, size, purpose.
  // All strings flow through el() text nodes so mesh-supplied names are
  // escaped on render (AGENTS.md: never innerHTML with peer/user text).
  // Node details popover: full name, engine, location, data size, on disk, quota ceiling, purpose.
  // All strings flow through el() text nodes so mesh-supplied names are
  // escaped on render (AGENTS.md: never innerHTML with peer/user text).
  const renderPopover = (): void => {
    const sel = store.state.selection;
    if (sel.kind !== "node" || !sel.nodeId) {
      popover.hidden = true;
      replace(popover);
      return;
    }
    const node = placed.find((q) => q.node.process.node_id === sel.nodeId)?.node;
    if (!node) {
      popover.hidden = true;
      replace(popover);
      return;
    }
    armPopoverOutsideClick();
    const status = convergenceOf(node, meshTip());
    const name = displayNameOf(node);
    const collections = node.brain?.collections ?? [];
    const defaultEngine = node.status?.default_engine
      ?? collections[0]?.engine
      ?? "—";
    const engines = [...new Set(collections.map((c) => c.engine))];
    const totalDocs = collections.reduce((sum, c) => sum + c.document_count, 0);
    const diskBytes = diskUsageCache.get(node.process.node_id);
    const allocMode = allocationModeCache.get(node.process.node_id);
    const uptime = node.status ? duration(node.status.uptime_seconds) : "—";
    const cached = purposeCache.get(node.process.node_id);
    const purpose = cached === undefined ? "Loading…" : (cached || "No description given");

    const closeBtn = el("button", { class: "mesh__popover-close", type: "button", title: "Close details", "aria-label": "Close node details" }, icon(Icons.close, 11));
    on(closeBtn, "click", (e) => {
      e.stopPropagation();
      store.select({ kind: "none" });
    });
    const ledgerBtn = el("button", { class: "btn btn--sm btn--ghost", type: "button", text: "Ledger" });
    on(ledgerBtn, "click", (e) => {
      e.stopPropagation();
      store.select({ kind: "ledger", nodeId: node.process.node_id });
    });
    const consoleBtn = el("button", { class: "btn btn--sm btn--ghost", type: "button", text: "Console" });
    on(consoleBtn, "click", (e) => {
      e.stopPropagation();
      store.select({ kind: "console", nodeId: node.process.node_id });
    });
    const passphraseBtn = node.process.encrypted
      ? el("button", { class: "btn btn--sm btn--ghost", type: "button", title: "Change the passphrase or migrate this node to one (old secret stops working)", text: "Passphrase…" })
      : null;
    if (passphraseBtn) {
      on(passphraseBtn, "click", (e) => {
        e.stopPropagation();
        openChangePassphraseModal({ node_id: node.process.node_id, node_name: name });
      });
    }

    replace(popover,
      el("div", { class: "mesh__popover-head" },
        el("span", { class: "dot", "data-status": status }),
        el("strong", { class: "mesh__popover-title", title: name, text: truncate(name, 40) }),
        el("span", { class: "badge", "data-tone": status, text: STATUS_TEXT[status] }),
        closeBtn,
      ),
      el("dl", { class: "kv mesh__popover-kv" },
        el("dt", { text: "Engine" }),
        el("dd", { text: engines.length > 0 ? engines.map((e) => engineLabel(e)).join(", ") : engineLabel(defaultEngine) }),
        el("dt", { text: "Location" }),
        el("dd", { class: "mono mesh__popover-path", title: node.process.data_dir, text: truncate(node.process.data_dir, 42) }),
        el("dt", { text: "Data" }),
        el("dd", {
          title: "User records stored across collections",
          text: node.quota
            ? `${bytes(node.quota.used_bytes)} (${count(totalDocs)} docs)`
            : `${count(totalDocs)} docs`,
        }),
        el("dt", { text: "On Disk" }),
        el("dd", {
          title: "Physical folder size on disk (files, logs, and keys).",
          text: diskBytes !== undefined ? bytes(diskBytes) : "Reading…",
        }),
        el("dt", { text: "Quota Cap" }),
        el("dd", {
          title: "Configured safety ceiling: writes are refused past this limit to protect disk space.",
          text: node.quota ? bytes(node.quota.limit_bytes) : (node.brain ? `${node.brain.free_quota_mb} MiB` : "2.0 GiB"),
        }),
        el("dt", { text: "Allocation" }),
        el("dd", {
          title: allocMode === "reserved"
            ? "Steam-style upfront reservation: quota space is physically allocated on disk with storage.reserved to guarantee headroom."
            : "Dynamic allocation: disk space grows on demand as records are written up to quota cap.",
          text: allocMode === "reserved" ? "Reserved" : (allocMode === "dynamic" ? "Dynamic" : "Detecting…"),
        }),
        el("dt", { text: "Purpose" }),
        el("dd", {
          class: ((cached ? "" : "muted ") + "mesh__popover-purpose").trim(),
          title: purpose,
          text: purpose,
        }),
        el("dt", { text: "Ledger" }),
        el("dd", { class: "mono", text: `#${(node.tip?.entry_id ?? node.brain?.ledger_tip.entry_id ?? 1)} · ${shortHash(node.tip?.entry_hash ?? node.brain?.ledger_tip.entry_hash, 6, 0)}` }),
        el("dt", { text: "Uptime" }),
        el("dd", { text: `${uptime} · ${node.peers.length} peer${node.peers.length === 1 ? "" : "s"}` }),
      ),
      collections.length > 0
        ? el("div", { class: "mesh__popover-cols" },
          ...collections.slice(0, 6).map((c) =>
            el("span", { class: "chip", title: `${c.document_count} documents`, text: `${truncate(c.name, 18)} · ${engineLabel(c.engine)}` }),
          ),
        )
        : null,
      el("div", { class: "mesh__popover-actions" }, ledgerBtn, consoleBtn, passphraseBtn),
    );
    schedulePositionPopover();

    // Lazy inspection: fetch manifest purpose, real directory size on disk, and allocation mode.
    if (cached === undefined || diskBytes === undefined || allocMode === undefined) {
      if (cached === undefined) purposeCache.set(node.process.node_id, null);
      const port = store.state.supervisorPort;
      if (port !== null) {
        apiFor(port).inspect(node.process.data_dir).then((candidate) => {
          const desc = candidate.description;
          purposeCache.set(node.process.node_id, typeof desc === "string" && desc.trim() !== "" ? desc : null);
          if (typeof candidate.used_bytes === "number") {
            diskUsageCache.set(node.process.node_id, candidate.used_bytes);
          }
          if (typeof candidate.preallocated === "boolean") {
            allocationModeCache.set(node.process.node_id, candidate.preallocated ? "reserved" : "dynamic");
          } else {
            allocationModeCache.set(node.process.node_id, "dynamic");
          }
          if (store.state.selection.kind === "node" && store.state.selection.nodeId === node.process.node_id) {
            renderPopover();
          }
        }).catch(() => {
          purposeCache.set(node.process.node_id, null);
          allocationModeCache.set(node.process.node_id, "dynamic");
        });
      }
    }
  };
  renderPopover();
  // Escape dismisses without a full re-render round-trip.
  popover.addEventListener("keydown", (e) => {
    if ((e as KeyboardEvent).key === "Escape") {
      e.stopPropagation();
      store.select({ kind: "none" });
    }
  });

  // Floating Controls (zoom slider flanked by magnifying - / +, zoom pill, reset, layout toggle)
  const zoomPill = el("button", {
    class: "btn btn--sm btn--ghost mesh__zoom-pill",
    type: "button",
    title: "Zoom level — click to reset to 100%",
    "aria-label": `Zoom level ${Math.round(zoomScale * 100)} percent. Activate to reset.`,
    "aria-live": "polite",
    text: `${Math.round(zoomScale * 100)}%`,
  });
  on(zoomPill, "click", () => {
    panX = 0;
    panY = 0;
    zoomScale = 1.0;
    updateTransform();
  });

  const zoomSlider = el("input", {
    type: "range",
    class: "mesh__zoom-slider",
    min: String(Math.round(ZOOM_MIN * 100)),
    max: String(Math.round(ZOOM_MAX * 100)),
    value: String(Math.round(zoomScale * 100)),
    title: "Zoom slider",
    "aria-label": "Mesh zoom slider",
  }) as HTMLInputElement;

  on(zoomSlider, "input", () => {
    const val = Number(zoomSlider.value);
    if (Number.isFinite(val) && val > 0) {
      zoomScale = Math.max(ZOOM_MIN, Math.min(ZOOM_MAX, val / 100));
      updateTransform();
    }
  });

  const zoomOutBtn = el(
    "button",
    { class: "mesh__zoom-btn", type: "button", title: "Zoom out", "aria-label": "Zoom out" },
    icon(Icons.zoomOut, 14),
  );
  on(zoomOutBtn, "click", () => {
    zoomScale = Math.max(ZOOM_MIN, zoomScale * 0.85);
    updateTransform();
  });

  const zoomInBtn = el(
    "button",
    { class: "mesh__zoom-btn", type: "button", title: "Zoom in", "aria-label": "Zoom in" },
    icon(Icons.zoomIn, 14),
  );
  on(zoomInBtn, "click", () => {
    zoomScale = Math.min(ZOOM_MAX, zoomScale * 1.15);
    updateTransform();
  });

  const zoomGroup = el("div", { class: "mesh__zoom-group" }, zoomOutBtn, zoomSlider, zoomInBtn);

  const resetBtn = el("button", { class: "btn btn--sm btn--ghost", type: "button", title: "Reset View", "aria-label": "Reset View" }, icon(Icons.zoomReset, 14));
  on(resetBtn, "click", () => {
    panX = 0;
    panY = 0;
    zoomScale = 1.0;
    updateTransform();
  });

  const layoutBtn = el(
    "button",
    {
      class: "btn btn--sm " + (meshPlacementMode === "hierarchical" ? "btn--primary" : "btn--ghost"),
      type: "button",
      title: meshPlacementMode === "hierarchical" ? "Hierarchical layout active (click for radial)" : "Radial layout active (click for hierarchical)",
    },
    icon(meshPlacementMode === "hierarchical" ? Icons.tree : Icons.mesh, 13),
    el("span", { style: "font-size: 11px; margin-left: 4px;", text: meshPlacementMode === "hierarchical" ? "Hierarchy" : "Radial" }),
  );
  on(layoutBtn, "click", () => {
    meshPlacementMode = meshPlacementMode === "hierarchical" ? "concentric" : "hierarchical";
    store.notify();
  });

  const controls = el("div", { class: "canvas__controls" }, layoutBtn, zoomGroup, zoomPill, resetBtn);

  wrapper.appendChild(controls);
  wrapper.appendChild(minimap);

  (wrapper as HTMLElement & { __positionPopover?: () => void }).__positionPopover = schedulePositionPopover;

  try {
    const ro = new ResizeObserver(() => {
      positionPopover();
    });
    ro.observe(wrapper);
  } catch {
    // ResizeObserver unavailable
  }

  return wrapper;
}

function treeView(nodes: NodeView[]): HTMLElement {
  const tip = meshTip();
  const selectedId = store.state.selection.nodeId;
  const wrapper = el("div", { class: "tree-canvas-wrapper", style: "position: relative; height: 100%; overflow-y: auto; padding: var(--space-md);" });

  const treeRoot = el("div", { class: "hierarchy-tree", style: "max-width: 980px; margin: 0 auto;" });

  const rootHeader = el(
    "div",
    { class: "hierarchy-root-header", style: "display: flex; align-items: center; justify-content: space-between; padding-bottom: 12px; border-bottom: 1px solid var(--color-hairline); margin-bottom: 16px;" },
    el("div", { class: "row", style: "gap: 8px; align-items: center;" },
      icon(Icons.mesh, 16),
      el("strong", { style: "font-size: 15px;", text: "De-Sentry Hierarchical Topology" }),
      el("span", { class: "badge", text: `${nodes.length} node${nodes.length === 1 ? "" : "s"} online` }),
    ),
    el("span", { class: "muted", style: "font-size: 12px;", text: "Tiered View: Root → Storage Media → Nodes → Collections" }),
  );
  treeRoot.appendChild(rootHeader);

  // Group nodes by Medium: Local Machine vs Removable Storage
  const byGroup = new Map<string, { label: string; kind: string; nodes: NodeView[] }>();
  for (const node of nodes) {
    const isRemovable = node.process.removable;
    const key = isRemovable ? "Removable Media" : "Host Device";
    let grp = byGroup.get(key);
    if (!grp) {
      grp = { label: key, kind: isRemovable ? "removable" : "local", nodes: [] };
      byGroup.set(key, grp);
    }
    grp.nodes.push(node);
  }

  const branches = el("div", { class: "hierarchy-branches", style: "margin-left: 8px; border-left: 2px solid var(--color-hairline); padding-left: 16px; display: flex; flex-direction: column; gap: 20px;" });

  for (const [groupName, groupData] of byGroup.entries()) {
    const groupBranch = el("div", { class: "hierarchy-group-branch" });
    const groupLabel = el(
      "div",
      { class: "row", style: "gap: 8px; align-items: center; margin-bottom: 12px;" },
      el("span", { style: "color: var(--color-ink-muted-48); font-family: var(--font-mono);" }, "├─"),
      icon(groupData.kind === "removable" ? Icons.drive : Icons.folder, 14),
      el("strong", { style: "font-size: 13px; color: var(--color-ink-muted-80);", text: groupName }),
      el("span", { class: "badge", text: `${groupData.nodes.length} nodes` }),
    );
    groupBranch.appendChild(groupLabel);

    const nodesGrid = el("div", { class: "hierarchy-nodes-grid", style: "display: grid; grid-template-columns: repeat(auto-fill, minmax(320px, 1fr)); gap: 14px; margin-left: 20px; border-left: 1px dashed rgba(255,255,255,0.12); padding-left: 14px;" });

    for (const node of groupData.nodes) {
      const status = convergenceOf(node, tip);
      const selected = selectedId === node.process.node_id;
      const rawTipId = node.tip?.entry_id ?? node.brain?.ledger_tip.entry_id ?? 0;
      const tipId = rawTipId < 1 ? 1 : rawTipId;
      const behind = tip !== null ? tip.entry_id - tipId : 0;

      const collections = node.brain?.collections ?? [];
      const totalDocs = collections.reduce((sum, c) => sum + c.document_count, 0);
      const diskBytes = diskUsageCache.get(node.process.node_id);
      const diskNote = diskBytes !== undefined ? ` · ${bytes(diskBytes)} on disk` : "";
      const storageSummary = node.quota
        ? `${count(totalDocs)} docs · ${bytes(node.quota.used_bytes)} stored of ${bytes(node.quota.limit_bytes)} limit${diskNote}`
        : node.brain
          ? `${count(totalDocs)} docs · ${node.brain.free_quota_mb} MiB free${diskNote}`
          : `${count(totalDocs)} docs${diskNote}`;

      const card = el(
        "article",
        {
          class: selected ? "card card--elevated hierarchy-card" : "card hierarchy-card",
          tabindex: "0",
          role: "button",
          style: "border: 1px solid " + (selected ? "var(--color-primary)" : "var(--color-hairline)") + "; background: var(--chrome-fill);",
        },
        el(
          "div",
          { class: "row row--between", style: "align-items: center;" },
          el(
            "div",
            { class: "row", style: "align-items: center; gap: 8px;" },
            el("span", { class: "dot", "data-status": status }),
            el("h3", { class: "card__title", style: "margin: 0;", text: displayNameOf(node), title: rawNameOf(node) }),
          ),
          el(
            "div",
            { class: "row" },
            el("span", { class: "badge", "data-tone": status, text: STATUS_TEXT[status] }),
          ),
        ),
        el(
          "dl",
          { class: "kv", style: "margin-top: 10px;" },
          el("dt", { text: "Location" }),
          el("dd", { class: "mono", style: "font-size: 11px; word-break: break-all;", text: truncate(node.process.data_dir, 38) }),
          el("dt", { text: "Ledger" }),
          el("dd", { class: "mono", text: `#${tipId}${behind > 0 ? ` (${behind} behind)` : ""}` }),
          el("dt", { text: "Storage" }),
          el("dd", { text: storageSummary }),
        ),
        collections.length > 0 &&
          el(
            "div",
            { style: "margin-top: 10px; padding-top: 8px; border-top: 1px solid var(--color-hairline);" },
            el("span", { class: "muted", style: "font-size: 11px; display: block; margin-bottom: 6px;", text: "Collections:" }),
            el(
              "div",
              { class: "row", style: "flex-wrap: wrap; gap: 6px;" },
              ...collections.map((c) =>
                el("span", {
                  class: "chip",
                  title: `${c.document_count} documents · checksum ${shortHash(c.checksum)}`,
                  text: `${c.name} · ${engineLabel(c.engine)}`,
                }),
              ),
            ),
          ),
      );

      const select = () => {
        store.select({ kind: "node", nodeId: node.process.node_id });
      };
      on(card, "click", (e) => {
        e.stopPropagation();
        select();
      });
      on(card, "keydown", (event) => {
        if (event.key === "Enter" || event.key === " ") {
          event.preventDefault();
          select();
        }
      });
      nodesGrid.appendChild(card);
    }

    groupBranch.appendChild(nodesGrid);
    branches.appendChild(groupBranch);
  }

  treeRoot.appendChild(branches);
  wrapper.appendChild(treeRoot);
  return wrapper;
}

function discoveredCanvas(candidates: DiscoveredCandidate[], onNewNode: () => void): HTMLElement {
  const wrapper = el("div", {
    style: "width: 100%; height: 100%; display: flex; align-items: center; justify-content: center; padding: var(--space-lg); overflow-y: auto;",
  });

  const card = el("div", {
    class: "card card--elevated",
    style: "max-width: 580px; width: 100%; padding: var(--space-lg); background: var(--chrome-fill); border: 1px solid var(--color-hairline); box-shadow: var(--elev-popover);",
  });

  const header = el(
    "div",
    { style: "margin-bottom: var(--space-md);" },
    el(
      "div",
      { class: "row", style: "gap: 10px; align-items: center; margin-bottom: 6px;" },
      el("span", { style: "color: var(--color-primary); display: flex;" }, icon(Icons.folder, 22)),
      el("h2", { style: "margin: 0; font-size: 18px;", text: "Discovered Nodes on this Device" }),
      el("span", { class: "badge", "data-tone": "converged", text: `${candidates.length} found` }),
    ),
    el("p", {
      class: "muted",
      style: "font-size: 13px; line-height: 1.5; margin: 0;",
      text: "Unmanaged or encrypted database directories were detected. Unlock or adopt a node to start the mesh, or ignite a brand new node.",
    }),
  );

  const list = el("div", { class: "stack", style: "gap: 10px; margin-bottom: var(--space-md); max-height: 320px; overflow-y: auto;" });

  for (const c of candidates) {
    const isEncrypted = c.encrypted;
    const name = c.path.split(/[\\/]/).filter(Boolean).pop() || "node";

    const unlockBtn = el(
      "button",
      {
        class: "btn btn--sm " + (isEncrypted ? "btn--primary" : "btn--secondary"),
        type: "button",
      },
      icon(isEncrypted ? Icons.lock : Icons.plus, 12),
      el("span", { style: "margin-left: 4px;", text: isEncrypted ? "Unlock Node" : "Adopt Node" }),
    );

    on(unlockBtn, "click", async (e) => {
      e.stopPropagation();
      if (isEncrypted) {
        openUnlockModal({ node_id: c.node_id, node_name: name, data_dir: c.path });
      } else {
        unlockBtn.setAttribute("disabled", "true");
        try {
          await sidecar.startExistingNode(c.path);
          store.dismissCandidate(c.path);
          await refreshNodeList();
          await refreshTopology();
          store.toast("success", "Node adopted", name);
        } catch (err) {
          const msg = err instanceof Error ? err.message : String(err);
          store.toast("error", `Could not adopt ${name}`, msg);
          unlockBtn.removeAttribute("disabled");
        }
      }
    });

    const item = el(
      "div",
      {
        class: "card",
        style: "display: flex; align-items: center; justify-content: space-between; padding: 12px 14px; background: var(--color-surface-pearl); border: 1px solid var(--color-hairline);",
      },
      el(
        "div",
        { class: "stack", style: "gap: 4px; min-width: 0;" },
        el(
          "div",
          { class: "row", style: "gap: 8px; align-items: center;" },
          el("strong", { style: "font-size: 13px;", text: name }),
          isEncrypted ? el("span", { class: "badge", "data-tone": "lagging", text: "encrypted" }) : null,
          c.removable ? el("span", { class: "badge", text: "removable" }) : null,
        ),
        el("span", { class: "mono muted", style: "font-size: 11px; word-break: break-all;", text: c.path }),
      ),
      el("div", { style: "margin-left: 12px; flex-shrink: 0;" }, unlockBtn),
    );

    list.appendChild(item);
  }

  const newBtn = el("button", { class: "btn btn--sm btn--ghost", type: "button", text: "Ignite new node instead" });
  on(newBtn, "click", onNewNode);

  const footer = el(
    "div",
    {
      class: "row row--between",
      style: "align-items: center; border-top: 1px solid var(--color-hairline); padding-top: 14px;",
    },
    el("span", { class: "muted", style: "font-size: 12px;", text: "Need a new, empty storage location?" }),
    newBtn,
  );

  card.appendChild(header);
  card.appendChild(list);
  card.appendChild(footer);
  wrapper.appendChild(card);
  return wrapper;
}

function emptyCanvas(onNewNode: () => void): HTMLElement {
  const button = el("button", { class: "btn btn--primary", type: "button", text: "Ignite first node" });
  on(button, "click", onNewNode);
  return emptyState({
    title: "This sector is dark",
    body: "No nodes yet — a node is a place to keep data, a folder on this machine or a drive you can carry. Ignite one and its star appears here, along with every peer it finds.",
    actions: [button],
  });
}

export interface CanvasHandles {
  render(): void;
  element: HTMLElement;
}

export function createCanvas(onNewNode: () => void): CanvasHandles {
  const body = el("div", { style: "width: 100%; height: 100%; position: relative; z-index: 1;" });
  const element = el("main", { class: "canvas", style: "position: relative; overflow: hidden;" }, body);

  function render(): void {
    const nodes = store.dataNodes();
    const mode = store.state.canvasMode;
    const candidates = store.state.discoveredCandidates;

    if (nodes.length === 0) {
      if (candidates.length > 0) {
        replace(body, discoveredCanvas(candidates, onNewNode));
      } else {
        replace(body, emptyCanvas(onNewNode));
      }
      return;
    }

    if (mode === "mesh") {
      const side = Math.max(760, 260 + Math.ceil(Math.sqrt(nodes.length)) * 200);
      // Supervisors ride along as hub-only entries: never data, never a
      // replication hop, but the honest anchor for supervision spokes so no
      // node floats unconnected. Tree intentionally stays data-only.
      const hubs = [...store.state.nodes.values()].filter((n) => n.process.supervisor);
      const view = meshView([...nodes, ...hubs], side, Math.round(side * 0.65), element);
      replace(body, view);
      (view as HTMLElement & { __positionPopover?: () => void }).__positionPopover?.();
    } else {
      replace(body, treeView(nodes));
    }
  }

  return { render, element };
}

export function statusText(status: Convergence): string {
  return STATUS_TEXT[status];
}

/** Live-update the mounted viewport transform, if a mesh is on screen. */
function pushZoomToDom(): void {
  try {
    const vp = document.querySelector(".mesh__viewport");
    vp?.setAttribute("transform", `translate(${panX} ${panY}) scale(${zoomScale})`);
    const host = document.querySelector(".mesh-zoom-host");
    host?.classList.toggle("mesh-canvas--compact", zoomScale < 0.6);
    host?.classList.toggle("mesh-canvas--surface", zoomScale > 1.6);
    const slider = document.querySelector<HTMLInputElement>(".mesh__zoom-slider");
    if (slider) slider.value = String(Math.round(zoomScale * 100));
    const pill = document.querySelector(".mesh__zoom-pill");
    if (pill) pill.textContent = `${Math.round(zoomScale * 100)}%`;
  } catch {
    // No mesh mounted: module vars still apply on next render.
  }
}

/** Global keyboard parity: +/- step, Ctrl/⌘+0 fit (wired in main.ts). */
export function canvasZoomStep(factor: number): void {
  zoomScale = Math.max(ZOOM_MIN, Math.min(ZOOM_MAX, zoomScale * factor));
  pushZoomToDom();
}

export function canvasZoomFit(): void {
  panX = 0;
  panY = 0;
  zoomScale = 1.0;
  pushZoomToDom();
}
