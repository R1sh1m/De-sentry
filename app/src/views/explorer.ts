/**
 * The file explorer: documents in a collection, read and edited in place.
 *
 * Keys on the left, one document on the right. Paging is by start key rather
 * than by offset, because the underlying scan is ordered and cursor-based --
 * an offset would re-read everything up to the page and would skip or repeat
 * rows whenever a write landed mid-scan.
 *
 * Editing is deliberately blunt: the raw JSON, a Save button, and a parse
 * check before anything is sent. There is no form generated from the schema.
 * The people who open this pane are looking at a document because something is
 * wrong with it, and a form that cannot represent the malformed value is
 * exactly the wrong tool for that.
 */

import { apiFor, ApiError, type DocumentRow } from "../api.js";
import { refreshNode, store } from "../state.js";
import { count, documentPreview, engineLabel, json, truncate } from "../util/format.js";
import { el, icon, Icons, on, replace } from "../util/dom.js";

const PAGE_SIZE = 200;

function describeError(error: unknown): string {
  if (error instanceof ApiError) return error.message;
  if (error instanceof Error) return error.message;
  return String(error);
}

interface ExplorerState {
  nodeId: string;
  collection: string;
  rows: DocumentRow[];
  /** Start key of each page already fetched, so Back is exact rather than guessed. */
  pageStack: string[];
  selectedKey: string | null;
  draft: string;
  dirty: boolean;
  loading: boolean;
  error: string;
  /** Set when the last page came back full, meaning there is probably more. */
  mayHaveMore: boolean;
}

export interface ExplorerHandles {
  render(): void;
  element: HTMLElement;
}

