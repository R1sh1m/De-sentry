/**
 * Pairing dialog: join an existing mesh via a 6-digit code or QR scan.
 *
 * The dialog offers two paths:
 *   * Enter a code manually (formatted as `482-915`).
 *   * Show this device's code as a QR for the other device to scan.
 *
 * All text is escaped through el() text nodes (AGENTS.md: never innerHTML
 * with peer/user text).
 */

import { el, icon, Icons, on, replace } from "../util/dom.js";
import { sidecar, type PairingInfo } from "../bridge.js";
import { qrSvg } from "../util/qr.js";
import { refreshNodeList, refreshTopology, store } from "../state.js";

interface PairingDialogOptions {
  /** Called when the dialog is dismissed without joining. */
  onCancel: () => void;
  /** Called after a successful join. */
  onJoin: () => void;
}

/** Formats a raw 6-digit code as `XXX-XXX`. */
function formatCode(raw: string): string {
  const digits = raw.replace(/\D/g, "").slice(0, 6);
  if (digits.length <= 3) return digits;
  return `${digits.slice(0, 3)}-${digits.slice(3, 6)}`;
}

/** Parses a formatted code back to raw digits. */
function parseCode(formatted: string): string {
  return formatted.replace(/\D/g, "");
}

export function openPairingDialog(options: PairingDialogOptions): void {
  const overlay = el("div", { class: "modal-overlay", role: "dialog", "aria-modal": "true", "aria-labelledby": "pairing-dialog-title" });
  const dialog = el("div", { class: "modal modal--narrow" });

  let pairingInfo: PairingInfo | null = null;
  let qrContainer: HTMLElement | null = null;

  const title = el("h2", { id: "pairing-dialog-title", class: "modal__title", text: "Join Mesh" });
  
  const introText = el("p", {
    class: "modal__body",
    text: "Enter the 6-digit code from the device you want to join, or show your code for them to scan.",
  });

  // Code input section
  const inputSection = el("div", { class: "stack", style: "gap: 12px;" });
  
  const inputLabel = el("label", {
    for: "pairing-code-input",
    class: "label",
    text: "Mesh code",
  });
  
  const codeInput = el("input", {
    id: "pairing-code-input",
    type: "text",
    class: "input",
    placeholder: "000-000",
    maxLength: 7, // 6 digits + optional dash
    autocomplete: "off",
  }) as HTMLInputElement;

  // Auto-format on input
  on(codeInput, "input", () => {
    const cursorPos = codeInput.selectionStart || 0;
    const raw = parseCode(codeInput.value);
    const formatted = formatCode(raw);
    codeInput.value = formatted;
    // Adjust cursor for auto-inserted dash
    if (formatted.length > raw.length && cursorPos >= 3) {
      codeInput.setSelectionRange(cursorPos + 1, cursorPos + 1);
    }
  });

  // Allow paste of full code
  on(codeInput, "paste", (e) => {
    e.preventDefault();
    const pasteData = (e as ClipboardEvent).clipboardData?.getData("text") || "";
    const raw = parseCode(pasteData);
    codeInput.value = formatCode(raw);
  });

  const inputActions = el("div", { class: "row", style: "gap: 8px;" });
  
  const joinBtn = el("button", {
    class: "btn btn--primary",
    type: "button",
    disabled: true,
  }, icon(Icons.plug, 14), el("span", { style: "margin-left: 6px;", text: "Join Mesh" }));

  const showMyCodeBtn = el("button", {
    class: "btn btn--ghost",
    type: "button",
    text: "Show my code",
  });

  inputActions.appendChild(joinBtn);
  inputActions.appendChild(showMyCodeBtn);

  inputSection.appendChild(inputLabel);
  inputSection.appendChild(codeInput);
  inputSection.appendChild(inputActions);

  // QR display section (hidden initially)
  const qrSection = el("div", { class: "stack", style: "gap: 12px;", hidden: true });
  
  const qrTitle = el("h3", { class: "modal__subtitle", text: "Scan to Join" });
  const qrHint = el("p", {
    class: "muted",
    style: "font-size: 13px;",
    text: "On the other device, scan this QR code or enter the code below.",
  });
  
  qrContainer = el("div", {
    class: "qr-container",
    style: "display: flex; justify-content: center; padding: 16px; background: #fff; border-radius: 8px;",
  });

  const codeDisplay = el("div", {
    class: "mono",
    style: "text-align: center; font-size: 24px; letter-spacing: 4px; padding: 12px;",
    text: "Loading…",
  });

  const copyBtn = el("button", {
    class: "btn btn--sm btn--ghost",
    type: "button",
  }, icon(Icons.copy, 12), el("span", { style: "margin-left: 4px;", text: "Copy code" }));

  on(copyBtn, "click", async () => {
    if (pairingInfo) {
      try {
        await navigator.clipboard.writeText(pairingInfo.code);
        store.toast("success", "Code copied", pairingInfo.code);
      } catch {
        store.toast("error", "Copy failed", "Select and copy manually");
      }
    }
  });

  const hideQrBtn = el("button", {
    class: "btn btn--sm btn--ghost",
    type: "button",
    text: "Enter code instead",
  });

  qrSection.appendChild(qrTitle);
  qrSection.appendChild(qrHint);
  qrSection.appendChild(qrContainer);
  qrSection.appendChild(codeDisplay);
  qrSection.appendChild(el("div", { class: "row", style: "gap: 8px; justify-content: center;" }, copyBtn, hideQrBtn));

  // Dialog actions
  const actions = el("div", { class: "modal__actions" });
  
  const cancelBtn = el("button", {
    class: "btn btn--ghost",
    type: "button",
    text: "Cancel",
  });

  actions.appendChild(cancelBtn);

  dialog.appendChild(title);
  dialog.appendChild(introText);
  dialog.appendChild(inputSection);
  dialog.appendChild(qrSection);
  dialog.appendChild(actions);
  overlay.appendChild(dialog);

  // Load pairing code for QR display
  const loadPairingCode = async () => {
    try {
      pairingInfo = await sidecar.pairingCode();
      if (qrContainer) {
        replace(qrContainer, qrSvg(pairingInfo.code, {
          scale: 5,
          quietZone: 4,
          title: "Scan to join mesh",
        }));
      }
      if (codeDisplay) {
        // Extract just the 6-digit portion from the full code for display
        const codeMatch = pairingInfo.code.match(/DSN1-([A-Z0-9]{6})/);
        codeDisplay.textContent = codeMatch ? codeMatch[1].replace(/(.{3})(.{3})/, "$1-$2") : pairingInfo.code;
      }
    } catch (err) {
      if (qrContainer) {
        replace(qrContainer, el("div", {
          class: "muted",
          style: "text-align: center; padding: 24px;",
          text: "No running node available to generate a pairing code. Start a node first.",
        }));
      }
      if (codeDisplay) {
        codeDisplay.textContent = "Unavailable";
      }
    }
  };

  // Show QR code section
  on(showMyCodeBtn, "click", () => {
    inputSection.hidden = true;
    qrSection.hidden = false;
    loadPairingCode();
  });

  // Hide QR, show input
  on(hideQrBtn, "click", () => {
    qrSection.hidden = true;
    inputSection.hidden = false;
    codeInput.focus();
  });

  // Validate input and enable join button
  on(codeInput, "input", () => {
    const raw = parseCode(codeInput.value);
    joinBtn.toggleAttribute("disabled", raw.length !== 6);
  });

  // Join mesh
  on(joinBtn, "click", async () => {
    const raw = parseCode(codeInput.value);
    if (raw.length !== 6) return;

    joinBtn.setAttribute("disabled", "true");
    joinBtn.textContent = "Joining…";

    try {
      // For now, use the raw 6-digit code as the mesh secret
      // Lane A will implement the full pairing code protocol
      await sidecar.joinMesh(raw);
      await refreshNodeList();
      await refreshTopology();
      store.toast("success", "Mesh joined", "Your node is now connected to the mesh.");
      overlay.remove();
      options.onJoin();
    } catch (err) {
      const msg = err instanceof Error ? err.message : String(err);
      store.toast("error", "Could not join mesh", msg);
      joinBtn.removeAttribute("disabled");
      joinBtn.innerHTML = "";
      joinBtn.appendChild(icon(Icons.plug, 14));
      joinBtn.appendChild(el("span", { style: "margin-left: 6px;", text: "Join Mesh" }));
    }
  });

  // Cancel
  on(cancelBtn, "click", () => {
    overlay.remove();
    options.onCancel();
  });

  // Escape key dismisses
  on(overlay, "keydown", (e) => {
    if ((e as KeyboardEvent).key === "Escape") {
      overlay.remove();
      options.onCancel();
    }
  });

  // Click outside dialog dismisses
  on(overlay, "click", (e) => {
    if (e.target === overlay) {
      overlay.remove();
      options.onCancel();
    }
  });

  document.body.appendChild(overlay);
  codeInput.focus();
}