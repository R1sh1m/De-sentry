/**
 * Command palette (Ctrl/⌘+K): actions and views only.
 *
 * Deliberately not a mesh search — no nodes, collections or peers, so the
 * palette never renders untrusted content and stays off the data path. Every
 * action dispatches through the same store/sidecar calls the buttons use.
 * Vanilla fuzzy filter (subsequence match); no dependency.
 */

import { refreshNodeList, store } from "../state.js";
import { el, on, replace } from "../util/dom.js";

interface PaletteItem {
  id: string;
  group: string;
  label: string;
  hint: string;
  run: () => void;
}

/** Subsequence fuzzy match; contiguous runs score higher. Null = no match. */
function fuzzyMatch(query: string, label: string): { score: number; indices: number[] } | null {
  const q = query.toLowerCase();
  const l = label.toLowerCase();
  let score = 0;
  let li = 0;
  let run = 0;
  const indices: number[] = [];
  for (let qi = 0; qi < q.length; qi++) {
    const found = l.indexOf(q[qi], li);
    if (found < 0) return null;
    if (found === li) {
      run++;
      score += 2 + run;
    } else {
      run = 0;
      score += 1;
    }
    indices.push(found);
    li = found + 1;
  }
  // Prefer shorter labels on ties: exact names surface first.
  return { score: score * 100 - l.length, indices };
}

/** Recent command ids, newest first, capped at 3. Best-effort storage. */
const RECENT_KEY = "desentry:palette:recent";
const RECENT_MAX = 3;
function loadRecents(): string[] {
  try {
    const raw = localStorage.getItem(RECENT_KEY);
    if (!raw) return [];
    const parsed: unknown = JSON.parse(raw);
    return Array.isArray(parsed) ? parsed.filter((x): x is string => typeof x === "string").slice(0, RECENT_MAX) : [];
  } catch {
    return [];
  }
}
function recordRecent(id: string): void {
  try {
    const next = [id, ...loadRecents().filter((x) => x !== id)].slice(0, RECENT_MAX);
    localStorage.setItem(RECENT_KEY, JSON.stringify(next));
  } catch {
    // Private window: recents simply don't persist.
  }
}

/** Render a label with matched character runs wrapped in <mark>. */
function highlightedLabel(label: string, indices: number[] | null): HTMLElement {
  const span = el("span", {});
  if (indices === null || indices.length === 0) {
    span.textContent = label;
    return span;
  }
  const hits = new Set(indices);
  let run = "";
  const flush = (marked: boolean): void => {
    if (run === "") return;
    span.appendChild(marked ? el("mark", { class: "palette__match", text: run }) : document.createTextNode(run));
    run = "";
  };
  let inRun = false;
  for (let i = 0; i < label.length; i++) {
    const hit = hits.has(i);
    if (hit !== inRun) {
      flush(inRun);
      inRun = hit;
    }
    run += label[i];
  }
  flush(inRun);
  return span;
}

