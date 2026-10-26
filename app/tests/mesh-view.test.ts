import { describe, it, expect, beforeEach, vi } from "vitest";

// ---- Mock Tauri API so the import chain (bridge.ts -> @tauri-apps/api) loads in jsdom ----
vi.mock("@tauri-apps/api/core", () => ({
  invoke: vi.fn().mockResolvedValue(undefined),
}));
vi.mock("@tauri-apps/plugin-updater", () => ({
  check: vi.fn().mockResolvedValue(undefined),
}));

// ---- Mock ../api.js so apiFor() returns a safe stub ----
const mockInspect = vi.fn().mockResolvedValue({});
vi.mock("../api.js", () => ({
  apiFor: vi.fn(() => ({
    inspect: mockInspect,
    ping: vi.fn().mockResolvedValue(undefined),
    status: vi.fn().mockResolvedValue({}),
    brain: vi.fn().mockResolvedValue({ collections: [], known_peers: [], ledger_tip: { entry_id: 1, entry_hash: "0", signature: "" } }),
    quota: vi.fn().mockResolvedValue(null),
    changes: vi.fn().mockResolvedValue({ tip: 0, count: 0, changes: [], truncated: false }),
    peers: vi.fn().mockResolvedValue([]),
    peerSync: vi.fn().mockResolvedValue([]),
    collection: vi.fn().mockResolvedValue(null),
    engines: vi.fn().mockResolvedValue([]),
    topology: vi.fn().mockResolvedValue({}),
  })),
}));

// ---- Mock ../bridge.js so sidecar is a safe stub ----
vi.mock("../bridge.js", () => ({
  sidecar: {
    listNodes: vi.fn().mockResolvedValue([]),
    appInfo: vi.fn().mockResolvedValue({ on_battery: false }),
    scanForNodes: vi.fn().mockResolvedValue([]),
    startExistingNode: vi.fn().mockResolvedValue({ node_id: "mock", node_name: "mock", api_port: 7701, data_dir: "/mock", process: "running", supervisor: false }),
    syncStorageReservation: vi.fn().mockResolvedValue(undefined),
  },
  onSidecarEvent: vi.fn(),
  isTauri: vi.fn().mockReturnValue(false),
  NoSidecarError: class NoSidecarError extends Error {},
}));

import { layout, edgesOf, meshView } from "../src/views/canvas.js";
import { store, type NodeView } from "../src/state.js";

// ---- Helpers ----
function makeProcess(overrides: Partial<{
  node_id: string;
  node_name: string;
  data_dir: string;
  api_port: number;
  supervisor: boolean;
  process: string;
  mesh_isolated: boolean;
  removable: boolean;
  encrypted: boolean;
}> = {}): any {
  return {
    node_id: overrides.node_id ?? `node-${Math.random().toString(36).slice(2, 7)}`,
    node_name: overrides.node_name ?? "Mock Node",
    data_dir: overrides.data_dir ?? `/tmp/mock-node`,
    api_port: overrides.api_port ?? 7701,
    supervisor: overrides.supervisor ?? false,
    process: overrides.process ?? "running",
    mesh_isolated: overrides.mesh_isolated ?? false,
    removable: overrides.removable ?? false,
    encrypted: overrides.encrypted ?? false,
    last_error: "",
    last_exit_code: 0,
  };
}

function makeNode(overrides: Partial<{
  process: any;
  reachable: boolean;
  status: any;
  brain: any;
  tip: any;
  quota: any;
  peers: any[];
  peerSyncSummaries: any[];
}> = {}): NodeView {
  const proc = overrides.process ?? makeProcess();
  return {
    process: proc,
    reachable: overrides.reachable ?? true,
    status: overrides.status ?? { default_engine: "kv", uptime_seconds: 100, public_key_hex: "abc" },
    brain: overrides.brain ?? { collections: [], free_quota_mb: 2048, known_peers: [], ledger_tip: { entry_id: 1, entry_hash: "0", signature: "" } },
    tip: overrides.tip ?? { node_id: proc.node_id, entry_id: 1, entry_hash: "0", signature: "", public_key_hex: "" },
    quota: overrides.quota ?? null,
    peers: overrides.peers ?? [],
    peerSyncSummaries: overrides.peerSyncSummaries ?? [],
    lastVerify: null,
    lastVerifyMs: 0,
    changeCursor: -1,
    resyncs: 0,
    firstSeenMs: Date.now(),
    lastUpdatedMs: Date.now(),
  } as NodeView;
}

