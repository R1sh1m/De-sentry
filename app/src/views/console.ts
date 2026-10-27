/**
 * Interactive Node & Engine Console.
 *
 * Provides a direct, comprehensive workspace to interact with, query, and test
 * the various storage engines (KV/Documents, Vectors, Time-Series, Graph, and Raw API).
 */

import { apiFor, ApiError } from "../api.js";
import { sidecar, onSidecarEvent, type LogLine } from "../bridge.js";
import { store, convergenceOf, type NodeView } from "../state.js";
import { displayNodeName, engineLabel, json } from "../util/format.js";
import { el, icon, Icons, on, replace } from "../util/dom.js";
import { emptyState } from "../util/empty.js";

function describeError(error: unknown): string {
  if (error instanceof ApiError) return error.message;
  if (error instanceof Error) return error.message;
  return String(error);
}

type ConsoleTab = "documents" | "vector" | "timeseries" | "graph" | "api" | "logs";

export interface ConsoleHandles {
  render(): void;
  element: HTMLElement;
}

export function createConsole(): ConsoleHandles {
  const element = el("div", { class: "canvas console-view" });
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
  let nodeLogLines: Awaited<ReturnType<typeof sidecar.nodeLogs>> = [];
  let logsLoading = false;
  let logUnsubscribe: (() => void) | null = null;

  function render(): void {
    const node = store.selectedNode() ?? store.dataNodes()[0];

    if (!node) {
      replace(element, emptyState({ title: "No Node Selected", body: "Select or start a data node to open its interactive engine console." }));
      return;
    }

    const collections = node.brain?.collections ?? [];
    // Each specialized tab only lists collections bound to its engine: a KV
    // collection queried as a vector index fails with a backend mismatch,
    // so offering it there is offering an error. Documents & KV keeps all.
    const namesFor = (engine: string): string[] =>
      collections.filter((c) => c.engine === engine).map((c) => c.name);
    const vecNames = namesFor("vector_hnsw_lite");
    const tsNames = namesFor("ts_rollup");
    const graphNames = namesFor("graph_adj");
    if (!docCollection && collections.length > 0) docCollection = collections[0].name;
    if (!vecCollection || !vecNames.includes(vecCollection)) vecCollection = vecNames[0] ?? "";
    if (!tsCollection || !tsNames.includes(tsCollection)) tsCollection = tsNames[0] ?? "";
    if (!graphCollection || !graphNames.includes(graphCollection)) {
      graphCollection = graphNames[0] ?? "";
    }

    // -- Header bar -----------------------------------------------------------
    const backBtn = el(
      "button",
      { class: "btn btn--sm btn--ghost", type: "button", title: "Back to canvas" },
      icon(Icons.chevronLeft, 14),
      " Canvas",
    );
    on(backBtn, "click", () => {
      store.select({ kind: "node", nodeId: node.process.node_id });
    });

    const nodeSelector = el("select", {
      class: "select-rounded",
    }) as HTMLSelectElement;

    const STATUS_SYMBOLS: Record<string, string> = {
      converged: "● ",
      lagging: "◐ ",
      waiting: "◐ ",
      checking: "◑ ",
      offline: "○ ",
      isolated: "◔ ",
      supervisor: "◆ ",
    };

    for (const d of store.dataNodes()) {
      const status = convergenceOf(d);
      const symbol = STATUS_SYMBOLS[status] ?? "";
      const statusLabel = status === "converged" ? "In sync"
        : status === "lagging" ? "Catching up"
        : status === "waiting" ? "Waiting"
        : status === "checking" ? "Checking"
        : status === "offline" ? "Offline"
        : status === "isolated" ? "Local only"
        : status;
      const opt = el("option", {
        value: d.process.node_id,
        text: `${symbol}${displayNodeName(d.process.node_name, d.process.data_dir, d.process.node_id)} (port ${d.process.api_port} — ${statusLabel})`,
      }) as HTMLOptionElement;
      if (d.process.node_id === node.process.node_id) opt.selected = true;
      nodeSelector.appendChild(opt);
    }
    on(nodeSelector, "change", () => {
      store.select({ kind: "console", nodeId: nodeSelector.value });
    });

    const enginesBadges = el(
      "div",
      { class: "console-view__badge-row" },
      ...(node.brain?.collections ?? []).map((c) =>
        el("span", { class: "badge", text: `${c.name} (${engineLabel(c.engine)})` }),
      ),
    );

    // Cluster-membership posture: closed mesh shows its fingerprint so two
    // machines can confirm they will join the SAME mesh when pairing.
    const meshBadge = el("span", {
      class: "console-view__badge-muted console-view__badge-margin",
      title: "Cluster membership: closed meshes refuse outsiders",
      text: "",
    });
    void sidecar.membershipStatus().then((status) => {
      meshBadge.textContent = status.closed ? `Mesh closed ${status.fingerprint}` : "Mesh open";
    });

    const header = el(
      "div",
      {
        class: "console-view__header",
      },
      el("div", { class: "console-view__controls" }, backBtn, nodeSelector, enginesBadges),
      el(
        "div",
        { class: "console-view__controls" },
        meshBadge,
        el("span", {
          class: "mono muted",
          title: "Node-local write-ahead log position; peer nodes can have different ledger heights.",
        }, `Local ledger #${node.tip?.entry_id ?? 0}`),
      ),
    );

    // -- Tab switcher ---------------------------------------------------------
    // Tabs with no bound collection stay clickable: their content explains
    // what to bind rather than failing on a mismatched engine. The title
    // says the requirement up front.
    const tabs: { id: ConsoleTab; label: string; icon: string; title: string }[] = [
      { id: "documents", label: "Documents & KV", icon: Icons.folder, title: "Any collection" },
      {
        id: "vector",
        label: `Vector Search${vecNames.length > 0 ? ` (${vecNames.length})` : ""}`,
        icon: Icons.search,
        title: vecNames.length > 0 ? "Collections bound to vector_hnsw_lite" : "Needs a collection bound to vector_hnsw_lite",
      },
      {
        id: "timeseries",
        label: `Time-Series${tsNames.length > 0 ? ` (${tsNames.length})` : ""}`,
        icon: Icons.chart,
        title: tsNames.length > 0 ? "Collections bound to ts_rollup" : "Needs a collection bound to ts_rollup",
      },
      {
        id: "graph",
        label: `Graph Explorer${graphNames.length > 0 ? ` (${graphNames.length})` : ""}`,
        icon: Icons.mesh,
        title: graphNames.length > 0 ? "Collections bound to graph_adj" : "Needs a collection bound to graph_adj",
      },
      { id: "api", label: "Direct API", icon: Icons.inspector, title: "Raw method + path against this node" },
      { id: "logs", label: "Node Logs", icon: Icons.terminal, title: "Captured output from this node's background process" },
    ];

    const tabButtons = tabs.map((t) => {
      const btn = el(
        "button",
        {
          class: `btn btn--sm ${activeTab === t.id ? "btn--primary" : "btn--ghost"}`,
          type: "button",
          title: t.title,
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
      { class: "console-view__tab-nav" },
      ...tabButtons,
    );

    // -- Content area ---------------------------------------------------------
    let content: HTMLElement;
    switch (activeTab) {
      case "documents":
        content = renderDocumentsTab(node, collections.map((c) => c.name));
        break;
      case "vector":
        content = renderVectorTab(node, vecNames);
        break;
      case "timeseries":
        content = renderTimeSeriesTab(node, tsNames);
        break;
      case "graph":
        content = renderGraphTab(node, graphNames);
        break;
      case "api":
        content = renderApiTab(node);
        break;
      case "logs":
        content = renderLogsTab(node);
        break;
    }

    replace(element, header, nav, content);
  }

  function renderLogsTab(node: NodeView): HTMLElement {
    // Tear down any previous real-time subscription so we only listen for the
    // currently selected node while this tab is open.
    if (logUnsubscribe) { logUnsubscribe(); logUnsubscribe = null; }

    const refreshBtn = el("button", {
      class: "btn btn--sm btn--ghost",
      type: "button",
      text: logsLoading ? "Refreshing…" : "Refresh logs",
      disabled: logsLoading,
    });
    const output = el("pre", {
      class: "console-view__output-pre mono",
      text: nodeLogLines.length
        ? nodeLogLines.map((entry) => `${new Date(entry.ts_ms).toLocaleString()} [${entry.stream}] ${entry.line}`).join("\n")
        : "No captured output yet. Refresh after the node has been running for a moment.",
    });

    const appendLog = (entry: LogLine) => {
      nodeLogLines.push(entry);
      if (nodeLogLines.length > 400) nodeLogLines.shift();
      output.textContent = nodeLogLines.map((e) => `${new Date(e.ts_ms).toLocaleString()} [${e.stream}] ${e.line}`).join("\n");
      output.scrollTop = output.scrollHeight;
    };

    on(refreshBtn, "click", async () => {
      logsLoading = true;
      refreshBtn.textContent = "Refreshing…";
      refreshBtn.setAttribute("disabled", "true");
      try {
        const fresh = await sidecar.nodeLogs(store.selectedNode()?.process.node_id ?? node.process.node_id, 400);
        nodeLogLines = fresh;
        output.textContent = nodeLogLines.length
          ? nodeLogLines.map((entry) => `${new Date(entry.ts_ms).toLocaleString()} [${entry.stream}] ${entry.line}`).join("\n")
          : "No captured output yet. Refresh after the node has been running for a moment.";
      } catch (error) {
        output.textContent = `Could not read node logs: ${describeError(error)}`;
      } finally {
        logsLoading = false;
        refreshBtn.textContent = "Refresh logs";
        refreshBtn.removeAttribute("disabled");
      }
    });

    // Real-time tailing: the sidecar emits `node-log` events for every line.
    logUnsubscribe = onSidecarEvent((event) => {
      if (event.kind === "node-log" && event.node_id === node.process.node_id) {
        appendLog(event.line);
      }
    });

    return el("div", { class: "console-view__section" },
      el("div", { class: "row row--between" },
        el("div", { class: "muted", text: `Recent output captured from ${displayNodeName(node.process.node_name, node.process.data_dir, node.process.node_id)}.` }),
        refreshBtn,
      ),
      output,
    );
  }

  // -- 1. Documents Tab -------------------------------------------------------
  function renderDocumentsTab(node: NodeView, colNames: string[]): HTMLElement {
    const colSelect = el("select", { class: "select-rounded" }) as HTMLSelectElement;
    for (const name of colNames) {
      const opt = el("option", { value: name, text: name }) as HTMLOptionElement;
      if (name === docCollection) opt.selected = true;
      colSelect.appendChild(opt);
    }
    on(colSelect, "change", () => { docCollection = colSelect.value; });

    const keyInput = el("input", {
      type: "text",
      class: "input console-view__key-input",
      placeholder: "Key (e.g. user_101)",
      value: docKey,
    }) as HTMLInputElement;
    on(keyInput, "input", () => { docKey = keyInput.value.trim(); });

    const getBtn = el("button", { class: "btn btn--sm btn--ghost", type: "button" }, "GET Key");
    const putBtn = el("button", { class: "btn btn--sm btn--primary", type: "button" }, "PUT Document");
    const listBtn = el("button", { class: "btn btn--sm btn--ghost", type: "button" }, "List Keys");
    const delBtn = el("button", { class: "btn btn--sm btn--danger", type: "button" }, "DEL Key");

    const bodyArea = el("textarea", {
      class: "console-view__body-textarea",
      placeholder: 'JSON payload...',
    }) as HTMLTextAreaElement;
    bodyArea.value = docBody;
    on(bodyArea, "input", () => { docBody = bodyArea.value; });

    const outputPre = el("pre", {
      class: "console-view__output-pre mono",
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
      { class: "console-view__row console-view__row-wrap console-view__section-margin" },
      el("span", { class: "console-view__label" }, "Collection:"),
      colSelect,
      el("span", { class: "console-view__label" }, "Key:"),
      keyInput,
      getBtn,
      putBtn,
      listBtn,
      delBtn,
    );

    const grid = el(
      "div",
      { class: "console-view__grid-full" },
      el("div", { class: "console-view__col" }, el("strong", { text: "Document Payload" }), bodyArea),
      el("div", { class: "console-view__col" }, el("strong", { text: "Server Response" }), outputPre),
    );

    return el("div", { class: "console-view__container" }, controls, grid);
  }

  // -- 2. Vector Search Tab ---------------------------------------------------
  function renderVectorTab(node: NodeView, colNames: string[]): HTMLElement {
    if (colNames.length === 0) {
      return emptyState({
        title: "No vector collections",
        body: "This node has no collection bound to vector_hnsw_lite. Drop embeddings through the Dropbox (it binds the engine automatically), or bind one by hand: PUT /_collection/<name>/engine {\"engine\": \"vector_hnsw_lite\"}.",
      });
    }
    const colSelect = el("select", { class: "select-rounded" }) as HTMLSelectElement;
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
      class: "console-input",
    }) as HTMLInputElement;
    on(kInput, "input", () => { vecK = parseInt(kInput.value, 10) || 5; });

    const searchBtn = el("button", { class: "btn btn--sm btn--primary", type: "button" }, "Run Vector Search");

    const vecTextArea = el("textarea", {
      class: "console-textarea",
      placeholder: "[0.12, -0.45, 0.77, ...]",
    }) as HTMLTextAreaElement;
    vecTextArea.value = vecInput;
    on(vecTextArea, "input", () => { vecInput = vecTextArea.value; });

    const resultBox = el("div", { class: "console-view__section-card" });

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
              class: "console-view__row console-view__row--spread card console-view__section-card",
            },
            el(
              "div",
              {},
              el("span", { class: "badge console-view__badge-margin", text: `#${i + 1}` }),
              el("strong", { text: h.key }),
            ),
            el("span", { class: "console-view__score", text: `Score: ${h.score.toFixed(4)}` }),
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
      { class: "console-view__container" },
      el("div", { class: "console-view__row console-view__row-wrap console-view__section-margin" },
        el("span", { class: "console-view__label" }, "Collection:"),
        colSelect,
        el("span", { class: "console-view__label" }, "Top K:"),
        kInput,
        searchBtn,
      ),
      el("strong", { text: "Query Vector (Float Array)" }),
      vecTextArea,
      el("strong", { text: "Nearest Neighbor Hits" }),
      resultBox,
    );
  }

  // -- 3. Time-Series Tab -----------------------------------------------------
  function renderTimeSeriesTab(node: NodeView, colNames: string[]): HTMLElement {
    if (colNames.length === 0) {
      return emptyState({
        title: "No time-series collections",
        body: "This node has no collection bound to ts_rollup. Drop metric logs through the Dropbox (it binds the engine automatically), or bind one by hand: PUT /_collection/<name>/engine {\"engine\": \"ts_rollup\"}.",
      });
    }
    const colSelect = el("select", { class: "select-rounded" }) as HTMLSelectElement;
    for (const name of colNames) {
      const opt = el("option", { value: name, text: name }) as HTMLOptionElement;
      if (name === tsCollection) opt.selected = true;
      colSelect.appendChild(opt);
    }
    on(colSelect, "change", () => { tsCollection = colSelect.value; });

    const seriesInput = el("input", {
      type: "text",
      class: "input console-view__series-input",
      value: tsSeries,
      placeholder: "Series (e.g. cpu_usage)",
    }) as HTMLInputElement;
    on(seriesInput, "input", () => { tsSeries = seriesInput.value.trim(); });

    const queryBtn = el("button", { class: "btn btn--sm btn--primary", type: "button" }, "Query Rollups");
    const resultBox = el("div", { class: "console-view__section-card" });

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
              { class: "console-view__row console-view__row--spread console-view__bucket-row" },
              el("span", { class: "mono", text: new Date(b.bucket_start_ms).toLocaleTimeString() }),
              el("span", { text: `Count: ${b.count}` }),
              el("span", { text: `Min: ${b.min.toFixed(2)}` }),
              el("span", { class: "console-view__bucket-avg", text: `Avg: ${avg}` }),
              el("span", { text: `Max: ${b.max.toFixed(2)}` }),
            );
          });
          bucketCards.push(
            el(
              "div",
              { class: "console-view__section-card" },
              el("h4", { class: "card__title", text: `Series: ${s} (${buckets.length} buckets)` }),
              el("div", { class: "console-view__container-stack" }, ...rows),
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
      { class: "console-view__container" },
      el("div", { class: "console-view__row console-view__row-wrap console-view__section-margin" },
        el("span", { class: "console-view__label" }, "Collection:"),
        colSelect,
        el("span", { class: "console-view__label" }, "Series:"),
        seriesInput,
        queryBtn,
      ),
      resultBox,
    );
  }

  // -- 4. Graph Tab -----------------------------------------------------------
  function renderGraphTab(node: NodeView, colNames: string[]): HTMLElement {
    if (colNames.length === 0) {
      return emptyState({
        title: "No graph collections",
        body: "This node has no collection bound to graph_adj. Drop edge lists through the Dropbox (it binds the engine automatically), or bind one by hand: PUT /_collection/<name>/engine {\"engine\": \"graph_adj\"}.",
      });
    }
    const colSelect = el("select", { class: "select-rounded" }) as HTMLSelectElement;
    for (const name of colNames) {
      const opt = el("option", { value: name, text: name }) as HTMLOptionElement;
      if (name === graphCollection) opt.selected = true;
      colSelect.appendChild(opt);
    }
    on(colSelect, "change", () => { graphCollection = colSelect.value; });

    const keyInput = el("input", {
      type: "text",
      class: "input console-view__series-input",
      value: graphKey,
      placeholder: "Key (e.g. user_1)",
    }) as HTMLInputElement;
    on(keyInput, "input", () => { graphKey = keyInput.value.trim(); });

    const exploreBtn = el("button", { class: "btn btn--sm btn--primary", type: "button" }, "Traverse Neighbourhood");
    const resultBox = el("div", { class: "console-view__section-card" });

    on(exploreBtn, "click", async () => {
      try {
        const res = await apiFor(node.process.api_port).graphNeighbourhood(graphCollection, graphKey, graphDepth);

        replace(
          resultBox,
          el("div", { class: "card" },
            el("h4", { class: "card__title", text: `Graph Adjacency for "${res.key}"` }),
            el("div", { class: "console-view__container-stack" },
              el("strong", { text: `Out-Edges (${res.out_edges.length}):` }),
              ...res.out_edges.map((e) => el("div", { class: "console-view__graph-edge" }, `→ ${e.to}${e.label ? ` (${e.label})` : ""}`)),
              el("strong", { class: "console-view__graph-strong", text: `In-Edges (${res.in_edges.length}):` }),
              ...res.in_edges.map((src) => el("div", { class: "console-view__graph-edge" }, `← ${src}`)),
              el("strong", { class: "console-view__graph-strong", text: `Descendants Reachable (${res.descendants.length}):` }),
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
      { class: "console-view__container" },
      el("div", { class: "console-view__row console-view__row-wrap console-view__section-margin" },
        el("span", { class: "console-view__label" }, "Collection:"),
        colSelect,
        el("span", { class: "console-view__label" }, "Node Key:"),
        keyInput,
        exploreBtn,
      ),
      resultBox,
    );
  }

  // -- 5. Direct API Playground ----------------------------------------------
  function renderApiTab(node: NodeView): HTMLElement {
    const methodSelect = el("select", { class: "select-rounded" }) as HTMLSelectElement;
    for (const m of ["GET", "POST", "PUT", "DELETE"]) {
      const opt = el("option", { value: m, text: m }) as HTMLOptionElement;
      if (m === apiMethod) opt.selected = true;
      methodSelect.appendChild(opt);
    }
    on(methodSelect, "change", () => { apiMethod = methodSelect.value; });

    const pathInput = el("input", {
      type: "text",
      class: "input console-view__path-input",
      value: apiPath,
      placeholder: "/_status",
    }) as HTMLInputElement;
    on(pathInput, "input", () => { apiPath = pathInput.value.trim(); });

    const sendBtn = el("button", { class: "btn btn--sm btn--primary", type: "button" }, "Send");

    const bodyArea = el("textarea", {
      class: "console-view__body-area mono",
      placeholder: "Request Body (JSON)...",
    }) as HTMLTextAreaElement;
    bodyArea.value = apiBody;
    on(bodyArea, "input", () => { apiBody = bodyArea.value; });

    on(methodSelect, "change", () => {
      bodyArea.style.display = apiMethod === "GET" ? "none" : "block";
    });

    const statusBadge = el("span", { class: "badge", text: apiStatus || "Ready" });
    const durationSpan = el("span", { class: "console-view__duration", text: apiDuration ? `${apiDuration}ms` : "" });
    const timestampSpan = el("span", { class: "console-view__timestamp", text: "" });

    const responsePre = el("pre", {
      class: "console-view__response-box mono",
      text: apiResult || "// Response will appear here...",
    });

    const execute = async () => {
      sendBtn.setAttribute("disabled", "");
      statusBadge.textContent = "Loading…";
      const start = Date.now();
      try {
        // Same timeout + unreachable semantics as every other call (a dead
        // node reports honestly, never a hung spinner); non-2xx surfaces as
        // data so the playground can display it.
        const res = await apiFor(node.process.api_port).rawRequest(
          apiMethod,
          apiPath,
          apiMethod !== "GET" ? apiBody : undefined,
        );
        apiDuration = Date.now() - start;
        durationSpan.textContent = `${apiDuration}ms`;
        timestampSpan.textContent = new Date().toLocaleTimeString();
        apiStatus = `${res.status}${res.statusText ? ` ${res.statusText}` : ""}`;
        statusBadge.textContent = apiStatus;
        apiResult = typeof res.body === "string" ? res.body : json(res.body);
        responsePre.textContent = apiResult;
      } catch (err) {
        apiDuration = Date.now() - start;
        durationSpan.textContent = `${apiDuration}ms`;
        timestampSpan.textContent = new Date().toLocaleTimeString();
        apiStatus = "Error";
        statusBadge.textContent = apiStatus;
        apiResult = describeError(err);
        responsePre.textContent = apiResult;
      } finally {
        sendBtn.removeAttribute("disabled");
      }
    };

    on(sendBtn, "click", execute);

    // Quick presets (registered after `execute` so TDZ is avoided)
    const presets = ["/_brain", "/_status", "/_quota", "/_engines", "/_ledger/tip", "/_collections"];
    const presetBtns = presets.map((p) => {
      const b = el("button", { class: "btn btn--sm btn--ghost console-view__preset-btn", type: "button" }, p);
      on(b, "click", () => {
        apiMethod = "GET";
        methodSelect.value = "GET";
        apiPath = p;
        pathInput.value = p;
        void execute();
      });
      return b;
    });

    return el(
      "div",
      { class: "console-view__container--gap" },
      el("div", { class: "console-view__row console-view__badge-row" }, methodSelect, pathInput, sendBtn),
      el("div", { class: "console-view__preset-row" }, el("span", { class: "console-view__preset-label" }, "Quick endpoints:"), ...presetBtns),
      bodyArea,
      el("div", { class: "console-view__response-header" }, el("strong", { text: "Response" }), statusBadge, durationSpan),
      responsePre,
    );
  }

  return { render, element };
}
