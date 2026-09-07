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
import { sidecar } from "./bridge.js";
import { boot, refreshNodeList, refreshTopology, stopAllSubscriptions, store } from "./state.js";
import { shortNode } from "./util/format.js";
import { el, icon, Icons, on, replace } from "./util/dom.js";
import { qrSvg } from "./util/qr.js";
import { createCanvas } from "./views/canvas.js";
import { createExplorer } from "./views/explorer.js";
import { createInspector } from "./views/inspector.js";
import { createSidebar } from "./views/sidebar.js";
import { createWizard } from "./views/wizard.js";

const THEME_KEY = "desentry.theme";

function describeError(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

// -- theme -------------------------------------------------------------------

type Theme = "system" | "light" | "dark";

function applyTheme(theme: Theme): void {
  const root = document.documentElement;
  if (theme === "system") root.removeAttribute("data-theme");
  else root.setAttribute("data-theme", theme);
  try {
    localStorage.setItem(THEME_KEY, theme);
  } catch {
    // Private windows and locked-down profiles refuse storage. The theme still
    // applies for this session; only the memory of it is lost.
  }
}

function storedTheme(): Theme {
  try {
    const value = localStorage.getItem(THEME_KEY);
    if (value === "light" || value === "dark" || value === "system") return value;
  } catch {
    // fall through
  }
  return "system";
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

  const body = el("div", { class: "wizard" }, el("div", { class: "skeleton", style: "height: 260px" }));
  const sheet = el("div", { class: "sheet", role: "dialog", "aria-modal": "true", tabindex: "-1" }, body);
  const dismiss = () => sheet.remove();
  on(sheet, "click", (event) => {
    if (event.target === sheet) dismiss();
  });
  on(sheet, "keydown", (event) => {
    if (event.key === "Escape") dismiss();
  });
  document.body.appendChild(sheet);
  // Focused so Escape reaches the handler; a div does not receive key events
  // otherwise, and a modal you cannot dismiss with Escape is a trap.
  sheet.focus();

  void apiFor(port)
    .pairing()
    .then((pairing) => {
      const close = el("button", { class: "btn btn--ghost", type: "button", text: "Close" });
      on(close, "click", dismiss);
      const payload = JSON.stringify(pairing);

      replace(
        body,
        el("h2", { class: "wizard__title", text: "Add another device" }),
        el("p", {
          class: "wizard__lead",
          text: "Scan this from De-Sentry on the other machine. It carries addresses and a public key — no secret is in the code, so a photograph of it grants nothing on its own.",
        }),
        el("div", { class: "qr" }, qrSvg(payload, { scale: 4, title: "Pairing code" })),
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
        el("div", { class: "wizard__footer" }, el("span", { class: "header__spacer" }), close),
      );
    })
    .catch((error) => {
      replace(
        body,
        el("p", { class: "error-note", text: `Could not build a pairing code: ${describeError(error)}` }),
      );
    });
}

// -- shell -------------------------------------------------------------------

function build(): void {
  const root = document.getElementById("app");
  if (root === null) throw new Error("#app is missing from index.html");

  const wizard = createWizard();
  const sidebar = createSidebar(() => wizard.open());
  const canvas = createCanvas(() => wizard.open());
  const explorer = createExplorer();
  const inspector = createInspector();

  // Header ------------------------------------------------------------------
  const title = el("div", {}, el("p", { class: "header__title", text: "De-Sentry" }), el("p", { class: "header__sub" }));

  const themeButton = el("button", { class: "btn btn--sm btn--ghost", type: "button", title: "Appearance" });
  const themes: Theme[] = ["system", "light", "dark"];
  const themeLabel: Record<Theme, string> = { system: "Auto", light: "Light", dark: "Dark" };
  let theme = storedTheme();
  const paintTheme = () => {
    themeButton.textContent = themeLabel[theme];
    applyTheme(theme);
  };
  on(themeButton, "click", () => {
    theme = themes[(themes.indexOf(theme) + 1) % themes.length];
    paintTheme();
  });
  paintTheme();

  const pairButton = el("button", { class: "btn btn--sm btn--ghost", type: "button" }, icon(Icons.shield, 14), "Pair");
  on(pairButton, "click", openPairingSheet);

  const refreshButton = el("button", { class: "btn btn--sm btn--ghost", type: "button", title: "Refresh everything" }, icon(Icons.refresh, 14));
  on(refreshButton, "click", () => {
    void refreshNodeList();
    void refreshTopology();
  });

  const newButton = el("button", { class: "btn btn--primary btn--sm", type: "button" }, icon(Icons.plus, 14), "New node");
  on(newButton, "click", () => wizard.open());

  const busyNote = el("span", { class: "muted" });

  const header = el(
    "header",
    { class: "header" },
    title,
    el("span", { class: "header__spacer" }),
    busyNote,
    themeButton,
    pairButton,
    refreshButton,
    newButton,
  );

  const centre = el("div", {});
  root.removeAttribute("data-loading");
  replace(root, header, sidebar.element, centre, inspector.element);
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
    const subtitle = [
      state.supervisorPort === null ? "supervisor starting…" : "supervisor running",
      `${reachable}/${nodes.length} nodes answering`,
      info?.on_battery ? "on battery — gossip throttled" : null,
      info?.background_mode ? "syncing in the background" : null,
    ]
      .filter(Boolean)
      .join(" · ");
    const sub = title.querySelector(".header__sub");
    if (sub !== null) sub.textContent = subtitle;

    busyNote.textContent = state.busy;

    sidebar.render();
    inspector.render();

    // The explorer replaces the canvas when a collection is open: they are two
    // ways of looking at the same node, and showing both halves the room each
    // gets.
    if (state.selection.kind === "collection") {
      explorer.render();
      replace(centre, el("main", { class: "canvas" }, explorer.element));
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
    const target = event.target as HTMLElement | null;
    const typing = target !== null && /^(INPUT|TEXTAREA|SELECT)$/.test(target.tagName);
    if (typing) return;

    if (event.key === "n" && !event.metaKey && !event.ctrlKey) {
      event.preventDefault();
      wizard.open();
    } else if (event.key === "r" && (event.metaKey || event.ctrlKey)) {
      event.preventDefault();
      void refreshNodeList();
    } else if (event.key === "Escape" && store.state.selection.kind === "collection") {
      const nodeId = store.state.selection.nodeId;
      store.select(nodeId === undefined ? { kind: "none" } : { kind: "node", nodeId });
    } else if (event.key === "1") {
      store.setCanvasMode("tree");
    } else if (event.key === "2") {
      store.setCanvasMode("mesh");
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