function makePeer(nodeId: string, overrides: Partial<{
  node_id: string;
  state: string;
  last_seen_ms: number;
  handshake_proven: boolean;
  suspicion: string;
  supervisor: boolean;
  fitness: { score: number; latency_ms: number; success_rate: number } | null;
}> = {}): any {
  return {
    node_id: overrides.node_id ?? nodeId,
    state: overrides.state ?? "running",
    last_seen_ms: overrides.last_seen_ms ?? Date.now(),
    handshake_proven: overrides.handshake_proven ?? true,
    suspicion: overrides.suspicion ?? "healthy",
    supervisor: overrides.supervisor ?? false,
    fitness: overrides.fitness ?? { score: 0.9, latency_ms: 10, success_rate: 1.0 },
  };
}

// ---- Test setup ----
beforeEach(() => {
  // Reset the store to a clean state
  store.state.nodes.clear();
  store.state.selection = { kind: "none" };
  store.state.supervisorPort = null;
  vi.clearAllMocks();
});

// ============================================================================
// layout() tests
// ============================================================================

describe("layout() — multi-node positioning", () => {
  it("places a single node at the centre", () => {
    const node = makeNode({ process: makeProcess({ node_id: "node-1" }) });
    const placed = layout([node], 800, 600);
    expect(placed).toHaveLength(1);
    expect(placed[0].x).toBe(400);
    expect(placed[0].y).toBe(300);
  });

  it("places multiple nodes without overlap (radial mode)", () => {
    const nodes = Array.from({ length: 12 }, (_, i) =>
      makeNode({ process: makeProcess({ node_id: `node-${i}` }) }),
    );
    const placed = layout(nodes, 1000, 700);

    expect(placed).toHaveLength(12);

    // No two nodes closer than MIN_DIST (40px)
    for (let i = 0; i < placed.length; i++) {
      for (let j = i + 1; j < placed.length; j++) {
        const dx = placed[i].x - placed[j].x;
        const dy = placed[i].y - placed[j].y;
        const dist = Math.sqrt(dx * dx + dy * dy);
        expect(dist).toBeGreaterThanOrEqual(39); // allow 1px rounding tolerance
      }
    }

    // All nodes within viewport bounds (with card half-size margin)
    const CARD_W = 200;
    const CARD_H = 60;
    for (const p of placed) {
      expect(p.x).toBeGreaterThanOrEqual(CARD_W / 2 - 10);
      expect(p.x).toBeLessThanOrEqual(1000 - CARD_W / 2 + 10);
      expect(p.y).toBeGreaterThanOrEqual(CARD_H / 2 - 10);
      expect(p.y).toBeLessThanOrEqual(700 - CARD_H / 2 + 10);
    }
  });

  it("places multiple nodes without overlap (hierarchical mode)", () => {
    // Force hierarchical mode by making node 0 a supervisor with peers
    const nodes = Array.from({ length: 8 }, (_, i) =>
      makeNode({ process: makeProcess({ node_id: `node-${i}`, supervisor: i === 0 }) }),
    );
    // Make node 0 a supervisor so hierarchical mode picks it as primary
    nodes[0].process.supervisor = true;
    // Give the supervisor peers to other nodes
    nodes[0].process.peers = nodes.slice(1).map((n) => makePeer(n.process.node_id));

    const placed = layout(nodes, 1000, 700);

    expect(placed).toHaveLength(8);

    for (let i = 0; i < placed.length; i++) {
      for (let j = i + 1; j < placed.length; j++) {
        const dx = placed[i].x - placed[j].x;
        const dy = placed[i].y - placed[j].y;
        const dist = Math.sqrt(dx * dx + dy * dy);
        expect(dist).toBeGreaterThanOrEqual(39);
      }
    }
  });

  it("keeps nodes centred around the canvas midpoint", () => {
    const nodes = Array.from({ length: 20 }, (_, i) =>
      makeNode({ process: makeProcess({ node_id: `node-${i}` }) }),
    );
    const placed = layout(nodes, 1200, 800);

    const cx = 600;
    const cy = 400;
    const avgX = placed.reduce((sum, p) => sum + p.x, 0) / placed.length;
    const avgY = placed.reduce((sum, p) => sum + p.y, 0) / placed.length;

    // The constellation should be roughly centred (within 15% of viewport)
    expect(Math.abs(avgX - cx)).toBeLessThan(180);
    expect(Math.abs(avgY - cy)).toBeLessThan(120);
  });
});

// ============================================================================
// edgesOf() tests
// ============================================================================

