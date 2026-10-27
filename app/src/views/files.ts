/**
 * Universal File Browser.
 *
 * A cross-collection file browser that lets the user search, filter, sort,
 * and preview files stored across the mesh. Supports grid and list views,
 * a collapsible filter panel, and a recent-files sidebar.
 */

import { apiFor, ApiError } from "../api.js";
import { store } from "../state.js";
import { el, icon, Icons, on, replace } from "../util/dom.js";
import { truncate } from "../util/format.js";
import { emptyState } from "../util/empty.js";
import { openFilePreview } from "./file-preview.js";
import { getFileIcon, getFileCategory, categoryLabel } from "../file-types.js";

// -- types -------------------------------------------------------------------

export interface FileItem {
  collection: string;
  key: string;
  filename: string;
  mime: string;
  size_bytes: number;
  sha256: string;
  created_ms: number;
  nodePort: number;
}

export interface FileFilters {
  search: string;
  fileType: string;
  sizeFilter: string;
  collection: string;
  dateFilter: string;
  sort: string;
  viewMode: "grid" | "list";
}

export interface FileListResponse {
  files: FileItem[];
  total: number;
  hasMore: boolean;
}

// -- helpers -----------------------------------------------------------------

function describeError(error: unknown): string {
  if (error instanceof ApiError) return error.message;
  if (error instanceof Error) return error.message;
  return String(error);
}

function formatFileSize(b: number): string {
  if (b < 1024) return `${b} B`;
  if (b < 1024 * 1024) return `${(b / 1024).toFixed(1)} KB`;
  if (b < 1024 * 1024 * 1024) return `${(b / (1024 * 1024)).toFixed(1)} MB`;
  return `${(b / (1024 * 1024 * 1024)).toFixed(2)} GB`;
}

function formatRelativeTime(ms: number): string {
  const delta = Date.now() - ms;
  if (delta < 60_000) return "just now";
  if (delta < 3_600_000) return `${Math.floor(delta / 60_000)} min ago`;
  if (delta < 86_400_000) return `${Math.floor(delta / 3_600_000)} hours ago`;
  if (delta < 604_800_000) return `${Math.floor(delta / 86_400_000)} days ago`;
  if (delta < 2_592_000_000) return `${Math.floor(delta / 604_800_000)} weeks ago`;
  return `${Math.floor(delta / 2_592_000_000)} months ago`;
}

// -- state -------------------------------------------------------------------

interface FilesState {
  files: FileItem[];
  recentFiles: FileItem[];
  loading: boolean;
  error: string;
  filters: FileFilters;
  page: number;
  hasMore: boolean;
  collections: string[];
  filterPanelOpen: boolean;
}

// -- view --------------------------------------------------------------------

export interface FilesHandles {
  activate(force?: boolean): void;
  render(): void;
  element: HTMLElement;
}

