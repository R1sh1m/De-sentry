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
import { el, icon, Icons, on, replace } from "../util/dom.js";
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
  CHECKPOINT: "Checkpoint (GC)",
  TRANSIT_INTENT: "Transit Intent (held for offline node)",
  TRANSIT_CLAIMED: "Transit Claimed",
};

const OPERATION_TONES: Record<string, string> = {
  PUT: "converged",
  DEL: "offline",
  CHECKPOINT: "lagging",
  TRANSIT_INTENT: "lagging",
  TRANSIT_CLAIMED: "converged",
};

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
  const body = el("div", { class: "stack" });
  const element = el("main", { class: "canvas ledger-view" }, body);

  let currentFrom = 0;
  let currentTip: number | null = null;
  let loading = false;
  let filter: FilterState = { operation: "", collection: "", key: "", originNode: "" };
  let showHiddenCols = false;
  // Debounce text filters so typing does not reset pagination + refetch per keystroke.
  let filterTimer = 0;

  function scheduleFilterApply(apply: () => void): void {
    window.clearTimeout(filterTimer);
    filterTimer = window.setTimeout(apply, 200);
  }

  /** Re-rendering rebuilds the filter bar; put the caret back where typing was. */
  function restoreFilterFocus(field: string): void {
    try {
      const input = body.querySelector<HTMLInputElement>(`input[data-filter-field="${field}"]`);
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

  function buildFilterBar(node: NodeView): HTMLElement {
    const opOptions = ["", "PUT", "DEL", "CHECKPOINT", "TRANSIT_INTENT", "TRANSIT_CLAIMED"];
    const originOptions = ["", ...node.peers.map((p) => p.node_id)];

    const opSelect = el("select", { class: "field", style: "flex: 1;" },
      ...opOptions.map((op) => el("option", { value: op, selected: filter.operation === op }, op || "All operations")),
    );
    on(opSelect, "change", () => { filter.operation = opSelect.value; currentFrom = 0; render(); });

    const collectionInput = el("input", {
      type: "text",
      class: "field",
      placeholder: "Filter loaded entries — collection",
      value: filter.collection,
      style: "flex: 1;",
      "data-filter-field": "collection",
    });
    on(collectionInput, "input", () => {
      const value = collectionInput.value;
      const field = "collection";
      scheduleFilterApply(() => {
        filter.collection = value;
        currentFrom = 0;
        void render().then(() => restoreFilterFocus(field));
      });
    });

    const keyInput = el("input", {
      type: "text",
      class: "field",
      placeholder: "Filter loaded entries — key",
      value: filter.key,
      style: "flex: 1;",
      "data-filter-field": "key",
    });
    on(keyInput, "input", () => {
      const value = keyInput.value;
      const field = "key";
      scheduleFilterApply(() => {
        filter.key = value;
        currentFrom = 0;
        void render().then(() => restoreFilterFocus(field));
      });
    });

    const originSelect = el("select", { class: "field", style: "flex: 1;" },
      ...originOptions.map((id) => el("option", { value: id, selected: filter.originNode === id }, id || "All origin nodes")),
    );
    on(originSelect, "change", () => { filter.originNode = originSelect.value; currentFrom = 0; render(); });

    const toggleHidden = el("button", {
      class: "btn btn--sm btn--ghost", type: "button",
      "aria-pressed": String(showHiddenCols),
      title: "Toggle hidden columns (hashes, signatures)",
    }, icon(Icons.inspector, 13), " Hashes");
    on(toggleHidden, "click", () => { showHiddenCols = !showHiddenCols; toggleHidden.setAttribute("aria-pressed", String(showHiddenCols)); render(); });

    const clearFilters = el("button", { class: "btn btn--sm btn--ghost", type: "button" }, icon(Icons.refresh, 13), " Clear");
    on(clearFilters, "click", () => { filter = { operation: "", collection: "", key: "", originNode: "" }; currentFrom = 0; render(); });

    return el("div", { class: "card", style: "margin-bottom: var(--space-sm);" },
      el("h3", { class: "card__title", text: "Filters" }),
      el("div", { class: "row", style: "flex-wrap: wrap; gap: var(--space-xs);" },
        el("div", { class: "row", style: "flex: 1; min-width: 180px; gap: var(--space-xxs);" }, el("span", { class: "tree__group-label", text: "Op" }), opSelect),
        el("div", { class: "row", style: "flex: 1; min-width: 180px; gap: var(--space-xxs);" }, el("span", { class: "tree__group-label", text: "Collection" }), collectionInput),
        el("div", { class: "row", style: "flex: 1; min-width: 140px; gap: var(--space-xxs);" }, el("span", { class: "tree__group-label", text: "Key" }), keyInput),
        el("div", { class: "row", style: "flex: 1; min-width: 180px; gap: var(--space-xxs);" }, el("span", { class: "tree__group-label", text: "Origin" }), originSelect),
        toggleHidden,
        clearFilters,
      ),
    );
  }

  function buildTable(node: NodeView, entries: LedgerEntry[], _from: number, _to: number, _tip: number | null): HTMLElement {
    const filtered = entries.filter((entry) => {
      if (filter.operation && entry.operation !== filter.operation) return false;
      if (filter.collection) {
        const coll = isTransitOp(entry.operation) ? entry.collection : entry.collection;
        if (!coll.toLowerCase().includes(filter.collection.toLowerCase())) return false;
      }
      if (filter.key && !entry.key.toLowerCase().includes(filter.key.toLowerCase())) return false;
      if (filter.originNode && entry.origin_node_id !== filter.originNode) return false;
      return true;
    });

    if (filtered.length === 0) {
      return el("div", { class: "empty" },
        el("p", { class: "empty__title", text: "No entries match on this page" }),
        el("p", { class: "empty__body", text: "Adjust the filters or load an earlier page." }),
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

    return el("div", { class: "table-container", style: "overflow-x: auto;" },
      el("table", { class: "table table--numeric" }, thead, tbody),
    );
  }

  function buildPager(_node: NodeView, from: number, to: number, tip: number | null, entries: LedgerEntry[], _totalCount: number): HTMLElement {
    const isFirstPage = from === 0;
    const hasPrev = !isFirstPage;
    const hasNext = tip !== null && to < tip;

    const prevBtn = el("button", { class: "btn btn--sm", type: "button", disabled: !hasPrev }, icon(Icons.chevronLeft, 13), " Earlier");
    on(prevBtn, "click", () => {
      const newFrom = Math.max(0, from - MAX_ENTRIES_PER_CALL);
      currentFrom = newFrom;
      render();
    });

    const nextBtn = el("button", { class: "btn btn--sm", type: "button", disabled: !hasNext }, "Later ", icon(Icons.chevronRight, 13));
    on(nextBtn, "click", () => {
      currentFrom = to + 1;
      render();
    });

    const info = el("span", { class: "muted", style: "flex: 1; text-align: center;" },
      `Showing ${count(from + 1)}–${count(from + filteredCount(entries))} of ${tip !== null ? count(tip + 1) : "unknown"} entries`);

    return el("div", { class: "row row--between", style: "margin-top: var(--space-sm); padding: var(--space-xs) var(--space-sm);" },
      prevBtn, info, nextBtn,
    );
  }

  function filteredCount(entries: LedgerEntry[]): number {
    return entries.filter((entry) => {
      if (filter.operation && entry.operation !== filter.operation) return false;
      if (filter.collection) {
        const coll = isTransitOp(entry.operation) ? entry.collection : entry.collection;
        if (!coll.toLowerCase().includes(filter.collection.toLowerCase())) return false;
      }
      if (filter.key && !entry.key.toLowerCase().includes(filter.key.toLowerCase())) return false;
      if (filter.originNode && entry.origin_node_id !== filter.originNode) return false;
      return true;
    }).length;
  }

  async function render(): Promise<void> {
    const selection = store.state.selection;
    if (selection.kind !== "ledger" || !selection.nodeId) {
      replace(body, emptyState({ title: "No ledger selected", body: "Select a node from the sidebar or canvas to view its ledger." }));
      return;
    }

    const node = store.state.nodes.get(selection.nodeId);
    if (!node) {
      replace(body, emptyState({ title: "Node not found", body: "The selected node is no longer available." }));
      return;
    }

    if (loading) {
      // Must match buildTable: 7 base columns, 11 with hashes shown.
      const visibleCols = showHiddenCols ? 11 : 7;
      replace(body, buildFilterBar(node), card("Ledger", el("table", { class: "table" },
        el("thead", {}, el("tr", {}, ...Array.from({ length: visibleCols }, () => el("th", { text: "…" })))),
        el("tbody", {}, ...Array.from({ length: 5 }, () => skeletonRow(visibleCols))),
      )));
      return;
    }

    loading = true;
    store.notify();

    try {
      const to = currentFrom + MAX_ENTRIES_PER_CALL - 1;
      const page = await loadEntries(node, currentFrom, to);
      currentTip = page.to;

      replace(body,
        buildFilterBar(node),
        card("Ledger",
          buildTable(node, page.entries, page.from, page.to, currentTip),
          buildPager(node, page.from, page.to, currentTip, page.entries, page.count),
        ),
      );
    } catch (error) {
      replace(body, buildFilterBar(node), card("Ledger",
        el("p", { class: "error-note", text: `Could not load ledger: ${describeError(error)}` }),
      ));
    } finally {
      loading = false;
      store.notify();
    }
  }

  return { render, element };
}