describe("edgesOf() — edge consistency", () => {
  it("creates edges for peer relationships", () => {
    const nodeA = makeNode({ process: makeProcess({ node_id: "node-a" }) });
    const nodeB = makeNode({ process: makeProcess({ node_id: "node-b" }) });
    const nodeC = makeNode({ process: makeProcess({ node_id: "node-c" }) });

    // A peers with B and C
    nodeA.peers = [makePeer("node-b"), makePeer("node-c")];
    // B peers with A
    nodeB.peers = [makePeer("node-a")];
    // C peers with A
    nodeC.peers = [makePeer("node-a")];

    store.state.nodes.set(nodeA.process.node_id, nodeA);
    store.state.nodes.set(nodeB.process.node_id, nodeB);
    store.state.nodes.set(nodeC.process.node_id, nodeC);

    const placed = layout([nodeA, nodeB, nodeC], 800, 600);
    const edges = edgesOf(placed);

    // Should have 2 unique edges: A-B and A-C (deduplicated)
    expect(edges.length).toBe(2);

    const edgePairs = edges.map((e) => [e.a.node.process.node_id, e.b.node.process.node_id].sort());
    expect(edgePairs).toContainEqual(["node-a", "node-b"]);
    expect(edgePairs).toContainEqual(["node-a", "node-c"]);
  });

  it("does not create edges for unknown peer ids", () => {
    const nodeA = makeNode({ process: makeProcess({ node_id: "node-a" }) });
    // A claims to peer with a node that doesn't exist in the layout
    nodeA.peers = [makePeer("nonexistent-node")];

    store.state.nodes.set(nodeA.process.node_id, nodeA);

    const placed = layout([nodeA], 800, 600);
    const edges = edgesOf(placed);

    expect(edges).toHaveLength(0);
  });
});

// ============================================================================
// meshView() DOM tests
// ============================================================================

