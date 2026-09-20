/**
 * Modal dialog for rotating a node's passphrase (or migrating a generated
 * recovery-key node to a passphrase).
 *
 * Verifies the old secret server-side, enforces the 12-char minimum +
 * confirm-match, shows the same strength meter as the wizard, and requires an
 * explicit ack for weak-but-long phrases. The old secret stops working on
 * success; the DEK is re-wrapped, no data is re-encrypted.
 */

import { sidecar } from "../bridge.js";
import { refreshNodeList, store } from "../state.js";
import { el, icon, Icons, on } from "../util/dom.js";

function describeError(error: unknown): string {
  if (error instanceof Error) return error.message;
  return String(error);
}

function scoreOf(passphrase: string): { score: number; label: string } {
  const len = [...passphrase].length;
  let classes = 0;
  if (/[a-z]/.test(passphrase)) classes += 1;
  if (/[A-Z]/.test(passphrase)) classes += 1;
  if (/[0-9]/.test(passphrase)) classes += 1;
  if (/[^A-Za-z0-9]/.test(passphrase)) classes += 1;
  const blocklisted = ["password", "passw0rd", "123456", "qwerty", "letmein", "welcome", "admin", "desentry", "changeme", "iloveyou"].some((b) =>
    passphrase.toLowerCase().includes(b),
  );
  let score = 0;
  if (len >= 12) score += 1;
  if (len >= 16) score += 1;
  if (len >= 20) score += 1;
  if (classes >= 3) score += 1;
  if (blocklisted) score -= 2;
  score = Math.max(0, Math.min(4, score));
  const label = score <= 1 ? "weak" : score === 2 ? "fair" : score === 3 ? "strong" : "excellent";
  return { score, label };
}