export function createFiles(): FilesHandles {
  const element = el("div", { class: "canvas files-view" });

  let state: FilesState = {
    files: [],
    recentFiles: [],
    loading: false,
    error: "",
    filters: {
      search: "",
      fileType: "all",
      sizeFilter: "any",
      collection: "all",
      dateFilter: "anytime",
      sort: "newest",
      viewMode: "grid",
    },
    page: 0,
    hasMore: false,
    collections: [],
    filterPanelOpen: false,
  };

  let searchTimer = 0;
  let lastActivationKey: string | null = null;

  function activate(force = false): void {
    const availableNodes = store.dataNodes().filter(
      (node) => node.reachable && node.process.process === "running",
    );
    const activationKey = availableNodes
      .map((node) => `${node.process.node_id}:${node.process.api_port}`)
      .join("|");
    if (!force && activationKey === lastActivationKey) return;
    lastActivationKey = activationKey;
    state.page = 0;
    void fetchFiles();
    void fetchRecentFiles();
  }

  // -- API calls ---------------------------------------------------------------

  async function fetchFiles(): Promise<void> {
    const nodes = store.dataNodes().filter((n) => n.reachable && n.process.process === "running");
    if (nodes.length === 0) {
      state.error = "No running node available.";
      state.loading = false;
      render();
      return;
    }

    state.loading = true;
    state.error = "";
    render();

    try {
      const params = new URLSearchParams();
      if (state.filters.search) params.set("q", state.filters.search);
      if (state.filters.fileType !== "all") params.set("type", state.filters.fileType);
      if (state.filters.sizeFilter !== "any") params.set("size", state.filters.sizeFilter);
      if (state.filters.collection !== "all") params.set("collection", state.filters.collection);
      if (state.filters.dateFilter !== "anytime") params.set("date", state.filters.dateFilter);
      params.set("sort", state.filters.sort);
      // Query each running node. Assets can exist on a different local node,
      // and the first node in the tree is not necessarily the file's holder.
      const pageSize = 50;
      params.set("offset", String(state.page * pageSize));
      params.set("limit", String(pageSize));
      const batches = await Promise.all(nodes.map(async (node) => {
        try {
          const res = await fetch(`http://127.0.0.1:${node.process.api_port}/_files?${params}`, {
            headers: await getAuthHeaders(),
          });
          if (!res.ok) throw new Error(`File list returned ${res.status}`);
          const data = await res.json() as { files?: FileItem[]; total?: number; has_more?: boolean; hasMore?: boolean };
          return {
            node,
            files: (data.files ?? []).map((file) => ({ ...file, nodePort: node.process.api_port })),
            hasMore: data.has_more ?? data.hasMore ?? false,
          };
        } catch {
          const files = await buildFilesFromCollections(node);
          return { node, files, hasMore: false };
        }
      }));

      const merged = new Map<string, FileItem>();
      for (const batch of batches) {
        for (const file of batch.files) {
          const identity = `${file.collection}\u0000${file.key}`;
          if (!merged.has(identity)) merged.set(identity, file);
        }
      }
      const pageFiles = [...merged.values()];
      if (state.page === 0) state.files = pageFiles;
      else {
        const existing = new Map(state.files.map((file) => [`${file.collection}\u0000${file.key}`, file]));
        for (const file of pageFiles) {
          const identity = `${file.collection}\u0000${file.key}`;
          if (!existing.has(identity)) existing.set(identity, file);
        }
        state.files = [...existing.values()];
      }
      state.hasMore = batches.some((batch) => batch.hasMore);
      state.collections = [...new Set(nodes.flatMap((node) => node.brain?.collections.map((c) => c.name) ?? []))];
    } catch (err) {
      state.error = describeError(err);
      // Try fallback
      const node = nodes[0];
      if (node) {
        state.files = await buildFilesFromCollections(node);
        state.hasMore = false;
      }
    } finally {
      state.loading = false;
      render();
    }
  }

  async function getAuthHeaders(): Promise<Record<string, string>> {
    try {
      const { sidecar } = await import("../bridge.js");
      const token = await sidecar.apiToken();
      return token ? { Authorization: "Bearer " + token } : {};
    } catch {
      return {};
    }
  }

  async function buildFilesFromCollections(node: { process: { api_port: number } }): Promise<FileItem[]> {
    // Fallback: iterate collections and list documents
    const files: FileItem[] = [];
    const brain = store.dataNodes().find((n) => n.process.api_port === node.process.api_port)?.brain;
    if (!brain) return [];

    for (const col of brain.collections) {
      try {
        const api = apiFor(node.process.api_port);
        const page = await api.listDocuments(col.name, { limit: 100 });
        for (const doc of page.documents) {
          const d = doc.document as Record<string, unknown> | undefined;
          if (d && typeof d.filename === "string") {
            files.push({
              collection: col.name,
              key: doc.key,
              filename: d.filename as string,
              mime: (d.mime as string) ?? "application/octet-stream",
              size_bytes: (d.size_bytes as number) ?? 0,
              sha256: (d.sha256 as string) ?? "",
              created_ms: (d.created_ms as number) ?? Date.now(),
              nodePort: node.process.api_port,
            });
          }
        }
      } catch {
        // Skip collections that fail
      }
    }
    return files;
  }

  async function fetchRecentFiles(): Promise<void> {
    const node = store.dataNodes().find((n) => n.reachable && n.process.process === "running");
    if (!node) return;

    try {
      const res = await fetch(`http://127.0.0.1:${node.process.api_port}/_files/recent`, {
        headers: await getAuthHeaders(),
      });
      if (res.ok) {
        const data = await res.json() as { files?: FileItem[] };
        state.recentFiles = (data.files ?? []).map((f) => ({ ...f, nodePort: node.process.api_port }));
      }
    } catch {
      // Recent files are supplementary
    }
  }

  // -- filtering & sorting -----------------------------------------------------

  function applyFilters(files: FileItem[]): FileItem[] {
    let result = [...files];

    // Search filter
    if (state.filters.search) {
      const q = state.filters.search.toLowerCase();
      result = result.filter((f) =>
        f.filename.toLowerCase().includes(q) ||
        f.key.toLowerCase().includes(q) ||
        f.collection.toLowerCase().includes(q),
      );
    }

    // File type filter
    if (state.filters.fileType !== "all") {
      result = result.filter((f) => {
        const cat = getFileCategory(f.mime, f.filename);
        return cat === state.filters.fileType;
      });
    }

    // Size filter
    if (state.filters.sizeFilter !== "any") {
      result = result.filter((f) => {
        switch (state.filters.sizeFilter) {
          case "small": return f.size_bytes < 1024 * 1024;
          case "medium": return f.size_bytes >= 1024 * 1024 && f.size_bytes <= 10 * 1024 * 1024;
          case "large": return f.size_bytes > 10 * 1024 * 1024;
          default: return true;
        }
      });
    }

    // Collection filter
    if (state.filters.collection !== "all") {
      result = result.filter((f) => f.collection === state.filters.collection);
    }

    // Date filter
    if (state.filters.dateFilter !== "anytime") {
      const now = Date.now();
      result = result.filter((f) => {
        const age = now - f.created_ms;
        switch (state.filters.dateFilter) {
          case "today": return age < 86_400_000;
          case "week": return age < 604_800_000;
          case "month": return age < 2_592_000_000;
          case "year": return age < 31_536_000_000;
          default: return true;
        }
      });
    }

    // Sort
    switch (state.filters.sort) {
      case "newest":
        result.sort((a, b) => b.created_ms - a.created_ms);
        break;
      case "oldest":
        result.sort((a, b) => a.created_ms - b.created_ms);
        break;
      case "largest":
        result.sort((a, b) => b.size_bytes - a.size_bytes);
        break;
      case "smallest":
        result.sort((a, b) => a.size_bytes - b.size_bytes);
        break;
      case "name":
        result.sort((a, b) => a.filename.localeCompare(b.filename));
        break;
    }

    return result;
  }

  // -- render helpers ----------------------------------------------------------

  function renderFileCard(file: FileItem): HTMLElement {
    const card = el("div", { class: "files-view__file-card" });

    const iconEl = el("div", { class: "files-view__file-card-icon" }, getFileIcon(file.mime, file.filename));
    const nameEl = el("div", { class: "mono files-view__file-card-name", title: file.filename, text: file.filename });
    const sizeEl = el("div", { class: "muted files-view__file-card-size", text: formatFileSize(file.size_bytes) });
    const badgeEl = el("span", { class: "badge files-view__file-card-badge", text: file.collection });
    const dateEl = el("div", { class: "muted files-view__file-card-date", text: formatRelativeTime(file.created_ms) });

    const actions = el("div", { class: "files-view__file-card-actions", "data-actions": "true" });

    const previewBtn = el("button", { class: "btn btn--sm btn--ghost", type: "button", title: "Preview" }, icon(Icons.search, 12));
    on(previewBtn, "click", (e) => {
      e.stopPropagation();
      openFilePreview({
        collection: file.collection,
        key: file.key,
        filename: file.filename,
        mime: file.mime,
        size_bytes: file.size_bytes,
        sha256: file.sha256,
        created_ms: file.created_ms,
        nodePort: file.nodePort,
      });
    });

    const downloadBtn = el("button", { class: "btn btn--sm btn--ghost", type: "button", title: "Download" }, icon(Icons.copy, 12));
    on(downloadBtn, "click", (e) => {
      e.stopPropagation();
      // Trigger download via stream
      void downloadFile(file);
    });

    actions.appendChild(previewBtn);
    actions.appendChild(downloadBtn);

    card.appendChild(iconEl);
    card.appendChild(nameEl);
    card.appendChild(sizeEl);
    card.appendChild(badgeEl);
    card.appendChild(dateEl);
    card.appendChild(actions);

    // Hover shows actions
    on(card, "mouseenter", () => {
      actions.style.opacity = "1";
    });
    on(card, "mouseleave", () => {
      actions.style.opacity = "0";
    });

    // Double-click to preview
    on(card, "dblclick", () => {
      openFilePreview({
        collection: file.collection,
        key: file.key,
        filename: file.filename,
        mime: file.mime,
        size_bytes: file.size_bytes,
        sha256: file.sha256,
        created_ms: file.created_ms,
        nodePort: file.nodePort,
      });
    });

    return card;
  }

  async function downloadFile(file: FileItem): Promise<void> {
    try {
      const token = await (await import("../bridge.js")).sidecar.apiToken();
      const res = await fetch(`http://127.0.0.1:${file.nodePort}/_files/${encodeURIComponent(file.collection)}/${encodeURIComponent(file.key)}/stream`, {
        headers: token ? { Authorization: "Bearer " + token } : {},
      });
      if (!res.ok) {
        const detail = (await res.text()).trim();
        throw new Error(`Download failed (${res.status})${detail ? `: ${detail.slice(0, 240)}` : ""}`);
      }
      const blob = await res.blob();
      const base64 = await new Promise<string>((resolve, reject) => {
        const reader = new FileReader();
        reader.onload = () => {
          if (typeof reader.result !== "string") {
            reject(new Error("File encoding produced no data."));
            return;
          }
          const separator = reader.result.indexOf(",");
          if (separator < 0) {
            reject(new Error("File encoding produced an invalid data URL."));
            return;
          }
          resolve(reader.result.slice(separator + 1));
        };
        reader.onerror = () => reject(reader.error ?? new Error("File read failed."));
        reader.readAsDataURL(blob);
      });
      const path = await (await import("../bridge.js")).sidecar.saveAssetFile(file.filename, base64, file.size_bytes, file.sha256);
      if (path) store.toast("success", "Downloaded", `Saved ${file.filename} to ${path}.`);
    } catch (err) {
      store.toast("error", "Download failed", describeError(err));
    }
  }

  function renderFileRow(file: FileItem): HTMLElement {
    const row = el("tr", { class: "files-view__file-row" });

    const nameCell = el("td", {},
      el("span", { class: "files-view__file-row-icon", text: getFileIcon(file.mime, file.filename) }),
      el("span", { class: "mono", text: truncate(file.filename, 40) }),
    );
    const typeCell = el("td", { class: "muted", text: categoryLabel(getFileCategory(file.mime, file.filename)) });
    const sizeCell = el("td", { class: "muted", text: formatFileSize(file.size_bytes) });
    const collectionCell = el("td", {}, el("span", { class: "badge files-view__file-row-badge", text: file.collection }));
    const modifiedCell = el("td", { class: "muted", text: formatRelativeTime(file.created_ms) });

    const actionsCell = el("td", { class: "row files-view__file-row-actions" });
    const previewBtn = el("button", { class: "btn btn--sm btn--ghost", type: "button", title: "Preview" }, icon(Icons.search, 12));
    on(previewBtn, "click", (e) => {
      e.stopPropagation();
      openFilePreview({
        collection: file.collection,
        key: file.key,
        filename: file.filename,
        mime: file.mime,
        size_bytes: file.size_bytes,
        sha256: file.sha256,
        created_ms: file.created_ms,
        nodePort: file.nodePort,
      });
    });
    const downloadBtn = el("button", { class: "btn btn--sm btn--ghost", type: "button", title: "Download" }, icon(Icons.copy, 12));
    on(downloadBtn, "click", (e) => {
      e.stopPropagation();
      void downloadFile(file);
    });
    actionsCell.appendChild(previewBtn);
    actionsCell.appendChild(downloadBtn);

    row.appendChild(nameCell);
    row.appendChild(typeCell);
    row.appendChild(sizeCell);
    row.appendChild(collectionCell);
    row.appendChild(modifiedCell);
    row.appendChild(actionsCell);

    on(row, "dblclick", () => {
      openFilePreview({
        collection: file.collection,
        key: file.key,
        filename: file.filename,
        mime: file.mime,
        size_bytes: file.size_bytes,
        sha256: file.sha256,
        created_ms: file.created_ms,
        nodePort: file.nodePort,
      });
    });

    return row;
  }

  function renderRecentFiles(): HTMLElement {
    const section = el("div", { class: "card files-view__recent-section" });
    const header = el("div", { class: "row row--between files-view__recent-section-header" },
      el("strong", { text: "Recent Files" }),
      el("button", { class: "btn btn--sm btn--ghost", type: "button", text: "Clear" }),
    );
    const clearBtn = header.querySelector("button") as HTMLButtonElement;
    on(clearBtn, "click", () => {
      state.recentFiles = [];
      render();
    });

    section.appendChild(header);

    if (state.recentFiles.length === 0) {
      section.appendChild(el("p", { class: "muted files-view__recent-section-empty", text: "No recent files." }));
      return section;
    }

    const list = el("div", { class: "stack files-view__recent-section-list" });
    for (const file of state.recentFiles.slice(0, 20)) {
      const item = el("div", { class: "row files-view__recent-section-item" },
        el("span", { text: getFileIcon(file.mime, file.filename) }),
        el("span", { class: "mono files-view__recent-section-item-name", text: file.filename }),
        el("span", { class: "muted files-view__recent-section-item-time", text: formatRelativeTime(file.created_ms) }),
      );
      on(item, "click", () => {
        openFilePreview({
          collection: file.collection,
          key: file.key,
          filename: file.filename,
          mime: file.mime,
          size_bytes: file.size_bytes,
          sha256: file.sha256,
          created_ms: file.created_ms,
          nodePort: file.nodePort,
        });
      });
      list.appendChild(item);
    }
    section.appendChild(list);
    return section;
  }

  // -- main render -------------------------------------------------------------

  function render(): void {
    const filtered = applyFilters(state.files);

    // Title
    const title = el("h2", { class: "files-view__title", text: "File Browser" });

    // Search bar
    const searchInput = el("input", {
      class: "input",
      type: "search",
      placeholder: "Search files...",
      value: state.filters.search,
    }) as HTMLInputElement;

    on(searchInput, "input", () => {
      window.clearTimeout(searchTimer);
      searchTimer = window.setTimeout(() => {
        state.filters.search = searchInput.value;
        state.page = 0;
        void fetchFiles();
      }, 300);
    });

    const searchBox = el("div", { class: "files-view__search" },
      el("span", { class: "files-view__search-icon" }, icon(Icons.search, 14)),
      searchInput,
    );

    // Filter toggle
    const filterToggle = el("button", { class: "btn btn--sm btn--ghost", type: "button", text: state.filterPanelOpen ? "Hide Filters" : "Show Filters" });
    on(filterToggle, "click", () => {
      state.filterPanelOpen = !state.filterPanelOpen;
      render();
    });

    // Sort dropdown
    const sortSelect = el("select", { class: "select-rounded files-view__sort-select" }) as HTMLSelectElement;
    const sortOptions = [
      { value: "newest", label: "Newest" },
      { value: "oldest", label: "Oldest" },
      { value: "largest", label: "Largest" },
      { value: "smallest", label: "Smallest" },
      { value: "name", label: "Name (A-Z)" },
    ];
    for (const opt of sortOptions) {
      const o = el("option", { value: opt.value, text: opt.label }) as HTMLOptionElement;
      if (opt.value === state.filters.sort) o.selected = true;
      sortSelect.appendChild(o);
    }
    on(sortSelect, "change", () => {
      state.filters.sort = sortSelect.value;
      state.page = 0;
      void fetchFiles();
    });

    // View toggle (segmented control)
    const gridBtn = el("button", { class: "btn btn--sm", type: "button", text: "Grid", "aria-pressed": state.filters.viewMode === "grid" ? "true" : "false" });
    const listBtn = el("button", { class: "btn btn--sm", type: "button", text: "List", "aria-pressed": state.filters.viewMode === "list" ? "true" : "false" });
    on(gridBtn, "click", () => {
      state.filters.viewMode = "grid";
      gridBtn.setAttribute("aria-pressed", "true");
      listBtn.setAttribute("aria-pressed", "false");
      render();
    });
    on(listBtn, "click", () => {
      state.filters.viewMode = "list";
      gridBtn.setAttribute("aria-pressed", "false");
      listBtn.setAttribute("aria-pressed", "true");
      render();
    });

    const toolbar = el("div", { class: "files-view__toolbar" },
      el("div", { class: "files-view__toolbar-group" }, filterToggle, sortSelect),
      el("div", { class: "files-view__toolbar-group" }, el("div", { class: "segmented" }, gridBtn, listBtn)),
    );

    // Filter panel (collapsible)
    let filterPanel: HTMLElement | null = null;
    if (state.filterPanelOpen) {
      const fileTypeSelect = el("select", { class: "select-rounded" }) as HTMLSelectElement;
      const fileTypes = [
        { value: "all", label: "All" },
        { value: "document", label: "Documents" },
        { value: "image", label: "Images" },
        { value: "pdf", label: "PDFs" },
        { value: "spreadsheet", label: "Spreadsheets" },
        { value: "other", label: "Other" },
      ];
      for (const ft of fileTypes) {
        const o = el("option", { value: ft.value, text: ft.label }) as HTMLOptionElement;
        if (ft.value === state.filters.fileType) o.selected = true;
        fileTypeSelect.appendChild(o);
      }
      on(fileTypeSelect, "change", () => {
        state.filters.fileType = fileTypeSelect.value;
        state.page = 0;
        void fetchFiles();
      });

      const sizeSelect = el("select", { class: "select-rounded" }) as HTMLSelectElement;
      const sizes = [
        { value: "any", label: "Any" },
        { value: "small", label: "< 1MB" },
        { value: "medium", label: "1-10MB" },
        { value: "large", label: "> 10MB" },
      ];
      for (const s of sizes) {
        const o = el("option", { value: s.value, text: s.label }) as HTMLOptionElement;
        if (s.value === state.filters.sizeFilter) o.selected = true;
        sizeSelect.appendChild(o);
      }
      on(sizeSelect, "change", () => {
        state.filters.sizeFilter = sizeSelect.value;
        state.page = 0;
        void fetchFiles();
      });

      const collectionSelect = el("select", { class: "select-rounded" }) as HTMLSelectElement;
      const allCol = el("option", { value: "all", text: "All Collections" }) as HTMLOptionElement;
      if (state.filters.collection === "all") allCol.selected = true;
      collectionSelect.appendChild(allCol);
      for (const col of state.collections) {
        const o = el("option", { value: col, text: col }) as HTMLOptionElement;
        if (col === state.filters.collection) o.selected = true;
        collectionSelect.appendChild(o);
      }
      on(collectionSelect, "change", () => {
        state.filters.collection = collectionSelect.value;
        state.page = 0;
        void fetchFiles();
      });

      const dateSelect = el("select", { class: "select-rounded" }) as HTMLSelectElement;
      const dates = [
        { value: "anytime", label: "Anytime" },
        { value: "today", label: "Today" },
        { value: "week", label: "This Week" },
        { value: "month", label: "This Month" },
        { value: "year", label: "This Year" },
      ];
      for (const d of dates) {
        const o = el("option", { value: d.value, text: d.label }) as HTMLOptionElement;
        if (d.value === state.filters.dateFilter) o.selected = true;
        dateSelect.appendChild(o);
      }
      on(dateSelect, "change", () => {
        state.filters.dateFilter = dateSelect.value;
        state.page = 0;
        void fetchFiles();
      });

      filterPanel = el("div", { class: "files-view__filter-panel" },
        el("div", { class: "stack files-view__filter-field" },
          el("label", { class: "muted files-view__filter-label", text: "File Type" }),
          fileTypeSelect,
        ),
        el("div", { class: "stack files-view__filter-field" },
          el("label", { class: "muted files-view__filter-label", text: "Size" }),
          sizeSelect,
        ),
        el("div", { class: "stack files-view__filter-field" },
          el("label", { class: "muted files-view__filter-label", text: "Collection" }),
          collectionSelect,
        ),
        el("div", { class: "stack files-view__filter-field" },
          el("label", { class: "muted files-view__filter-label", text: "Date" }),
          dateSelect,
        ),
      );
    }

    // Content area
    const content = el("div", { class: "files-view__content" });

    if (state.loading) {
      content.appendChild(el("div", { class: "card" }, el("div", { class: "skeleton files-view__skeleton" })));
    } else if (state.error && state.files.length === 0) {
      content.appendChild(el("div", { class: "files-view__error" },
        el("span", { class: "files-view__error-chip", text: "Node unavailable" }),
        el("p", { class: "muted", text: state.error }),
      ));
    } else if (filtered.length === 0) {
      content.appendChild(emptyState({ title: "No files found", body: "Try adjusting your filters or search query." }));
    } else if (state.filters.viewMode === "grid") {
      const grid = el("div", { class: "files-view__grid" });
      for (const file of filtered) {
        grid.appendChild(renderFileCard(file));
      }
      content.appendChild(grid);
    } else {
      const table = el("table", { class: "table files-view__table-wrap" });
      const thead = el("thead", {},
        el("tr", {},
          el("th", { text: "Name" }),
          el("th", { text: "Type" }),
          el("th", { text: "Size" }),
          el("th", { text: "Collection" }),
          el("th", { text: "Modified" }),
          el("th", { text: "Actions" }),
        ),
      );
      const tbody = el("tbody");
      for (const file of filtered) {
        tbody.appendChild(renderFileRow(file));
      }
      table.appendChild(thead);
      table.appendChild(tbody);
      content.appendChild(table);
    }

    // Load more button
    if (state.hasMore) {
      const loadMore = el("button", { class: "btn btn--sm btn--ghost files-view__load-more", type: "button", text: "Load more" });
      on(loadMore, "click", () => {
        state.page += 1;
        void fetchFiles();
      });
      content.appendChild(loadMore);
    }

    // Layout: main content + recent files sidebar
    const layout = el("div", { class: "files-view__layout" });
    const mainCol = el("div", { class: "files-view__main" });
    mainCol.appendChild(title);
    mainCol.appendChild(searchBox);
    mainCol.appendChild(toolbar);
    if (filterPanel) mainCol.appendChild(filterPanel);
    mainCol.appendChild(content);

    const sideCol = el("div", { class: "files-view__recent" });
    sideCol.appendChild(renderRecentFiles());

    layout.appendChild(mainCol);
    layout.appendChild(sideCol);

    replace(element, layout);
  }

  return { activate, render, element };
}