export function openPalette(opts: { onNewNode: () => void; onPair: () => void }): void {
  const dialog = el("dialog", { class: "modal-dialog palette-dialog", "aria-label": "Command palette" });
  const dismiss = () => {
    if (dialog.open) dialog.close();
    dialog.remove();
  };

  // View jumps mirror the sidebar pinned-nav logic (kept local so the
  // palette depends only on the store, not on sidebar internals).
  const ledgerTarget = (): void => {
    const node = store.selectedNode()?.process.node_id ?? store.dataNodes()[0]?.process.node_id;
    if (node) store.select({ kind: "ledger", nodeId: node });
  };
  const consoleTarget = (): void => {
    const node = store.selectedNode()?.process.node_id ?? store.dataNodes()[0]?.process.node_id;
    if (node) store.select({ kind: "console", nodeId: node });
  };
  const canvasHome = (): void => {
    const first = store.selectedNode() ?? store.dataNodes()[0];
    store.select(first ? { kind: "node", nodeId: first.process.node_id } : { kind: "none" });
  };

  const items: PaletteItem[] = [
    { id: "new", group: "Actions", label: "New node", hint: "N", run: () => opts.onNewNode() },
    { id: "pair", group: "Actions", label: "Pair another device", hint: "QR", run: () => opts.onPair() },
    { id: "refresh", group: "Actions", label: "Refresh nodes", hint: "Ctrl+R", run: () => void refreshNodeList() },
    { id: "home", group: "Views", label: "Mesh map home", hint: "Esc", run: canvasHome },
    { id: "tree", group: "Views", label: "Tree view", hint: "1", run: () => store.setCanvasMode("tree") },
    { id: "mesh", group: "Views", label: "Mesh view", hint: "2", run: () => store.setCanvasMode("mesh") },
    { id: "ledger", group: "Views", label: "Ledger feed", hint: "3", run: ledgerTarget },
    { id: "console", group: "Views", label: "Console", hint: "4", run: consoleTarget },
    { id: "dropbox", group: "Views", label: "Dropbox", hint: "5", run: () => store.select({ kind: "dropbox" }) },
  ];

  const input = el("input", {
    class: "palette__input",
    type: "text",
    placeholder: "Type a command or view…",
    role: "combobox",
    "aria-expanded": "true",
    "aria-controls": "palette-list",
    "aria-activedescendant": "",
    "aria-label": "Command palette",
    autocomplete: "off",
    spellcheck: "false",
  }) as HTMLInputElement;
  const list = el("div", { class: "palette__list", role: "listbox", id: "palette-list" });
  const footer = el(
    "div",
    { class: "palette__footer", "aria-hidden": "true" },
    el("span", {}, el("kbd", { text: "↑↓" }), " Navigate"),
    el("span", {}, el("kbd", { text: "↵" }), " Run"),
    el("span", {}, el("kbd", { text: "esc" }), " Dismiss"),
  );
  replace(dialog, el("div", { class: "palette" }, input, list, footer));
  document.body.appendChild(dialog);

  const byId = new Map(items.map((item) => [item.id, item]));
  let active = 0;
  let visible: { item: PaletteItem; indices: number[] | null }[] = items.map((item) => ({ item, indices: null }));

  const runItem = (entry: { item: PaletteItem }): void => {
    recordRecent(entry.item.id);
    dismiss();
    entry.item.run();
  };

  function paint(): void {
    const q = input.value.trim();
    let recentFlags: boolean[];
    if (q === "") {
      // Empty query: recents first (deduped), then everything else in order.
      const recents = loadRecents()
        .map((id) => byId.get(id))
        .filter((x): x is PaletteItem => x !== undefined)
        .map((item) => ({ item, indices: null as number[] | null, recent: true }));
      const recentIds = new Set(recents.map((r) => r.item.id));
      const rest = items.filter((item) => !recentIds.has(item.id)).map((item) => ({ item, indices: null as number[] | null, recent: false }));
      const ordered = [...recents, ...rest];
      visible = ordered.map(({ item, indices }) => ({ item, indices }));
      recentFlags = ordered.map((o) => o.recent);
    } else {
      visible = items
        .map((item) => {
          const m = fuzzyMatch(q, item.label);
          return m === null ? null : { item, indices: m.indices as number[] | null, score: m.score };
        })
        .filter((s): s is { item: PaletteItem; indices: number[] | null; score: number } => s !== null)
        .sort((a, b) => b.score - a.score)
        .map((s) => ({ item: s.item, indices: s.indices }));
      recentFlags = visible.map(() => false);
    }
    active = Math.max(0, Math.min(active, Math.max(0, visible.length - 1)));
    if (visible.length === 0) active = -1;

    const children: (Node | string)[] = [];
    let lastGroup = "";
    visible.forEach((entry, i) => {
      const group = q === "" && recentFlags[i] ? "Recent" : entry.item.group;
      if (group !== lastGroup) {
        lastGroup = group;
        children.push(el("p", { class: "palette__group", text: group }));
      }
      const opt = el(
        "div",
        {
          class: "palette__option",
          role: "option",
          id: `palette-opt-${i}`,
          "aria-selected": String(i === active),
        },
        highlightedLabel(entry.item.label, q === "" ? null : entry.indices),
        el("span", { class: "palette__hint", text: entry.item.hint }),
      );
      on(opt, "click", () => runItem(entry));
      on(opt, "mousemove", () => {
        if (active !== i) {
          active = i;
          paint();
        }
      });
      children.push(opt);
    });
    if (visible.length === 0) {
      children.push(el("p", { class: "palette__empty", text: "No matching command." }));
    }
    replace(list, ...children);
    input.setAttribute(
      "aria-activedescendant",
      active >= 0 ? `palette-opt-${active}` : "",
    );
    list.querySelector('[aria-selected="true"]')?.scrollIntoView({ block: "nearest" });
  }

  on(input, "input", () => {
    active = 0;
    paint();
  });
  on(input, "keydown", (e) => {
    if (e.key === "ArrowDown") {
      e.preventDefault();
      active = Math.min(visible.length - 1, active + 1);
      paint();
    } else if (e.key === "ArrowUp") {
      e.preventDefault();
      active = Math.max(0, active - 1);
      paint();
    } else if (e.key === "Enter") {
      e.preventDefault();
      const entry = visible[active];
      if (entry) runItem(entry);
    } else if (e.key === "Escape") {
      dismiss();
    }
  });
  on(dialog, "click", (e) => {
    if (e.target === dialog) dismiss();
  });
  on(dialog, "close", () => dialog.remove());

  try {
    dialog.showModal();
  } catch {
    dialog.setAttribute("open", "");
  }
  paint();
  input.focus();
}
