/**
 * Entry point: builds the shell, wires the views to the store, and renders.
 *
 * The render loop is one function. Every view exposes `render()` and reads the
 * store directly, so a state change re-renders everything that is on screen --
 * which, at this size, costs less than the bookkeeping needed to render less.
 * The one exception is the document editor, which keeps its own text so a
 * re-render does not move the caret.
 */

import { apiFor } from "./api.js";
import { isTauri, sidecar } from "./bridge.js";
import { boot, convergenceOf, meshTip, refreshNodeList, stopAllSubscriptions, store } from "./state.js";
import { shortNode } from "./util/format.js";
import { el, icon, Icons, on, replace } from "./util/dom.js";
import { sentryLogoSvg } from "./util/logo.js";
import { qrSvg } from "./util/qr.js";
import { createMeshGlobe, type GlobeHandle, type GlobeNodePoint } from "./util/meshGlobe.js";
import { canvasZoomFit, canvasZoomStep, createCanvas } from "./views/canvas.js";
import { createExplorer } from "./views/explorer.js";
import { createHealthAlerts } from "./views/healthAlerts.js";
import { createLedger } from "./views/ledger.js";
import { createSidebar } from "./views/sidebar.js";
import { createWizard } from "./views/wizard.js";
import { createConsole } from "./views/console.js";
import { createDropbox } from "./views/dropbox.js";
import { openPalette } from "./views/palette.js";

