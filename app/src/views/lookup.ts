/**
 * Universal Lookup view.
 *
 * A centre-panel search interface that fans out queries to all reachable nodes
 * in parallel, merges and deduplicates results, and displays them in a
 * keyboard-navigable list. Results can be opened in the Explorer or nodes can
 * be selected in the Canvas.
 *
 * Entry: Ctrl/Cmd + L or header lookup button
 * Layout: Three zones - Header (search + scope), Results (scrollable list), Footer (actions)
 */

import { apiFor } from "../api.js";
import { convergenceOf, store, type Convergence } from "../state.js";
import { engineLabel, json } from "../util/format.js";
import { el, icon, Icons, on, replace } from "../util/dom.js";
import { emptyState } from "../util/empty.js";

type Scope = "all" | "collections" | "keys";

interface LookupResult {
  collection: string;
  key: string;
  document: unknown;
  engine: string;
  nodes: { node_id: string; node_name: string; status: Convergence }[];
}

export interface LookupHandles {
  render(): void;
  element: HTMLElement;
  navigate(direction: 1 | -1): void;
  activate(): void;
}

export function createLookup(): LookupHandles {
  const element = el("div", { class: "canvas lookup-view" });

  let query = "";
  let scope: Scope = "all";
  let results: LookupResult[] = [];
  let active = 0;
  let loading = false;
  let error = "";
  let debounceTimer = 0;
  let abortController: AbortController | null = null;

  // DOM references
  let searchInput: HTMLInputElement | null = null;
  let listEl: HTMLElement | null = null;

  /** Middle-truncate text for display (e.g., long keys). */
  function truncateMiddle(text: string, max: number): string {
    if (text.length <= max) return text;
    const half = Math.floor((max - 1) / 2);
    return `${text.slice(0, half)}…${text.slice(-half)}`;
  }

  /** Short node ID for display. */
  function shortNode(nodeId: string, head = 8): string {
    if (!nodeId) return "—";
    return nodeId.length <= head ? nodeId : `${nodeId.slice(0, head)}…`;
  }

  /** Preview of document as JSON string (first 200 chars). */
  function preview(doc: unknown): string {
    if (doc === null || doc === undefined) return "null";
    const s = JSON.stringify(doc);
    return s.length > 200 ? s.slice(0, 200) + "…" : s;
  }

  /** Run search across all reachable nodes in parallel. */
  async function runSearch(): Promise<void> {
    // Cancel previous search
    if (abortController) abortController.abort();
    abortController = new AbortController();

    loading = true;
    error = "";
    results = [];
    active = 0;
    render();

    const q = query.trim().toLowerCase();
    const nodes = store.dataNodes().filter((n) => n.reachable && n.process.process === "running");

    try {
      if (scope === "collections") {
        // "Collections" scope: gather all collection names from all nodes, filter by query
        const collectionSet = new Map<string, { engine: string; nodes: { node_id: string; node_name: string; status: Convergence }[] }>();

        for (const node of nodes) {
          const brain = await node.brain;
          if (!brain) continue;
          for (const c of brain.collections) {
            if (q && !c.name.toLowerCase().includes(q)) continue;
            let entry = collectionSet.get(c.name);
            if (!entry) {
              entry = { engine: c.engine, nodes: [] };
              collectionSet.set(c.name, entry);
            }
            entry.nodes.push({
              node_id: node.process.node_id,
              node_name: node.process.node_name || node.process.node_id.slice(0, 8),
              status: convergenceOf(node),
            });
          }
        }

        results = [...collectionSet.entries()].map(([name, data]) => ({
          collection: name,
          key: "",
          document: null,
          engine: data.engine,
          nodes: data.nodes,
        }));

        // Sort by collection name
        results.sort((a, b) => a.collection.localeCompare(b.collection));
      } else {
        // "All" or "Keys" scope: search document keys
        const nodeResults = await Promise.allSettled(
          nodes.map(async (node) => {
            const api = apiFor(node.process.api_port);
            const hits = await api.searchLookup(undefined, q, 50);
            return { node, hits };
          }),
        );

        // Merge and dedupe by (collection, key)
        const byKey = new Map<string, LookupResult>();

        for (const result of nodeResults) {
          if (result.status !== "fulfilled") continue;
          const { node, hits } = result.value;
          for (const hit of hits) {
            const keyId = `${hit.collection}::${hit.key}`;
            let existing = byKey.get(keyId);
            if (!existing) {
              existing = {
                collection: hit.collection,
                key: hit.key,
                document: hit.document,
                engine: hit.engine,
                nodes: [],
              };
              byKey.set(keyId, existing);
            }
            existing.nodes.push({
              node_id: node.process.node_id,
              node_name: node.process.node_name || node.process.node_id.slice(0, 8),
              status: convergenceOf(node),
            });
          }
        }

        results = [...byKey.values()];

        // Sort: exact key matches first, then by collection
        results.sort((a, b) => {
          const aExact = a.key.toLowerCase() === q;
          const bExact = b.key.toLowerCase() === q;
          if (aExact && !bExact) return -1;
          if (!aExact && bExact) return 1;
          return a.collection.localeCompare(b.collection);
        });

        // For "All" scope, also include collection name matches
        if (scope === "all" && q) {
          const collectionSet = new Map<string, { engine: string; nodes: { node_id: string; node_name: string; status: Convergence }[] }>();
          for (const node of nodes) {
            const brain = await node.brain;
            if (!brain) continue;
            for (const c of brain.collections) {
              if (!c.name.toLowerCase().includes(q)) continue;
              let entry = collectionSet.get(c.name);
              if (!entry) {
                entry = { engine: c.engine, nodes: [] };
                collectionSet.set(c.name, entry);
              }
              entry.nodes.push({
                node_id: node.process.node_id,
                node_name: node.process.node_name || node.process.node_id.slice(0, 8),
                status: convergenceOf(node),
              });
            }
          }

          for (const [name, data] of collectionSet.entries()) {
            // Check if we already have a result for this collection
            const exists = results.some((r) => r.collection === name && r.key === "");
            if (!exists) {
              results.push({
                collection: name,
                key: "",
                document: null,
                engine: data.engine,
                nodes: data.nodes,
              });
            }
          }
        }
      }

      active = Math.max(0, Math.min(active, results.length - 1));
      if (results.length === 0) active = -1;
    } catch (err) {
      error = err instanceof Error ? err.message : String(err);
    } finally {
      loading = false;
      render();
    }
  }

  /** Navigate results list by direction (+1 or -1). */
  function navigate(direction: 1 | -1): void {
    if (results.length === 0) return;
    active = Math.max(0, Math.min(active + direction, results.length - 1));
    render();
  }

  /** Activate (open) the currently selected result. */
  function activate(): void {
    if (active < 0 || active >= results.length) return;
    const r = results[active];
    const firstNode = r.nodes[0];
    if (firstNode) {
      store.select({ kind: "collection", nodeId: firstNode.node_id, collection: r.collection });
    }
  }

  /** Render the lookup view. */
  function render(): void {
    if (store.state.selection.kind !== "lookup") {
      replace(element);
      return;
    }

    // Zone A: Header (~64px)
    const header = el("div", { class: "lookup__header" });

    // Search input with icon
    const searchBox = el("div", { class: "lookup__search" });
    const searchIcon = icon(Icons.search, 14);
    searchInput = el("input", {
      class: "lookup__search-input",
      type: "search",
      placeholder: "Search collections and keys across the mesh…",
      value: query,
      "aria-label": "Universal lookup",
      "aria-controls": "lookup-list",
      "aria-expanded": String(results.length > 0),
      "aria-activedescendant": active >= 0 ? `lookup-opt-${active}` : "",
    }) as HTMLInputElement;

    on(searchInput, "input", () => {
      query = searchInput!.value;
      window.clearTimeout(debounceTimer);
      debounceTimer = window.setTimeout(() => void runSearch(), 150);
    });

    on(searchInput, "keydown", (e) => {
      if (e.key === "ArrowDown") {
        e.preventDefault();
        if (results.length > 0) {
          active = 0;
          render();
        }
      } else if (e.key === "ArrowUp") {
        e.preventDefault();
        if (results.length > 0) {
          active = results.length - 1;
          render();
        }
      } else if (e.key === "Escape") {
        e.preventDefault();
        store.select({ kind: "none" });
      } else if (e.key === "Enter" && active >= 0) {
        e.preventDefault();
        activate();
      }
    });

    searchBox.appendChild(searchIcon);
    searchBox.appendChild(searchInput);
    header.appendChild(searchBox);

    // Segmented control (All/Collections/Keys)
    const segmented = el("div", { class: "segmented", role: "group", "aria-label": "Search scope" });
    const scopes: [Scope, string][] = [["all", "All"], ["collections", "Collections"], ["keys", "Keys"]];
    for (const [id, label] of scopes) {
      const btn = el("button", {
        type: "button",
        "aria-pressed": String(scope === id),
        text: label,
      });
      on(btn, "click", () => {
        scope = id;
        active = 0;
        void runSearch();
      });
      segmented.appendChild(btn);
    }
    header.appendChild(segmented);

    // Zone B: Results list (scrollable)
    const resultsZone = el("div", { class: "lookup__results" });
    listEl = el("div", {
      class: "lookup__list",
      id: "lookup-list",
      role: "listbox",
      "aria-label": "Lookup results",
    });

    if (loading) {
      listEl.appendChild(el("div", { class: "lookup__empty", text: "Searching mesh…" }));
    } else if (error) {
      listEl.appendChild(el("div", { class: "lookup__empty lookup__empty--error", text: `Search error: ${error}` }));
    } else if (results.length === 0) {
      listEl.appendChild(
        emptyState({
          title: "No results",
          body: query ? "No matching collections or keys found across the mesh." : "Type a query to search all reachable nodes.",
        }),
      );
    } else {
      for (let i = 0; i < results.length; i++) {
        const r = results[i];
        const selected = i === active;
        const option = el("div", {
          class: `lookup__option${selected ? " lookup__option--selected" : ""}`,
          role: "option",
          id: `lookup-opt-${i}`,
          "aria-selected": String(selected),
          tabindex: selected ? "0" : "-1",
        });

        // Collection (monospace)
        option.appendChild(el("span", { class: "lookup__collection mono", text: r.collection, title: r.collection }));

        // Key (monospace, middle-truncated)
        const keyText = r.key ? truncateMiddle(r.key, 40) : "—";
        option.appendChild(el("span", { class: "lookup__key mono", text: keyText, title: r.key || undefined }));

        // Preview (first 200 chars JSON)
        const previewText = r.document === null || r.document === undefined ? "—" : preview(r.document);
        option.appendChild(el("span", { class: "lookup__preview mono", text: previewText, title: r.document === null || r.document === undefined ? undefined : json(r.document) }));

        // Nodes (chips with status dots)
        const nodesZone = el("div", { class: "lookup__nodes" });
        for (const n of r.nodes) {
          const chip = el("div", { class: "lookup__node-chip" });
          const dot = el("span", { class: "dot", "data-status": n.status, role: "img", "aria-label": n.status });
          const name = el("span", { class: "mono", text: shortNode(n.node_id, 8) });
          chip.appendChild(dot);
          chip.appendChild(name);
          on(chip, "click", (e) => {
            e.stopPropagation();
            store.select({ kind: "node", nodeId: n.node_id });
          });
          nodesZone.appendChild(chip);
        }
        option.appendChild(nodesZone);

        // Engine badge
        option.appendChild(el("span", { class: "badge", text: engineLabel(r.engine) || "—" }));

        // Last modified (relative time) - v1 shows "—" since we don't have per-document timestamps
        option.appendChild(el("span", { class: "lookup__modified", text: "—", title: "Last modified time not available in v1" }));

        // Click to open in Explorer
        on(option, "click", () => {
          active = i;
          activate();
        });

        // Keyboard on option
        on(option, "keydown", (e) => {
          if (e.key === "Enter") {
            e.preventDefault();
            activate();
          }
        });

        listEl.appendChild(option);
      }
    }
    resultsZone.appendChild(listEl);

    // Zone C: Footer (actions for selected row)
    const footer = el("div", { class: "lookup__footer", hidden: results.length === 0 || active < 0 });
    const openBtn = el("button", {
      class: "btn btn--sm btn--primary",
      type: "button",
      disabled: active < 0,
      text: "Open in Explorer",
    });
    on(openBtn, "click", () => activate());
    footer.appendChild(openBtn);

    replace(element, header, resultsZone, footer);

    // Focus management: focus active option
    if (active >= 0) {
      const opt = document.getElementById(`lookup-opt-${active}`);
      opt?.focus();
    }
  }

  return { render, element, navigate, activate };
}
