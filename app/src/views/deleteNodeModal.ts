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

  const sheet = el("div", { class: "sheet", role: "dialog", "aria-modal": "true", tabindex: "-1" });

  const dismiss = () => sheet.remove();

  // Traffic lights
  const closeDot = el("button", { class: "traffic-dot traffic-dot--close", type: "button", title: "Close" });
  const minDot = el("button", { class: "traffic-dot traffic-dot--minimize", type: "button", title: "Minimize" });
  const zoomDot = el("button", { class: "traffic-dot traffic-dot--zoom", type: "button", title: "Zoom" });
  on(closeDot, "click", dismiss);
  on(minDot, "click", dismiss);

  const trafficLights = el("div", { class: "traffic-lights" }, closeDot, minDot, zoomDot);
  const titleBar = el(
    "div",
    { class: "macos-modal__titlebar" },
    trafficLights,
    el("span", { class: "macos-modal__title", text: "Delete Node Confirmation" }),
    el("span", { style: "width: 52px" }), // Balancer for traffic lights
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

  on(sheet, "click", (event) => {
    if (event.target === sheet && !busy) dismiss();
  });

  on(sheet, "keydown", (event) => {
    if (event.key === "Escape" && !busy) dismiss();
  });

  document.body.appendChild(sheet);
  sheet.focus();
}
