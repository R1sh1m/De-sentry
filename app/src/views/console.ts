/**
 * Interactive Node & Engine Console.
 *
 * Provides a direct, comprehensive workspace to interact with, query, and test
 * the various storage engines (KV/Documents, Vectors, Time-Series, Graph, and Raw API).
 */

import { apiFor, ApiError } from "../api.js";
import { store, type NodeView } from "../state.js";
import { engineLabel, json, shortNode } from "../util/format.js";
import { el, icon, Icons, on, replace } from "../util/dom.js";

function describeError(error: unknown): string {
  if (error instanceof ApiError) return error.message;
  if (error instanceof Error) return error.message;
  return String(error);
}

type ConsoleTab = "documents" | "vector" | "timeseries" | "graph" | "api";

export interface ConsoleHandles {
  render(): void;
  element: HTMLElement;
}

export function createConsole(): ConsoleHandles {
  const element = el("div", { class: "canvas console-view", style: "display: flex; flex-direction: column; overflow: hidden; padding: 0;" });
  let activeTab: ConsoleTab = "documents";

  // Tab State
  let docCollection = "";
  let docKey = "";
  let docBody = '{\n  "name": "sample_record",\n  "value": 42\n}';
  let docResult = "";

  let vecCollection = "";
  let vecInput = "[0.15, -0.42, 0.88, 0.05]";
  let vecK = 5;

  let tsCollection = "";
  let tsSeries = "cpu_usage";
  const tsBucketMs = 60000;

  let graphCollection = "";
  let graphKey = "node_1";
  const graphDepth = 1;

  let apiMethod = "GET";
  let apiPath = "/_brain";
  let apiBody = "";
  let apiStatus = "";
  let apiDuration = 0;
  let apiResult = "";

  function render(): void {
    const node = store.selectedNode() ?? store.dataNodes()[0];

    if (!node) {
      replace(
        element,
        el(
          "div",
          { class: "empty", style: "margin: auto;" },
          el("p", { class: "empty__title", text: "No Node Selected" }),
          el("p", { class: "empty__body", text: "Select or start a data node to open its interactive engine console." }),
        ),
      );
      return;
    }

    const collections = node.brain?.collections ?? [];
    if (!docCollection && collections.length > 0) docCollection = collections[0].name;
    if (!vecCollection && collections.length > 0) vecCollection = collections[0].name;
    if (!tsCollection && collections.length > 0) tsCollection = collections[0].name;
    if (!graphCollection && collections.length > 0) graphCollection = collections[0].name;

    // -- Header bar -----------------------------------------------------------
    const backBtn = el(
      "button",
      { class: "btn btn--sm btn--ghost", type: "button", title: "Back to mesh" },
      icon(Icons.chevronLeft, 14),
      " Canvas",
    );
    on(backBtn, "click", () => {
      store.select({ kind: "node", nodeId: node.process.node_id });
    });

    const nodeSelector = el("select", {
      class: "input",
      style: "font: var(--text-sm); font-weight: 600; padding: 4px 8px; background: var(--color-surface-pearl); border: 1px solid var(--color-hairline); border-radius: var(--radius-sm); color: var(--color-ink);",
    }) as HTMLSelectElement;

    for (const d of store.dataNodes()) {
      const opt = el("option", { value: d.process.node_id, text: `${d.process.node_name || shortNode(d.process.node_id)} (port ${d.process.api_port})` }) as HTMLOptionElement;
      if (d.process.node_id === node.process.node_id) opt.selected = true;
      nodeSelector.appendChild(opt);
    }
    on(nodeSelector, "change", () => {
      store.select({ kind: "console", nodeId: nodeSelector.value });
    });

    const enginesBadges = el(
      "div",
      { class: "row", style: "gap: 4px; flex-wrap: wrap;" },
      ...(node.brain?.collections ?? []).map((c) =>
        el("span", { class: "badge", text: `${c.name} (${engineLabel(c.engine)})` }),
      ),
    );

    const header = el(
      "div",
      {
        class: "row row--between",
        style: "padding: 12px 16px; border-bottom: 1px solid var(--color-hairline); background: var(--color-surface); flex-shrink: 0;",
      },
      el("div", { class: "row", style: "gap: 12px; align-items: center;" }, backBtn, nodeSelector, enginesBadges),
      el("span", { class: "mono muted", style: "font-size: var(--text-xs);" }, `Height #${node.tip?.entry_id ?? 0}`),
    );

    // -- Tab switcher ---------------------------------------------------------
    const tabs: { id: ConsoleTab; label: string; icon: string }[] = [
      { id: "documents", label: "Documents & KV", icon: Icons.folder },
      { id: "vector", label: "Vector Search", icon: Icons.search },
      { id: "timeseries", label: "Time-Series", icon: Icons.tree },
      { id: "graph", label: "Graph Explorer", icon: Icons.mesh },
      { id: "api", label: "Direct API", icon: Icons.inspector },
    ];

    const tabButtons = tabs.map((t) => {
      const btn = el(
        "button",
        {
          class: `btn btn--sm ${activeTab === t.id ? "btn--primary" : "btn--ghost"}`,
          type: "button",
          style: "gap: 6px;",
        },
        icon(t.icon, 13),
        t.label,
      );
      on(btn, "click", () => {
        activeTab = t.id;
        render();
      });
      return btn;
    });

    const nav = el(
      "div",
      {
        class: "row",
        style: "padding: 8px 16px; gap: 8px; border-bottom: 1px solid var(--color-hairline); background: var(--color-surface-pearl); flex-shrink: 0;",
      },
      ...tabButtons,
    );

    // -- Content area ---------------------------------------------------------
    let content: HTMLElement;
    switch (activeTab) {
      case "documents":
        content = renderDocumentsTab(node, collections.map((c) => c.name));
        break;
      case "vector":
        content = renderVectorTab(node, collections.map((c) => c.name));
        break;
      case "timeseries":
        content = renderTimeSeriesTab(node, collections.map((c) => c.name));
        break;
      case "graph":
        content = renderGraphTab(node, collections.map((c) => c.name));
        break;
      case "api":
        content = renderApiTab(node);
        break;
    }

    replace(element, header, nav, content);
  }

  // -- 1. Documents Tab -------------------------------------------------------
  function renderDocumentsTab(node: NodeView, colNames: string[]): HTMLElement {
    const colSelect = el("select", { class: "input", style: "padding: 6px 10px; font-size: var(--text-sm);" }) as HTMLSelectElement;
    for (const name of colNames) {
      const opt = el("option", { value: name, text: name }) as HTMLOptionElement;
      if (name === docCollection) opt.selected = true;
      colSelect.appendChild(opt);
    }
    on(colSelect, "change", () => { docCollection = colSelect.value; });

    const keyInput = el("input", {
      type: "text",
      class: "input",
      placeholder: "Key (e.g. user_101)",
      value: docKey,
      style: "width: 180px; padding: 6px 10px; font-family: var(--font-mono); font-size: var(--text-sm);",
    }) as HTMLInputElement;
    on(keyInput, "input", () => { docKey = keyInput.value.trim(); });

    const getBtn = el("button", { class: "btn btn--sm btn--ghost", type: "button" }, "GET Key");
    const putBtn = el("button", { class: "btn btn--sm btn--primary", type: "button" }, "PUT Document");
    const listBtn = el("button", { class: "btn btn--sm btn--ghost", type: "button" }, "List Keys");
    const delBtn = el("button", { class: "btn btn--sm btn--danger", type: "button" }, "DEL Key");

    const bodyArea = el("textarea", {
      class: "mono",
      style: "width: 100%; height: 260px; padding: 12px; font-size: var(--text-xs); line-height: 1.5; background: var(--color-surface); border: 1px solid var(--color-hairline); border-radius: var(--radius-sm); color: var(--color-ink); resize: vertical;",
      placeholder: 'JSON payload...',
    }) as HTMLTextAreaElement;
    bodyArea.value = docBody;
    on(bodyArea, "input", () => { docBody = bodyArea.value; });

    const outputPre = el("pre", {
      class: "mono",
      style: "flex: 1; margin: 0; padding: 12px; overflow: auto; background: var(--color-surface-pearl); border: 1px solid var(--color-hairline); border-radius: var(--radius-sm); font-size: var(--text-xs); color: var(--color-ink); max-height: 400px;",
      text: docResult || "// Response will appear here...",
    });

    on(getBtn, "click", async () => {
      if (!docCollection || !docKey) {
        store.toast("warning", "Missing fields", "Please select a collection and enter a key.");
        return;
      }
      try {
        const res = await apiFor(node.process.api_port).getDocument(docCollection, docKey);
        docResult = json(res);
        bodyArea.value = docResult;
        docBody = docResult;
        outputPre.textContent = docResult;
        store.toast("success", "Document retrieved", `${docCollection}/${docKey}`, 2000);
      } catch (err) {
        docResult = `Error: ${describeError(err)}`;
        outputPre.textContent = docResult;
      }
    });

    on(putBtn, "click", async () => {
      if (!docCollection || !docKey) {
        store.toast("warning", "Missing fields", "Please select a collection and enter a key.");
        return;
      }
      try {
        let parsed: unknown;
        try {
          parsed = JSON.parse(docBody);
        } catch {
          parsed = { data: docBody };
        }
        const res = await apiFor(node.process.api_port).putDocument(docCollection, docKey, parsed);
        docResult = json(res);
        outputPre.textContent = docResult;
        store.toast("success", "Document stored", `Entry #${res.entry_id} in ${docCollection}/${docKey}`, 3000);
      } catch (err) {
        docResult = `Error: ${describeError(err)}`;
        outputPre.textContent = docResult;
      }
    });

    on(listBtn, "click", async () => {
      if (!docCollection) return;
      try {
        const res = await apiFor(node.process.api_port).listDocuments(docCollection, { limit: 100 });
        docResult = json(res);
        outputPre.textContent = docResult;
      } catch (err) {
        docResult = `Error: ${describeError(err)}`;
        outputPre.textContent = docResult;
      }
    });

    on(delBtn, "click", async () => {
      if (!docCollection || !docKey) return;
      try {
        const res = await apiFor(node.process.api_port).deleteDocument(docCollection, docKey);
        docResult = json(res);
        outputPre.textContent = docResult;
        store.toast("info", "Document deleted", `${docCollection}/${docKey}`);
      } catch (err) {
        docResult = `Error: ${describeError(err)}`;
        outputPre.textContent = docResult;
      }
    });

    const controls = el(
      "div",
      { class: "row", style: "gap: 8px; flex-wrap: wrap; align-items: center; margin-bottom: 12px;" },
      el("span", { class: "muted", style: "font-size: var(--text-sm);" }, "Collection:"),
      colSelect,
      el("span", { class: "muted", style: "font-size: var(--text-sm);" }, "Key:"),
      keyInput,
      getBtn,
      putBtn,
      listBtn,
      delBtn,
    );

    const grid = el(
      "div",
      { style: "display: grid; grid-template-columns: 1fr 1fr; gap: 16px; flex: 1; overflow: hidden;" },
      el("div", { class: "stack", style: "overflow: hidden;" }, el("strong", { text: "Document Payload" }), bodyArea),
      el("div", { class: "stack", style: "overflow: hidden;" }, el("strong", { text: "Server Response" }), outputPre),
    );

    return el("div", { style: "padding: 16px; display: flex; flex-direction: column; flex: 1; overflow: auto;" }, controls, grid);
  }

  // -- 2. Vector Search Tab ---------------------------------------------------
  function renderVectorTab(node: NodeView, colNames: string[]): HTMLElement {
    const colSelect = el("select", { class: "input", style: "padding: 6px 10px; font-size: var(--text-sm);" }) as HTMLSelectElement;
    for (const name of colNames) {
      const opt = el("option", { value: name, text: name }) as HTMLOptionElement;
      if (name === vecCollection) opt.selected = true;
      colSelect.appendChild(opt);
    }
    on(colSelect, "change", () => { vecCollection = colSelect.value; });

    const kInput = el("input", {
      type: "number",
      min: "1",
      max: "50",
      value: String(vecK),
      style: "width: 70px; padding: 6px; font-size: var(--text-sm);",
    }) as HTMLInputElement;
    on(kInput, "input", () => { vecK = parseInt(kInput.value, 10) || 5; });

    const searchBtn = el("button", { class: "btn btn--sm btn--primary", type: "button" }, "Run Vector Search");

    const vecTextArea = el("textarea", {
      class: "mono",
      style: "width: 100%; height: 100px; padding: 10px; font-size: var(--text-xs); background: var(--color-surface); border: 1px solid var(--color-hairline); border-radius: var(--radius-sm); color: var(--color-ink);",
      placeholder: "[0.12, -0.45, 0.77, ...]",
    }) as HTMLTextAreaElement;
    vecTextArea.value = vecInput;
    on(vecTextArea, "input", () => { vecInput = vecTextArea.value; });

    const resultBox = el("div", { class: "stack", style: "flex: 1; overflow: auto;" });

    const runSearch = async () => {
      try {
        let vec: number[];
        try {
          const raw = JSON.parse(vecInput);
          vec = Array.isArray(raw) ? raw.map(Number) : [];
        } catch {
          vec = vecInput.split(",").map((s) => parseFloat(s.trim())).filter((n) => !isNaN(n));
        }

        if (vec.length === 0) {
          store.toast("warning", "Invalid Vector", "Please enter an array of numbers.");
          return;
        }

        const res = await apiFor(node.process.api_port).vectorSearch(vecCollection, vec, vecK);

        const hitRows = res.hits.map((h, i) =>
          el(
            "div",
            {
              class: "row row--between card",
              style: "padding: 10px 14px; margin-bottom: 8px; align-items: center;",
            },
            el(
              "div",
              {},
              el("span", { class: "badge", text: `#${i + 1}`, style: "margin-right: 8px;" }),
              el("strong", { text: h.key }),
            ),
            el("span", { class: "mono", style: "font-size: var(--text-xs); color: var(--color-primary);" }, `Score: ${h.score.toFixed(4)}`),
          ),
        );

        replace(
          resultBox,
          el("p", { class: "muted", text: `Found ${res.count} hits in ${vecCollection}:` }),
          ...hitRows,
        );
      } catch (err) {
        replace(resultBox, el("p", { class: "error-note", text: `Search failed: ${describeError(err)}` }));
      }
    };

    on(searchBtn, "click", runSearch);

    return el(
      "div",
      { style: "padding: 16px; display: flex; flex-direction: column; flex: 1; overflow: auto;" },
      el("div", { class: "row", style: "gap: 8px; margin-bottom: 12px; align-items: center;" },
        el("span", { class: "muted" }, "Collection:"),
        colSelect,
        el("span", { class: "muted" }, "Top K:"),
        kInput,
        searchBtn,
      ),
      el("strong", { text: "Query Vector (Float Array)", style: "margin-bottom: 6px;" }),
      vecTextArea,
      el("strong", { text: "Nearest Neighbor Hits", style: "margin: 16px 0 6px;" }),
      resultBox,
    );
  }

  // -- 3. Time-Series Tab -----------------------------------------------------
  function renderTimeSeriesTab(node: NodeView, colNames: string[]): HTMLElement {
    const colSelect = el("select", { class: "input", style: "padding: 6px 10px; font-size: var(--text-sm);" }) as HTMLSelectElement;
    for (const name of colNames) {
      const opt = el("option", { value: name, text: name }) as HTMLOptionElement;
      if (name === tsCollection) opt.selected = true;
      colSelect.appendChild(opt);
    }
    on(colSelect, "change", () => { tsCollection = colSelect.value; });

    const seriesInput = el("input", {
      type: "text",
      class: "input",
      value: tsSeries,
      placeholder: "Series (e.g. cpu_usage)",
      style: "width: 160px; padding: 6px 10px; font-size: var(--text-sm);",
    }) as HTMLInputElement;
    on(seriesInput, "input", () => { tsSeries = seriesInput.value.trim(); });

    const queryBtn = el("button", { class: "btn btn--sm btn--primary", type: "button" }, "Query Rollups");
    const resultBox = el("div", { class: "stack", style: "flex: 1; overflow: auto; margin-top: 16px;" });

    on(queryBtn, "click", async () => {
      try {
        const res = await apiFor(node.process.api_port).tsRollups(tsCollection, {
          series: tsSeries,
          bucket_ms: tsBucketMs,
        });

        const seriesKeys = Object.keys(res);
        if (seriesKeys.length === 0) {
          replace(resultBox, el("p", { class: "muted", text: "No rollups recorded for this series yet." }));
          return;
        }

        const bucketCards: HTMLElement[] = [];
        for (const s of seriesKeys) {
          const buckets = res[s] || [];
          const rows = buckets.map((b) => {
            const avg = b.count > 0 ? (b.sum / b.count).toFixed(2) : "0.00";
            return el(
              "div",
              { class: "row row--between", style: "padding: 6px 10px; border-bottom: 1px solid var(--color-hairline); font-size: var(--text-xs);" },
              el("span", { class: "mono", text: new Date(b.bucket_start_ms).toLocaleTimeString() }),
              el("span", { text: `Count: ${b.count}` }),
              el("span", { text: `Min: ${b.min.toFixed(2)}` }),
              el("span", { text: `Avg: ${avg}`, style: "font-weight: 600;" }),
              el("span", { text: `Max: ${b.max.toFixed(2)}` }),
            );
          });
          bucketCards.push(
            el(
              "div",
              { class: "card", style: "margin-bottom: 12px;" },
              el("h4", { class: "card__title", text: `Series: ${s} (${buckets.length} buckets)` }),
              el("div", { class: "stack" }, ...rows),
            ),
          );
        }

        replace(resultBox, ...bucketCards);
      } catch (err) {
        replace(resultBox, el("p", { class: "error-note", text: `Query failed: ${describeError(err)}` }));
      }
    });

    return el(
      "div",
      { style: "padding: 16px; display: flex; flex-direction: column; flex: 1; overflow: auto;" },
      el("div", { class: "row", style: "gap: 8px; margin-bottom: 12px; align-items: center;" },
        el("span", { class: "muted" }, "Collection:"),
        colSelect,
        el("span", { class: "muted" }, "Series:"),
        seriesInput,
        queryBtn,
      ),
      resultBox,
    );
  }

  // -- 4. Graph Tab -----------------------------------------------------------
  function renderGraphTab(node: NodeView, colNames: string[]): HTMLElement {
    const colSelect = el("select", { class: "input", style: "padding: 6px 10px; font-size: var(--text-sm);" }) as HTMLSelectElement;
    for (const name of colNames) {
      const opt = el("option", { value: name, text: name }) as HTMLOptionElement;
      if (name === graphCollection) opt.selected = true;
      colSelect.appendChild(opt);
    }
    on(colSelect, "change", () => { graphCollection = colSelect.value; });

    const keyInput = el("input", {
      type: "text",
      class: "input",
      value: graphKey,
      placeholder: "Key (e.g. user_1)",
      style: "width: 160px; padding: 6px 10px; font-size: var(--text-sm);",
    }) as HTMLInputElement;
    on(keyInput, "input", () => { graphKey = keyInput.value.trim(); });

    const exploreBtn = el("button", { class: "btn btn--sm btn--primary", type: "button" }, "Traverse Neighbourhood");
    const resultBox = el("div", { class: "stack", style: "flex: 1; overflow: auto; margin-top: 16px;" });

    on(exploreBtn, "click", async () => {
      try {
        const res = await apiFor(node.process.api_port).graphNeighbourhood(graphCollection, graphKey, graphDepth);

        replace(
          resultBox,
          el("div", { class: "card" },
            el("h4", { class: "card__title", text: `Graph Adjacency for "${res.key}"` }),
            el("div", { class: "stack", style: "margin-top: 8px;" },
              el("strong", { text: `Out-Edges (${res.out_edges.length}):` }),
              ...res.out_edges.map((e) => el("div", { class: "mono", style: "padding-left: 12px; font-size: var(--text-xs);" }, `→ ${e.to}${e.label ? ` (${e.label})` : ""}`)),
              el("strong", { text: `In-Edges (${res.in_edges.length}):`, style: "margin-top: 8px;" }),
              ...res.in_edges.map((src) => el("div", { class: "mono", style: "padding-left: 12px; font-size: var(--text-xs);" }, `← ${src}`)),
              el("strong", { text: `Descendants Reachable (${res.descendants.length}):`, style: "margin-top: 8px;" }),
              ...res.descendants.map((d) => el("span", { class: "badge", text: d })),
            ),
          ),
        );
      } catch (err) {
        replace(resultBox, el("p", { class: "error-note", text: `Traversal failed: ${describeError(err)}` }));
      }
    });

    return el(
      "div",
      { style: "padding: 16px; display: flex; flex-direction: column; flex: 1; overflow: auto;" },
      el("div", { class: "row", style: "gap: 8px; margin-bottom: 12px; align-items: center;" },
        el("span", { class: "muted" }, "Collection:"),
        colSelect,
        el("span", { class: "muted" }, "Node Key:"),
        keyInput,
        exploreBtn,
      ),
      resultBox,
    );
  }

  // -- 5. Direct API Playground ----------------------------------------------
  function renderApiTab(node: NodeView): HTMLElement {
    const methodSelect = el("select", { class: "input", style: "padding: 6px 10px; font-size: var(--text-sm); font-weight: bold;" }) as HTMLSelectElement;
    for (const m of ["GET", "POST", "PUT", "DELETE"]) {
      const opt = el("option", { value: m, text: m }) as HTMLOptionElement;
      if (m === apiMethod) opt.selected = true;
      methodSelect.appendChild(opt);
    }
    on(methodSelect, "change", () => { apiMethod = methodSelect.value; });

    const pathInput = el("input", {
      type: "text",
      class: "input",
      value: apiPath,
      placeholder: "/_status",
      style: "flex: 1; padding: 6px 12px; font-family: var(--font-mono); font-size: var(--text-sm);",
    }) as HTMLInputElement;
    on(pathInput, "input", () => { apiPath = pathInput.value.trim(); });

    const sendBtn = el("button", { class: "btn btn--sm btn--primary", type: "button" }, "Send");

    // Quick presets
    const presets = ["/_brain", "/_status", "/_quota", "/_engines", "/_ledger/tip", "/_collections"];
    const presetBtns = presets.map((p) => {
      const b = el("button", { class: "btn btn--sm btn--ghost", type: "button", style: "font-size: var(--text-xs);" }, p);
      on(b, "click", () => {
        apiMethod = "GET";
        methodSelect.value = "GET";
        apiPath = p;
        pathInput.value = p;
        void execute();
      });
      return b;
    });

    const bodyArea = el("textarea", {
      class: "mono",
      style: "width: 100%; height: 120px; padding: 10px; font-size: var(--text-xs); background: var(--color-surface); border: 1px solid var(--color-hairline); border-radius: var(--radius-sm); color: var(--color-ink); resize: vertical; display: " + (apiMethod === "GET" ? "none" : "block"),
      placeholder: "Request Body (JSON)...",
    }) as HTMLTextAreaElement;
    bodyArea.value = apiBody;
    on(bodyArea, "input", () => { apiBody = bodyArea.value; });

    on(methodSelect, "change", () => {
      bodyArea.style.display = apiMethod === "GET" ? "none" : "block";
    });

    const statusBadge = el("span", { class: "badge", text: apiStatus || "Ready" });
    const durationSpan = el("span", { class: "muted", style: "font-size: var(--text-xs); margin-left: 8px;", text: apiDuration ? `${apiDuration}ms` : "" });

    const responsePre = el("pre", {
      class: "mono",
      style: "flex: 1; margin: 0; padding: 12px; overflow: auto; background: var(--color-surface-pearl); border: 1px solid var(--color-hairline); border-radius: var(--radius-sm); font-size: var(--text-xs); color: var(--color-ink);",
      text: apiResult || "// Response will appear here...",
    });

    const execute = async () => {
      sendBtn.setAttribute("disabled", "");
      statusBadge.textContent = "Loading…";
      const start = Date.now();
      try {
        const url = `http://127.0.0.1:${node.process.api_port}${apiPath}`;
        const res = await fetch(url, {
          method: apiMethod,
          headers: apiMethod !== "GET" && apiBody ? { "Content-Type": "application/json" } : undefined,
          body: apiMethod !== "GET" && apiBody ? apiBody : undefined,
        });
        apiDuration = Date.now() - start;
        durationSpan.textContent = `${apiDuration}ms`;
        apiStatus = `${res.status} ${res.statusText}`;
        statusBadge.textContent = apiStatus;
        const text = await res.text();
        try {
          apiResult = json(JSON.parse(text));
        } catch {
          apiResult = text;
        }
        responsePre.textContent = apiResult;
      } catch (err) {
        apiDuration = Date.now() - start;
        durationSpan.textContent = `${apiDuration}ms`;
        apiStatus = "Error";
        statusBadge.textContent = apiStatus;
        apiResult = describeError(err);
        responsePre.textContent = apiResult;
      } finally {
        sendBtn.removeAttribute("disabled");
      }
    };

    on(sendBtn, "click", execute);

    return el(
      "div",
      { style: "padding: 16px; display: flex; flex-direction: column; flex: 1; overflow: auto; gap: 10px;" },
      el("div", { class: "row", style: "gap: 6px; align-items: center;" }, methodSelect, pathInput, sendBtn),
      el("div", { class: "row", style: "gap: 6px; flex-wrap: wrap;" }, el("span", { class: "muted", style: "font-size: var(--text-xs); align-self: center;" }, "Quick endpoints:"), ...presetBtns),
      bodyArea,
      el("div", { class: "row", style: "align-items: center; margin-top: 6px;" }, el("strong", { text: "Response" }), el("span", { style: "margin-left: 8px;" }), statusBadge, durationSpan),
      responsePre,
    );
  }

  return { render, element };
}
