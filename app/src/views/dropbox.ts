/**
 * Universal Smart Dropbox.
 *
 * A general intake zone where the user can drop or paste any data (JSON, CSV,
 * numeric vectors, time series, notes, files). The Dropbox analyzes the payload,
 * classifies the optimal database engine, routes it to the most suitable node in
 * the mesh, and ingests it into the ledger.
 */

import { apiFor, ApiError } from "../api.js";
import { refreshCollection, refreshNode, store, type NodeView } from "../state.js";
import { bytes, displayNodeName, engineLabel, json, truncate } from "../util/format.js";
import { el, icon, Icons, on, replace } from "../util/dom.js";

function describeError(error: unknown): string {
  if (error instanceof ApiError) return error.message;
  if (error instanceof Error) return error.message;
  return String(error);
}

export type DetectedWorkload = "vector" | "timeseries" | "graph" | "document" | "tabular" | "raw";

interface AnalyzedPayload {
  workload: DetectedWorkload;
  title: string;
  summary: string;
  suggestedCollection: string;
  suggestedEngine: string;
  records: { key: string; doc: unknown }[];
  rawText: string;
}

export interface DropboxHandles {
  render(): void;
  element: HTMLElement;
}

export function createDropbox(): DropboxHandles {
  const element = el("div", { class: "canvas dropbox-view", style: "display: flex; flex-direction: column; overflow: auto; padding: 24px;" });

  interface StagedItem {
    id: number;
    /** Filename, or "Pasted text" for paste intakes. */
    sourceName: string;
    payload: AnalyzedPayload;
    targetNodeId: string;
    targetCollection: string;
    isIngesting: boolean;
    logs: string[];
  }

  let staged: StagedItem[] = [];
  let nextStagedId = 1;

  function analyzeText(text: string, filename = ""): AnalyzedPayload {
    const trimmed = text.trim();

    // 1. Check for Vector (array of numbers)
    try {
      const parsed = JSON.parse(trimmed);
      if (Array.isArray(parsed) && parsed.length > 0 && parsed.every((x) => typeof x === "number")) {
        const id = `vec_${Date.now()}`;
        return {
          workload: "vector",
          title: "Numeric Vector Embedding",
          summary: `Float vector with ${parsed.length} dimensions.`,
          suggestedCollection: "vectors",
          suggestedEngine: "vector_hnsw_lite",
          records: [{ key: id, doc: { vector: parsed, dimensions: parsed.length, added_ms: Date.now() } }],
          rawText: text,
        };
      }
      // Check for Graph adjacency: array of { source, target } or { from, to }
      if (Array.isArray(parsed) && parsed.length > 0 && (parsed[0].source || parsed[0].from)) {
        const records = parsed.map((item, i) => {
          const src = item.source || item.from;
          const dst = item.target || item.to;
          return { key: `edge_${src}_${dst}_${i}`, doc: item };
        });
        return {
          workload: "graph",
          title: "Graph Edge Adjacency Data",
          summary: `${records.length} graph relationship edges.`,
          suggestedCollection: "graph",
          suggestedEngine: "graph_adj",
          records,
          rawText: text,
        };
      }
      // Check for Time-Series records
      if (Array.isArray(parsed) && parsed.length > 0 && (parsed[0].timestamp || parsed[0].time || parsed[0].ts)) {
        const records = parsed.map((item, i) => {
          const t = item.timestamp || item.time || item.ts;
          const k = `ts_${t}_${i}`;
          return { key: k, doc: item };
        });
        return {
          workload: "timeseries",
          title: "Time-Series Metric Log",
          summary: `${records.length} timestamped metric points.`,
          suggestedCollection: "metrics",
          suggestedEngine: "ts_rollup",
          records,
          rawText: text,
        };
      }
      // General JSON Array of documents
      if (Array.isArray(parsed)) {
        const records = parsed.map((item, i) => {
          const k = item.id || item.key || item._id || `item_${i + 1}`;
          return { key: String(k), doc: item };
        });
        return {
          workload: "document",
          title: "JSON Document Collection",
          summary: `Collection of ${records.length} structured records.`,
          suggestedCollection: filename ? filename.replace(/\.[^/.]+$/, "") : "records",
          suggestedEngine: "kv_bplus",
          records,
          rawText: text,
        };
      }
      // Single JSON object
      if (typeof parsed === "object" && parsed !== null) {
        const k = parsed.id || parsed.key || parsed.name || `doc_${Date.now()}`;
        return {
          workload: "document",
          title: "JSON Document",
          summary: "Single structured JSON record.",
          suggestedCollection: filename ? filename.replace(/\.[^/.]+$/, "") : "documents",
          suggestedEngine: "kv_bplus",
          records: [{ key: String(k), doc: parsed }],
          rawText: text,
        };
      }
    } catch {
      // Not JSON, check CSV / Tabular
    }

    // 2. Check for CSV
    if (trimmed.includes("\n") && (trimmed.includes(",") || trimmed.includes("\t"))) {
      const lines = trimmed.split("\n").map((l) => l.trim()).filter(Boolean);
      if (lines.length > 1) {
        const delimiter = trimmed.includes("\t") ? "\t" : ",";
        const headers = lines[0].split(delimiter).map((h) => h.replace(/^["']|["']$/g, "").trim());
        const records: { key: string; doc: unknown }[] = [];
        for (let i = 1; i < lines.length; i++) {
          const parts = lines[i].split(delimiter).map((p) => p.replace(/^["']|["']$/g, "").trim());
          const obj: Record<string, unknown> = {};
          headers.forEach((h, idx) => {
            const val = parts[idx] ?? "";
            const num = Number(val);
            obj[h] = !isNaN(num) && val !== "" ? num : val;
          });
          records.push({ key: `row_${i}`, doc: obj });
        }
        return {
          workload: "tabular",
          title: "CSV / Tabular Dataset",
          summary: `${records.length} rows with ${headers.length} columns: ${headers.slice(0, 4).join(", ")}...`,
          suggestedCollection: filename ? filename.replace(/\.[^/.]+$/, "") : "tables",
          suggestedEngine: "duckdb",
          records,
          rawText: text,
        };
      }
    }

    // 3. Fallback: Plain Text or Document Blob
    const key = filename ? filename.replace(/[^a-zA-Z0-9_-]/g, "_") : `note_${Date.now()}`;
    return {
      workload: "raw",
      title: filename ? `File: ${filename}` : "Unstructured Note",
      summary: `Plain text document (${bytes(new Blob([text]).size)}).`,
      suggestedCollection: "notes",
      suggestedEngine: "kv_bplus",
      records: [{ key, doc: { content: trimmed, filename: filename || undefined, created_ms: Date.now() } }],
      rawText: text,
    };
  }

  function pickBestNode(_workload: DetectedWorkload, engine: string): NodeView | undefined {
    const nodes = store.dataNodes().filter((n) => n.reachable && n.process.process === "running");
    if (nodes.length === 0) return undefined;

    // First preference: node with the target engine active in its collections or config
    const withEngine = nodes.find((n) =>
      n.brain?.collections.some((c) => c.engine === engine),
    );
    if (withEngine) return withEngine;

    // Second preference: node with most free quota
    const sorted = [...nodes].sort((a, b) => {
      const freeA = (a.quota?.limit_bytes ?? 1) - (a.quota?.used_bytes ?? 0);
      const freeB = (b.quota?.limit_bytes ?? 1) - (b.quota?.used_bytes ?? 0);
      return freeB - freeA;
    });

    return sorted[0];
  }

  function render(): void {
    const nodes = store.dataNodes().filter((n) => n.reachable && n.process.process === "running");

    // Title and introduction
    const titleCluster = el(
      "div",
      { style: "margin-bottom: 20px;" },
      el("h2", { class: "title", text: "Universal Dropbox" }),
      el("p", {
        class: "muted",
        style: "max-width: 680px; margin-top: 4px; line-height: 1.5;",
        text: "Drop any files, JSON records, vector embeddings, time-series metrics, or text notes here. The Dropbox classifies the workload, chooses the optimal storage engine, and synchronizes the records into the mesh.",
      }),
    );

    const fileInput = el("input", { type: "file", multiple: true, style: "display: none;" }) as HTMLInputElement;
    on(fileInput, "change", () => {
      if (fileInput.files && fileInput.files.length > 0) {
        for (const file of Array.from(fileInput.files)) readFile(file);
        // Reset so picking the same files again still fires a change.
        fileInput.value = "";
      }
    });

    const browseBtn = el("button", { class: "btn btn--sm btn--primary", type: "button" }, "Browse Files…");
    on(browseBtn, "click", (e) => {
      e.stopPropagation();
      fileInput.click();
    });

    // Drop zone element
    const dropZone = el(
      "div",
      {
        class: "card drop-zone",
        style: "border: 2px dashed var(--color-hairline); border-radius: var(--radius-md); padding: 40px 20px; text-align: center; cursor: pointer; transition: all 0.2s ease; background: var(--color-surface-pearl);",
      },
      el("div", { class: "drop-zone__icon-wrap" }, icon(Icons.inbox, 30)),
      el("h3", { style: "margin: 12px 0 4px; font-size: var(--text-base);", text: "Drag & drop data files here" }),
      el("p", { class: "muted", style: "font-size: var(--text-sm); margin-bottom: 16px;", text: "Supports JSON, CSV, Vectors, Logs, or Plain Text" }),
      el("div", { class: "row", style: "justify-content: center;" }, browseBtn, fileInput),
    );

    on(dropZone, "click", () => {
      fileInput.click();
    });
    on(dropZone, "mouseenter", () => {
      dropZone.style.borderColor = "var(--color-primary)";
      dropZone.style.background = "var(--color-surface)";
    });
    on(dropZone, "mouseleave", () => {
      dropZone.style.borderColor = "var(--color-hairline)";
      dropZone.style.background = "var(--color-surface-pearl)";
    });
    on(dropZone, "dragover", (e) => {
      e.preventDefault();
      dropZone.style.borderColor = "var(--color-primary)";
      dropZone.style.background = "var(--color-surface)";
    });
    on(dropZone, "dragleave", () => {
      dropZone.style.borderColor = "var(--color-hairline)";
      dropZone.style.background = "var(--color-surface-pearl)";
    });
    on(dropZone, "drop", (e) => {
      e.preventDefault();
      dropZone.style.borderColor = "var(--color-hairline)";
      dropZone.style.background = "var(--color-surface-pearl)";
      const files = (e as DragEvent).dataTransfer?.files;
      if (files && files.length > 0) {
        for (const file of Array.from(files)) readFile(file);
      }
    });

    // Paste area for quick text ingestion
    const pasteArea = el("textarea", {
      class: "mono",
      style: "width: 100%; height: 90px; padding: 10px; font-size: var(--text-xs); background: var(--color-surface); border: 1px solid var(--color-hairline); border-radius: var(--radius-sm); color: var(--color-ink); resize: vertical; margin-top: 12px;",
      placeholder: "Or paste JSON, vector array, CSV or raw text here…",
    }) as HTMLTextAreaElement;

    const analyzePasteBtn = el("button", { class: "btn btn--sm btn--ghost", type: "button", style: "align-self: flex-start; margin-top: 6px;" }, "Analyze Pasted Text");
    on(analyzePasteBtn, "click", () => {
      if (pasteArea.value.trim()) {
        stageData(analyzeText(pasteArea.value.trim()), "Pasted text");
        pasteArea.value = "";
      }
    });

    const intakeSection = el("div", { class: "stack" }, dropZone, pasteArea, analyzePasteBtn);

    // Staged files — one card per dropped/pasted payload so different files
    // keep their own engine suggestion, destination, collection and log.
    const stagedCards: HTMLElement[] = staged.map((item) => stagedCard(item, nodes));

    const stagedHeader = staged.length > 1
      ? el("div", { class: "row row--between", style: "margin-top: 24px; align-items: center;" },
          el("strong", { text: `${staged.length} files staged`, style: "font-size: var(--text-sm);" }),
          (() => {
            const anyBusy = staged.some((s) => s.isIngesting);
            const all = el("button", {
              class: "btn btn--sm btn--primary",
              type: "button",
              disabled: anyBusy ? "true" : undefined,
              title: anyBusy ? "Wait for the running ingestion to finish" : undefined,
            }, `Ingest all ${staged.length} files`);
            on(all, "click", () => void ingestAll());
            return all;
          })(),
        )
      : null;

    replace(element, titleCluster, intakeSection, stagedHeader, ...stagedCards);
  }

  function stagedCard(item: StagedItem, nodes: NodeView[]): HTMLElement {
    const payload = item.payload;

    const nodeSelect = el("select", { class: "input", style: "padding: 6px 10px; font-size: var(--text-sm);", "aria-label": `Destination node for ${item.sourceName || payload.title}` }) as HTMLSelectElement;
    for (const n of nodes) {
      const quota = n.status?.quota;
      const freeMiB = n.brain?.free_quota_mb
        ?? (quota ? Math.max(0, Math.round((quota.limit_bytes - quota.used_bytes) / (1024 * 1024))) : null)
        ?? null;
      const opt = el("option", {
        value: n.process.node_id,
        text: `${displayNodeName(n.process.node_name, n.process.data_dir, n.process.node_id)}${freeMiB !== null ? ` (${freeMiB} MiB free)` : ""}`,
      }) as HTMLOptionElement;
      if (n.process.node_id === item.targetNodeId) opt.selected = true;
      nodeSelect.appendChild(opt);
    }
    on(nodeSelect, "change", () => { item.targetNodeId = nodeSelect.value; });

    const colInput = el("input", {
      type: "text",
      class: "input",
      value: item.targetCollection,
      placeholder: "collection_name",
      "aria-label": `Collection for ${item.sourceName || payload.title}`,
      style: "padding: 6px 10px; font-size: var(--text-sm); font-family: var(--font-mono); width: 160px;",
    }) as HTMLInputElement;
    on(colInput, "input", () => { item.targetCollection = colInput.value.trim(); });

    const commitBtn = el(
      "button",
      {
        class: "btn btn--primary",
        type: "button",
        style: "gap: 6px;",
        disabled: item.isIngesting || nodes.length === 0 ? "true" : undefined,
        title: nodes.length === 0 ? "No online nodes to ingest into" : undefined,
      },
      icon(Icons.plug, 14),
      item.isIngesting ? "Ingesting…" : `Ingest ${payload.records.length} records`,
    );

    const cancelBtn = el("button", { class: "btn btn--ghost", type: "button" }, "Discard");
    on(cancelBtn, "click", () => {
      staged = staged.filter((s) => s.id !== item.id);
      render();
    });

    on(commitBtn, "click", () => void commitIngestion(item));

    const previewRows = payload.records.slice(0, 3).map((r) =>
      el(
        "div",
        { class: "row row--between", style: "padding: 6px 0; border-bottom: 1px solid var(--color-hairline); font-size: var(--text-xs);" },
        el("span", { class: "mono", text: r.key, style: "font-weight: 600;" }),
        el("span", { class: "mono muted", text: truncate(json(r.doc), 60) }),
      ),
    );

    const failures = item.logs.filter((l) => l.startsWith("✗")).length;

    return el(
      "div",
      { class: "card", style: "margin-top: 16px; padding: 20px; border: 1px solid var(--color-primary);" },
      el("div", { class: "row row--between", style: "align-items: center; margin-bottom: 12px;" },
        el("div", {},
          el("span", { class: "badge", text: payload.workload.toUpperCase(), style: "margin-right: 8px;" }),
          el("strong", { text: payload.title }),
          item.sourceName ? el("span", { class: "muted", style: "font-size: var(--text-xs); margin-left: 8px;", text: item.sourceName }) : null,
        ),
        el("span", { class: "muted", style: "font-size: var(--text-xs);" }, `Engine: ${engineLabel(payload.suggestedEngine)}`),
      ),
      el("p", { style: "font-size: var(--text-sm); margin-bottom: 16px;", text: payload.summary }),
      el("div", { class: "row", style: "gap: 12px; margin-bottom: 16px; align-items: center; flex-wrap: wrap;" },
        el("span", { class: "muted", style: "font-size: var(--text-sm);" }, "Destination Node:"),
        nodeSelect,
        el("span", { class: "muted", style: "font-size: var(--text-sm);" }, "Collection:"),
        colInput,
      ),
      el("div", { class: "stack", style: "background: var(--color-surface-pearl); padding: 12px; border-radius: var(--radius-sm); margin-bottom: 16px;" },
        el("strong", { text: payload.records.length > 3 ? `Preview (first 3 of ${payload.records.length} records):` : `Preview (${payload.records.length} ${payload.records.length === 1 ? "record" : "records"}):`, style: "font-size: var(--text-xs); margin-bottom: 4px;" }),
        ...previewRows,
      ),
      el("div", { class: "row", style: "gap: 10px;" }, commitBtn, cancelBtn),
      failures > 0 && !item.isIngesting
        ? el("p", { class: "error-note", text: `${failures} record${failures === 1 ? "" : "s"} failed — see the log below.` })
        : null,
      item.logs.length > 0
        ? el("div", { class: "stack", style: "margin-top: 16px; padding: 12px; background: var(--color-surface-pearl); border-radius: var(--radius-sm); font-size: var(--text-xs);" },
            el("strong", { text: "Ingestion Log:" }),
            ...item.logs.map((l) => el("div", { class: "mono", text: l })),
          )
        : null,
    );
  }

  function readFile(file: File): void {
    const reader = new FileReader();
    reader.onload = () => {
      if (typeof reader.result === "string") {
        stageData(analyzeText(reader.result, file.name), file.name);
      } else {
        store.toast("error", `Could not read '${file.name}'`, "The file could not be decoded as text.");
      }
    };
    reader.onerror = () => {
      store.toast("error", `Could not read '${file.name}'`, reader.error ? reader.error.message : "Read failed.");
    };
    reader.onabort = () => {
      store.toast("warning", `Skipped '${file.name}'`, "The read was aborted.");
    };
    try {
      reader.readAsText(file);
    } catch (error) {
      store.toast("error", `Could not read '${file.name}'`, describeError(error));
    }
  }

  function stageData(payload: AnalyzedPayload, sourceName = ""): void {
    const best = pickBestNode(payload.workload, payload.suggestedEngine);
    staged = [...staged, {
      id: nextStagedId++,
      sourceName,
      payload,
      targetCollection: payload.suggestedCollection,
      targetNodeId: best ? best.process.node_id : (store.dataNodes()[0]?.process.node_id ?? ""),
      isIngesting: false,
      logs: [],
    }];
    render();
  }

  async function ingestAll(): Promise<void> {
    for (const item of staged) {
      if (item.isIngesting) continue;
      if (!item.targetNodeId || !item.targetCollection) continue;
      await commitIngestion(item);
    }
  }

  async function commitIngestion(item: StagedItem): Promise<void> {
    if (!item.targetNodeId || !item.targetCollection) return;
    const node = store.state.nodes.get(item.targetNodeId);
    if (!node) {
      store.toast("error", "Node unavailable", `The destination node for '${item.sourceName || item.payload.title}' is offline.`);
      return;
    }

    item.isIngesting = true;
    render();
    item.logs = [`Starting ingestion into node ${node.process.node_name || item.targetNodeId} / ${item.targetCollection}...`];

    const api = apiFor(node.process.api_port);
    let successCount = 0;
    let failCount = 0;

    try {
      // Ingest each record into the target node; failures are per-record so
      // one bad document never aborts the rest of the file.
      for (const r of item.payload.records) {
        try {
          const res = await api.putDocument(item.targetCollection, r.key, r.doc);
          successCount++;
          if (successCount <= 5 || successCount + failCount === item.payload.records.length) {
            item.logs.push(`✓ Ingested "${r.key}" -> entry #${res.entry_id}`);
          }
        } catch (err) {
          failCount++;
          item.logs.push(`✗ Failed "${r.key}": ${describeError(err)}`);
        }
      }

      const total = item.payload.records.length;
      const label = item.sourceName || item.targetCollection;
      if (failCount === 0) {
        store.toast(
          "success",
          "Ingestion complete",
          `Ingested ${successCount}/${total} records from '${label}' into ${item.targetCollection}.`,
          4000,
        );
      } else if (successCount === 0) {
        store.toast("error", `Ingestion failed for '${label}'`, `${failCount}/${total} records failed. See the log on its card.`, 6000);
      } else {
        store.toast("warning", `Ingestion partially failed for '${label}'`, `Ingested ${successCount}/${total}; ${failCount} failed. See the log on its card.`, 6000);
      }

      await refreshNode(item.targetNodeId);
      await refreshCollection(item.targetNodeId, item.targetCollection);
    } catch (err) {
      store.toast("error", `Ingestion failed for '${item.sourceName || item.targetCollection}'`, describeError(err));
    } finally {
      item.isIngesting = false;
      render();
    }
  }

  return { render, element };
}