export function createExplorer(): ExplorerHandles {
  const element = el("div", { class: "explorer" });

  let state: ExplorerState | null = null;

  /**
   * `start` is inclusive -- the engine's scan is a lower bound -- so paging
   * forward asks from the last key of the previous page and drops it. Nudging
   * the key instead (appending a byte, incrementing the last character) would
   * silently skip keys whose next neighbour sorts between the two.
   */
  async function load(
    nodeId: string,
    collection: string,
    start = "",
    dropLeading = "",
  ): Promise<void> {
    const node = store.node(nodeId);
    if (node === undefined) return;
    if (state !== null) {
      state.loading = true;
      state.error = "";
      render();
    }

    try {
      const page = await apiFor(node.process.api_port).listDocuments(collection, {
        start: start || undefined,
        limit: PAGE_SIZE,
      });
      const rows =
        dropLeading !== "" && page.documents[0]?.key === dropLeading
          ? page.documents.slice(1)
          : page.documents;
      const selected = rows.length > 0 ? rows[0].key : null;
      state = {
        nodeId,
        collection,
        rows,
        pageStack: state !== null && state.collection === collection ? state.pageStack : [],
        selectedKey: selected,
        draft: selected !== null ? json(rows[0].document) : "",
        dirty: false,
        loading: false,
        error: "",
        mayHaveMore: page.documents.length === PAGE_SIZE,
      };
    } catch (error) {
      state = {
        nodeId,
        collection,
        rows: [],
        pageStack: [],
        selectedKey: null,
        draft: "",
        dirty: false,
        loading: false,
        error: describeError(error),
        mayHaveMore: false,
      };
    }
    render();
  }

  function selectKey(key: string): void {
    if (state === null) return;
    if (state.dirty && !confirmDiscard()) return;
    const row = state.rows.find((r) => r.key === key);
    state.selectedKey = key;
    state.draft = row ? json(row.document) : "";
    state.dirty = false;
    render();
  }

  function confirmDiscard(): boolean {
    return window.confirm("This document has unsaved edits. Discard them?");
  }

  async function save(): Promise<void> {
    const current = state;
    if (current === null || current.selectedKey === null) return;
    const node = store.node(current.nodeId);
    if (node === undefined) return;
    const key = current.selectedKey;

    let parsed: unknown;
    try {
      parsed = JSON.parse(current.draft);
    } catch (error) {
      // Caught here rather than at the node, so the message points at the
      // character rather than at "invalid JSON body".
      current.error = `Not valid JSON: ${describeError(error)}`;
      render();
      return;
    }

    try {
      await apiFor(node.process.api_port).putDocument(current.collection, key, parsed);
      current.dirty = false;
      current.error = "";
      const row = current.rows.find((r) => r.key === key);
      if (row !== undefined) row.document = parsed;
      store.toast("success", "Saved", `${current.collection}/${key}`);
      await refreshNode(current.nodeId, { quota: false });
    } catch (error) {
      current.error = describeError(error);
      store.toast("error", "Could not save the document", current.error);
    }
    render();
  }

  async function remove(): Promise<void> {
    if (state === null || state.selectedKey === null) return;
    const node = store.node(state.nodeId);
    if (node === undefined) return;
    const key = state.selectedKey;
    if (!window.confirm(`Delete ${state.collection}/${key}? Replicas converge on the deletion.`)) return;

    try {
      await apiFor(node.process.api_port).deleteDocument(state.collection, key);
      state.rows = state.rows.filter((r) => r.key !== key);
      state.selectedKey = state.rows.length > 0 ? state.rows[0].key : null;
      state.draft = state.selectedKey !== null ? json(state.rows[0].document) : "";
      state.dirty = false;
      store.toast("info", "Deleted", `${state.collection}/${key}`);
      await refreshNode(state.nodeId, { quota: false });
    } catch (error) {
      store.toast("error", "Could not delete the document", describeError(error));
    }
    render();
  }

  async function create(): Promise<void> {
    if (state === null) return;
    const node = store.node(state.nodeId);
    if (node === undefined) return;
    const key = window.prompt(`New document key in ${state.collection}`, "");
    if (key === null || key.trim() === "") return;

    try {
      await apiFor(node.process.api_port).putDocument(state.collection, key.trim(), {});
      await load(state.nodeId, state.collection);
      selectKey(key.trim());
      store.toast("success", "Created", `${state.collection}/${key.trim()}`);
      await refreshNode(state.nodeId, { quota: false });
    } catch (error) {
      store.toast("error", "Could not create the document", describeError(error));
    }
  }

  function keyList(current: ExplorerState): HTMLElement {
    const list = el("div", { class: "card explorer__keys", role: "listbox", "aria-label": "Document keys" });

    if (current.rows.length === 0) {
      list.appendChild(
        el("p", {
          class: "muted",
          text: current.loading ? "Reading…" : "No documents in this collection yet.",
        }),
      );
      return list;
    }

    for (const row of current.rows) {
      const button = el("button", {
        class: "explorer__key",
        type: "button",
        role: "option",
        "aria-selected": String(row.key === current.selectedKey),
        title: `${row.key} — ${documentPreview(row.document)}`,
        text: truncate(row.key, 60),
      });
      on(button, "click", () => selectKey(row.key));
      list.appendChild(button);
    }
    return list;
  }

  function pager(current: ExplorerState): HTMLElement {
    const back = el("button", {
      class: "btn btn--sm btn--ghost",
      type: "button",
      text: "Back",
      disabled: current.pageStack.length === 0,
    });
    on(back, "click", () => {
      const previous = current.pageStack.pop() ?? "";
      void load(current.nodeId, current.collection, previous);
    });

    const next = el("button", {
      class: "btn btn--sm btn--ghost",
      type: "button",
      text: "Next",
      disabled: !current.mayHaveMore,
    });
    on(next, "click", () => {
      const last = current.rows[current.rows.length - 1];
      if (last === undefined) return;
      const stack = [...current.pageStack, current.rows[0]?.key ?? ""];
      void load(current.nodeId, current.collection, `${last.key} `).then(() => {
        if (state !== null) state.pageStack = stack;
        render();
      });
    });

    return el(
      "div",
      { class: "row row--between" },
      el("span", {
        class: "muted",
        text: `${count(current.rows.length)} key${current.rows.length === 1 ? "" : "s"} on this page`,
      }),
      el("div", { class: "row" }, back, next),
    );
  }

  function editor(current: ExplorerState): HTMLElement {
    const node = store.node(current.nodeId);
    const detail = store.state.collectionDetails.get(`${current.nodeId}::${current.collection}`);
    const engine = detail?.engine ?? node?.brain?.collections.find((c) => c.name === current.collection)?.engine;

    if (current.selectedKey === null) {
      return el(
        "div",
        { class: "card" },
        el("p", { class: "empty__title", text: "No document selected" }),
        el("p", {
          class: "empty__body",
          text: "Pick a key on the left, or create one. New documents start as an empty object.",
        }),
      );
    }

    const area = el("textarea", {
      class: "explorer__doc",
      spellcheck: "false",
      "aria-label": `JSON for ${current.selectedKey}`,
    });
    area.value = current.draft;
    on(area, "input", () => {
      current.draft = area.value;
      current.dirty = true;
      // Only the footer's state changes on every keystroke; re-rendering the
      // whole pane here would move the caret.
      saveButton.disabled = false;
      dirtyNote.textContent = "Unsaved edits";
    });

    const saveButton = el("button", {
      class: "btn btn--primary btn--sm",
      type: "button",
      text: "Save",
      disabled: !current.dirty,
    });
    on(saveButton, "click", () => void save());

    const deleteButton = el("button", { class: "btn btn--danger btn--sm", type: "button", text: "Delete" });
    on(deleteButton, "click", () => void remove());

    const dirtyNote = el("span", { class: "muted", text: current.dirty ? "Unsaved edits" : "" });

    return el(
      "div",
      { class: "card stack" },
      el(
        "div",
        { class: "row row--between" },
        el("span", { class: "mono", text: current.selectedKey }),
        engine ? el("span", { class: "chip", text: engineLabel(engine) }) : null,
      ),
      area,
      current.error ? el("p", { class: "error-note", text: current.error }) : null,
      el("div", { class: "row row--between" }, dirtyNote, el("div", { class: "row" }, deleteButton, saveButton)),
    );
  }

  function render(): void {
    const selection = store.state.selection;
    if (selection.kind !== "collection" || !selection.nodeId || !selection.collection) {
      replace(element);
      return;
    }

    // The selection moved to a different collection: reload rather than show
    // the previous one's rows under the new one's heading.
    if (state === null || state.nodeId !== selection.nodeId || state.collection !== selection.collection) {
      void load(selection.nodeId, selection.collection);
      replace(
        element,
        el("div", { class: "card" }, el("div", { class: "skeleton", style: "height: 240px" })),
      );
      return;
    }

    const current = state;
    const newButton = el("button", { class: "btn btn--sm", type: "button" }, icon(Icons.plus, 13), "New document");
    on(newButton, "click", () => void create());

    const closeButton = el(
      "button",
      { class: "btn btn--sm btn--ghost", type: "button", title: "Back to the canvas" },
      icon(Icons.close, 13),
    );
    on(closeButton, "click", () => store.select({ kind: "node", nodeId: current.nodeId }));

    replace(
      element,
      el(
        "div",
        { class: "stack" },
        el(
          "div",
          { class: "row row--between" },
          el("strong", { text: current.collection }),
          el("div", { class: "row" }, newButton, closeButton),
        ),
        current.error && current.rows.length === 0
          ? el("p", { class: "error-note", text: current.error })
          : keyList(current),
        pager(current),
      ),
      editor(current),
    );
  }

  return { render, element };
}
