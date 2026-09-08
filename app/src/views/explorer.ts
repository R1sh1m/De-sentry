/**
 * The file explorer: documents in a collection, read and edited in place.
 *
 * Keys on the left, document on the right. Paging is by start key rather
 * than by offset.
 *
 * Upgraded with in-app modal sheets replacing browser prompt/confirm,
 * zero-dependency JSON syntax highlighting, formatting, and key search.
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

function showModalPrompt(title: string, message: string, defaultValue = ""): Promise<string | null> {
  return new Promise((resolve) => {
    const input = el("input", {
      type: "text",
      style: "width: 100%; font: var(--text-body); background: var(--color-surface-pearl); border: 1px solid var(--color-hairline); border-radius: var(--radius-sm); padding: 8px var(--space-sm); color: var(--color-ink);",
      value: defaultValue,
      placeholder: "e.g. user_101",
    }) as HTMLInputElement;

    const cancelBtn = el("button", { type: "button", class: "btn btn--ghost btn--sm", text: "Cancel" });
    const okBtn = el("button", { type: "button", class: "btn btn--primary btn--sm", text: "Create" });

    const scrim = el(
      "div",
      { class: "modal-scrim", role: "dialog", "aria-modal": "true" },
      el(
        "div",
        { class: "modal-box" },
        el("h3", { class: "modal-box__title", text: title }),
        el("p", { class: "modal-box__body", text: message }),
        input,
        el("div", { class: "modal-box__footer" }, cancelBtn, okBtn),
      ),
    );

    const close = (val: string | null) => {
      scrim.remove();
      resolve(val);
    };

    on(cancelBtn, "click", () => close(null));
    on(okBtn, "click", () => close(input.value.trim() || null));
    on(input, "keydown", (e) => {
      if (e.key === "Enter") close(input.value.trim() || null);
      else if (e.key === "Escape") close(null);
    });

    document.body.appendChild(scrim);
    input.focus();
  });
}

function showModalConfirm(title: string, message: string, confirmLabel = "Delete", isDanger = true): Promise<boolean> {
  return new Promise((resolve) => {
    const cancelBtn = el("button", { type: "button", class: "btn btn--ghost btn--sm", text: "Cancel" });
    const okBtn = el("button", {
      type: "button",
      class: isDanger ? "btn btn--danger btn--sm" : "btn btn--primary btn--sm",
      text: confirmLabel,
    });

    const scrim = el(
      "div",
      { class: "modal-scrim", role: "dialog", "aria-modal": "true" },
      el(
        "div",
        { class: "modal-box" },
        el("h3", { class: "modal-box__title", text: title }),
        el("p", { class: "modal-box__body", text: message }),
        el("div", { class: "modal-box__footer" }, cancelBtn, okBtn),
      ),
    );

    const close = (val: boolean) => {
      scrim.remove();
      resolve(val);
    };

    on(cancelBtn, "click", () => close(false));
    on(okBtn, "click", () => close(true));
    on(scrim, "keydown", (e) => {
      if (e.key === "Escape") close(false);
    });

    document.body.appendChild(scrim);
    okBtn.focus();
  });
}

function highlightJson(jsonStr: string): DocumentFragment {
  const frag = document.createDocumentFragment();
  const tokenRegex = /("(?:\\u[a-zA-Z0-9]{4}|\\[^u]|[^\\"])*"(\s*:)?|\b(true|false|null)\b|-?\d+(?:\.\d*)?(?:[eE][+-]?\d+)?|[{}[\],:])/g;

  let lastIndex = 0;
  let match: RegExpExecArray | null;

  while ((match = tokenRegex.exec(jsonStr)) !== null) {
    if (match.index > lastIndex) {
      frag.appendChild(document.createTextNode(jsonStr.slice(lastIndex, match.index)));
    }
    const token = match[0];
    const isKey = match[2] !== undefined;

    let cls = "syntax-punct";
    if (isKey) {
      cls = "syntax-key";
    } else if (token.startsWith('"')) {
      cls = "syntax-string";
    } else if (token === "true" || token === "false") {
      cls = "syntax-boolean";
    } else if (token === "null") {
      cls = "syntax-null";
    } else if (/^-?\d/.test(token)) {
      cls = "syntax-number";
    }

    const span = document.createElement("span");
    span.className = cls;
    span.textContent = token;
    frag.appendChild(span);

    lastIndex = tokenRegex.lastIndex;
  }

  if (lastIndex < jsonStr.length) {
    frag.appendChild(document.createTextNode(jsonStr.slice(lastIndex)));
  }

  return frag;
}

interface ExplorerState {
  nodeId: string;
  collection: string;
  rows: DocumentRow[];
  pageStack: string[];
  selectedKey: string | null;
  draft: string;
  dirty: boolean;
  loading: boolean;
  error: string;
  mayHaveMore: boolean;
  keyFilter: string;
  viewMode: "formatted" | "edit";
}

export interface ExplorerHandles {
  render(): void;
  element: HTMLElement;
}

export function createExplorer(): ExplorerHandles {
  const element = el("div", { class: "explorer" });

  let state: ExplorerState | null = null;

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
        keyFilter: "",
        viewMode: "formatted",
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
        keyFilter: "",
        viewMode: "formatted",
      };
    }
    render();
  }

  async function selectKey(key: string): Promise<void> {
    if (state === null) return;
    if (state.dirty) {
      const discard = await showModalConfirm("Unsaved Edits", "Discard unsaved changes to this document?", "Discard", true);
      if (!discard) return;
    }
    const row = state.rows.find((r) => r.key === key);
    state.selectedKey = key;
    state.draft = row ? json(row.document) : "";
    state.dirty = false;
    state.viewMode = "formatted";
    render();
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
      current.viewMode = "formatted";
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

    const confirmed = await showModalConfirm(
      "Delete Document",
      `Permanently delete "${state.collection}/${key}"? A CRDT tombstone will replicate this deletion to all peers.`,
      "Delete",
      true,
    );
    if (!confirmed) return;

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

    const key = await showModalPrompt("New Document", `Enter a unique key in collection "${state.collection}":`);
    if (key === null || key.trim() === "") return;

    try {
      await apiFor(node.process.api_port).putDocument(state.collection, key.trim(), {});
      await load(state.nodeId, state.collection);
      void selectKey(key.trim());
      store.toast("success", "Created", `${state.collection}/${key.trim()}`);
      await refreshNode(state.nodeId, { quota: false });
    } catch (error) {
      store.toast("error", "Could not create the document", describeError(error));
    }
  }

  function keyList(current: ExplorerState): HTMLElement {
    const list = el("div", { class: "card explorer__keys", role: "listbox", "aria-label": "Document keys" });

    const searchInput = el("input", {
      class: "explorer__search-input",
      type: "search",
      placeholder: "Filter keys…",
      value: current.keyFilter,
    }) as HTMLInputElement;

    on(searchInput, "input", () => {
      current.keyFilter = searchInput.value.trim().toLowerCase();
      renderKeysOnly();
    });

    const searchBox = el(
      "div",
      { class: "explorer__search" },
      el("span", { class: "explorer__search-icon" }, icon(Icons.search, 12)),
      searchInput,
    );

    const keysContainer = el("div", { class: "stack", style: "gap: 2px;" });

    function renderKeysOnly(): void {
      replace(keysContainer);
      const filtered = current.rows.filter((r) =>
        current.keyFilter ? r.key.toLowerCase().includes(current.keyFilter) : true,
      );

      if (filtered.length === 0) {
        keysContainer.appendChild(
          el("p", {
            class: "muted",
            style: "padding: var(--space-xs); font: var(--text-caption);",
            text: current.loading ? "Reading…" : current.keyFilter ? "No matching keys" : "No documents in this collection yet.",
          }),
        );
        return;
      }

      for (const row of filtered) {
        const button = el("button", {
          class: "explorer__key",
          type: "button",
          role: "option",
          "aria-selected": String(row.key === current.selectedKey),
          title: `${row.key} — ${documentPreview(row.document)}`,
          text: truncate(row.key, 60),
        });
        on(button, "click", () => void selectKey(row.key));
        keysContainer.appendChild(button);
      }
    }

    renderKeysOnly();
    list.appendChild(searchBox);
    list.appendChild(keysContainer);
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
      void load(current.nodeId, current.collection, `${last.key} `).then(() => {
        if (state !== null) state.pageStack = stack;
        render();
      });
    });

    return el(
      "div",
      { class: "row row--between" },
      el("span", {
        class: "muted",
        style: "font: var(--text-fine);",
        text: `${count(current.rows.length)} key${current.rows.length === 1 ? "" : "s"} on page`,
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
        { class: "card", style: "display: grid; place-items: center; min-height: 280px;" },
        el("p", { class: "empty__title", text: "No document selected" }),
        el("p", {
          class: "empty__body",
          text: "Pick a key on the left, or create one with + New document.",
        }),
      );
    }

    const modeBtn = el(
      "button",
      { class: "btn btn--sm btn--ghost", type: "button" },
      current.viewMode === "formatted" ? "Edit raw JSON" : "View formatted",
    );
    on(modeBtn, "click", () => {
      current.viewMode = current.viewMode === "formatted" ? "edit" : "formatted";
      render();
    });

    const formatBtn = el("button", { class: "btn btn--sm btn--ghost", type: "button", text: "Format" });
    on(formatBtn, "click", () => {
      try {
        current.draft = JSON.stringify(JSON.parse(current.draft), null, 2);
        current.dirty = true;
        render();
      } catch {
        // Leave unformatted if invalid
      }
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

    const dirtyNote = el("span", { class: "muted", style: "font: var(--text-fine);", text: current.dirty ? "Unsaved edits" : "" });

    let contentArea: HTMLElement;
    if (current.viewMode === "formatted" && !current.dirty) {
      contentArea = el("div", { class: "syntax-viewer" });
      contentArea.appendChild(highlightJson(current.draft));
    } else {
      const area = el("textarea", {
        class: "explorer__doc",
        spellcheck: "false",
        "aria-label": `JSON for ${current.selectedKey}`,
      }) as HTMLTextAreaElement;
      area.value = current.draft;
      on(area, "input", () => {
        current.draft = area.value;
        current.dirty = true;
        saveButton.disabled = false;
        dirtyNote.textContent = "Unsaved edits";
      });
      contentArea = area;
    }

    return el(
      "div",
      { class: "card stack" },
      el(
        "div",
        { class: "row row--between" },
        el("span", { class: "mono", style: "font-weight: 600;", text: current.selectedKey }),
        el(
          "div",
          { class: "row" },
          engine ? el("span", { class: "chip", text: engineLabel(engine) }) : null,
          formatBtn,
          modeBtn,
        ),
      ),
      contentArea,
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

    if (state === null || state.nodeId !== selection.nodeId || state.collection !== selection.collection) {
      void load(selection.nodeId, selection.collection);
      replace(
        element,
        el("div", { class: "card" }, el("div", { class: "skeleton", style: "height: 240px" })),
      );
      return;
    }

    const current = state;
    const newButton = el("button", { class: "btn btn--sm btn--primary", type: "button" }, icon(Icons.plus, 13), "New document");
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
          el("strong", { style: "font: var(--text-body-strong);", text: current.collection }),
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
