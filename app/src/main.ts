/**
 * Entry point: builds the shell, wires the views to the store, and renders.
 *
 * The render loop is one function. Every view exposes `render()` and reads the
 * store directly, so a state change re-renders everything that is on screen --
 * which, at this size, costs less than the bookkeeping needed to render less.
 * The one exception is the document editor, which keeps its own text so a
 * re-render does not move the caret.
 */

import { isTauri, sidecar } from "./bridge.js";
import { boot, convergenceOf, reachabilitySummary, refreshNodeList, stopAllSubscriptions, store } from "./state.js";
import { el, icon, Icons, on, replace } from "./util/dom.js";
import { sentryLogoSvg } from "./util/logo.js";
import { qrSvg } from "./util/qr.js";
import { createMeshGlobe, type GlobeHandle, type GlobeLink, type GlobeNodePoint } from "./util/meshGlobe.js";
import { canvasZoomFit, canvasZoomStep, createCanvas } from "./views/canvas.js";
import { createExplorer } from "./views/explorer.js";
import { createHealthAlerts } from "./views/healthAlerts.js";
import { createLedger } from "./views/ledger.js";
import { createSidebar } from "./views/sidebar.js";
import { createWizard } from "./views/wizard.js";
import { createConsole } from "./views/console.js";
import { createDropbox } from "./views/dropbox.js";
import { createLookup } from "./views/lookup.js";
import { openPalette } from "./views/palette.js";

