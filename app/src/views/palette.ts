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

/** Subsequence fuzzy score; contiguous runs score higher. -1 means no match. */
function fuzzyScore(query: string, label: string): number {
  const q = query.toLowerCase();
  const l = label.toLowerCase();
  let score = 0;
  let li = 0;
  let run = 0;
  for (let qi = 0; qi < q.length; qi++) {
    const found = l.indexOf(q[qi], li);
    if (found < 0) return -1;
    if (found === li) {
      run++;
      score += 2 + run;
    } else {
      run = 0;
      score += 1;
    }
    li = found + 1;
  }
  // Prefer shorter labels on ties: exact names surface first.
  return score * 100 - l.length;
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
  replace(dialog, el("div", { class: "palette" }, input, list));
  document.body.appendChild(dialog);

  let active = 0;
  let visible: PaletteItem[] = items;

  function paint(): void {
    const q = input.value.trim();
    visible =
      q === ""
        ? items
        : items
            .map((item) => ({ item, score: fuzzyScore(q, item.label) }))
            .filter((s) => s.score >= 0)
            .sort((a, b) => b.score - a.score)
            .map((s) => s.item);
    active = Math.max(0, Math.min(active, Math.max(0, visible.length - 1)));
    if (visible.length === 0) active = -1;

    const children: (Node | string)[] = [];
    let lastGroup = "";
    visible.forEach((item, i) => {
      if (item.group !== lastGroup) {
        lastGroup = item.group;
        children.push(el("p", { class: "palette__group", text: item.group }));
      }
      const opt = el(
        "div",
        {
          class: "palette__option",
          role: "option",
          id: `palette-opt-${i}`,
          "aria-selected": String(i === active),
        },
        el("span", { text: item.label }),
        el("span", { class: "palette__hint", text: item.hint }),
      );
      on(opt, "click", () => {
        dismiss();
        item.run();
      });
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
      const item = visible[active];
      if (item) {
        dismiss();
        item.run();
      }
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
