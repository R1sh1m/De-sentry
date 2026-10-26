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
import { sidecar } from "../bridge.js";
import { refreshNode, store } from "../state.js";
import { bytes, count, documentPreview, engineLabel, json, truncate } from "../util/format.js";
import { el, icon, Icons, on, replace } from "../util/dom.js";
import { emptyState } from "../util/empty.js";

const PAGE_SIZE = 200;
const ASSET_CHUNK_CHARS = 3500;
const MAX_ASSET_BYTES = 64 * 1024 * 1024;
const MAX_ASSET_CHUNKS = Math.ceil((Math.ceil(MAX_ASSET_BYTES / 3) * 4) / ASSET_CHUNK_CHARS);

/** Monotonic navigation id: stale page turns must not install their stacks. */
let loadGeneration = 0;
/** Invalidates part reads when the selected row or collection changes. */
let reassemblyGeneration = 0;

/** Pending key-filter debounce, shared so a re-render retires the orphan. */
let sharedFilterTimer = 0;

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

    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") {
        e.preventDefault();
        e.stopPropagation();
        close(null);
      }
    };
    window.addEventListener("keydown", onKey);
    const close = (val: string | null) => {
      window.removeEventListener("keydown", onKey);
      scrim.remove();
      resolve(val);
    };

    on(scrim, "click", (e) => {
      if (e.target === scrim) close(null);
    });
    on(cancelBtn, "click", () => close(null));
    on(okBtn, "click", () => close(input.value.trim() || null));
    on(input, "keydown", (e) => {
      if (e.key === "Enter") close(input.value.trim() || null);
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

    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") {
        e.preventDefault();
        e.stopPropagation();
        close(false);
      }
    };
    window.addEventListener("keydown", onKey);
    const close = (val: boolean) => {
      window.removeEventListener("keydown", onKey);
      scrim.remove();
      resolve(val);
    };

    on(scrim, "click", (e) => {
      if (e.target === scrim) close(false);
    });
    on(cancelBtn, "click", () => close(false));
    on(okBtn, "click", () => close(true));

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
  isChunked: boolean;
  chunkTotal: number;
  chunkField: string;
  reassembled: string | null;
  reassemblyError: string | null;
  reassemblyProgress: number;
  savingAsset: boolean;
  savedAssetPath: string | null;
  assetActionError: string | null;
}

export interface ExplorerHandles {
  render(): void;
  element: HTMLElement;
}

