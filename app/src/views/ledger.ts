/**
 * Ledger viewer: a paginated, filterable table of ledger entries.
 *
 * Reads from GET /_ledger/entries?from=&to= (backfill) and GET /_changes?since=
 * (live tail). The ledger is per-node; this view shows one node's chain.
 * Transit entries have collection=owner_node_id — they are flagged and the real
 * collection is unknown from the ledger alone.
 */

import { apiFor, type LedgerEntry, type LedgerEntriesPage } from "../api.js";
import {
  ago,
  count,
  shortHash,
  shortNode,
} from "../util/format.js";
import { el, icon, Icons, on, replace, svg } from "../util/dom.js";
import { emptyState } from "../util/empty.js";
import { store, type NodeView } from "../state.js";

const MAX_ENTRIES_PER_CALL = 5000;

type FilterState = {
  operation: string;
  collection: string;
  key: string;
  originNode: string;
};

function describeError(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

function card(title: string, ...children: (Node | string | false | null | undefined)[]): HTMLElement {
  return el("section", { class: "card" }, el("h3", { class: "card__title", text: title }), ...children);
}


const OPERATION_LABELS: Record<string, string> = {
  PUT: "Write",
  DEL: "Delete",
  CHECKPOINT: "Checkpoint",
  TRANSIT_INTENT: "Transit intent",
  TRANSIT_CLAIMED: "Transit claimed",
};

const OPERATION_TONES: Record<string, string> = {
  PUT: "converged",
  DEL: "offline",
  CHECKPOINT: "lagging",
  TRANSIT_INTENT: "lagging",
  TRANSIT_CLAIMED: "converged",
};

/** Badge tone → status color var (same mapping the table badges use). */
const RIVER_TONE_VAR: Record<string, string> = {
  converged: "var(--color-status-converged)",
  lagging: "var(--color-status-lagging)",
  offline: "var(--color-status-offline)",
  supervisor: "var(--color-status-supervisor)",
};

const RIVER_LANE_ORDER = ["PUT", "DEL", "CHECKPOINT", "TRANSIT_INTENT", "TRANSIT_CLAIMED"];

function parseHLC(hlc: string): { date: Date | null; logical: number; nodeId: string } {
  if (!hlc) return { date: null, logical: 0, nodeId: "" };
  const match = hlc.match(/^(\d+)\.(\d+)@(.+)$/);
  if (!match) return { date: null, logical: 0, nodeId: hlc };
  const physicalMs = Number(match[1]);
  const logical = Number(match[2]);
  const nodeId = match[3];
  return {
    date: physicalMs > 0 ? new Date(physicalMs) : null,
    logical,
    nodeId,
  };
}

function formatHLC(hlc: string): string {
  const { date, logical, nodeId } = parseHLC(hlc);
  if (!date) return hlc || "—";
  return `${date.toLocaleString(undefined, {
    year: "numeric",
    month: "short",
    day: "numeric",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  })} · logical ${logical} · ${shortNode(nodeId)}`;
}

/** Relative timestamp for the table cell; full detail stays in `title`. */
function formatHLCRelative(hlc: string): { text: string; title: string } {
  const { date } = parseHLC(hlc);
  if (!date) return { text: hlc || "—", title: hlc || "" };
  return { text: ago(date.getTime()), title: formatHLC(hlc) };
}

function isTransitOp(op: string): boolean {
  return op === "TRANSIT_INTENT" || op === "TRANSIT_CLAIMED";
}

function renderEntryRow(
  entry: LedgerEntry,
  node: NodeView,
  _filter: FilterState,
  showHiddenCols: boolean,
): HTMLElement {
  const isTransit = isTransitOp(entry.operation);
  const opTone = OPERATION_TONES[entry.operation] || "converged";
  const opLabel = OPERATION_LABELS[entry.operation] || entry.operation;

  const originNodeName = node.peers.find((p) => p.node_id === entry.origin_node_id)?.hostname ||
    (node.brain?.known_peers.find((p) => p.node_id === entry.origin_node_id)?.hostname) ||
    shortNode(entry.origin_node_id);

  const collectionDisplay = isTransit
    ? `held for ${shortNode(entry.collection)}`
    : entry.collection;

  const hlc = formatHLCRelative(entry.hlc);
  const cells: HTMLElement[] = [
    el("td", { class: "mono", text: `#${entry.entry_id}` }),
    el("td", {}, el("span", { class: "badge", "data-tone": opTone, text: opLabel })),
    el("td", { class: "mono", text: collectionDisplay }),
    el("td", { class: "mono", text: entry.key || "—" }),
    el("td", { class: "mono", text: hlc.text, title: hlc.title }),
    el("td", { class: "mono", text: originNodeName, title: entry.origin_node_id }),
    el("td", { class: "mono", text: count(entry.document_bytes) }),
  ];

  if (showHiddenCols) {
    cells.push(
      el("td", { class: "mono", text: shortHash(entry.key_hash, 8, 4), title: entry.key_hash }),
      el("td", { class: "mono", text: shortHash(entry.prev_hash, 8, 4), title: entry.prev_hash }),
      el("td", { class: "mono", text: shortHash(entry.entry_hash, 8, 4), title: entry.entry_hash }),
      el("td", { class: "mono", text: entry.origin_signature ? shortHash(entry.origin_signature, 8, 4) : "—", title: entry.origin_signature || "unsigned" }),
    );
  }

  return el("tr", { tabindex: "0" }, ...cells);
}

function skeletonRow(colCount: number): HTMLElement {
  return el(
    "tr",
    {},
    ...Array.from({ length: colCount }, () =>
      el("td", {}, el("div", { class: "skeleton", style: "height: 14px; width: 80%;" })),
    ),
  );
}

export interface LedgerHandles {
  render(): void;
  element: HTMLElement;
}

export function createLedger(): LedgerHandles {
  // Outer element — the full-height main frame. Has padding so cards breathe.
  const element = el("main", { class: "canvas ledger-view" });

  // ── Stable header ───────────────────────────────────────────────────────────
  // Filter bar and view-toggle live here and are NEVER destroyed/replaced.
  // Only the input values are updated, preventing layout jumps from typing.
  const filterAreaEl = el("div", { class: "ledger__filter-area" });
  const viewToggleRow = el("div", {
    class: "row row--between ledger__view-row",
  });

  // ── Scrollable data panel ────────────────────────────────────────────────────
  // Only this part swaps out on each async load.
  const dataPanel = el("div", { class: "ledger__data" });

  const inner = el("div", { class: "ledger__inner" },
    filterAreaEl,
    viewToggleRow,
    dataPanel,
  );
  element.appendChild(inner);

  let currentFrom = 0;
  let currentTip: number | null = null;
  let filter: FilterState = { operation: "", collection: "", key: "", originNode: "" };
  let showHiddenCols = false;
  let riverMode: "table" | "river" = "table";
  let selectedRiverId: number | null = null;
  let filterTimer = 0;

  // Track which node the filter bar was last built for so we only rebuild
  // it when the node changes (peers list could differ).
  let filterBarNodeId = "";

  function scheduleFilterApply(apply: () => void): void {
    window.clearTimeout(filterTimer);
    filterTimer = window.setTimeout(apply, 200);
  }

  /** Restore caret inside the still-mounted filter input after a filter change. */
  function restoreFilterFocus(field: string): void {
    try {
      const input = filterAreaEl.querySelector<HTMLInputElement>(`input[data-filter-field="${field}"]`);
      if (input) {
        input.focus();
        input.setSelectionRange(input.value.length, input.value.length);
      }
    } catch {
      // Focus restore is a nicety; a missing field must never break render.
    }
  }

  async function loadEntries(node: NodeView, from: number, to?: number): Promise<LedgerEntriesPage> {
    return apiFor(node.process.api_port).ledgerEntries(from, to);
  }

  /**
   * Build or update the filter bar in-place.
   * If node hasn't changed we only update `value` / `selected` on the existing
   * inputs — this avoids focus loss and layout jumps while typing.
   */
  function ensureFilterBar(node: NodeView): void {
    if (filterBarNodeId === node.process.node_id && filterAreaEl.firstChild) {
      // Node unchanged — update existing inputs without DOM teardown.
      const colInput = filterAreaEl.querySelector<HTMLInputElement>("input[data-filter-field=\"collection\"]");
      const keyInput = filterAreaEl.querySelector<HTMLInputElement>("input[data-filter-field=\"key\"]");
      if (colInput && colInput !== document.activeElement) colInput.value = filter.collection;
      if (keyInput && keyInput !== document.activeElement) keyInput.value = filter.key;
      return;
    }

    filterBarNodeId = node.process.node_id;

    const opOptions = ["", "PUT", "DEL", "CHECKPOINT", "TRANSIT_INTENT", "TRANSIT_CLAIMED"];
    const originOptions = ["", ...node.peers.map((p) => p.node_id)];

    const peerLabel = (id: string): string => {
      const peer = node.peers.find((p) => p.node_id === id)
        ?? node.brain?.known_peers.find((p) => p.node_id === id);
      const host = peer?.hostname || peer?.host || "";
      return host ? `${host} · ${shortNode(id, 8)}` : shortNode(id, 12);
    };
    const opSelect = el("select", { class: "ledger__filter-control", "aria-label": "Filter by operation" },
      ...opOptions.map((op) => el("option", { value: op, selected: filter.operation === op }, op ? (OPERATION_LABELS[op] ?? op) : "All operations")),
    ) as HTMLSelectElement;
    on(opSelect, "change", () => { filter.operation = opSelect.value; void render(); });

    const collectionInput = el("input", {
      type: "text",
      class: "ledger__filter-control",
      placeholder: "Filter collection…",
      value: filter.collection,
      "aria-label": "Filter by collection",
      "data-filter-field": "collection",
    }) as HTMLInputElement;
    on(collectionInput, "input", () => {
      const value = collectionInput.value;
      scheduleFilterApply(() => {
        filter.collection = value;
        void render().then(() => restoreFilterFocus("collection"));
      });
    });

    const keyInput = el("input", {
      type: "text",
      class: "ledger__filter-control",
      placeholder: "Filter key…",
      value: filter.key,
      "aria-label": "Filter by key",
      "data-filter-field": "key",
    }) as HTMLInputElement;
    on(keyInput, "input", () => {
      const value = keyInput.value;
      scheduleFilterApply(() => {
        filter.key = value;
        void render().then(() => restoreFilterFocus("key"));
      });
    });

    const originSelect = el("select", { class: "ledger__filter-control", "aria-label": "Filter by origin node" },
      ...originOptions.map((id) => {
        const label = id ? peerLabel(id) : "All origin nodes";
        const option = el("option", { value: id, selected: filter.originNode === id, title: id || undefined }, label);
        return option;
      }),
    ) as HTMLSelectElement;
    on(originSelect, "change", () => { filter.originNode = originSelect.value; void render(); });

    const toggleHidden = el("button", {
      class: "btn btn--sm btn--ghost", type: "button",
      "aria-pressed": String(showHiddenCols),
      title: "Toggle hidden columns (hashes, signatures)",
    }, icon(Icons.inspector, 13), " Hashes");
    on(toggleHidden, "click", () => {
      showHiddenCols = !showHiddenCols;
      toggleHidden.setAttribute("aria-pressed", String(showHiddenCols));
      void render();
    });

    const clearFilters = el("button", { class: "btn btn--sm btn--ghost", type: "button" }, icon(Icons.refresh, 13), " Clear");
    on(clearFilters, "click", () => {
      filter = { operation: "", collection: "", key: "", originNode: "" };
      if (collectionInput) collectionInput.value = "";
      if (keyInput) keyInput.value = "";
      if (opSelect) opSelect.value = "";
      if (originSelect) originSelect.value = "";
      void render();
    });

    const filterBar = el("div", { class: "card ledger__filter-card" },
      el("h3", { class: "card__title", text: "Filters" }),
      el("div", { class: "ledger__filter-grid" },
        el("div", { class: "ledger__filter-item" },
          el("label", { class: "ledger__filter-label", for: "ledger-filter-op", text: "Operation" }),
          opSelect,
        ),
        el("div", { class: "ledger__filter-item" },
          el("label", { class: "ledger__filter-label", for: "ledger-filter-collection", text: "Collection" }),
          collectionInput,
        ),
        el("div", { class: "ledger__filter-item" },
          el("label", { class: "ledger__filter-label", for: "ledger-filter-key", text: "Key" }),
          keyInput,
        ),
        el("div", { class: "ledger__filter-item" },
          el("label", { class: "ledger__filter-label", for: "ledger-filter-origin", text: "Origin" }),
          originSelect,
        ),
        el("div", { class: "ledger__filter-actions" },
          toggleHidden,
          clearFilters,
        ),
      ),
    );
    opSelect.id = "ledger-filter-op";
    collectionInput.id = "ledger-filter-collection";
    keyInput.id = "ledger-filter-key";
    originSelect.id = "ledger-filter-origin";
    replace(filterAreaEl, filterBar);
  }

  function ensureViewToggle(): void {
    const tableBtn = el("button", { type: "button", "aria-pressed": String(riverMode === "table"), text: "Table" });
    const riverBtn = el("button", { type: "button", "aria-pressed": String(riverMode === "river"), text: "River" });
    on(tableBtn, "click", () => {
      if (riverMode !== "table") {
        riverMode = "table";
        void render();
      }
    });
    on(riverBtn, "click", () => {
      if (riverMode !== "river") {
        riverMode = "river";
        void render();
      }
    });
    const segmented = el("div", { class: "segmented", role: "group", "aria-label": "Ledger view" }, tableBtn, riverBtn);

    replace(viewToggleRow,
      el("span", { class: "ledger__view-label", text: "Visualization" }),
      segmented,
    );
  }

  /** Shared filter so table and river always agree on what is shown. */
  function applyEntryFilter(entries: LedgerEntry[]): LedgerEntry[] {
    return entries.filter((entry) => {
      if (filter.operation && entry.operation !== filter.operation) return false;
      if (filter.collection && !entry.collection.toLowerCase().includes(filter.collection.toLowerCase())) return false;
      if (filter.key && !entry.key.toLowerCase().includes(filter.key.toLowerCase())) return false;
      if (filter.originNode && entry.origin_node_id !== filter.originNode) return false;
      return true;
    });
  }

  function buildTable(node: NodeView, entries: LedgerEntry[], _from: number, _to: number, _tip: number | null): HTMLElement {
    const filtered = applyEntryFilter(entries);

    if (filtered.length === 0) {
      return el("div", { class: "empty", style: "padding: var(--space-xl) var(--space-md);" },
        el("p", { class: "empty__title", text: "No entries match on this page" }),
        el("p", { class: "empty__body", text: "Adjust the filters or load another page." }),
      );
    }

    const headers = [
      "Entry",
      "Operation",
      "Collection",
      "Key",
      "HLC Timestamp",
      "Origin Node",
      "Bytes",
    ];
    if (showHiddenCols) headers.push("Key Hash", "Prev Hash", "Entry Hash", "Signature");

    const thead = el("thead", {},
      el("tr", {},
        ...headers.map((h) => el("th", { text: h })),
      ),
    );

    const tbody = el("tbody", {});
    for (const entry of filtered) {
      tbody.appendChild(renderEntryRow(entry, node, filter, showHiddenCols));
    }

    return el("div", { class: "table-container ledger__table-wrap" },
      el("table", { class: "table table--numeric" }, thead, tbody),
    );
  }

  /**
   * Time-river: entries drift downstream left→right (x = entry_id), one lane
   * per operation, checkpoints drawn as dams across all lanes. Selecting a
   * log opens its detail card below. The table stays the audit surface.
   */
  function buildRiver(node: NodeView, entries: LedgerEntry[], from: number, to: number): HTMLElement {
    void node;
    const filtered = applyEntryFilter(entries);
    if (filtered.length === 0) {
      return el("div", { class: "empty", style: "padding: var(--space-xl) var(--space-md);" },
        el("p", { class: "empty__title", text: "No entries match on this page" }),
        el("p", { class: "empty__body", text: "Adjust the filters or load another page." }),
      );
    }
    const lanes = RIVER_LANE_ORDER.filter((op) => filtered.some((e) => e.operation === op));
    const laneOf = new Map(lanes.map((op, i) => [op, i]));
    const laneCount = Math.max(1, lanes.length);
    const laneY = (i: number): number => 36 + i * 46;
    const height = laneY(laneCount - 1) + 40;
    const X0 = 150;
    const X1 = 980;
    const xOf = (id: number): number => (to <= from ? (X0 + X1) / 2 : X0 + ((id - from) / (to - from)) * (X1 - X0));

    const flow = svg("svg", {
      class: "river",
      viewBox: `0 0 1000 ${height}`,
      role: "img",
      "aria-label": `Ledger river, ${filtered.length} entries`,
      style: "width: 100%; height: auto; display: block; min-height: 220px;",
    });
    lanes.forEach((op, i) => {
      const y = laneY(i);
      flow.appendChild(svg("line", { x1: X0, y1: y, x2: X1, y2: y, class: "river__lane" }));
      flow.appendChild(svg("text", { x: 12, y: y + 4, class: "river__lane-label" }, OPERATION_LABELS[op] ?? op));
    });

    const checkpoints = filtered.filter((e) => e.operation === "CHECKPOINT");
    checkpoints.forEach((cp) => {
      const x = xOf(cp.entry_id);
      flow.appendChild(svg("line", { x1: x, y1: 18, x2: x, y2: height - 12, class: "river__dam" }));
    });

    filtered.forEach((entry) => {
      const lane = laneOf.get(entry.operation) ?? 0;
      const x = xOf(entry.entry_id);
      const y = laneY(lane);
      const tone = OPERATION_TONES[entry.operation] ?? "converged";
      const color = RIVER_TONE_VAR[tone] ?? "var(--color-primary)";
      const isSelected = selectedRiverId === entry.entry_id;
      const dot = svg("circle", {
        cx: x,
        cy: y,
        r: isSelected ? 7 : 4.5,
        fill: color,
        class: "river__dot",
        tabindex: "0",
        role: "button",
        "aria-label": `${entry.operation} #${entry.entry_id} on ${entry.collection}/${entry.key}`,
        "data-selected": String(isSelected),
      });
      dot.addEventListener("click", (ev) => {
        ev.stopPropagation();
        selectedRiverId = entry.entry_id;
        void render();
      });
      dot.addEventListener("keydown", (ev) => {
        const key = (ev as KeyboardEvent).key;
        if (key === "Enter" || key === " ") {
          ev.preventDefault();
          selectedRiverId = entry.entry_id;
          void render();
        }
      });
      flow.appendChild(dot);
    });

    const wrap = el("div", { class: "river__wrap" }, flow);

    const selected = selectedRiverId !== null ? filtered.find((e) => e.entry_id === selectedRiverId) : null;
    if (selected) {
      const close = el("button", { class: "btn btn--sm btn--ghost", type: "button", text: "Close" });
      on(close, "click", () => {
        selectedRiverId = null;
        void render();
      });
      wrap.appendChild(
        el("div", { class: "card", style: "margin-top: var(--space-sm); border: 1px solid var(--color-hairline);" },
          el("div", { class: "row row--between" },
            el("h4", { class: "card__title", text: `Entry #${selected.entry_id} — ${OPERATION_LABELS[selected.operation] ?? selected.operation}` }),
            close,
          ),
          el("dl", { class: "kv" },
            el("dt", { text: "Collection" }), el("dd", { class: "mono", text: selected.collection }),
            el("dt", { text: "Key" }), el("dd", { class: "mono", text: selected.key }),
            el("dt", { text: "Origin" }), el("dd", { class: "mono", text: selected.origin_node_id }),
            el("dt", { text: "HLC" }), el("dd", { class: "mono", text: selected.hlc }),
            el("dt", { text: "Bytes" }), el("dd", { class: "mono", text: `${selected.document_bytes} B` }),
            selected.prev_hash ? el("dt", { text: "Prev Hash" }) : null,
            selected.prev_hash ? el("dd", { class: "mono text-fine text-break", text: selected.prev_hash }) : null,
            selected.entry_hash ? el("dt", { text: "Entry Hash" }) : null,
            selected.entry_hash ? el("dd", { class: "mono text-fine text-break", text: selected.entry_hash }) : null,
            selected.origin_signature ? el("dt", { text: "Signature" }) : null,
            selected.origin_signature ? el("dd", { class: "mono text-fine text-break", text: selected.origin_signature }) : null,
          ),
        ),
      );
    } else {
      wrap.appendChild(
        el("p", { class: "muted", style: "font: var(--text-fine); padding-top: var(--space-xs);", text: "Select a log to inspect it. The table view stays the audit surface." }),
      );
    }
    return wrap;
  }

  function buildPager(_node: NodeView, from: number, to: number, tip: number | null, entries: LedgerEntry[], _totalCount: number): HTMLElement {
    const isFirstPage = from === 0;
    const hasPrev = !isFirstPage;
    const hasNext = tip !== null && to < tip;

    const prevBtn = el("button", { class: "btn btn--sm", type: "button", disabled: !hasPrev }, icon(Icons.chevronLeft, 13), " Earlier");
    on(prevBtn, "click", () => {
      const newFrom = Math.max(0, from - MAX_ENTRIES_PER_CALL);
      currentFrom = newFrom;
      void render();
    });

    const nextBtn = el("button", { class: "btn btn--sm", type: "button", disabled: !hasNext }, "Later ", icon(Icons.chevronRight, 13));
    on(nextBtn, "click", () => {
      currentFrom = to + 1;
      void render();
    });

    const filtered = applyEntryFilter(entries);
    // Empty pages carry no range: the table's "No entries match" empty state
    // speaks, and a "Showing 1–0 of 0 entries" line would contradict it.
    if (filtered.length === 0) {
      return el("div", { class: "row row--between ledger__pager" },
        prevBtn,
        el("span", { class: "muted", hidden: true }),
        nextBtn,
      );
    }
    const firstId = filtered[0].entry_id;
    const lastId = filtered[filtered.length - 1].entry_id;
    const info = el("span", { class: "muted", style: "flex: 1; text-align: center; font: var(--text-caption);" },
      `Showing ${count(firstId)}–${count(lastId)} of ${tip !== null ? count(tip + 1) : "unknown"} entries`);

    return el("div", { class: "row row--between ledger__pager" },
      prevBtn, info, nextBtn,
    );
  }

  // ── In-memory page cache to prevent re-fetching when filtering or toggling view modes ──
  let cachedNodeId = "";
  let cachedFrom = -1;
  let cachedPage: LedgerEntriesPage | null = null;
  let inFlightPromise: Promise<void> | null = null;

  function renderDataPanel(node: NodeView, page: LedgerEntriesPage): void {
    replace(dataPanel,
      card("Ledger",
        riverMode === "river"
          ? buildRiver(node, page.entries, page.from, page.to)
          : buildTable(node, page.entries, page.from, page.to, currentTip),
        buildPager(node, page.from, page.to, currentTip, page.entries, page.count),
      ),
    );
  }

  async function render(): Promise<void> {
    const selection = store.state.selection;
    if (selection.kind !== "ledger" || !selection.nodeId) {
      replace(filterAreaEl);
      replace(viewToggleRow);
      replace(dataPanel, emptyState({ title: "No ledger selected", body: "Select a node from the sidebar or canvas to view its ledger." }));
      filterBarNodeId = "";
      cachedNodeId = "";
      cachedPage = null;
      return;
    }

    const node = store.state.nodes.get(selection.nodeId);
    if (!node) {
      replace(filterAreaEl);
      replace(viewToggleRow);
      replace(dataPanel, emptyState({ title: "Node not found", body: "The selected node is no longer available." }));
      filterBarNodeId = "";
      cachedNodeId = "";
      cachedPage = null;
      return;
    }

    // Always keep the filter bar + view toggle stable.
    ensureFilterBar(node);
    ensureViewToggle();

    // If we have cached entries for this node and offset, render immediately (no flicker!)
    if (cachedNodeId === node.process.node_id && cachedFrom === currentFrom && cachedPage) {
      renderDataPanel(node, cachedPage);
      return;
    }

    // Prevent duplicate concurrent loads
    if (inFlightPromise) return;

    // Show skeleton only during actual network fetch
    const visibleCols = showHiddenCols ? 11 : 7;
    replace(dataPanel, card("Ledger",
      el("div", { class: "table-container ledger__table-wrap" },
        el("table", { class: "table table--numeric" },
          el("thead", {}, el("tr", {}, ...Array.from({ length: visibleCols }, () => el("th", { text: "…" })))),
          el("tbody", {}, ...Array.from({ length: 5 }, () => skeletonRow(visibleCols))),
        ),
      ),
    ));

    const fetchNodeId = node.process.node_id;
    const fetchFrom = currentFrom;

    inFlightPromise = (async () => {
      try {
        const to = fetchFrom + MAX_ENTRIES_PER_CALL - 1;
        const page = await loadEntries(node, fetchFrom, to);
        currentTip = page.to;
        cachedNodeId = fetchNodeId;
        cachedFrom = fetchFrom;
        cachedPage = page;

        if (store.state.selection.kind === "ledger" && store.state.selection.nodeId === fetchNodeId && currentFrom === fetchFrom) {
          renderDataPanel(node, page);
        }
      } catch (error) {
        replace(dataPanel, card("Ledger",
          el("p", { class: "error-note", text: `Could not load ledger: ${describeError(error)}` }),
        ));
      } finally {
        inFlightPromise = null;
      }
    })();
  }

  return { render, element };
}