describe("meshView() — multi-node DOM rendering", () => {
  it("renders node cards and minimap dots for N nodes", () => {
    const nodes = Array.from({ length: 6 }, (_, i) =>
      makeNode({ process: makeProcess({ node_id: `node-${i}` }) }),
    );
    // Add peer links so edges are rendered
    nodes[0].peers = [makePeer(nodes[1].process.node_id), makePeer(nodes[2].process.node_id)];
    nodes[1].peers = [makePeer(nodes[0].process.node_id)];
    nodes[2].peers = [makePeer(nodes[0].process.node_id)];

    for (const n of nodes) store.state.nodes.set(n.process.node_id, n);

    const container = document.createElement("div");
    container.style.width = "1000px";
    container.style.height = "700px";
    document.body.appendChild(container);

    const view = meshView(nodes, 1000, 700, container);
    container.appendChild(view);

    // Node cards
    const cards = container.querySelectorAll(".mesh__node");
    expect(cards).toHaveLength(6);

    // Minimap dots
    const mmDots = container.querySelectorAll(".mesh-minimap__dot, .mesh-minimap__dot--selected");
    expect(mmDots).toHaveLength(6);

    // Minimap edges match main edges
    const mmEdges = container.querySelectorAll(".mesh-minimap__edge, .mesh-minimap__edge--stale");
    // 2 unique peer edges: node-0 ↔ node-1, node-0 ↔ node-2
    expect(mmEdges.length).toBeGreaterThanOrEqual(2);

    document.body.removeChild(container);
  });

  it("renders a supervisor dot that is visually distinct", () => {
    const sup = makeNode({ process: makeProcess({ node_id: "sup-1", supervisor: true }) });
    const data = makeNode({ process: makeProcess({ node_id: "data-1" }) });
    sup.peers = [makePeer(data.process.node_id)];
    data.peers = [makePeer(sup.process.node_id)];

    store.state.nodes.set(sup.process.node_id, sup);
    store.state.nodes.set(data.process.node_id, data);

    const container = document.createElement("div");
    container.style.width = "800px";
    container.style.height = "600px";
    document.body.appendChild(container);

    const view = meshView([data, sup], 800, 600, container);
    container.appendChild(view);

    const mmDots = container.querySelectorAll(".mesh-minimap__dot, .mesh-minimap__dot--selected");
    expect(mmDots).toHaveLength(2);

    // Supervisor dot should have a white stroke
    let supDot: Element | null = null;
    for (const d of mmDots) {
      const nid = d.getAttribute("data-node-id");
      if (nid === "sup-1") supDot = d;
    }
    expect(supDot).not.toBeNull();
    if (supDot) {
      const stroke = supDot.getAttribute("stroke");
      expect(stroke).toBe("#ffffff");
    }

    document.body.removeChild(container);
  });

  it("does not overlap node cards in a dense mesh", () => {
    const nodes = Array.from({ length: 30 }, (_, i) =>
      makeNode({ process: makeProcess({ node_id: `dense-${i}` }) }),
    );
    for (const n of nodes) store.state.nodes.set(n.process.node_id, n);

    const container = document.createElement("div");
    container.style.width = "1200px";
    container.style.height = "900px";
    document.body.appendChild(container);

    const view = meshView(nodes, 1200, 900, container);
    container.appendChild(view);

    const cards = container.querySelectorAll(".mesh__node");
    expect(cards).toHaveLength(30);

    // Verify via the layout function that no two nodes are too close
    // (jsdom doesn't run layout, so getBoundingClientRect is all zeros)
    const placed = layout(nodes, 1200, 900);
    const MIN_DIST = 40;
    for (let i = 0; i < placed.length; i++) {
      for (let j = i + 1; j < placed.length; j++) {
        const dx = placed[i].x - placed[j].x;
        const dy = placed[i].y - placed[j].y;
        const dist = Math.sqrt(dx * dx + dy * dy);
        expect(dist).toBeGreaterThanOrEqual(MIN_DIST - 1); // 1px rounding tolerance
      }
    }

    // Verify each card has a transform attribute positioning it
    for (const card of cards) {
      const transform = card.getAttribute("transform");
      expect(transform).toMatch(/translate\(/);
    }

    document.body.removeChild(container);
  });

  it("minimap dots match node positions", () => {
    const nodes = Array.from({ length: 5 }, (_, i) =>
      makeNode({ process: makeProcess({ node_id: `pos-${i}` }) }),
    );
    for (const n of nodes) store.state.nodes.set(n.process.node_id, n);

    const container = document.createElement("div");
    container.style.width = "800px";
    container.style.height = "600px";
    document.body.appendChild(container);

    const view = meshView(nodes, 800, 600, container);
    container.appendChild(view);

    const placed = layout(nodes, 800, 600);
    const mmDots = container.querySelectorAll(".mesh-minimap__dot, .mesh-minimap__dot--selected");
    expect(mmDots).toHaveLength(placed.length);

    // Each minimap dot should correspond to a placed node
    for (const p of placed) {
      const dot = container.querySelector(`.mesh-minimap__dot[data-node-id="${p.node.process.node_id}"], .mesh-minimap__dot--selected[data-node-id="${p.node.process.node_id}"]`);
      expect(dot).not.toBeNull();
      if (dot) {
        const cx = parseFloat(dot.getAttribute("cx") || "0");
        const cy = parseFloat(dot.getAttribute("cy") || "0");
        // Minimap uses same coordinate space as main view (viewBox = width x height)
        expect(cx).toBeCloseTo(p.x, 0);
        expect(cy).toBeCloseTo(p.y, 0);
      }
    }

    document.body.removeChild(container);
  });

  it("clicking a minimap dot selects the node", () => {
    const nodes = Array.from({ length: 3 }, (_, i) =>
      makeNode({ process: makeProcess({ node_id: `sel-${i}` }) }),
    );
    for (const n of nodes) store.state.nodes.set(n.process.node_id, n);

    const container = document.createElement("div");
    container.style.width = "800px";
    container.style.height = "600px";
    document.body.appendChild(container);

    const view = meshView(nodes, 800, 600, container);
    container.appendChild(view);

    // Click the first minimap dot
    const firstDot = container.querySelector('.mesh-minimap__dot[data-node-id="sel-0"], .mesh-minimap__dot--selected[data-node-id="sel-0"]');
    expect(firstDot).not.toBeNull();
    if (firstDot) {
      firstDot.dispatchEvent(new MouseEvent("click", { bubbles: true }));
    }

    expect(store.state.selection.kind).toBe("node");
    expect(store.state.selection.nodeId).toBe("sel-0");

    document.body.removeChild(container);
  });

  it("renders edge tooltips on hover", () => {
    const nodeA = makeNode({ process: makeProcess({ node_id: "tip-a" }) });
    const nodeB = makeNode({ process: makeProcess({ node_id: "tip-b" }) });
    nodeA.peers = [makePeer("tip-b")];
    nodeB.peers = [makePeer("tip-a")];

    store.state.nodes.set(nodeA.process.node_id, nodeA);
    store.state.nodes.set(nodeB.process.node_id, nodeB);

    const container = document.createElement("div");
    container.style.width = "800px";
    container.style.height = "600px";
    document.body.appendChild(container);

    const view = meshView([nodeA, nodeB], 800, 600, container);
    container.appendChild(view);

    const tooltip = container.querySelector(".mesh__tooltip");
    expect(tooltip).not.toBeNull();

    const edges = container.querySelectorAll(".mesh__edge");
    expect(edges.length).toBeGreaterThanOrEqual(1);

    if (edges.length > 0) {
      edges[0].dispatchEvent(new MouseEvent("mouseenter", { bubbles: true }));
      expect(tooltip?.getAttribute("hidden")).toBeNull(); // tooltip should be visible
      edges[0].dispatchEvent(new MouseEvent("mouseleave", { bubbles: true }));
      expect(tooltip?.getAttribute("hidden")).toBe(""); // tooltip should be hidden again
    }

    document.body.removeChild(container);
  });
});
