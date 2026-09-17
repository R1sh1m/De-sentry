/**
 * Apple macOS-styled node deletion confirmation modal.
 *
 * Implements the exact confirmation prompt requirement:
 * "The selected node {node name} has {amount of data} in it related to {its data description}, are you sure you want to delete this?"
 */

import { el, icon, Icons, on } from "../util/dom.js";

export interface DeleteNodeOptions {
  nodeName: string;
  amountOfData: string;
  dataDescription: string;
  allowForgetOnly?: boolean;
  onConfirm: (deleteData: boolean) => Promise<void> | void;
}

export function openDeleteNodeModal(opts: DeleteNodeOptions): void {
  const { nodeName, amountOfData, dataDescription, allowForgetOnly = true, onConfirm } = opts;

  // Exact confirmation question as requested:
  const confirmationText = `The selected node ${nodeName} has ${amountOfData} in it related to ${dataDescription}, are you sure you want to delete this?`;

  const sheet = el("dialog", { class: "modal-dialog sheet-dialog", "aria-label": "Delete Node Confirmation" });

  const onKey = (event: KeyboardEvent) => {
    if (event.key === "Escape" && !busy) {
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

  const closeButton = el("button", { class: "sheet-close", type: "button", title: "Close", "aria-label": "Close" }, icon(Icons.close, 16));
  on(closeButton, "click", dismiss);
  const titleBar = el(
    "div",
    { class: "macos-modal__titlebar" },
    el("span", { style: "width: 28px" }),
    el("span", { class: "macos-modal__title", text: "Delete Node Confirmation" }),
    closeButton,
  );

  const cancelBtn = el("button", { class: "btn btn--ghost", type: "button", text: "Cancel" });
  on(cancelBtn, "click", dismiss);

  const deletePermanentlyBtn = el(
    "button",
    { class: "btn btn--danger", type: "button" },
    icon(Icons.trash, 14),
    " Delete Node & Data",
  );

  let busy = false;
  const runDelete = async (deleteData: boolean) => {
    if (busy) return;
    busy = true;
    deletePermanentlyBtn.setAttribute("disabled", "true");
    cancelBtn.setAttribute("disabled", "true");
    try {
      await onConfirm(deleteData);
      dismiss();
    } catch (err) {
      busy = false;
      deletePermanentlyBtn.removeAttribute("disabled");
      cancelBtn.removeAttribute("disabled");
    }
  };

  on(deletePermanentlyBtn, "click", () => void runDelete(true));

  const footerButtons = [cancelBtn];

  if (allowForgetOnly) {
    const forgetOnlyBtn = el("button", { class: "btn btn--outline", type: "button", text: "Forget Only (Keep Data)" });
    on(forgetOnlyBtn, "click", () => void runDelete(false));
    footerButtons.push(forgetOnlyBtn);
  }

  footerButtons.push(deletePermanentlyBtn);

  const modalBody = el(
    "div",
    { class: "macos-modal__content" },
    el(
      "div",
      { class: "macos-modal__icon-wrap macos-modal__icon-wrap--danger" },
      icon(Icons.trash, 28),
    ),
    el("h3", { class: "macos-modal__heading", text: `Delete "${nodeName}"?` }),
    el("p", { class: "macos-modal__prompt-text", text: confirmationText }),
    el(
      "p",
      { class: "macos-modal__warning-note" },
      "Deleting this node will permanently remove its cryptographic identity and storage files from disk. This cannot be undone.",
    ),
    el("div", { class: "macos-modal__footer" }, ...footerButtons),
  );

  const dialogBox = el(
    "div",
    { class: "macos-modal macos-modal--confirm" },
    titleBar,
    modalBody,
  );

  sheet.appendChild(dialogBox);

  // The destructive action must not be Esc-dismissable mid-flight: native
  // `cancel` is vetoable, unlike a keydown listener racing the browser.
  on(sheet, "cancel", (event) => {
    if (busy) event.preventDefault();
  });
  on(sheet, "click", (event) => {
    if (event.target === sheet && !busy) dismiss();
  });
  on(sheet, "keydown", (event) => {
    if (event.key === "Escape" && !busy) dismiss();
  });
  on(sheet, "close", () => sheet.remove());

  document.body.appendChild(sheet);
  try {
    sheet.showModal();
  } catch {
    sheet.setAttribute("open", "");
  }
  sheet.focus();
}
