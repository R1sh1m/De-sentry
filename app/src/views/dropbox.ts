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
import { bytes, engineLabel, json, shortNode, truncate } from "../util/format.js";
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

  let stagedPayload: AnalyzedPayload | null = null;
  let targetNodeId: string = "";
  let targetCollection: string = "";
  let isIngesting = false;
  let ingestionLogs: string[] = [];

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
      el("h2", { class: "title", text: "Universal Mesh Dropbox" }),
      el("p", {
        class: "muted",
        style: "max-width: 680px; margin-top: 4px; line-height: 1.5;",
        text: "Drop any files, JSON records, vector embeddings, time-series metrics, or text notes here. The Dropbox classifies the workload, chooses the optimal storage engine, and synchronizes the records into the mesh.",
      }),
    );

    // Drop zone element
    const dropZone = el(
      "div",
      {
        class: "card drop-zone",
        style: "border: 2px dashed var(--color-hairline); border-radius: var(--radius-md); padding: 40px 20px; text-align: center; cursor: pointer; transition: all 0.2s ease; background: var(--color-surface-pearl);",
      },
      icon(Icons.drive, 36),
      el("h3", { style: "margin: 12px 0 4px; font-size: var(--text-base);", text: "Drag & drop data files here" }),
      el("p", { class: "muted", style: "font-size: var(--text-sm); margin-bottom: 16px;", text: "Supports JSON, CSV, Vectors, Logs, or Plain Text" }),
      (() => {
        const fileInput = el("input", { type: "file", style: "display: none;" }) as HTMLInputElement;
        on(fileInput, "change", () => {
          if (fileInput.files && fileInput.files[0]) {
            const file = fileInput.files[0];
            const reader = new FileReader();
            reader.onload = () => {
              if (typeof reader.result === "string") {
                stageData(analyzeText(reader.result, file.name));
              }
            };
            reader.readAsText(file);
          }
        });

        const browseBtn = el("button", { class: "btn btn--sm btn--primary", type: "button" }, "Browse File…");
        on(browseBtn, "click", (e) => {
          e.stopPropagation();
          fileInput.click();
        });
        return el("div", { class: "row", style: "justify-content: center;" }, browseBtn, fileInput);
      })(),
    );

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
      if (files && files[0]) {
        const file = files[0];
        const reader = new FileReader();
        reader.onload = () => {
          if (typeof reader.result === "string") {
            stageData(analyzeText(reader.result, file.name));
          }
        };
        reader.readAsText(file);
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
        stageData(analyzeText(pasteArea.value.trim()));
      }
    });

    const intakeSection = el("div", { class: "stack" }, dropZone, pasteArea, analyzePasteBtn);

    // Staged preview card (if data has been dropped/pasted)
    let stagedSection: HTMLElement | null = null;
    if (stagedPayload) {
      const payload = stagedPayload;

      const nodeSelect = el("select", { class: "input", style: "padding: 6px 10px; font-size: var(--text-sm);" }) as HTMLSelectElement;
      for (const n of nodes) {
        const opt = el("option", {
          value: n.process.node_id,
          text: `${n.process.node_name || shortNode(n.process.node_id)} (port ${n.process.api_port})`,
        }) as HTMLOptionElement;
        if (n.process.node_id === targetNodeId) opt.selected = true;
        nodeSelect.appendChild(opt);
      }
      on(nodeSelect, "change", () => { targetNodeId = nodeSelect.value; });

      const colInput = el("input", {
        type: "text",
        class: "input",
        value: targetCollection,
        placeholder: "collection_name",
        style: "padding: 6px 10px; font-size: var(--text-sm); font-family: var(--font-mono); width: 160px;",
      }) as HTMLInputElement;
      on(colInput, "input", () => { targetCollection = colInput.value.trim(); });

      const commitBtn = el(
        "button",
        {
          class: "btn btn--primary",
          type: "button",
          style: "gap: 6px;",
          disabled: isIngesting ? "true" : undefined,
        },
        icon(Icons.plug, 14),
        isIngesting ? "Ingesting…" : "Organize & Ingest",
      );

      const cancelBtn = el("button", { class: "btn btn--ghost", type: "button" }, "Discard");
      on(cancelBtn, "click", () => {
        stagedPayload = null;
        ingestionLogs = [];
        render();
      });

      on(commitBtn, "click", () => void commitIngestion());

      const previewRows = payload.records.slice(0, 5).map((r) =>
        el(
          "div",
          { class: "row row--between", style: "padding: 6px 0; border-bottom: 1px solid var(--color-hairline); font-size: var(--text-xs);" },
          el("span", { class: "mono", text: r.key, style: "font-weight: 600;" }),
          el("span", { class: "mono muted", text: truncate(json(r.doc), 60) }),
        ),
      );

      stagedSection = el(
        "div",
        { class: "card", style: "margin-top: 24px; padding: 20px; border: 1px solid var(--color-primary);" },
        el("div", { class: "row row--between", style: "align-items: center; margin-bottom: 12px;" },
          el("div", {},
            el("span", { class: "badge", text: payload.workload.toUpperCase(), style: "margin-right: 8px;" }),
            el("strong", { text: payload.title }),
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
          el("strong", { text: `Preview (${payload.records.length} total records):`, style: "font-size: var(--text-xs); margin-bottom: 4px;" }),
          ...previewRows,
        ),
        el("div", { class: "row", style: "gap: 10px;" }, commitBtn, cancelBtn),
        ingestionLogs.length > 0
          ? el("div", { class: "stack", style: "margin-top: 16px; padding: 12px; background: var(--color-surface-pearl); border-radius: var(--radius-sm); font-size: var(--text-xs);" },
              el("strong", { text: "Ingestion Log:" }),
              ...ingestionLogs.map((l) => el("div", { class: "mono", text: l })),
            )
          : null,
      );
    }

    replace(element, titleCluster, intakeSection, stagedSection);
  }

  function stageData(payload: AnalyzedPayload): void {
    stagedPayload = payload;
    targetCollection = payload.suggestedCollection;
    const best = pickBestNode(payload.workload, payload.suggestedEngine);
    targetNodeId = best ? best.process.node_id : (store.dataNodes()[0]?.process.node_id ?? "");
    ingestionLogs = [];
    render();
  }

  async function commitIngestion(): Promise<void> {
    if (!stagedPayload || !targetNodeId || !targetCollection) return;
    const node = store.state.nodes.get(targetNodeId);
    if (!node) {
      store.toast("error", "Node unavailable", "Selected target node is offline.");
      return;
    }

    isIngesting = true;
    render();
    ingestionLogs = [`Starting ingestion into node ${node.process.node_name || targetNodeId} / ${targetCollection}...`];

    const api = apiFor(node.process.api_port);
    let successCount = 0;

    try {
      // Ingest each record into the target node
      for (const r of stagedPayload.records) {
        try {
          const res = await api.putDocument(targetCollection, r.key, r.doc);
          successCount++;
          if (successCount <= 5 || successCount === stagedPayload.records.length) {
            ingestionLogs.push(`✓ Ingested "${r.key}" -> entry #${res.entry_id}`);
          }
        } catch (err) {
          ingestionLogs.push(`✗ Failed "${r.key}": ${describeError(err)}`);
        }
      }

      store.toast(
        "success",
        "Ingestion Complete",
        `Ingested ${successCount}/${stagedPayload.records.length} records into ${targetCollection}.`,
        4000,
      );

      await refreshNode(targetNodeId);
      await refreshCollection(targetNodeId, targetCollection);
    } catch (err) {
      store.toast("error", "Ingestion failed", describeError(err));
    } finally {
      isIngesting = false;
      render();
    }
  }

  return { render, element };
}