export function openChangePassphraseModal(node: { node_id: string; node_name?: string }): void {
  const dialog = el("dialog", { class: "modal-dialog", "aria-label": `Change passphrase for ${node.node_name || "node"}` });
  const dismiss = () => {
    if (dialog.open) dialog.close();
    dialog.remove();
  };

  const title = el("h3", { class: "modal-box__title", text: `Change passphrase — ${node.node_name || node.node_id.slice(0, 8)}` });
  const lead = el("p", {
    class: "modal-box__body",
    text: "The old secret stops working immediately. The node's data is not re-encrypted — its key is re-wrapped under the new passphrase.",
  });

  const inputStyle =
    "flex: 1; min-width: 0; font-family: var(--font-mono); font-size: var(--text-sm); background: var(--color-surface-pearl); border: 1px solid var(--color-hairline); border-radius: var(--radius-sm); padding: 8px 12px; color: var(--color-ink);";

  const oldInput = el("input", { type: "password", class: "input", style: inputStyle, placeholder: "Current recovery key or passphrase…", autocomplete: "off" }) as HTMLInputElement;
  const newInput = el("input", { type: "password", class: "input", style: inputStyle, placeholder: "New passphrase (at least 12 characters)…", autocomplete: "new-password" }) as HTMLInputElement;
  const confirmInput = el("input", { type: "password", class: "input", style: inputStyle, placeholder: "Retype the new passphrase…", autocomplete: "new-password" }) as HTMLInputElement;

  const meter = el("p", { class: "muted", style: "margin: 4px 0 0;" });
  const ackLabel = el("label", { class: "row", style: "gap: var(--space-xs); align-items: center; cursor: pointer; margin-top: 8px; display: none;" });
  const ackBox = el("input", { type: "checkbox" }) as HTMLInputElement;
  ackLabel.appendChild(ackBox);
  ackLabel.appendChild(el("span", { text: "I understand this passphrase is weak but want to use it anyway" }));

  const refreshMeter = () => {
    const pw = newInput.value.trim();
    const confirm = confirmInput.value.trim();
    if (pw === "") {
      meter.textContent = "At least 12 characters. Longer phrases with mixed words, numbers and symbols are strongest.";
      meter.className = "muted";
      ackLabel.style.display = "none";
      return;
    }
    const { score, label } = scoreOf(pw);
    if ([...pw].length < 12) {
      meter.textContent = `Too short: ${[...pw].length} of 12 characters minimum.`;
      meter.className = "error-note";
      ackLabel.style.display = "none";
    } else if (confirm !== "" && pw !== confirm) {
      meter.textContent = `Strength: ${label} (${score}/4) — the two new passphrases do not match yet.`;
      meter.className = "error-note";
      ackLabel.style.display = "none";
    } else {
      meter.textContent = `Strength: ${label} (${score}/4).${score <= 1 ? " This will work, but please confirm you understand it is guessable." : ""}`;
      meter.className = score <= 1 ? "error-note" : "muted";
      ackLabel.style.display = score <= 1 ? "" : "none";
      if (score > 1) ackBox.checked = false;
    }
  };
  on(newInput, "input", refreshMeter);
  on(confirmInput, "input", refreshMeter);
  refreshMeter();

  const errorNote = el("p", { class: "error-note", style: "display: none; margin: 8px 0;" });

  const cancelBtn = el("button", { type: "button", class: "btn btn--ghost btn--sm", text: "Cancel" });
  const saveBtn = el("button", { type: "button", class: "btn btn--primary btn--sm", text: "Change passphrase" });
  on(cancelBtn, "click", dismiss);

  const submit = async () => {
    const oldSecret = oldInput.value.trim();
    const pw = newInput.value.trim();
    const confirm = confirmInput.value.trim();
    if (!oldSecret) {
      errorNote.textContent = "Enter the current recovery key or passphrase first.";
      errorNote.style.display = "block";
      oldInput.focus();
      return;
    }
    if ([...pw].length < 12) {
      errorNote.textContent = "Choose a passphrase with at least 12 characters, then confirm it matches.";
      errorNote.style.display = "block";
      newInput.focus();
      return;
    }
    if (pw !== confirm) {
      errorNote.textContent = "The two new passphrases do not match — retype both.";
      errorNote.style.display = "block";
      confirmInput.focus();
      return;
    }
    if (scoreOf(pw).score <= 1 && !ackBox.checked) {
      errorNote.textContent = "This passphrase looks guessable — tick the acknowledgement to use it anyway.";
      errorNote.style.display = "block";
      return;
    }
    saveBtn.setAttribute("disabled", "true");
    saveBtn.textContent = "Changing…";
    errorNote.style.display = "none";
    try {
      await sidecar.changePassphrase(node.node_id, oldSecret, pw, confirm);
      store.toast("success", "Passphrase changed", "The old secret no longer works.");
      dismiss();
      await refreshNodeList();
    } catch (err) {
      errorNote.textContent = describeError(err);
      errorNote.style.display = "block";
      saveBtn.removeAttribute("disabled");
      saveBtn.textContent = "Change passphrase";
    }
  };
  on(saveBtn, "click", () => void submit());
  on(confirmInput, "keydown", (e) => {
    if (e.key === "Enter") void submit();
  });

  const showRow = (input: HTMLElement, label: string) => {
    const toggle = el(
      "button",
      { type: "button", class: "btn btn--sm btn--ghost", title: "Show", "aria-label": `Show ${label}`, style: "margin-left: 6px;" },
      icon(Icons.eye, 14),
    );
    let shown = false;
    on(toggle, "click", () => {
      shown = !shown;
      (input as HTMLInputElement).type = shown ? "text" : "password";
    });
    return el("div", { class: "row", style: "margin: 8px 0; align-items: center;" }, input, toggle);
  };

  const box = el(
    "div",
    { class: "modal-box", style: "max-width: 520px;" },
    title,
    lead,
    el("p", { class: "muted", style: "margin-bottom: 0;", text: "Current secret" }),
    showRow(oldInput, "current secret"),
    el("p", { class: "muted", style: "margin-bottom: 0;", text: "New passphrase" }),
    showRow(newInput, "new passphrase"),
    showRow(confirmInput, "passphrase confirmation"),
    meter,
    ackLabel,
    errorNote,
    el("div", { class: "modal-box__footer" }, cancelBtn, saveBtn),
  );

  dialog.appendChild(box);
  on(dialog, "click", (e) => {
    if (e.target === dialog) dismiss();
  });
  on(dialog, "close", () => dialog.remove());
  document.body.appendChild(dialog);
  try {
    dialog.showModal();
  } catch {
    dialog.setAttribute("open", "");
  }
  oldInput.focus();
}