export function createExplorer(): ExplorerHandles {
  const element = el("div", { class: "explorer" });

  let state: ExplorerState | null = null;
  let lastFocusRequest: number | null = null;

  async function load(
    nodeId: string,
    collection: string,
    start = "",
    dropLeading = "",
  ): Promise<void> {
    const node = store.node(nodeId);
    if (node === undefined) return;
    const generation = ++loadGeneration;
    reassemblyGeneration++;
    if (state !== null && state.nodeId === nodeId && state.collection === collection) {
      state.loading = true;
      state.error = "";
      render();
    } else {
      // Install the new target before any render. Previously a node switch
      // kept the old node's state here, then render() saw the mismatch and
      // called load() again. Each call repeated that cycle synchronously,
      // spawning a burst of duplicate page requests against the new node.
      // Apart from leaving the skeleton stuck, that request storm could make
      // every node's API appear unreachable.
      const previous = state;
      const sameCollection = previous !== null && previous.collection === collection;
      state = {
        nodeId,
        collection,
        rows: [],
        pageStack: sameCollection ? [...previous.pageStack] : [],
        selectedKey: sameCollection ? previous.selectedKey : null,
        draft: "",
        dirty: false,
        loading: true,
        error: "",
        mayHaveMore: false,
        keyFilter: sameCollection ? previous.keyFilter : "",
        viewMode: sameCollection ? previous.viewMode : "formatted",
        isChunked: false,
        chunkTotal: 0,
        chunkField: "",
        reassembled: null,
        reassemblyError: null,
        reassemblyProgress: 0,
        savingAsset: false,
        savedAssetPath: null,
        assetActionError: null,
      };
    }

    try {
      const page = await apiFor(node.process.api_port).listDocuments(collection, {
        start: start || undefined,
        limit: PAGE_SIZE,
      });
      if (generation !== loadGeneration) return;
      // The engine's scan bound is inclusive: ask from the last visible key
      // and drop the repeated first row. (A trailing-space suffix hack was
      // tried before; it breaks on keys that genuinely end in a space.)
      const rows =
        dropLeading !== "" && page.documents[0]?.key === dropLeading
          ? page.documents.slice(1)
          : page.documents;
      // Preserve the selection across page turns when the key is still
      // visible; otherwise fall back to the first row. The filter text and
      // view mode are page chrome, not page content -- keep them too.
      const prev = state;
      const prevSelected =
        prev !== null && prev.collection === collection ? prev.selectedKey : null;
      const kept = prevSelected !== null ? rows.find((r) => r.key === prevSelected) : undefined;
      const requestedKey = store.state.selection.kind === "collection" &&
        store.state.selection.nodeId === nodeId &&
        store.state.selection.collection === collection &&
        store.state.selection.key === start
          ? store.state.selection.key
          : null;
      const selected = requestedKey !== null && rows.some((row) => row.key === requestedKey)
        ? requestedKey
        : kept !== undefined ? kept.key : (rows.length > 0 ? rows[0].key : null);
      const selectedRow = rows.find((r) => r.key === selected);
      state = {
        nodeId,
        collection,
        rows,
        pageStack: state !== null && state.collection === collection ? state.pageStack : [],
        selectedKey: selected,
        draft: selectedRow !== undefined ? json(selectedRow.document) : "",
        dirty: false,
        loading: false,
        error: "",
        mayHaveMore: page.documents.length === PAGE_SIZE,
        keyFilter: prev !== null && prev.collection === collection ? prev.keyFilter : "",
        viewMode: prev !== null && prev.collection === collection ? prev.viewMode : "formatted",
        isChunked: false,
        chunkTotal: 0,
        chunkField: "",
        reassembled: null,
        reassemblyError: null,
        reassemblyProgress: 0,
        savingAsset: false,
        savedAssetPath: null,
        assetActionError: null,
      };
      // If the selected row is a chunked manifest, reassemble its parts.
      const selectedDoc = selectedRow?.document as Record<string, unknown> | undefined;
      if (selectedRow && selectedDoc?.chunked === true && typeof selectedDoc.chunk_total === 'number') {
        state.isChunked = true;
        state.chunkTotal = selectedDoc.chunk_total as number;
        state.chunkField = typeof selectedDoc.chunk_field === 'string' ? (selectedDoc.chunk_field as string) : 'text';
        void reassemble(selectedRow);
      }
    } catch (error) {
      if (generation !== loadGeneration) return;
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
        isChunked: false,
        chunkTotal: 0,
        chunkField: "",
        reassembled: null,
        reassemblyError: null,
        reassemblyProgress: 0,
        savingAsset: false,
        savedAssetPath: null,
        assetActionError: null,
      };
    }
    render();
  }

  /** Fetches and validates ordered parts before enabling a reconstructed file. */
  async function reassemble(row: DocumentRow): Promise<void> {
    const current = state;
    if (current === null) return;
    const generation = ++reassemblyGeneration;
    const node = store.node(current.nodeId);
    if (node === undefined) return;

    const doc = row.document as Record<string, unknown>;
    const chunkTotal = typeof doc.chunk_total === 'number' ? doc.chunk_total : 0;
    const chunkField = typeof doc.chunk_field === 'string' ? doc.chunk_field : 'text';
    current.reassemblyProgress = 0;
    current.reassemblyError = null;
    current.reassembled = null;
    render();

    const stillSelected = (): boolean => state === current && current.selectedKey === row.key && generation === reassemblyGeneration;
    const failReassembly = (message: string): void => {
      if (!stillSelected()) return;
      current.reassemblyError = message;
      current.reassembled = null;
      current.reassemblyProgress = 0;
      render();
    };

    if (!Number.isSafeInteger(chunkTotal) || chunkTotal < 1 || chunkTotal > MAX_ASSET_CHUNKS) {
      failReassembly("This file’s manifest has an invalid or unsupported number of parts.");
      return;
    }
    if (!chunkField) {
      failReassembly("This file’s manifest does not identify its chunk data.");
      return;
    }

    const parts: string[] = [];
    const api = apiFor(node.process.api_port);
    const batchSize = 32;
    for (let start = 0; start < chunkTotal; start += batchSize) {
      const end = Math.min(start + batchSize, chunkTotal);
      const results = await Promise.all(Array.from({ length: end - start }, async (_, offset) => {
        const index = start + offset;
        try {
          const part = await api.getDocument(current.collection, `${row.key}#${index}`);
          return { index, part: part as Record<string, unknown>, error: null };
        } catch (error) {
          return { index, part: null, error };
        }
      }));
      if (!stillSelected()) return;
      for (const result of results) {
        const i = result.index;
        if (result.error !== null || result.part === null) {
          failReassembly(`Could not read part #${i} of ${chunkTotal}: ${describeError(result.error)}`);
          return;
        }
        const partDoc = result.part;
        if (
          partDoc.parent_key !== row.key ||
          partDoc.part_index !== i ||
          partDoc.part_total !== chunkTotal ||
          partDoc.field !== chunkField ||
          typeof partDoc.text !== 'string' ||
          partDoc.text.length === 0 ||
          partDoc.text.length > ASSET_CHUNK_CHARS ||
          (i < chunkTotal - 1 && partDoc.text.length !== ASSET_CHUNK_CHARS)
        ) {
          failReassembly(`Part #${i} does not match this file’s manifest. The file was not reconstructed.`);
          return;
        }
        parts.push(partDoc.text);
        current.reassemblyProgress = i + 1;
      }
      render();
    }

    const joined = parts.join('');
    if (chunkField === 'base64') {
      const expectedSize = doc.size_bytes;
      if (!Number.isSafeInteger(expectedSize) || (expectedSize as number) < 0 || (expectedSize as number) > MAX_ASSET_BYTES) {
        failReassembly("This file’s manifest has an invalid size. The file was not reconstructed.");
        return;
      }
      const expectedBase64Length = Math.ceil((expectedSize as number) / 3) * 4;
      if (joined.length !== expectedBase64Length || chunkTotal !== Math.ceil(joined.length / ASSET_CHUNK_CHARS)) {
        failReassembly("The reconstructed data length does not match the file manifest. The file was not saved.");
        return;
      }
    }
    if (!stillSelected()) return;
    current.reassembled = joined;
    current.reassemblyError = null;
    render();
  }

  async function saveAssetFile(
    current: ExplorerState,
    key: string,
    filename: string,
    base64Data: string,
    expectedSize: number,
    expectedSha256: string | undefined,
  ): Promise<void> {
    current.savingAsset = true;
    current.assetActionError = null;
    render();
    try {
      const path = await sidecar.saveAssetFile(filename, base64Data, expectedSize, expectedSha256);
      if (state !== current || current.selectedKey !== key) return;
      if (path !== null) {
        current.savedAssetPath = path;
        store.toast("success", "File reconstructed", `Saved ${filename} to ${path}.`, 5000);
      }
    } catch (error) {
      if (state !== current || current.selectedKey !== key) return;
      current.assetActionError = describeError(error);
      store.toast("error", "Could not save file", current.assetActionError, 5000);
    } finally {
      current.savingAsset = false;
      if (state === current && current.selectedKey === key) render();
    }
  }

  async function openSavedAsset(current: ExplorerState, reveal: boolean): Promise<void> {
    if (current.savedAssetPath === null) return;
    current.assetActionError = null;
    try {
      await sidecar.openSavedAsset(current.savedAssetPath, reveal);
      const locationName = store.state.appInfo?.platform === "macos" ? "Finder" : "your file manager";
      store.toast("success", reveal ? `File shown in ${locationName}` : "File opened", reveal ? `The reconstructed file is selected in ${locationName}.` : "The file was opened with your default app.", 4000);
    } catch (error) {
      current.assetActionError = describeError(error);
      store.toast("error", reveal ? "Could not show file" : "Could not open file", current.assetActionError, 5000);
      if (state === current) render();
    }
  }

  async function selectKey(key: string): Promise<void> {
    if (state === null) return;
    if (state.dirty) {
      const discard = await showModalConfirm("Unsaved Edits", "Discard unsaved changes to this document?", "Discard", true);
      if (!discard) return;
    }
    const row = state.rows.find((r) => r.key === key);
    reassemblyGeneration++;
    state.selectedKey = key;
    state.draft = row ? json(row.document) : "";
    state.dirty = false;
    state.viewMode = "formatted";

    // Reset chunked state for the newly selected row.
    state.isChunked = false;
    state.chunkTotal = 0;
    state.chunkField = "";
    state.reassembled = null;
    state.reassemblyError = null;
    state.reassemblyProgress = 0;
    state.savingAsset = false;
    state.savedAssetPath = null;
    state.assetActionError = null;

    // Detect a chunked manifest and reassemble its parts (read-only).
    const doc = row?.document as Record<string, unknown> | undefined;
    if (row && doc?.chunked === true && typeof doc.chunk_total === 'number') {
      state.isChunked = true;
      state.chunkTotal = doc.chunk_total as number;
      state.chunkField = typeof doc.chunk_field === 'string' ? (doc.chunk_field as string) : 'text';
      void reassemble(row);
      return;
    }

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
      placeholder: "Search all keys in collection...",
      title: "Search across the entire collection, not just the current page",
      value: current.keyFilter,
    }) as HTMLInputElement;

    // Debounced search across the entire collection via API
    window.clearTimeout(sharedFilterTimer);
    on(searchInput, "input", () => {
      window.clearTimeout(sharedFilterTimer);
      sharedFilterTimer = window.setTimeout(async () => {
        const query = searchInput.value.trim();
        current.keyFilter = query.toLowerCase();
        
        if (query) {
          // Search entire collection via API
          const node = store.node(current.nodeId);
          if (node !== undefined) {
            try {
              const api = apiFor(node.process.api_port);
              const results = await api.searchLookup(current.collection, query, PAGE_SIZE);
              // Replace current.rows with search results for rendering
              current.rows = results.map(hit => ({ key: hit.key, document: hit.document } as DocumentRow));
              current.mayHaveMore = results.length === PAGE_SIZE;
            } catch {
              // On error, fall back to empty results
              current.rows = [];
            }
          }
        } else {
          // Reload current page when search is cleared
          const startKey = current.pageStack.length > 0 ? current.pageStack[0] : "";
          await load(current.nodeId, current.collection, startKey);
          return; // load() calls render()
        }
        renderKeysOnly();
      }, 200);
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
        current.keyFilter
          ? `${r.key} ${String((r.document as Record<string, unknown> | undefined)?.filename ?? "")}`.toLowerCase().includes(current.keyFilter)
          : true,
      );

      if (filtered.length === 0) {
        keysContainer.appendChild(
          el("p", {
            class: "muted",
            style: "padding: var(--space-xs); font: var(--text-caption);",
            text: current.loading ? "Reading…" : current.keyFilter ? "No matching keys found" : "No documents in this collection yet.",
          }),
        );
        return;
      }

      for (const row of filtered) {
        const doc = row.document as Record<string, unknown> | undefined;
        const isChunked = doc?.chunked === true && typeof doc.chunk_total === 'number';
        const displayName = typeof doc?.filename === "string" ? doc.filename : row.key;
        const button = el("button", {
          class: "explorer__key",
          type: "button",
          role: "option",
          "aria-selected": String(row.key === current.selectedKey),
          title: `${displayName} (${row.key}) — ${documentPreview(row.document)}`,
        },
        el("span", { text: truncate(displayName, 60) }),
        isChunked ? el("span", {
          class: "badge",
          style: "margin-left: 8px; font-size: 10px; padding: 1px 5px;",
          text: `Chunked file (${doc.chunk_total} parts)`,
        }) : null);
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
    // Paging is locked while a load is in flight: rapid Next/Back clicks
    // otherwise interleave loads and cause stale stack updates.
    const busy = current.loading;
    const back = el("button", {
      class: "btn btn--sm btn--ghost",
      type: "button",
      text: "Back",
      disabled: busy || current.pageStack.length === 0,
    });
    on(back, "click", () => {
      // Clone the stack, pop the last entry, and update state before loading.
      const newStack = [...current.pageStack];
      const previous = newStack.pop() ?? "";
      // Update the state's pageStack so that load() preserves it.
      if (state !== null) state.pageStack = newStack;
      void load(current.nodeId, current.collection, previous);
    });

    const next = el("button", {
      class: "btn btn--sm btn--ghost",
      type: "button",
      text: "Next",
      disabled: busy || !current.mayHaveMore,
    });
    on(next, "click", () => {
      const last = current.rows[current.rows.length - 1];
      if (last === undefined) return;
      // Push the first key of the current page onto the stack.
      const newStack = [...current.pageStack, current.rows[0]?.key ?? ""];
      // Update state before loading the next page so load() retains the stack.
      if (state !== null) state.pageStack = newStack;
      // Inclusive bound + drop-first (see load()): correct for every key,
      // including ones ending in a space.
      void load(current.nodeId, current.collection, last.key, last.key);
    });

    // "N keys on page" says nothing about position; the from→to range does.
    // mayHaveMore is still heuristic (exactly PAGE_SIZE rows may be the end),
    // so Next on the last page harmlessly returns an empty page.
    const range =
      current.rows.length > 0
        ? `${truncate(current.rows[0].key, 18)} → ${truncate(current.rows[current.rows.length - 1].key, 18)}`
        : "empty page";
    // Explicit retention run: applies the collection's configured
    // retention_days immediately. The engine also performs periodic sweeps;
    // this button is useful when an operator wants no waiting period.
    const retain = el("button", {
      class: "btn btn--sm btn--ghost",
      type: "button",
      text: "Run retention",
      title: "Apply this collection's retention_days now (ts_rollup only)",
    });
    on(retain, "click", () => {
      const node = store.node(current.nodeId);
      if (node === undefined) return;
      retain.textContent = "Running…";
      (retain as HTMLButtonElement).disabled = true;
      void apiFor(node.process.api_port)
        .runRetention(current.collection)
        .then((result) => {
          store.toast("success", "Retention", `Dropped ${result.chunks_dropped} chunk(s) in ${current.collection}.`, 4000);
        })
        .catch((error: unknown) => {
          store.toast("error", "Retention", describeError(error), 5000);
        })
        .finally(() => {
          retain.textContent = "Run retention";
          (retain as HTMLButtonElement).disabled = false;
        });
    });
    return el(
      "div",
      { class: "row row--between" },
      el("span", {
        class: "muted",
        style: "font: var(--text-fine);",
        title: "Page-local range; the filter box below also applies to this page only",
        text: `${count(current.rows.length)} keys (${range})`,
      }),
      el("div", { class: "row" }, retain, back, next),
    );
  }

  function editor(current: ExplorerState): HTMLElement {
    const node = store.node(current.nodeId);
    const detail = store.state.collectionDetails.get(`${current.nodeId}::${current.collection}`);
    const engine = detail?.engine ?? node?.brain?.collections.find((c) => c.name === current.collection)?.engine;

    if (current.selectedKey === null) {
      return emptyState({ title: "No document selected", body: "Pick a key on the left, or create one with + New document." });
    }

    // If this is a chunked document, display the reassembled content read‑only.
    if (current.isChunked) {
      const manifest = current.rows.find((row) => row.key === current.selectedKey)?.document as Record<string, unknown> | undefined;
      const assetFilename = typeof manifest?.filename === "string" ? manifest.filename : "";
      const isBinaryAsset = assetFilename !== "" && current.chunkField === "base64";
      // Header row – same as non‑chunked but without edit controls.
      const header = el(
        "div",
        { class: "row row--between" },
        el("span", { class: "mono", style: "font-weight: 600;", text: current.selectedKey }),
        el(
          "div",
          { class: "row" },
          engine ? el("span", { class: "chip", text: engineLabel(engine) }) : null,
          // Show a badge indicating chunked status.
          el("span", { class: "badge", style: "margin-left: 8px; font-size: 10px; padding: 1px 5px;", text: `Chunked file (${current.chunkTotal} parts)` }),
        ),
      );

      // Content area – either the reassembled text, an error, or a loading placeholder.
      let contentArea: HTMLElement;
      if (current.reassemblyError) {
        contentArea = el("p", { class: "error-note", text: current.reassemblyError });
      } else if (isBinaryAsset && current.reassembled !== null) {
        const size = typeof manifest?.size_bytes === "number" ? bytes(manifest.size_bytes) : "Unknown size";
        const mime = typeof manifest?.mime === "string" ? manifest.mime : "application/octet-stream";
        const savedPath = current.savedAssetPath;
        contentArea = el("div", { class: "stack" },
          el("strong", { text: "Reconstructed file ready to save" }),
          el("p", { class: "muted", text: `${assetFilename} · ${size} · ${mime}` }),
          savedPath !== null
            ? el("p", { class: "muted", text: `Saved copy: ${savedPath}` })
            : el("p", { class: "muted", text: "Save a copy on this device to open it with your default app." }),
        );
      } else if (current.reassembled !== null) {
        // Use a <pre> to preserve whitespace; make it read‑only.
        contentArea = el("pre", { class: "explorer__doc", style: "white-space: pre-wrap;" }, current.reassembled);
      } else {
        contentArea = el("p", {
          class: "muted",
          text: isBinaryAsset
            ? `Reconstructing file parts (${current.reassemblyProgress}/${current.chunkTotal})…`
            : `Reassembling parts (${current.reassemblyProgress}/${current.chunkTotal})…`,
        });
      }

      // The manifest is read-only; reconstruction actions stay beside the file status.
      const deleteButton = el("button", { class: "btn btn--danger btn--sm", type: "button", text: "Delete" });
      on(deleteButton, "click", () => void remove());

      const expectedSize = manifest?.size_bytes;
      const validSize = typeof expectedSize === "number" && Number.isSafeInteger(expectedSize) && expectedSize >= 0 && expectedSize <= MAX_ASSET_BYTES;
      const expectedSha256 = typeof manifest?.sha256 === "string" ? manifest.sha256 : undefined;
      const downloadButton = isBinaryAsset
        ? el("button", {
            class: "btn btn--primary btn--sm",
            type: "button",
            text: current.savingAsset ? "Saving…" : current.savedAssetPath === null ? "Save a copy…" : "Save another copy…",
            disabled: current.reassembled === null || !validSize || current.savingAsset,
          })
        : null;
      if (downloadButton) on(downloadButton, "click", () => {
        if (current.reassembled !== null && validSize) {
          void saveAssetFile(current, current.selectedKey!, assetFilename, current.reassembled, expectedSize as number, expectedSha256);
        }
      });
      const openButton = current.savedAssetPath !== null
        ? el("button", { class: "btn btn--sm", type: "button", text: "Open file" })
        : null;
      if (openButton) on(openButton, "click", () => void openSavedAsset(current, false));
      const showInFolderButton = current.savedAssetPath !== null
        ? el("button", { class: "btn btn--sm btn--ghost", type: "button", text: store.state.appInfo?.platform === "macos" ? "Show in Finder" : "Show in folder" })
        : null;
      if (showInFolderButton) on(showInFolderButton, "click", () => void openSavedAsset(current, true));

      return el(
        "div",
        { class: "card stack" },
        header,
        contentArea,
        current.assetActionError ? el("p", { class: "error-note", text: current.assetActionError }) : null,
        // Show any generic error (e.g., from loading the manifest) below the content.
        current.error ? el("p", { class: "error-note", text: current.error }) : null,
        el("div", { class: "row row--between" }, el("span", { class: "muted", text: `Stored in ${current.collection} on the selected node.` }), el("div", { class: "row" }, downloadButton, openButton, showInFolderButton, deleteButton)),
      );
    }

    // Non‑chunked document handling (existing implementation).
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

    const selectedDoc = current.rows.find((row) => row.key === current.selectedKey)?.document as Record<string, unknown> | undefined;
    const assetFilename = typeof selectedDoc?.filename === "string" ? selectedDoc.filename : "";
    const isBinaryAsset = assetFilename !== "" && typeof selectedDoc?.base64 === "string";
    const assetSize = selectedDoc?.size_bytes;
    const validAssetSize = typeof assetSize === "number" && Number.isSafeInteger(assetSize) && assetSize >= 0 && assetSize <= MAX_ASSET_BYTES;
    const assetSha256 = typeof selectedDoc?.sha256 === "string" ? selectedDoc.sha256 : undefined;
    const downloadButton = isBinaryAsset
      ? el("button", {
          class: "btn btn--sm btn--primary",
          type: "button",
          text: current.savingAsset ? "Saving…" : current.savedAssetPath === null ? "Save a copy…" : "Save another copy…",
          disabled: !validAssetSize || current.savingAsset,
        })
      : null;
    if (downloadButton && isBinaryAsset && typeof selectedDoc?.base64 === "string") {
      on(downloadButton, "click", () => {
        if (validAssetSize) void saveAssetFile(current, current.selectedKey!, assetFilename, selectedDoc.base64 as string, assetSize as number, assetSha256);
      });
    }

    let contentArea: HTMLElement;
    if (isBinaryAsset) {
      const size = validAssetSize ? bytes(assetSize as number) : "Unknown size";
      const mime = typeof selectedDoc?.mime === "string" ? selectedDoc.mime : "application/octet-stream";
      contentArea = el("div", { class: "stack" },
        el("strong", { text: "Reconstructed file ready to save" }),
        el("p", { class: "muted", text: `${assetFilename} · ${size} · ${mime}` }),
        current.savedAssetPath !== null
          ? el("p", { class: "muted", text: `Saved copy: ${current.savedAssetPath}` })
          : el("p", { class: "muted", text: "Save a copy here to open it with your default app." }),
      );
    } else if (current.viewMode === "formatted" && !current.dirty) {
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

    const openButton = isBinaryAsset && current.savedAssetPath !== null
      ? el("button", { class: "btn btn--sm", type: "button", text: "Open file" })
      : null;
    if (openButton) on(openButton, "click", () => void openSavedAsset(current, false));
    const showInFolderButton = isBinaryAsset && current.savedAssetPath !== null
      ? el("button", { class: "btn btn--sm btn--ghost", type: "button", text: store.state.appInfo?.platform === "macos" ? "Show in Finder" : "Show in folder" })
      : null;
    if (showInFolderButton) on(showInFolderButton, "click", () => void openSavedAsset(current, true));

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
          downloadButton,
          formatBtn,
          modeBtn,
        ),
      ),
      contentArea,
      current.assetActionError ? el("p", { class: "error-note", text: current.assetActionError }) : null,
      current.error ? el("p", { class: "error-note", text: current.error }) : null,
      el("div", { class: "row row--between" }, dirtyNote, el("div", { class: "row" }, downloadButton, openButton, showInFolderButton, deleteButton, saveButton)),
    );
  }

  function render(): void {
    const selection = store.state.selection;
    if (selection.kind !== "collection" || !selection.nodeId || !selection.collection) {
      replace(element);
      return;
    }

    if (state === null || state.nodeId !== selection.nodeId || state.collection !== selection.collection) {
      lastFocusRequest = selection.focusRequest ?? null;
      void load(selection.nodeId, selection.collection, selection.key ?? "");
      replace(
        element,
        el("div", { class: "card" }, el("div", { class: "skeleton", style: "height: 240px" })),
      );
      return;
    }

    if (selection.key && selection.focusRequest !== undefined && selection.focusRequest !== lastFocusRequest) {
      lastFocusRequest = selection.focusRequest;
      if (!state.loading && state.rows.some((row) => row.key === selection.key)) {
        void selectKey(selection.key);
      } else {
        void load(selection.nodeId, selection.collection, selection.key);
      }
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
