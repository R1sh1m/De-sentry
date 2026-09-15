/**
 * Modal dialog for unlocking an encrypted node with its passphrase or recovery key.
 */

import { sidecar } from "../bridge.js";
import { refreshNode, refreshNodeList, refreshTopology, store } from "../state.js";
import { el, icon, Icons, on } from "../util/dom.js";

function describeError(error: unknown): string {
  if (error instanceof Error) return error.message;
  return String(error);
}

export function openUnlockModal(node: { node_id: string; node_name?: string; data_dir?: string }): void {
  // Native <dialog>: top-layer backdrop, focus trap, Esc dismissal and focus
  // restore come from the browser instead of hand-rolled listeners. Falls back
  // to a plain open dialog on webviews without showModal support.
  const dialog = el("dialog", { class: "modal-dialog", "aria-label": `Unlock ${node.node_name || "node"}` });
  const dismiss = () => {
    if (dialog.open) dialog.close();
    dialog.remove();
  };

  const title = el("h3", { class: "modal-box__title", text: `Unlock ${node.node_name || node.node_id.slice(0, 8)}` });
  const lead = el("p", {
    class: "modal-box__body",
    text: "This node is encrypted at rest. Enter its passphrase or recovery key to decrypt the database and start the node.",
  });

  const input = el("input", {
    type: "password",
    class: "input",
    style: "width: 100%; font-family: var(--font-mono); font-size: var(--text-sm); background: var(--color-surface-pearl); border: 1px solid var(--color-hairline); border-radius: var(--radius-sm); padding: 8px 12px; color: var(--color-ink);",
    placeholder: "Enter recovery key or password…",
    autocomplete: "off",
  }) as HTMLInputElement;

  let showPassword = false;
  const toggleVisibility = el(
    "button",
    {
      type: "button",
      class: "btn btn--sm btn--ghost",
      title: "Show / hide password",
      style: "margin-left: 6px;",
    },
    icon(Icons.inspector, 13),
  );

  on(toggleVisibility, "click", () => {
    showPassword = !showPassword;
    input.type = showPassword ? "text" : "password";
  });

  const inputRow = el("div", { class: "row", style: "margin: 12px 0;" }, input, toggleVisibility);

  const errorNote = el("p", { class: "error-note", style: "display: none; margin-bottom: 8px;" });

  const cancelBtn = el("button", { type: "button", class: "btn btn--ghost btn--sm", text: "Cancel" });
  const unlockBtn = el("button", { type: "button", class: "btn btn--primary btn--sm", text: "Unlock Node" });

  on(cancelBtn, "click", dismiss);

  const submit = async () => {
    const secret = input.value.trim();
    if (!secret) {
      errorNote.textContent = "Please enter a key or password.";
      errorNote.style.display = "block";
      input.focus();
      return;
    }

    unlockBtn.setAttribute("disabled", "true");
    unlockBtn.textContent = "Unlocking…";
    errorNote.style.display = "none";

    try {
      await sidecar.unlockNode(node.node_id, secret);
      store.toast("success", "Node unlocked", node.node_name || node.node_id);
      dismiss();
      await refreshNodeList();
      await refreshTopology();
      await refreshNode(node.node_id);
    } catch (err) {
      errorNote.textContent = describeError(err);
      errorNote.style.display = "block";
      unlockBtn.removeAttribute("disabled");
      unlockBtn.textContent = "Unlock Node";
      input.focus();
    }
  };

  on(unlockBtn, "click", submit);
  on(input, "keydown", (e) => {
    if (e.key === "Enter") void submit();
  });
  // Backdrop click dismisses: on a native modal the backdrop targets the
  // dialog element itself. The explicit Escape handler stays as a fallback
  // for non-modal display; dismiss() is idempotent so double-dismiss is safe.
  on(dialog, "click", (e) => {
    if (e.target === dialog) dismiss();
  });
  on(dialog, "keydown", (e) => {
    if (e.key === "Escape") dismiss();
  });
  on(dialog, "close", () => dialog.remove());

  const box = el(
    "div",
    { class: "modal-box", style: "max-width: 480px;" },
    title,
    lead,
    inputRow,
    errorNote,
    el("div", { class: "modal-box__footer" }, cancelBtn, unlockBtn),
  );

  dialog.appendChild(box);
  document.body.appendChild(dialog);
  try {
    dialog.showModal();
  } catch {
    dialog.setAttribute("open", "");
  }
  input.focus();
}
