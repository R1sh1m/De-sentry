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
    /** Set by Discard: the detached PUT loop checks it per record and stops. */
    cancelled: boolean;
    logs: string[];
  }

  /** Files bigger than this are refused before readAsText (memory guard). */
  const MAX_INTAKE_BYTES = 64 * 1024 * 1024;

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
      // (guarded: parsed[0] may be a number, string or null, none of which
      // has edge fields -- and must fall through, not throw).
      if (
        Array.isArray(parsed) && parsed.length > 0 && typeof parsed[0] === "object" &&
        parsed[0] !== null && ((parsed[0] as Record<string, unknown>).source || (parsed[0] as Record<string, unknown>).from)
      ) {
        const records = parsed.map((item, i) => {
          const obj = (typeof item === "object" && item !== null ? item : {}) as Record<string, unknown>;
          const src = obj.source ?? obj.from ?? `unknown_${i}`;
          const dst = obj.target ?? obj.to ?? `unknown_${i}`;
          return { key: `edge_${String(src)}_${String(dst)}_${i}`, doc: item };
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
      // Check for Time-Series records (same null-guard as the graph check).
      if (
        Array.isArray(parsed) && parsed.length > 0 && typeof parsed[0] === "object" &&
        parsed[0] !== null &&
        ((parsed[0] as Record<string, unknown>).timestamp || (parsed[0] as Record<string, unknown>).time ||
          (parsed[0] as Record<string, unknown>).ts)
      ) {
        const records = parsed.map((item, i) => {
          const obj = (typeof item === "object" && item !== null ? item : {}) as Record<string, unknown>;
          const t = obj.timestamp ?? obj.time ?? obj.ts ?? i;
          const k = `ts_${String(t)}_${i}`;
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
          const obj = (typeof item === "object" && item !== null ? item : {}) as Record<string, unknown>;
          const k = obj.id ?? obj.key ?? obj._id ?? `item_${i + 1}`;
          return { key: String(k), doc: item };
        });
        return {
          workload: "document",
          title: "JSON Document Collection",
          summary: `Collection of ${records.length} structured records.`,
          suggestedCollection: filename ? filename.replace(/\.[^/.]+$/, "") : "records",
          suggestedEngine: "kv",
          records,
          rawText: text,
        };
      }
      // Single JSON object
      if (typeof parsed === "object" && parsed !== null) {
        const obj = parsed as Record<string, unknown>;
        const k = obj.id ?? obj.key ?? obj.name ?? `doc_${Date.now()}`;
        return {
          workload: "document",
          title: "JSON Document",
          summary: "Single structured JSON record.",
          suggestedCollection: filename ? filename.replace(/\.[^/.]+$/, "") : "documents",
          suggestedEngine: "kv",
          records: [{ key: String(k), doc: parsed }],
          rawText: text,
        };
      }
    } catch {
      // Not JSON, check CSV / Tabular
    }

    // 2. Check for CSV. Quoted fields may contain the delimiter and
    // doubled quotes ("") escape a literal quote -- a naive split() turns
    // `"a, b",c` into three fields instead of two. A delimiter with no
    // newline is headers without rows (not "raw text"): it stages zero
    // records with an honest summary rather than misfiling the headers.
    if (trimmed.includes(",") || trimmed.includes("\t")) {
      const lines = trimmed.split("\n").map((l) => l.trim()).filter(Boolean);
      // Multi-row, or a single header-like row with at least 3 fields: a lone
      // "hello, world" is prose, not a header row, and stays on the raw path.
      const firstFields = lines.length > 0 ? lines[0].split(/[,\t]/).length : 0;
      if (lines.length > 1 || firstFields >= 3) {
        const delimiter = trimmed.includes("\t") ? "\t" : ",";
        const splitRow = (line: string): string[] => {
          const fields: string[] = [];
          let cur = "";
          let inQuotes = false;
          for (let i = 0; i < line.length; i++) {
            const ch = line[i];
            if (inQuotes) {
              if (ch === '"') {
                if (line[i + 1] === '"') {
                  cur += '"';
                  i++;
                } else {
                  inQuotes = false;
                }
              } else {
                cur += ch;
              }
            } else if (ch === '"') {
              inQuotes = true;
            } else if (ch === delimiter) {
              fields.push(cur.trim());
              cur = "";
            } else {
              cur += ch;
            }
          }
          fields.push(cur.trim());
          return fields.map((f) =>
            f.length >= 2 && f.startsWith("'") && f.endsWith("'") ? f.slice(1, -1) : f,
          );
        };
        const headers = splitRow(lines[0]);
        const records: { key: string; doc: unknown }[] = [];
        for (let i = 1; i < lines.length; i++) {
          const parts = splitRow(lines[i]);
          const obj: Record<string, unknown> = {};
          headers.forEach((h, idx) => {
            const val = parts[idx] ?? "";
            // Numeric coercion is conservative: hex ("0x10"), Infinity and
            // NaN-shaped strings stay strings (Number() accepts them all),
            // and only finite decimal numbers convert.
            const num = Number(val);
            obj[h] = val !== "" && Number.isFinite(num) && !/^0x/i.test(val.trim()) ? num : val;
          });
          records.push({ key: `row_${i}`, doc: obj });
        }
        return {
          workload: "tabular",
          title: "CSV / Tabular Dataset",
          summary:
            records.length === 0
              ? `Headers found (${headers.length} columns) but no data rows — paste the full CSV to ingest.`
              : `${records.length} rows with ${headers.length} columns: ${headers.slice(0, 4).join(", ")}...`,
          suggestedCollection: filename ? filename.replace(/\.[^/.]+$/, "") : "tables",
          // columnar_lite: the built-in segment engine for tabular data.
          // (duckdb is vendored and usually not compiled in.)
          suggestedEngine: "columnar_lite",
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
      suggestedEngine: "kv",
      records: [{ key, doc: { content: trimmed, filename: filename || undefined, created_ms: Date.now() } }],
      rawText: text,
    };
  }

  /**
   * Splits records whose serialized form exceeds the engine's single-page
   * budget (~4 KiB) into a manifest + parts. A whole file in one `content`
   * string otherwise fails the entire PUT with kOutOfSpace. Parts are
   * `{parent_key, part_index, part_total, field, text}` under
   * `<key>#<i>`; the parent keeps every other field plus a `chunked`
   * marker. Records with no splittable string field (e.g. a huge embedding)
   * pass through untouched -- the backend then decides with a per-record
   * error the log already surfaces.
   */
  function chunkRecords(records: { key: string; doc: unknown }[]): { key: string; doc: unknown }[] {
    const out: { key: string; doc: unknown }[] = [];
    for (const r of records) {
      const text = JSON.stringify(r.doc);
      if (text.length <= 3500) {
        out.push(r);
        continue;
      }
      if (typeof r.doc === "object" && r.doc !== null && !Array.isArray(r.doc)) {
        const obj = r.doc as Record<string, unknown>;
        let biggest = "";
        let biggestLen = 0;
        for (const [k, v] of Object.entries(obj)) {
          if (typeof v === "string" && v.length > biggestLen) {
            biggest = k;
            biggestLen = v.length;
          }
        }
        if (biggestLen > 2000) {
          const full = obj[biggest] as string;
          const n = Math.ceil(full.length / 3500);
          for (let i = 0; i < n; i++) {
            out.push({
              key: `${r.key}#${i}`,
              doc: {
                parent_key: r.key,
                part_index: i,
                part_total: n,
                field: biggest,
                text: full.slice(i * 3500, (i + 1) * 3500),
              },
            });
          }
          const rest: Record<string, unknown> = {};
          for (const [k, v] of Object.entries(obj)) {
            if (k !== biggest) rest[k] = v;
          }
          out.push({
            key: r.key,
            doc: { ...rest, chunked: true, chunk_total: n, chunk_field: biggest },
          });
          continue;
        }
      }
      out.push(r);
    }
    return out;
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

    // Discard is disabled while ingesting: removing the card mid-loop used
    // to leave the PUT loop running on a detached item. Discarding now marks
    // the item cancelled first, and the loop checks per record.
    const cancelBtn = el("button", {
      class: "btn btn--ghost",
      type: "button",
      text: item.isIngesting ? "Cancel" : "Discard",
    });
    on(cancelBtn, "click", () => {
      item.cancelled = true;
      if (!item.isIngesting) {
        staged = staged.filter((s) => s.id !== item.id);
        render();
      }
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
    if (file.size > MAX_INTAKE_BYTES) {
      store.toast("error", `Could not read '${file.name}'`, `File is ${file.size} bytes; the intake limit is ${MAX_INTAKE_BYTES} bytes. Split it and retry.`);
      return;
    }
    const reader = new FileReader();
    reader.onload = () => {
      if (typeof reader.result !== "string") {
        store.toast("error", `Could not read '${file.name}'`, "The file could not be decoded as text.");
        return;
      }
      // readAsText never fails on binary -- it decodes garbage with U+FFFD
      // replacements. Detect that and re-read as a data URL instead, so
      // images, PDFs and zips ingest as base64 asset documents (chunked by
      // chunkRecords like any other oversized field) rather than mojibake.
      if (reader.result.includes("�")) {
        readFileAsAsset(file);
        return;
      }
      stageData(analyzeText(reader.result, file.name), file.name);
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

  function readFileAsAsset(file: File): void {
    const reader = new FileReader();
    reader.onload = () => {
      if (typeof reader.result !== "string") {
        store.toast("error", `Could not read '${file.name}'`, "Binary read produced no data.");
        return;
      }
      const match = /^data:([^;,]+)?;base64,(.*)$/s.exec(reader.result);
      if (!match) {
        store.toast("error", `Could not read '${file.name}'`, "Binary encoding failed.");
        return;
      }
      const key = file.name.replace(/[^a-zA-Z0-9_-]/g, "_");
      stageData(
        {
          workload: "raw",
          title: `Binary file: ${file.name}`,
          summary: `Binary asset (${bytes(file.size)}); stored base64-encoded and chunked.`,
          suggestedCollection: "assets",
          suggestedEngine: "kv",
          records: [{
            key,
            doc: {
              filename: file.name,
              mime: match[1] || "application/octet-stream",
              size_bytes: file.size,
              base64: match[2],
              created_ms: Date.now(),
            },
          }],
          rawText: "",
        },
        file.name,
      );
    };
    reader.onerror = () => {
      store.toast("error", `Could not read '${file.name}'`, reader.error ? reader.error.message : "Read failed.");
    };
    try {
      reader.readAsDataURL(file);
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
      cancelled: false,
      logs: [],
    }];
    render();
  }

  async function ingestAll(): Promise<void> {
    for (const item of staged) {
      if (item.isIngesting) continue;
      if (!item.targetNodeId || !item.targetCollection) {
        store.toast("warning", "Ingestion skipped", `Set a destination node and collection for '${item.sourceName || item.payload.title}' first.`);
        continue;
      }
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
      // Bind the suggested engine FIRST: without this the "Engine: X" label
      // is advice the backend never hears, and vector/ts/graph payloads land
      // as plain KV docs. BindCollection creates an empty entry for fresh
      // collections; on a populated one it refuses rebind, and the records
      // below still land in the existing binding -- either way one log line.
      try {
        await api.bindEngine(item.targetCollection, item.payload.suggestedEngine);
        item.logs.push(`Collection '${item.targetCollection}' bound to ${item.payload.suggestedEngine}.`);
      } catch (err) {
        item.logs.push(`Engine bind skipped (${describeError(err)}); ingesting into the existing binding.`);
      }
      // Oversized records are chunked into manifest + parts (see
      // chunkRecords); failures stay per-record so one bad document never
      // aborts the rest of the file.
      const outgoing = chunkRecords(item.payload.records);
      if (outgoing.length !== item.payload.records.length) {
        item.logs.push(`Split ${item.payload.records.length} oversized record(s) into ${outgoing.length} chunked writes (page budget ~4 KiB).`);
      }
      for (const r of outgoing) {
        if (item.cancelled) {
          item.logs.push("Cancelled by the user; remaining records were not sent.");
          break;
        }
        try {
          const res = await api.putDocument(item.targetCollection, r.key, r.doc);
          successCount++;
          if (successCount <= 5 || successCount + failCount === outgoing.length) {
            item.logs.push(`✓ Ingested "${r.key}" -> entry #${res.entry_id}`);
          }
        } catch (err) {
          failCount++;
          item.logs.push(`✗ Failed "${r.key}": ${describeError(err)}`);
        }
      }
      // A cancelled card leaves the list; an uncancelled one stays with its log.
      if (item.cancelled) {
        staged = staged.filter((s) => s.id !== item.id);
      }

      const total = outgoing.length;
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