function describeError(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

// -- theme -------------------------------------------------------------------
// Locked to dark mode: the mesh control room ships one palette. The explicit
// data-theme attribute is set once so every token, canvas and backdrop reads
// the dark ramp without a prefers-color-scheme branch.

function lockDarkTheme(): void {
  document.documentElement.setAttribute("data-theme", "dark");
  try {
    localStorage.removeItem("desentry.theme");
  } catch {
    // Private windows refuse storage; the attribute above is what matters.
  }
}

// -- shortcuts cheatsheet ------------------------------------------------------

function openShortcutsCheatsheet(): void {
  const done = el("button", { class: "btn btn--primary about-macos__done", type: "button", text: "Done" });
  const body = el("div", { class: "macos-modal about-macos" });
  const sheet = el("dialog", { class: "modal-dialog sheet-dialog sheet--about", "aria-label": "Keyboard Shortcuts" }, body);
  const dismiss = () => {
    if (sheet.open) sheet.close();
    sheet.remove();
  };

  const shortcuts: [string, string][] = [
    ["N", "New node"],
    ["1", "Tree view"],
    ["2", "Mesh view"],
    ["3", "Open ledger for selected node"],
    ["4", "Open console for selected node"],
    ["5", "Open Dropbox"],
    ["+", "Zoom mesh in"],
    ["-", "Zoom mesh out"],
    ["Ctrl/Cmd + 0", "Fit mesh view"],
    ["Esc", "Close panel / deselect node"],
    ["Ctrl/Cmd + K", "Command palette"],
    ["Ctrl/Cmd + R", "Refresh node list"],
    ["?", "Show this cheatsheet"],
  ];

  const content = el(
    "div",
    { class: "macos-modal__content about-macos__content" },
    el("div", { class: "about-macos__emblem" }, sentryLogoSvg({ size: 64, animated: false })),
    el(
      "div",
      { class: "about-macos__names" },
      el("h2", { class: "macos-modal__heading", text: "Keyboard Shortcuts" }),
    ),
    el(
      "div",
      { class: "shortcuts-grid", style: "display: grid; grid-template-columns: auto 1fr; gap: 8px 16px; margin-top: 16px; max-width: 400px;" },
      ...shortcuts.map(([key, desc]) =>
        el("div", { class: "shortcut-row" },
          el("kbd", { class: "shortcut-key", style: "background: var(--color-surface-pearl); border: 1px solid var(--color-hairline); border-radius: var(--radius-xs); padding: 2px 8px; font: var(--text-mono); font-size: 11px; white-space: nowrap;", text: key }),
          el("span", { class: "shortcut-desc", style: "font: var(--text-caption); color: var(--color-ink-muted-80);", text: desc }),
        ),
      ),
    ),
    done,
  );
  replace(body, content);

  on(done, "click", dismiss);
  on(sheet, "click", (event) => {
    if (event.target === sheet) dismiss();
  });
  on(sheet, "keydown", (event) => {
    if (event.key === "Escape") dismiss();
  });
  on(sheet, "close", () => sheet.remove());
  document.body.appendChild(sheet);
  try {
    sheet.showModal();
  } catch {
    sheet.setAttribute("open", "");
  }
  done.focus();
}

// -- pairing sheet -----------------------------------------------------------

/**
 * The pairing QR. It carries addresses and a public key, never a secret --
 * pairing tells a peer where to dial and whom to expect; the secure-channel
 * handshake is what actually authenticates.
 */
function openPairingSheet(): void {
  const port = store.state.supervisorPort;
  if (port === null) {
    store.toast("warning", "The supervisor is not running", "Pairing details come from it.");
    return;
  }

  const body = el("div", { class: "wizard" });
  const sheet = el("div", { class: "sheet", role: "dialog", "aria-modal": "true", tabindex: "-1" }, body);
  const dismiss = () => sheet.remove();
  on(sheet, "click", (event) => {
    if (event.target === sheet) dismiss();
  });
  on(sheet, "keydown", (event) => {
    if (event.key === "Escape") dismiss();
  });
  document.body.appendChild(sheet);
  sheet.focus();

  const closeButton = el("button", { class: "sheet-close", type: "button", title: "Close", "aria-label": "Close" }, icon(Icons.close, 16));
  on(closeButton, "click", dismiss);
  const titleBar = el(
    "div",
    { class: "wizard__titlebar" },
    el("span", { style: "width: 28px;" }),
    el("span", { class: "wizard__titlebar-title", text: "Add Device" }),
    closeButton,
  );

  const skeleton = el("div", { class: "skeleton", style: "height: 260px" });
  const content = el("div", { class: "wizard__body" }, skeleton);
  replace(body, titleBar, content);

  void apiFor(port)
    .pairing()
    .then((pairing) => {
      const close = el("button", { class: "btn btn--ghost", type: "button", text: "Close" });
      on(close, "click", dismiss);
      const payload = JSON.stringify(pairing);

      replace(
        content,
        el("h2", { class: "wizard__title", text: "Add another device" }),
        el("p", {
          class: "wizard__lead",
          text: "Scan this from De-Sentry on the other machine. It carries addresses and a public key — no secret is in the code, so a photograph of it grants nothing on its own.",
        }),
        el("div", { class: "qr qr--constellation" }, qrSvg(payload, { scale: 4, title: "Pairing code" })),
        el(
          "dl",
          { class: "kv" },
          el("dt", { text: "This mesh" }),
          el("dd", { class: "mono", text: shortNode(pairing.node_id, 16) }),
          el("dt", { text: "Discovery port" }),
          el("dd", { class: "mono", text: String(pairing.discovery_port) }),
          el("dt", { text: "Bootstrap peers" }),
          el("dd", {
            class: "mono",
            text:
              pairing.bootstrap_peers.length > 0
                ? pairing.bootstrap_peers.join("\n")
                : "none yet — the other device will find this one by broadcast",
          }),
        ),
      );
      replace(body, titleBar, content, el("div", { class: "wizard__footer" }, el("span", { class: "header__spacer" }), close));
    })
    .catch((error) => {
      replace(
        content,
        el("p", { class: "error-note", text: `Could not build a pairing code: ${describeError(error)}` }),
      );
    });
}

// -- about sheet -------------------------------------------------------------

function openAboutSheet(): void {
  const nodes = store.dataNodes();
  const reachable = nodes.filter((n) => n.reachable).length;
  const port = store.state.supervisorPort;

  // macOS-style About: one centred card, no titlebar/footer bands. The old
  // .wizard titlebar + footer painted darker rectangles behind the title and
  // Done button in dark mode; this layout has neither.
  const done = el("button", { class: "btn btn--primary about-macos__done", type: "button", text: "Done" });
  const body = el("div", { class: "macos-modal about-macos" });
  const sheet = el("dialog", { class: "modal-dialog sheet-dialog sheet--about", "aria-label": "About De-Sentry" }, body);
  const dismiss = () => {
    if (sheet.open) sheet.close();
    sheet.remove();
  };

  const content = el(
    "div",
    { class: "macos-modal__content about-macos__content" },
    el("div", { class: "about-macos__emblem" }, sentryLogoSvg({ size: 76, animated: true })),
    el(
      "div",
      { class: "about-macos__names" },
      el("h2", { class: "macos-modal__heading", text: "De-Sentry" }),
      el("span", { class: "brand-badge", text: "v2.0" }),
    ),
    el("p", { class: "about-macos__tagline", text: "Autonomous · Zero-Trust · Airplane-Mode Native" }),
    el(
      "p",
      { class: "about-macos__blurb", text: "A decentralized database mesh with zero fetched runtime dependencies, cryptographic verification, and conflict-free replication." },
    ),
    el(
      "div",
      { class: "about-macos__grid" },
      el("div", { class: "about-macos__card" }, el("strong", { text: "Cryptographic Aegis" }), el("span", { text: "Ed25519 · X25519 · AES-256-GCM" })),
      el("div", { class: "about-macos__card" }, el("strong", { text: "Tamper-Evident Ledger" }), el("span", { text: "Hash-chained feed · Quorum GC" })),
      el("div", { class: "about-macos__card" }, el("strong", { text: "Data Plane" }), el("span", { text: "CRDTs · Hybrid Logical Clocks" })),
      el("div", { class: "about-macos__card" }, el("strong", { text: "Storage Router" }), el("span", { text: "B+Tree · SQLite · Vector · DuckDB" })),
    ),
    el(
      "dl",
      { class: "kv about-macos__kv" },
      el("dt", { text: "Supervisor" }),
      el("dd", { class: "mono", text: port !== null ? `127.0.0.1:${port}` : "offline" }),
      el("dt", { text: "Active Mesh" }),
      el("dd", { text: `${reachable}/${nodes.length} data nodes online` }),
      el("dt", { text: "Shortcuts" }),
      el("dd", { class: "mono", text: "N (new) · 1 (tree) · 2 (mesh) · 3 (ledger) · Ctrl+R" }),
    ),
    done,
  );
  replace(body, content);

  on(done, "click", dismiss);
  on(sheet, "click", (event) => {
    if (event.target === sheet) dismiss();
  });
  on(sheet, "keydown", (event) => {
    if (event.key === "Escape") dismiss();
  });
  on(sheet, "close", () => sheet.remove());
  document.body.appendChild(sheet);
  try {
    sheet.showModal();
  } catch {
    sheet.setAttribute("open", "");
  }
  done.focus();
}

// -- shell -------------------------------------------------------------------

function build(): void {
  const root = document.getElementById("app");
  if (root === null) throw new Error("#app is missing from index.html");

  const wizard = createWizard();

  // onAddAsPeer: called when the user clicks "Add as peer" on a discovered
  // LAN node. Pre-opens the wizard so they can create a local node that
  // bootstraps from the remote peer's node_id. The wizard will surface the
  // peer in its bootstrap-peers field when supported.
  const onAddAsPeer = (peerNodeId: string) => {
    wizard.open({ bootstrapPeer: peerNodeId });
  };

  lockDarkTheme();

  const sidebar = createSidebar(() => wizard.open(), onAddAsPeer);
  const canvas = createCanvas(() => wizard.open());
  const explorer = createExplorer();
  const ledger = createLedger();
  const consoleView = createConsole();
  const dropbox = createDropbox();
  const healthAlerts = createHealthAlerts((nodeId) => {
    store.select({ kind: "node", nodeId });
  });

  const backdrop = el("canvas", { class: "mesh__backdrop mesh__backdrop--fullscreen", "aria-hidden": "true" }) as HTMLCanvasElement;
  let activeGlobe: GlobeHandle | null = null;
  try {
    activeGlobe = createMeshGlobe(backdrop);
  } catch {
    activeGlobe = null;
  }

  // Header ------------------------------------------------------------------
  const titleRow = el(
    "div",
    { class: "brand-cluster__row" },
    el("p", { class: "header__title", text: "De-Sentry" }),
    el("span", { class: "brand-badge", text: "v2.0" }),
  );
  const title = el(
    "button",
    {
      class: "brand-cluster",
      type: "button",
      title: "About De-Sentry & Mesh Architecture",
      "aria-label": "About De-Sentry",
    },
    el("div", { class: "brand-cluster__emblem" }, sentryLogoSvg({ size: 28, animated: true })),
    el("div", { class: "brand-cluster__text" }, titleRow, el("p", { class: "header__sub" })),
  );
  on(title, "click", openAboutSheet);

  // Pairing uses a QR glyph now: the sheet shows a scannable code that links
  // another device. The old shield conflated pairing with security status.
  const pairButton = el(
    "button",
    {
      class: "btn btn--sm btn--ghost btn--icon-only",
      type: "button",
      title: "Pair another device (QR code)",
      "aria-label": "Pair another device",
    },
    icon(Icons.qr, 15),
  );
  on(pairButton, "click", openPairingSheet);

  const openCommandPalette = () =>
    openPalette({ onNewNode: () => wizard.open(), onPair: openPairingSheet });
  const paletteButton = el(
    "button",
    {
      class: "btn btn--sm btn--ghost btn--icon-only",
      type: "button",
      title: "Command palette (Ctrl+K)",
      "aria-label": "Command palette",
    },
    icon(Icons.search, 14),
  );
  on(paletteButton, "click", openCommandPalette);

  const newButton = el("button", { class: "btn btn--primary btn--sm", type: "button" }, icon(Icons.plus, 13), "New node");
  on(newButton, "click", () => wizard.open());

  // Custom window chrome. The native titlebar (with its own De-Sentry icon +
  // title) is disabled in tauri.conf.json, so this header is the only brand
  // row. Controls render only inside the desktop shell; browsers get no dead
  // buttons.
  const winControls = el("div", { class: "win-controls", hidden: true });
  if (isTauri()) {
    winControls.hidden = false;
    const minBtn = el("button", { class: "win-btn", type: "button", title: "Minimize", "aria-label": "Minimize" }, icon(Icons.minus, 12));
    const maxBtn = el("button", { class: "win-btn", type: "button", title: "Maximize / Restore", "aria-label": "Maximize or restore" }, icon(Icons.square, 11));
    const closeBtn = el("button", { class: "win-btn win-btn--close", type: "button", title: "Close", "aria-label": "Close" }, icon(Icons.close, 12));
    const withWindow = async (fn: (w: { minimize: () => Promise<void>; toggleMaximize: () => Promise<void>; close: () => Promise<void> }) => Promise<void>) => {
      try {
        const mod = await import("@tauri-apps/api/window");
        await fn(mod.getCurrentWindow());
      } catch {
        // Browser dev session or old shell: controls stay visible but inert.
      }
    };
    on(minBtn, "click", () => void withWindow((w) => w.minimize()));
    on(maxBtn, "click", () => void withWindow((w) => w.toggleMaximize()));
    on(closeBtn, "click", () => void withWindow((w) => w.close()));
    replace(winControls, minBtn, maxBtn, closeBtn);
  }

  const header = el(
    "header",
    { class: "header" },
    title,
    el("span", { class: "header__spacer" }),
    paletteButton,
    pairButton,
    newButton,
    winControls,
  );

  const centre = el("div", { class: "centre" });
  root.removeAttribute("data-loading");
  replace(root, backdrop, header, sidebar.element, healthAlerts.element, centre);
  document.body.appendChild(wizard.element);

  // Toasts ------------------------------------------------------------------
  const toastHost = document.getElementById("toasts");

  function renderToasts(): void {
    if (toastHost === null) return;
    replace(
      toastHost,
      ...store.state.toasts.map((toast) => {
        const close = el("button", { class: "btn btn--sm btn--ghost", type: "button", "aria-label": "Dismiss" }, icon(Icons.close, 12));
        on(close, "click", () => store.dismissToast(toast.id));
        return el(
          "div",
          { class: "toast", "data-tone": toast.tone, role: "status" },
          el(
            "div",
            {},
            el("strong", { text: toast.title }),
            toast.detail ? el("p", { class: "muted", text: toast.detail }) : null,
          ),
          close,
        );
      }),
    );
  }

  // Render ------------------------------------------------------------------
  function render(): void {
    const state = store.state;

    if (state.bootError !== "") {
      replace(
        centre,
        el(
          "div",
          { class: "empty" },
          el("p", { class: "empty__title", text: "The control plane is not available" }),
          el("p", { class: "empty__body", text: state.bootError }),
        ),
      );
      renderToasts();
      return;
    }

    const nodes = store.dataNodes();
    const reachable = nodes.filter((n) => n.reachable).length;
    const info = state.appInfo;
    const tip = meshTip();

    const globePoints: GlobeNodePoint[] = nodes.map((n) => ({
      id: n.process.node_id,
      status: convergenceOf(n, tip),
    }));
    activeGlobe?.setData(globePoints, []);
    activeGlobe?.setOnBattery(info?.on_battery ?? false);

    const subtitle = [
      state.busy !== "" ? state.busy : state.supervisorPort === null ? "supervisor starting…" : "supervisor active",
      `${reachable}/${nodes.length} online`,
      info?.on_battery ? "on battery" : null,
      info?.background_mode ? "background sync" : null,
    ]
      .filter(Boolean)
      .join(" · ");
    const sub = title.querySelector(".header__sub");
    if (sub !== null) sub.textContent = subtitle;

    sidebar.render();
    healthAlerts.render();

    // The explorer replaces the canvas when a collection is open; the ledger
    // viewer replaces it when a ledger is open; console replaces it for engine
    // interaction; dropbox replaces it for smart data intake.
    if (state.selection.kind === "collection") {
      explorer.render();
      replace(centre, el("main", { class: "canvas" }, explorer.element));
    } else if (state.selection.kind === "ledger") {
      ledger.render();
      replace(centre, ledger.element);
    } else if (state.selection.kind === "console") {
      consoleView.render();
      replace(centre, consoleView.element);
    } else if (state.selection.kind === "dropbox") {
      dropbox.render();
      replace(centre, dropbox.element);
    } else {
      canvas.render();
      replace(centre, canvas.element);
    }

    renderToasts();
  }

  store.subscribe(render);
  render();

  // Keyboard ---------------------------------------------------------------
  on(document.body, "keydown", (event) => {
    // The palette manages its own keys once open; the global handler must
    // not steal them (its input is a typing target, but check explicitly so
    // a future handler reorder cannot break it).
    const inPalette =
      (event.target as HTMLElement | null)?.closest?.(".palette") != null;
    if (inPalette) return;
    const target = event.target as HTMLElement | null;
    const typing = target !== null && /^(INPUT|TEXTAREA|SELECT)$/.test(target.tagName);
    if (typing) return;

    if (event.key === "?" && !event.metaKey && !event.ctrlKey) {
      event.preventDefault();
      openShortcutsCheatsheet();
    } else if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === "k") {
      event.preventDefault();
      openCommandPalette();
    } else if (event.key === "n" && !event.metaKey && !event.ctrlKey) {
      event.preventDefault();
      wizard.open();
    } else if (event.key === "r" && (event.metaKey || event.ctrlKey)) {
      event.preventDefault();
      void refreshNodeList();
    } else if (event.key === "Escape" && store.state.selection.kind !== "none" && store.state.selection.kind !== "node") {
      const nodeId = store.state.selection.nodeId;
      store.select(nodeId === undefined ? { kind: "none" } : { kind: "node", nodeId });
    } else if (event.key === "1") {
      store.setCanvasMode("tree");
    } else if (event.key === "2") {
      store.setCanvasMode("mesh");
    } else if (event.key === "3") {
      const node = store.selectedNode();
      if (node) store.select({ kind: "ledger", nodeId: node.process.node_id });
    } else if (event.key === "4") {
      const node = store.selectedNode() ?? store.dataNodes()[0];
      if (node) store.select({ kind: "console", nodeId: node.process.node_id });
    } else if (event.key === "5") {
      store.select({ kind: "dropbox" });
    } else if ((event.metaKey || event.ctrlKey) && event.key === "0") {
      event.preventDefault();
      canvasZoomFit();
    } else if ((event.key === "+" || event.key === "=") && !event.metaKey && !event.ctrlKey) {
      event.preventDefault();
      canvasZoomStep(1.25);
    } else if (event.key === "-" && !event.metaKey && !event.ctrlKey) {
      event.preventDefault();
      canvasZoomStep(0.8);
    } else if (event.key === "Escape" && store.state.selection.kind === "node") {
      store.select({ kind: "none" });
    }
  });

  // Lifecycle ---------------------------------------------------------------
  window.addEventListener("beforeunload", () => {
    // Long polls hold a connection open; dropping them on unload stops the
    // node logging a torn-down request for every window close.
    stopAllSubscriptions();
  });

  // The window closing does not stop the nodes -- background mode is what the
  // tray offers, and it is the sidecar's business, not this window's.
  void sidecar
    .appInfo()
    .then((appInfo) => {
      if (!appInfo.background_mode) return;
      store.toast(
        "info",
        "Running in the background",
        "Nodes keep syncing with this window closed. Turn it off from the tray icon.",
        6000,
      );
    })
    .catch(() => undefined);
}

// -- start -------------------------------------------------------------------

build();
void boot().catch((error) => {
  store.state.bootError = describeError(error);
  store.state.ready = true;
  store.notify();
});
