/**
 * Health alert banners.
 *
 * Rendered as a thin strip above the main canvas when one or more data nodes
 * have crossed a health threshold: offline for > 60 s, ledger diverged, or
 * lagging for > 5 minutes. Each banner is dismissable for the session.
 *
 * The banners are intentionally restrained -- one line of text plus a close
 * button -- so they do not obscure the canvas on a small window.
 */

import { store, type NodeAlert } from "../state.js";
import { el, icon, Icons, on, replace } from "../util/dom.js";

const KIND_LABELS: Record<NodeAlert["kind"], string> = {
  offline: "is offline",
  diverged: "has diverged from the mesh",
  lagging: "has been lagging for over 5 minutes",
};

function alertBanner(alert: NodeAlert, onSelect: (nodeId: string) => void): HTMLElement {
  const closeBtn = el("button", {
    class: "health-alert__close",
    type: "button",
    "aria-label": `Dismiss alert for ${alert.nodeName}`,
  }, icon(Icons.close, 12));

  on(closeBtn, "click", () => store.dismissAlert(alert.id));

  const nameBtn = el("button", {
    class: "health-alert__node-link",
    type: "button",
  });
  nameBtn.textContent = alert.nodeName;
  on(nameBtn, "click", () => onSelect(alert.nodeId));

  const banner = el(
    "div",
    {
      class: "health-alert",
      "data-kind": alert.kind,
      role: "alert",
    },
    icon(Icons.warning, 13),
    nameBtn,
    el("span", { class: "health-alert__msg", text: KIND_LABELS[alert.kind] }),
    closeBtn,
  );

  return banner;
}

export interface HealthAlertHandles {
  render(): void;
  element: HTMLElement;
}

export function createHealthAlerts(
  onSelectNode: (nodeId: string) => void,
): HealthAlertHandles {
  const element = el("div", { class: "health-alerts", role: "status", "aria-live": "polite" });
  let expanded = false;

  function render(): void {
    const alerts = store.state.nodeAlerts.filter((a) => !a.dismissed);
    if (alerts.length === 0) {
      element.setAttribute("hidden", "");
      replace(element);
      expanded = false;
      return;
    }
    element.removeAttribute("hidden");
    if (alerts.length === 1) {
      replace(element, alertBanner(alerts[0], onSelectNode));
      return;
    }
    // Multiple alerts collapse to one summary line so the canvas is not
    // pushed down by a stack of banners; details expand on demand.
    const worst: NodeAlert["kind"] = alerts.some((a) => a.kind !== "lagging") ? "offline" : "lagging";
    const toggleBtn = el("button", {
      class: "btn btn--sm btn--ghost",
      type: "button",
      "aria-expanded": String(expanded),
      text: expanded ? "Hide" : "Review",
    });
    on(toggleBtn, "click", () => {
      expanded = !expanded;
      render();
    });
    const summary = el(
      "div",
      { class: "health-alert", "data-kind": worst, role: "alert" },
      icon(Icons.warning, 13),
      el("span", { class: "health-alert__msg", text: `${alerts.length} nodes need attention` }),
      toggleBtn,
    );
    if (!expanded) {
      replace(element, summary);
      return;
    }
    replace(element, summary, ...alerts.map((a) => alertBanner(a, onSelectNode)));
  }

  return { render, element };
}