function describeError(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

function formatPeerAddress(address: string, port: number): string {
  const host = address.includes(":") && !address.startsWith("[") ? `[${address}]` : address;
  return `${host}:${port}`;
}

function validPeerAddress(peer: string): boolean {
  const match = /^(?:\[[0-9a-fA-F:]+\]|[^\s,:]+):(\d{1,5})$/.exec(peer);
  return match !== null && Number(match[1]) >= 1 && Number(match[1]) <= 65535;
}

function parseLanMeshInvite(text: string): { peers: string[]; meshSecret: string } {
  let value: unknown;
  try {
    value = JSON.parse(text);
  } catch {
    throw new Error("The scanned text is not a De-Sentry LAN mesh invite.");
  }
  if (typeof value !== "object" || value === null) throw new Error("The scanned text is not a De-Sentry LAN mesh invite.");
  const invite = value as Record<string, unknown>;
  if (
    invite.type !== "de-sentry.lan-mesh-invite" ||
    invite.version !== 1 ||
    !Array.isArray(invite.peers) ||
    invite.peers.length === 0 ||
    !invite.peers.every((peer) => typeof peer === "string" && validPeerAddress(peer)) ||
    typeof invite.mesh_secret !== "string" ||
    !/^[0-9a-fA-F]{64}$/.test(invite.mesh_secret)
  ) {
    throw new Error("The QR invite is missing a valid host address or mesh code.");
  }
  return { peers: invite.peers as string[], meshSecret: invite.mesh_secret.toLowerCase() };
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
  const onKey = (event: KeyboardEvent) => {
    if (event.key === "Escape") {
      event.preventDefault();
      event.stopPropagation();
      dismiss();
    }
  };
  const dismiss = () => {
    window.removeEventListener("keydown", onKey);
    if (sheet.open) sheet.close();
    sheet.remove();
  };
  window.addEventListener("keydown", onKey);

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

/** Join another device to an existing LAN mesh, or show this node's invite details. */
function openPairingSheet(onJoin: (peers: string[], meshSecret: string) => void): void {

  const body = el("div", { class: "wizard" });
  const sheet = el("div", { class: "sheet", role: "dialog", "aria-modal": "true", tabindex: "-1" }, body);
  const onKey = (event: KeyboardEvent) => {
    if (event.key === "Escape") {
      event.preventDefault();
      event.stopPropagation();
      dismiss();
    }
  };
  const dismiss = () => {
    window.removeEventListener("keydown", onKey);
    sheet.remove();
  };
  window.addEventListener("keydown", onKey);
  on(sheet, "click", (event) => {
    if (event.target === sheet) dismiss();
  });
  document.body.appendChild(sheet);
  sheet.focus();

  const closeButton = el("button", { class: "sheet-close", type: "button", title: "Close", "aria-label": "Close" }, icon(Icons.close, 16));
  on(closeButton, "click", dismiss);
  const titleBar = el(
    "div",
    { class: "wizard__titlebar" },
    el("span", { style: "width: 28px;" }),
    el("span", { class: "wizard__titlebar-title", text: "Join a LAN Mesh" }),
    closeButton,
  );

  const skeleton = el("div", { class: "skeleton", style: "height: 260px" });
  const content = el("div", { class: "wizard__body" }, skeleton);
  replace(body, titleBar, content);

  const peerInput = el("textarea", {
    class: "input",
    rows: "2",
    placeholder: "192.168.1.20:7802",
    "aria-label": "Peer address or addresses",
  }) as HTMLTextAreaElement;
  const secretInput = el("input", {
    class: "input",
    type: "password",
    autocomplete: "off",
    placeholder: "Paste the mesh code from the other device",
    "aria-label": "Mesh code",
  }) as HTMLInputElement;
  const scannedInviteInput = el("textarea", {
    class: "input",
    rows: "2",
    placeholder: "Paste the text copied from the host’s QR code",
    "aria-label": "Scanned LAN mesh invite",
  }) as HTMLTextAreaElement;
  const useInviteButton = el("button", { class: "btn btn--sm", type: "button", text: "Use scanned invite" });
  on(useInviteButton, "click", () => {
    try {
      const invite = parseLanMeshInvite(scannedInviteInput.value.trim());
      peerInput.value = invite.peers.join("\n");
      secretInput.value = invite.meshSecret;
      store.toast("success", "Mesh invite loaded", "Host address and mesh code are ready.", 3500);
    } catch (error) {
      store.toast("warning", "Could not read mesh invite", describeError(error), 5000);
    }
  });
  const shareSection = el(
    "section",
    { class: "stack" },
    el("h3", { class: "card__title", text: "Share this mesh" }),
    el("div", { class: "muted", text: "Loading this mesh’s join details…" }),
  );
  const joinButton = el("button", { class: "btn btn--primary", type: "button", text: "Continue to create node" });
  on(joinButton, "click", () => {
    const peers = peerInput.value.split(/[\n,;]+/).map((peer) => peer.trim()).filter(Boolean);
    const valid = peers.every(validPeerAddress);
    if (peers.length === 0 || !valid) {
      store.toast("warning", "Enter a peer address", "Use the other computer’s LAN IP and P2P port, for example 192.168.1.20:7801.");
      return;
    }
    if (!/^[0-9a-fA-F]{64}$/.test(secretInput.value.trim())) {
      store.toast("warning", "Enter the mesh code", "Paste the 64-character code shown on a device in the mesh you want to join.");
      return;
    }
    dismiss();
    onJoin(peers, secretInput.value.trim().toLowerCase());
  });

  replace(
    content,
    el("h2", { class: "wizard__title", text: "Join a LAN Mesh" }),
    el("p", { class: "wizard__lead", text: "On the host computer, choose LAN Mesh when creating its first node. On each other computer, enter one of that node’s LAN addresses and its P2P port, then the shared mesh code." }),
    shareSection,
    el("hr"),
    el("div", { class: "pairing-field" },
      el("label", { class: "field-label", text: "Scanned host invite (optional)" }),
      scannedInviteInput,
      el("p", { class: "muted", text: "Scan the host QR with a camera or QR reader, copy its text, then paste it here to fill the address and mesh code." }),
      useInviteButton,
    ),
    el("div", { class: "pairing-field" },
      el("label", { class: "field-label", text: "Host address and P2P port" }),
      peerInput,
    ),
    el("div", { class: "pairing-field" },
      el("label", { class: "field-label", text: "Mesh code from the host" }),
      secretInput,
    ),
    el("p", { class: "muted", text: "The next screen will create a LAN Mesh node with these details. Isolated Standalone nodes keep discovery off and do not join this mesh." }),
    joinButton,
  );

  void (async () => {
    try {
      // Refresh the sidecar registry at the point the user asks to pair. This
      // also restores any managed local nodes into the sidebar if boot raced
      // the supervisor coming online.
      await refreshNodeList();
      const nodes = await sidecar.listNodes();
      const [meshSecret, lanAddress] = await Promise.all([sidecar.meshSecret(), sidecar.lanAddress()]);
      const localNodes = nodes.filter((node) => !node.supervisor && node.discovery_enabled && node.process === "running");
      const hostPeer = localNodes.length > 0 && lanAddress
        ? formatPeerAddress(lanAddress, localNodes[0].p2p_port)
        : null;
      const qrInvite = hostPeer === null
        ? null
        : qrSvg(JSON.stringify({
            type: "de-sentry.lan-mesh-invite",
            version: 1,
            peers: [hostPeer],
            mesh_secret: meshSecret,
          }), { scale: 4, title: "LAN mesh invite" });
      const shareDetails = el(
        "div",
        { class: "pairing-share" },
        qrInvite !== null
          ? el("div", { class: "pairing-share__qr" },
              el("div", { class: "qr qr--constellation" }, qrInvite),
              el("p", { class: "muted pairing-share__note", text: "Scan this invite to get the host address and mesh code." }),
            )
          : el("div", { class: "pairing-share__qr pairing-share__qr--empty" },
              el("p", { class: "muted", text: "Start a LAN Mesh data node with a detected LAN address to show its join QR." }),
            ),
        el("div", { class: "pairing-share__meta" },
          el("dl", { class: "kv" },
            el("dt", { text: "Host address and P2P port" }),
            el("dd", { class: "mono", style: "user-select: all; overflow-wrap: anywhere;", text: localNodes.length > 0 && lanAddress ? localNodes.map((node) => formatPeerAddress(lanAddress, node.p2p_port)).join("\n") : localNodes.length > 0 ? `LAN address could not be detected. Use this node's P2P port (${localNodes.map((node) => node.p2p_port).join(", ")}) with the host computer's LAN IP.` : "Create and start a LAN Mesh data node first." }),
            el("dt", { text: "Mesh code" }),
            el("dd", { class: "mono", style: "user-select: all; overflow-wrap: anywhere;", text: meshSecret }),
          ),
        ),
        el("p", { class: "muted pairing-share__note", text: "On each other computer, use Pair another device and enter one address above plus this mesh code." }),
        el("p", { class: "muted pairing-share__note", text: "The mesh code grants access to this mesh. Share it only with trusted people." }),
      );
      replace(shareSection, el("h3", { class: "card__title", text: "Share this mesh" }), shareDetails);
    } catch (error) {
      replace(shareSection, el("h3", { class: "card__title", text: "Share this mesh" }), el("p", { class: "error-note", text: `${describeError(error)} Joining another device is still available above.` }));
    }
  })();
}

// -- about sheet -------------------------------------------------------------

function openAboutSheet(): void {
  const nodes = store.dataNodes();
  const nodeSummary = reachabilitySummary(nodes);
  const port = store.state.supervisorPort;
  const appInfo = store.state.appInfo;
  const appVersion = appInfo?.version ?? "unknown";
  const engineVersion = appInfo?.engine_version ?? "unknown";

  // macOS-style About: one centred card, no titlebar/footer bands. The old
  // .wizard titlebar + footer painted darker rectangles behind the title and
  // Done button in dark mode; this layout has neither.
  const done = el("button", { class: "btn btn--primary about-macos__done", type: "button", text: "Done" });
  const body = el("div", { class: "macos-modal about-macos" });
  const sheet = el("dialog", { class: "modal-dialog sheet-dialog sheet--about", "aria-label": "About De-Sentry" }, body);
  const onKey = (event: KeyboardEvent) => {
    if (event.key === "Escape") {
      event.preventDefault();
      event.stopPropagation();
      dismiss();
    }
  };
  const dismiss = () => {
    window.removeEventListener("keydown", onKey);
    if (sheet.open) sheet.close();
    sheet.remove();
  };
  window.addEventListener("keydown", onKey);

  // Update check state
  let checkState: "idle" | "checking" | "up-to-date" | "available" | "installing" | "installed" | "error" = "idle";
  let updateStatusText = "";
  let pendingUpdate: import("@tauri-apps/plugin-updater").Update | null = null;
  let checkButton: HTMLButtonElement | null = null;
  let statusEl: HTMLElement | null = null;

  async function onUpdateClick(): Promise<void> {
    if (checkState === "available" && pendingUpdate) {
      await installUpdate(pendingUpdate);
    } else {
      await checkForUpdates();
    }
  }

  async function installUpdate(update: import("@tauri-apps/plugin-updater").Update): Promise<void> {
    checkState = "installing";
    updateStatusText = "Downloading update...";
    renderUpdateStatus();
    try {
      await update.downloadAndInstall();
      checkState = "installed";
      updateStatusText = `v${update.version} installed — restart app`;
    } catch (err) {
      console.warn("Update install failed:", err);
      checkState = "error";
      updateStatusText = "Install failed";
    }
    renderUpdateStatus();
  }

  async function checkForUpdates(): Promise<void> {
    if (!isTauri() || checkState === "checking" || checkState === "installing") return;
    checkState = "checking";
    updateStatusText = "";
    renderUpdateStatus();

    try {
      const { check } = await import("@tauri-apps/plugin-updater");
      const timeoutMs = 8_000;
      const timeoutPromise = new Promise<never>((_, reject) =>
        setTimeout(() => reject(new Error("Update check timed out")), timeoutMs)
      );
      const update = await Promise.race([
        check({ timeout: timeoutMs }),
        timeoutPromise,
      ]);
      if (update === null) {
        checkState = "up-to-date";
        pendingUpdate = null;
        updateStatusText = "You're up to date";
      } else {
        checkState = "available";
        pendingUpdate = update;
        updateStatusText = `v${update.version} available`;
      }
    } catch (error) {
      console.warn("Failed to check for updates:", error);
      checkState = "error";
      pendingUpdate = null;
      const errStr = String(error).toLowerCase();
      if (errStr.includes("target") || errStr.includes("platform")) {
        updateStatusText = navigator.userAgent.includes("Mac") ? "No macOS build in feed" : "No build for platform";
      } else if (errStr.includes("time") || errStr.includes("timeout")) {
        updateStatusText = "Timed out (check internet)";
      } else {
        updateStatusText = "Feed unreachable";
      }
    }
    renderUpdateStatus();
  }

  function renderUpdateStatus(): void {
    if (!checkButton || !statusEl) return;
    statusEl.textContent = updateStatusText;
    if (checkState === "checking") {
      checkButton.disabled = true;
      checkButton.textContent = "Checking...";
    } else if (checkState === "up-to-date") {
      checkButton.disabled = false;
      checkButton.textContent = "Check Again";
    } else if (checkState === "available" && pendingUpdate) {
      checkButton.disabled = false;
      checkButton.textContent = `Install v${pendingUpdate.version}`;
    } else if (checkState === "installing") {
      checkButton.disabled = true;
      checkButton.textContent = "Installing...";
    } else if (checkState === "installed") {
      checkButton.disabled = true;
      checkButton.textContent = "Restart Required";
    } else if (checkState === "error") {
      checkButton.disabled = false;
      checkButton.textContent = "Retry";
    } else {
      checkButton.disabled = false;
      checkButton.textContent = "Check for Updates";
    }
  }

  const checkForUpdatesBtn = el(
    "button",
    { class: "btn btn--sm btn--ghost", type: "button", text: "Check for Updates" },
  );
  checkButton = checkForUpdatesBtn;
  on(checkForUpdatesBtn, "click", () => { void onUpdateClick(); });

  statusEl = el("span", { class: "about-macos__update-status", text: "" });

  const content = el(
    "div",
    { class: "macos-modal__content about-macos__content" },
    el("div", { class: "about-macos__emblem" }, sentryLogoSvg({ size: 76, animated: true })),
    el(
      "div",
      { class: "about-macos__names" },
      el("h2", { class: "macos-modal__heading", text: "De-Sentry" }),
      el("span", { class: "brand-badge", text: `v${appVersion}` }),
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
      el("dt", { text: "App Version" }),
      el("dd", { class: "mono", text: appVersion }),
      el("dt", { text: "Engine Version" }),
      el("dd", { class: "mono", text: engineVersion }),
      el("dt", { text: "Supervisor" }),
      el("dd", { class: "mono", text: port !== null ? `127.0.0.1:${port}` : "offline" }),
      el("dt", { text: "Active Mesh" }),
      el("dd", { text: nodeSummary }),
      el("dt", { text: "Shortcuts" }),
      el("dd", { class: "mono", text: "N (new) · 1 (tree) · 2 (mesh) · 3 (ledger) · Ctrl+R" }),
      el("dt", { text: "Updates" }),
      el("dd", { class: "about-macos__update-row" }, checkForUpdatesBtn, statusEl),
    ),
    done,
  );
  replace(body, content);

  on(done, "click", dismiss);
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
  const showAddDevice = () => openPairingSheet((peers, meshSecret) => wizard.open({ bootstrapPeers: peers, meshSecret }));

  // Peer discovery rows route through the same Add Device flow.
  const onAddAsPeer = (_peerNodeId: string) => showAddDevice();

  lockDarkTheme();

  const sidebar = createSidebar(() => wizard.open(), onAddAsPeer);
  const canvas = createCanvas(() => wizard.open());
  const explorer = createExplorer();
  const explorerMain = el("main", { class: "canvas" }, explorer.element);
  const ledger = createLedger();
  const consoleView = createConsole();
  const dropbox = createDropbox();
  const lookup = createLookup();
  const lookupMount = el("div", { class: "canvas" }, lookup.element);
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
    el("span", { class: "brand-badge", text: "v1.0" }),
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

  // Mesh icon opens the QR invite and join form.
  const pairButton = el(
    "button",
    {
      class: "btn btn--sm btn--ghost btn--icon-only",
      type: "button",
      title: "Join or share a LAN mesh",
      "aria-label": "Pair another device",
    },
    icon(Icons.mesh, 15),
  );
  on(pairButton, "click", showAddDevice);

  const openCommandPalette = () =>
    openPalette({ onNewNode: () => wizard.open(), onPair: showAddDevice });
  const paletteButton = el(
    "button",
    {
      class: "btn btn--sm btn--ghost btn--icon-only",
      type: "button",
      title: "Command palette (Ctrl+K)",
      "aria-label": "Command palette",
    },
    icon(Icons.terminal, 14),
  );
  on(paletteButton, "click", openCommandPalette);

  const lookupButton = el(
    "button",
    {
      class: "btn btn--sm btn--ghost btn--icon-only",
      type: "button",
      title: "Universal Lookup (Ctrl+L)",
      "aria-label": "Universal Lookup",
    },
    icon(Icons.search, 14),
  );
  on(lookupButton, "click", () => store.select({ kind: "lookup" }));

  const newButton = el("button", { class: "btn btn--primary btn--sm", type: "button" }, icon(Icons.plus, 13), "New node");
  on(newButton, "click", () => wizard.open());

  // Windows custom window controls (integrated in header on Windows/non-macOS desktop)
  const isWindowsDesktop = isTauri() && !/Macintosh|Mac OS X/.test(navigator.userAgent);
  let winControls: HTMLElement | null = null;
  if (isWindowsDesktop) {
    import("@tauri-apps/api/window").then(({ getCurrentWindow }) => {
      const appWindow = getCurrentWindow();
      appWindow.setDecorations(false).catch(() => {});
    }).catch(() => {});

    const minBtn = el(
      "button",
      {
        class: "header__win-btn header__win-btn--min",
        type: "button",
        title: "Minimize",
        "aria-label": "Minimize",
        tabIndex: -1,
      },
      icon(Icons.minus, 10),
    );
    on(minBtn, "click", (e) => {
      e.stopPropagation();
      import("@tauri-apps/api/window").then(({ getCurrentWindow }) => {
        getCurrentWindow().minimize().catch(() => {});
      }).catch(() => {});
    });

    const maxBtn = el(
      "button",
      {
        class: "header__win-btn header__win-btn--max",
        type: "button",
        title: "Maximize",
        "aria-label": "Maximize",
        tabIndex: -1,
      },
      icon(Icons.square, 10),
    );
    const updateMaxIcon = () => {
      import("@tauri-apps/api/window").then(({ getCurrentWindow }) => {
        getCurrentWindow().isMaximized().then((maximized) => {
          replace(maxBtn, icon(maximized ? Icons.restoreWindow : Icons.square, 10));
          maxBtn.title = maximized ? "Restore" : "Maximize";
          maxBtn.setAttribute("aria-label", maxBtn.title);
        }).catch(() => {});
      }).catch(() => {});
    };
    on(maxBtn, "click", (e) => {
      e.stopPropagation();
      import("@tauri-apps/api/window").then(({ getCurrentWindow }) => {
        getCurrentWindow().toggleMaximize().then(updateMaxIcon).catch(() => {});
      }).catch(() => {});
    });

    const closeBtn = el(
      "button",
      {
        class: "header__win-btn header__win-btn--close",
        type: "button",
        title: "Close",
        "aria-label": "Close",
        tabIndex: -1,
      },
      icon(Icons.close, 10),
    );
    on(closeBtn, "click", (e) => {
      e.stopPropagation();
      import("@tauri-apps/api/window").then(({ getCurrentWindow }) => {
        getCurrentWindow().close().catch(() => {});
      }).catch(() => {});
    });

    winControls = el(
      "div",
      { class: "header__win-controls", "aria-label": "Window controls" },
      minBtn,
      maxBtn,
      closeBtn,
    );

    window.addEventListener("resize", updateMaxIcon);
  }

  const headerChildren: HTMLElement[] = [
    title,
    el("span", { class: "header__spacer" }),
    lookupButton,
    paletteButton,
    pairButton,
    newButton,
  ];
  if (winControls !== null) {
    headerChildren.push(winControls);
  }

  const header = el(
    "header",
    {
      class: `header${isTauri() && /Macintosh|Mac OS X/.test(navigator.userAgent) ? " header--macos-overlay" : ""}${isWindowsDesktop ? " header--windows" : ""}`,
      "data-tauri-drag-region": true,
    },
    ...headerChildren,
  );

  if (isWindowsDesktop) {
    on(header, "dblclick", (e) => {
      const target = e.target as HTMLElement | null;
      if (target === header || target?.classList.contains("header__spacer")) {
        import("@tauri-apps/api/window").then(({ getCurrentWindow }) => {
          getCurrentWindow().toggleMaximize().catch(() => {});
        }).catch(() => {});
      }
    });
  }

  const centre = el("div", { class: "centre" });
  root.removeAttribute("data-loading");
  replace(root, backdrop, header, sidebar.element, healthAlerts.element, centre);
  document.body.appendChild(wizard.element);

  // Toasts ------------------------------------------------------------------
  const toastHost = document.getElementById("toasts");
  const expandedToasts = new Set<number>();

  function renderToasts(): void {
    if (toastHost === null) return;
    replace(
      toastHost,
      ...store.state.toasts.map((toast) => {
        const isExpanded = expandedToasts.has(toast.id);
        const dot = el("span", { class: "toast__dot", "aria-hidden": "true" });
        const titleEl = el("span", { class: "toast__title", text: toast.title });
        const contentChildren = [titleEl];
        if (toast.detail) {
          contentChildren.push(el("span", { class: "toast__detail", text: toast.detail }));
        }
        const content = el("div", { class: "toast__content" }, ...contentChildren);
        const close = el(
          "button",
          { class: "toast__close", type: "button", "aria-label": "Dismiss", title: "Dismiss" },
          icon(Icons.close, 10),
        );
        on(close, "click", (e) => {
          e.stopPropagation();
          expandedToasts.delete(toast.id);
          store.dismissToast(toast.id);
        });
        const toastEl = el(
          "div",
          {
            class: isExpanded ? "toast toast--expanded" : "toast",
            "data-tone": toast.tone,
            "data-expanded": String(isExpanded),
            role: "status",
            title: isExpanded ? "Click to collapse" : (toast.detail ? "Click to expand" : undefined),
          },
          dot,
          content,
          close,
        );
        on(toastEl, "click", () => {
          if (expandedToasts.has(toast.id)) {
            expandedToasts.delete(toast.id);
          } else {
            expandedToasts.add(toast.id);
            store.freezeToast(toast.id);
          }
          renderToasts();
        });
        return toastEl;
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
    const info = state.appInfo;
    const globePoints: GlobeNodePoint[] = nodes.map((n) => ({
      id: n.process.node_id,
      status: convergenceOf(n),
    }));
    // Authentic backdrop: same peer edges the mesh view draws, so the
    // background constellation forms/breaks with the actual hierarchy.
    const knownIds = new Set(nodes.map((n) => n.process.node_id));
    const seenLinks = new Set<string>();
    const globeLinks: GlobeLink[] = [];
    for (const n of nodes) {
      for (const peer of n.peers) {
        if (!knownIds.has(peer.node_id)) continue;
        const key = [n.process.node_id, peer.node_id].sort().join("|");
        if (seenLinks.has(key)) continue;
        seenLinks.add(key);
        globeLinks.push({
          a: n.process.node_id,
          b: peer.node_id,
          live: peer.state === "running" && Date.now() - peer.last_seen_ms < 30_000,
          fitness: peer.fitness?.score ?? 0.5,
        });
      }
    }
    activeGlobe?.setData(globePoints, globeLinks);
    activeGlobe?.setOnBattery(info?.on_battery ?? false);

    const subtitle = [
      state.busy !== "" ? state.busy : state.supervisorPort === null ? "supervisor starting…" : "supervisor active",
      reachabilitySummary(nodes),
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
    const mountInCentre = (viewEl: HTMLElement) => {
      if (centre.firstElementChild !== viewEl || centre.childElementCount !== 1) {
        replace(centre, viewEl);
      }
    };

    if (state.selection.kind === "collection") {
      explorer.render();
      mountInCentre(explorerMain);
    } else if (state.selection.kind === "ledger") {
      ledger.render();
      mountInCentre(ledger.element);
    } else if (state.selection.kind === "console") {
      consoleView.render();
      mountInCentre(consoleView.element);
    } else if (state.selection.kind === "dropbox") {
      dropbox.render();
      mountInCentre(dropbox.element);
    } else if (state.selection.kind === "lookup") {
      lookup.render();
      mountInCentre(lookupMount);
    } else {
      canvas.render();
      mountInCentre(canvas.element);
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

    if (event.key === "Escape") {
      if (wizard.isOpen()) return;
    }

    if (event.key === "?" && !event.metaKey && !event.ctrlKey) {
      event.preventDefault();
      openShortcutsCheatsheet();
    } else if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === "k") {
      event.preventDefault();
      openCommandPalette();
    } else if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === "l") {
      event.preventDefault();
      store.select({ kind: "lookup" });
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
      const node = store.selectedNode() ?? store.dataNodes()[0];
      if (node) {
        store.select({ kind: "ledger", nodeId: node.process.node_id });
      } else {
        store.select({ kind: "ledger" });
        store.toast("info", "Ledger Feed", "No active nodes in mesh. Provision a node to view its transactions.", 3500);
      }
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
