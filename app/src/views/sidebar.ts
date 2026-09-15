/**
 * The sidebar: Device -> Directory/USB -> Node -> Collection.
 *
 * Grouping is by *where the storage lives*, not by node name, because that is
 * the question an operator actually has when something is wrong -- "which
 * disk, which stick, which machine".
 *
 * Includes instant search filtering and expandable collection trees.
 */

import type { MountPoint, TopologyPeer } from "../api.js";
import { convergenceOf, meshTip, refreshDiscovered, refreshNodeList, store, type Convergence, type NodeView } from "../state.js";
import { bytes, engineLabel, shortNode } from "../util/format.js";
import { el, icon, Icons, on, replace } from "../util/dom.js";
import { promptDeleteSupervisedNode } from "../util/nodeDeleteHelper.js";
import { sidecar, type DiscoveredCandidate } from "../bridge.js";
import { openUnlockModal } from "./unlockModal.js";

/** Collapsed groups, remembered for the session only. */
const collapsed = new Set<string>();
/** Expanded nodes showing their collections. */
const expandedNodes = new Set<string>();
/** Current search query for filtering sidebar items. */
let filterQuery = "";
/** Previous status per node id - used to detect changes for dot pulse. */
const prevStatus = new Map<string, Convergence>();

interface Group {
  id: string;
  label: string;
  sublabel: string;
  kind: "device" | "mount" | "network";
  mount?: MountPoint;
  nodes: NodeView[];
  peers: TopologyPeer[];
}

function parentDir(path: string): string {
  const normalized = path.replace(/[/\\]+$/, "");
  const cut = Math.max(normalized.lastIndexOf("/"), normalized.lastIndexOf("\\"));
  return cut <= 0 ? normalized : normalized.slice(0, cut);
}

function baseName(path: string): string {
  const normalized = path.replace(/[/\\]+$/, "");
  const cut = Math.max(normalized.lastIndexOf("/"), normalized.lastIndexOf("\\"));
  return cut < 0 ? normalized : normalized.slice(cut + 1) || normalized;
}

function buildGroups(): Group[] {
  const state = store.state;
  const topology = state.topology;
  const groups = new Map<string, Group>();

  const ensure = (id: string, label: string, sublabel: string, kind: Group["kind"]): Group => {
    let group = groups.get(id);
    if (group === undefined) {
      group = { id, label, sublabel, kind, nodes: [], peers: [] };
      groups.set(id, group);
    }
    return group;
  };

  for (const mount of topology?.mounts ?? []) {
    if (!mount.removable) continue;
    const group = ensure(`mount:${mount.path}`, mount.label || mount.path, mount.path, "mount");
    group.mount = mount;
  }

  for (const node of state.nodes.values()) {
    if (node.process.supervisor) continue;
    if (node.process.removable) {
      const mount = (topology?.mounts ?? []).find(
        (m) => m.removable && node.process.data_dir.startsWith(m.path),
      );
      const id = mount ? `mount:${mount.path}` : `mount:${parentDir(node.process.data_dir)}`;
      const group = ensure(id, mount?.label ?? baseName(parentDir(node.process.data_dir)), mount?.path ?? parentDir(node.process.data_dir), "mount");
      if (mount) group.mount = mount;
      group.nodes.push(node);
    } else {
      const parent = parentDir(node.process.data_dir);
      const group = ensure(`dir:${parent}`, baseName(parent) || parent, parent, "device");
      group.nodes.push(node);
    }
  }

  const ours = new Set([...state.nodes.keys()]);
  for (const peer of topology?.lan_peers ?? []) {
    if (ours.has(peer.node_id) || peer.supervisor) continue;
    const label = peer.hostname || peer.host;
    const group = ensure(`net:${label}`, label, peer.host, "network");
    group.peers.push(peer);
  }

  const order: Record<Group["kind"], number> = { device: 0, mount: 1, network: 2 };
  return [...groups.values()].sort((a, b) => {
    const byKind = order[a.kind] - order[b.kind];
    return byKind !== 0 ? byKind : a.label.localeCompare(b.label);
  });
}

const GROUP_HEADINGS: Record<Group["kind"], string> = {
  device: "This device",
  mount: "Removable",
  network: "On the network",
};

// -- context menu ------------------------------------------------------------

let activeCtxMenu: HTMLElement | null = null;

function closeCtxMenu(): void {
  if (activeCtxMenu) {
    activeCtxMenu.remove();
    activeCtxMenu = null;
  }
}

type CtxMenuItem =
  | { label: string; danger?: boolean; action: () => void }
  | "separator";

function showCtxMenu(anchor: HTMLElement, items: CtxMenuItem[]): void {
  closeCtxMenu();

  const menu = el("div", { class: "ctx-menu", role: "menu" });
  for (const item of items) {
    if (item === "separator") {
      menu.appendChild(el("div", { class: "ctx-menu__sep", role: "separator" }));
      continue;
    }
    const btn = el(
      "button",
      {
        class: "ctx-menu__item" + (item.danger ? " ctx-menu__item--danger" : ""),
        type: "button",
        role: "menuitem",
        text: item.label,
      },
    );
    on(btn, "click", () => {
      closeCtxMenu();
      item.action();
    });
    menu.appendChild(btn);
  }

  document.body.appendChild(menu);
  activeCtxMenu = menu;

  const rect = anchor.getBoundingClientRect();
  const mRect = menu.getBoundingClientRect();
  const top = Math.min(rect.bottom + 4, window.innerHeight - mRect.height - 8);
  const left = Math.min(rect.left, window.innerWidth - mRect.width - 8);
  menu.style.top = `${top}px`;
  menu.style.left = `${left}px`;

  const dismiss = (e: MouseEvent | KeyboardEvent) => {
    if (e instanceof KeyboardEvent && e.key !== "Escape") return;
    if (e instanceof MouseEvent && menu.contains(e.target as Node)) return;
    closeCtxMenu();
    document.removeEventListener("click", dismiss as EventListener, true);
    document.removeEventListener("keydown", dismiss as EventListener, true);
  };
  setTimeout(() => {
    document.addEventListener("click", dismiss as EventListener, true);
    document.addEventListener("keydown", dismiss as EventListener, true);
  }, 0);
}

// -- discovered candidates section ------------------------------------------

function discoveredCandidateRow(
  candidate: DiscoveredCandidate,
  onAddAsPeer: (address: string) => void,
): HTMLElement {
  const isRemoteOnly = !candidate.has_node_config && !candidate.adoptable;
  const isEncrypted = candidate.encrypted;
  const name = candidate.node_name || candidate.node_id.slice(0, 12) || candidate.path.split(/[/\\]/).pop() || "Unknown node";

  const actionLabel = isRemoteOnly ? " Add as peer" : isEncrypted ? " Unlock" : " Adopt";
  const actionIcon = isRemoteOnly ? Icons.network : isEncrypted ? Icons.lock : Icons.plug;

  const actionBtn = el(
    "button",
    {
      class: "btn btn--sm btn--primary",
      type: "button",
      title: isRemoteOnly
        ? `Add ${name} as a bootstrap peer`
        : isEncrypted
          ? `Unlock encrypted node ${name}`
          : `Adopt ${name} into this app`,
    },
    icon(actionIcon, 11),
    actionLabel,
  );

  on(actionBtn, "click", async (e) => {
    e.stopPropagation();
    if (isEncrypted) {
      openUnlockModal({ node_id: candidate.node_id, node_name: name, data_dir: candidate.path });
      return;
    }
    actionBtn.setAttribute("disabled", "");
    if (isRemoteOnly) {
      onAddAsPeer(candidate.node_id);
    } else {
      try {
        await sidecar.startExistingNode(candidate.path);
        store.dismissCandidate(candidate.path);
        await refreshNodeList();
      } catch (error) {
        const msg = error instanceof Error ? error.message : String(error);
        store.toast("error", `Could not adopt ${name}`, msg);
        actionBtn.removeAttribute("disabled");
      }
    }
  });

  const dismissBtn = el(
    "button",
    {
      class: "tree__row-action",
      type: "button",
      title: `Dismiss ${name}`,
      "aria-label": `Dismiss ${name}`,
    },
    icon(Icons.close, 11),
  );
  on(dismissBtn, "click", (e) => {
    e.stopPropagation();
    store.dismissCandidate(candidate.path);
  });

  const kindIcon = candidate.removable ? Icons.drive : isRemoteOnly ? Icons.network : Icons.folder;
  const meta = candidate.encrypted ? "encrypted" : candidate.removable ? "removable" : "local";

  return el(
    "div",
    {
      class: "tree__row discovered-row",
      "data-depth": "1",
      role: "treeitem",
      "aria-selected": "false",
      title: candidate.path || candidate.node_id,
    },
    icon(kindIcon, 13),
    el("span", { class: "tree__label", text: name }),
    el("span", { class: "tree__meta", text: meta }),
    actionBtn,
    dismissBtn,
  );
}

function discoveredSection(onAddAsPeer: (address: string) => void): (Node | string)[] {
  const candidates = store.state.discoveredCandidates;
  if (candidates.length === 0) return [];

  const autoConnectCheckbox = el("input", {
    class: "toggle__input",
    id: "sidebar-autoconnect",
    type: "checkbox",
    title: "Automatically connect to discovered unencrypted nodes and USB drives",
  }) as HTMLInputElement;
  autoConnectCheckbox.checked = store.state.autoConnectEnabled;
  on(autoConnectCheckbox, "change", () => {
    store.setAutoConnect(autoConnectCheckbox.checked);
  });

  const autoConnectLabel = el(
    "label",
    { class: "toggle", for: "sidebar-autoconnect" },
    autoConnectCheckbox,
    el("span", { class: "toggle__track" }),
    el("span", { class: "toggle__label", text: "Auto-connect" }),
  );

  const refreshBtn = el(
    "button",
    { class: "btn btn--sm btn--ghost", type: "button", title: "Scan again", "aria-label": "Scan for nodes" },
    icon(Icons.refresh, 11),
  );
  on(refreshBtn, "click", () => {
    void refreshDiscovered();
  });

  const rows: (Node | string)[] = [
    el(
      "div",
      { class: "tree__group-header" },
      el("p", { class: "tree__group-label", text: "Discovered" }),
      el("div", { class: "tree__group-actions" }, autoConnectLabel, refreshBtn),
    ),
  ];
  for (const candidate of candidates) {
    rows.push(discoveredCandidateRow(candidate, onAddAsPeer));
  }
  return rows;
}

function statusLabel(status: Convergence): string {
  switch (status) {
    case "converged": return "In sync";
    case "lagging": return "Catching up";
    case "diverged": return "Diverged";
    case "offline": return "Offline";
    case "supervisor": return "Supervisor";
  }
}

function dot(status: Convergence, nodeId?: string): HTMLElement {
  const node = el("span", { class: "dot", "data-status": status, role: "img" });
  node.setAttribute("aria-label", statusLabel(status));

  if (nodeId !== undefined) {
    const prev = prevStatus.get(nodeId);
    if (prev !== undefined && prev !== status) {
      node.classList.add("dot--pulse");
    }
    prevStatus.set(nodeId, status);
  }

  return node;
}

function nodeRow(node: NodeView, tip: ReturnType<typeof meshTip>): HTMLElement[] {
  const selection = store.state.selection;
  const isNodeSelected = selection.kind === "node" && selection.nodeId === node.process.node_id;
  const status = convergenceOf(node, tip);
  const name = node.process.node_name || shortNode(node.process.node_id);
  const collections = node.brain?.collections ?? [];
  const isExpanded = expandedNodes.has(node.process.node_id) || (filterQuery !== "" && collections.length > 0);

  const collectionCount = collections.length;
  const meta =
    status === "offline"
      ? node.unreachableReason || node.process.last_error || "not running"
      : collectionCount === 0
        ? "no collections yet"
        : `${collectionCount} ${collectionCount === 1 ? "collection" : "collections"}`;

  const hasUnderReplicated = collections.some(
    (c) => (c as unknown as { under_replicated?: boolean }).under_replicated,
  );

  const chevron = collections.length > 0
    ? (() => {
        const c = icon(Icons.chevronRight);
        c.setAttribute("class", "tree__chevron");
        c.setAttribute("data-expanded", String(isExpanded));
        return c;
      })()
    : el("span", { style: "width: 10px; flex: none;" });

  const ledgerBtn = el(
    "button",
    {
      class: "tree__row-action",
      type: "button",
      title: `View ledger for ${name}`,
      "aria-label": `View ledger for ${name}`,
    },
    icon(Icons.inspector, 12),
  );
  on(ledgerBtn, "click", (e) => {
    e.stopPropagation();
    store.select({ kind: "ledger", nodeId: node.process.node_id });
  });

  const deleteBtn = !node.process.supervisor
    ? el(
        "button",
        {
          class: "tree__row-action text-danger",
          type: "button",
          title: `Delete ${name}`,
          "aria-label": `Delete ${name}`,
        },
        icon(Icons.trash, 12),
      )
    : null;

  if (deleteBtn) {
    on(deleteBtn, "click", (e) => {
      e.stopPropagation();
      promptDeleteSupervisedNode(node);
    });
  }

  const isEncrypted = node.process.encrypted;
  const isRunning = node.process.process === "running";

  const lockUnlockBtn = isEncrypted
    ? el(
        "button",
        {
          class: "tree__row-action",
          type: "button",
          title: isRunning ? `Lock ${name}` : `Unlock ${name}`,
          "aria-label": isRunning ? `Lock ${name}` : `Unlock ${name}`,
        },
        icon(isRunning ? Icons.lockOpen : Icons.lock, 12),
      )
    : null;

  if (lockUnlockBtn) {
    on(lockUnlockBtn, "click", async (e) => {
      e.stopPropagation();
      if (isRunning) {
        try {
          await sidecar.lockNode(node.process.node_id);
          store.toast("info", "Node locked", name);
          await refreshNodeList();
        } catch (err) {
          store.toast("error", "Could not lock node", String(err));
        }
      } else {
        openUnlockModal(node.process);
      }
    });
  }

  const metaEl =
    status === "offline"
      ? el("span", { class: "tree__meta tree__meta--error", text: meta })
      : collectionCount === 0
        ? el("span", { class: "tree__meta tree__meta--soft", text: meta })
        : el("span", { class: "tree__meta", text: meta });

  const row = el(
    "div",
    {
      class: "tree__row",
      "data-depth": "2",
      role: "treeitem",
      tabindex: "0",
      "aria-selected": String(isNodeSelected),
      title: `${name} — ${node.process.data_dir}`,
    },
    chevron,
    dot(status, node.process.node_id),
    el("span", { class: "tree__label", text: name }),
    hasUnderReplicated
      ? el("span", { class: "badge badge--warning", title: "Under-replicated" }, icon(Icons.warning, 10))
      : null,
    metaEl,
    lockUnlockBtn,
    ledgerBtn,
    deleteBtn,
  );

  const toggleOrSelect = (e: MouseEvent | KeyboardEvent) => {
    const target = e.target as HTMLElement;
    if (target.closest(".tree__chevron") && collections.length > 0) {
      e.stopPropagation();
      if (expandedNodes.has(node.process.node_id)) expandedNodes.delete(node.process.node_id);
      else expandedNodes.add(node.process.node_id);
      store.notify();
      return;
    }
    store.select({ kind: "node", nodeId: node.process.node_id });
  };

  on(row, "click", (e) => toggleOrSelect(e));
  on(row, "keydown", (event) => {
    if (event.key === "Enter" || event.key === " ") {
      event.preventDefault();
      toggleOrSelect(event);
    }
  });

  // Right-click context menu
  on(row, "contextmenu", (e) => {
    e.preventDefault();
    e.stopPropagation();
    const menuItems: CtxMenuItem[] = [
      {
        label: "View Ledger",
        action: () => store.select({ kind: "ledger", nodeId: node.process.node_id }),
      },
    ];
    if (isEncrypted) {
      menuItems.push({
        label: isRunning ? "Lock node" : "Unlock node",
        action: () => {
          if (isRunning) {
            sidecar
              .lockNode(node.process.node_id)
              .then(() => {
                store.toast("info", "Node locked", name);
                return refreshNodeList();
              })
              .catch((err: unknown) => store.toast("error", "Could not lock node", String(err)));
          } else {
            openUnlockModal(node.process);
          }
        },
      });
    }
    if (!node.process.supervisor) {
      menuItems.push("separator");
      menuItems.push({
        label: "Delete node\u2026",
        danger: true,
        action: () => promptDeleteSupervisedNode(node),
      });
    }
    showCtxMenu(row, menuItems);
  });

  const rows: HTMLElement[] = [row];

  if (isExpanded && collections.length > 0) {
    for (const c of collections) {
      if (
        filterQuery &&
        !c.name.toLowerCase().includes(filterQuery.toLowerCase()) &&
        !name.toLowerCase().includes(filterQuery.toLowerCase())
      ) {
        continue;
      }
      const isColSelected =
        selection.kind === "collection" &&
        selection.nodeId === node.process.node_id &&
        selection.collection === c.name;
      const isUnderReplicated = (c as unknown as { under_replicated?: boolean }).under_replicated;

      const colRow = el(
        "div",
        {
          class: "tree__row",
          "data-depth": "3",
          role: "treeitem",
          tabindex: "0",
          "aria-selected": String(isColSelected),
          title: `${c.name} (${engineLabel(c.engine)}) — ${c.document_count} documents`,
        },
        el("span", { class: "dot", style: "background: var(--color-primary); width: 6px; height: 6px;" }),
        el("span", { class: "tree__label mono", text: c.name }),
        isUnderReplicated
          ? el("span", { class: "badge badge--warning", title: "Under-replicated" }, icon(Icons.warning, 10))
          : null,
        el("span", { class: "badge", text: engineLabel(c.engine) }),
        el("span", { class: "tree__meta", text: String(c.document_count) }),
      );

      const openCol = () => {
        store.select({ kind: "collection", nodeId: node.process.node_id, collection: c.name });
      };
      on(colRow, "click", openCol);
      on(colRow, "keydown", (e) => {
        if (e.key === "Enter") openCol();
      });
      rows.push(colRow);
    }
  }

  return rows;
}

function peerRow(peer: TopologyPeer, tip: ReturnType<typeof meshTip>): HTMLElement {
  const status: Convergence = peer.state === "running" ? "converged" : "offline";

  let meta: string;
  if (status === "offline") {
    meta = "offline";
  } else if (tip !== null && peer.ledger_entry_id !== undefined) {
    const lag = tip.entry_id - peer.ledger_entry_id;
    meta = lag <= 0 ? "in sync" : `\u2212${lag}`;
  } else {
    meta = shortNode(peer.node_id, 8);
  }

  return el(
    "div",
    {
      class: "tree__row",
      "data-depth": "2",
      role: "treeitem",
      "aria-selected": "false",
      "aria-disabled": "true",
      title: `${peer.host}:${peer.p2p_port} — not managed by this device`,
    },
    el("span", { style: "width: 10px; flex: none;" }),
    dot(status),
    el("span", { class: "tree__label", text: shortNode(peer.node_id, 12) }),
    el("span", { class: "tree__meta", text: meta }),
  );
}

function groupRow(group: Group): HTMLElement {
  const isCollapsed = collapsed.has(group.id) && !filterQuery;
  const chevron = icon(Icons.chevronRight);
  chevron.setAttribute("class", "tree__chevron");
  chevron.setAttribute("data-expanded", String(!isCollapsed));

  const glyph =
    group.kind === "mount" ? Icons.drive : group.kind === "network" ? Icons.network : Icons.folder;

  const count = group.nodes.length + group.peers.length;
  const meta =
    group.kind === "mount" && group.mount
      ? `${bytes(group.mount.free_bytes)} free`
      : count === 0
        ? "empty"
        : `${count}`;

  const row = el(
    "div",
    {
      class: "tree__row",
      "data-depth": "1",
      role: "treeitem",
      tabindex: "0",
      "aria-expanded": String(!isCollapsed),
      title: group.sublabel,
    },
    chevron,
    icon(glyph),
    el("span", { class: "tree__label", text: group.label }),
    el("span", { class: "tree__meta", text: meta }),
  );

  const toggle = () => {
    if (collapsed.has(group.id)) collapsed.delete(group.id);
    else collapsed.add(group.id);
    if (group.kind === "mount" && group.mount) {
      store.select({ kind: "mount", mountPath: group.mount.path });
    } else {
      store.notify();
    }
  };
  on(row, "click", toggle);
  on(row, "keydown", (event) => {
    if (event.key === "Enter" || event.key === " ") {
      event.preventDefault();
      toggle();
    }
  });
  return row;
}

function emptyMountRow(): HTMLElement {
  return el(
    "div",
    { class: "tree__row", "data-depth": "2", "aria-disabled": "true" },
    el("span", { class: "tree__label tree__meta--soft", text: "No node here yet" }),
  );
}

// -- pinned navigation -------------------------------------------------------

const PINNED_NAV = [
  { id: "mesh",    label: "Mesh Map",    iconName: "mesh"     as keyof typeof Icons },
  { id: "dropbox", label: "Dropbox",     iconName: "inbox"    as keyof typeof Icons },
  { id: "console", label: "Console",     iconName: "terminal" as keyof typeof Icons },
  { id: "ledger",  label: "Ledger Feed", iconName: "ledger"   as keyof typeof Icons },
] as const;

export interface SidebarHandles {
  render(): void;
  element: HTMLElement;
  /** Collapse toggle button — caller should insert into the header. */
  collapseBtn: HTMLElement;
}

export function createSidebar(onNewNode: () => void, onAddAsPeer: (nodeId: string) => void): SidebarHandles {
  void onNewNode;

  let sidebarCollapsed = false;

  const tree = el("div", {
    class: "stack sidebar__tree-scroll",
    role: "tree",
    "aria-label": "Devices and nodes",
  });

  const searchInput = el("input", {
    class: "sidebar__search-input",
    type: "search",
    placeholder: "Search\u2026",
    "aria-label": "Filter storage",
  }) as HTMLInputElement;

  on(searchInput, "input", () => {
    filterQuery = searchInput.value.trim();
    render();
  });

  const searchBox = el(
    "div",
    { class: "sidebar__search" },
    el("span", { class: "sidebar__search-icon" }, icon(Icons.search, 13)),
    searchInput,
  );

  const pinnedNav = el("nav", { class: "sidebar__pinned-nav", "aria-label": "Primary views" });

  const element = el(
    "aside",
    { class: "sidebar", "data-open": "false", "data-collapsed": "false" },
    pinnedNav,
    searchBox,
    tree,
  );

  // Sidebar collapse button (H2) — caller inserts into header toolbar
  const collapseBtn = el(
    "button",
    {
      class: "btn btn--icon-only",
      type: "button",
      title: "Toggle sidebar",
      "aria-label": "Toggle sidebar",
      "aria-expanded": "true",
    },
    icon(Icons.tree, 14),
  );
  on(collapseBtn, "click", () => {
    sidebarCollapsed = !sidebarCollapsed;
    element.setAttribute("data-collapsed", String(sidebarCollapsed));
    collapseBtn.setAttribute("aria-expanded", String(!sidebarCollapsed));
  });

  function isNavActive(id: string): boolean {
    const sel = store.state.selection;
    switch (id) {
      case "mesh":    return sel.kind === "none" || sel.kind === "node";
      case "dropbox": return sel.kind === "dropbox";
      case "console": return sel.kind === "console";
      case "ledger":  return sel.kind === "ledger";
      default:        return false;
    }
  }

  function navAction(id: string): void {
    switch (id) {
      case "mesh": {
        const first = store.selectedNode() ?? store.dataNodes()[0];
        store.select(first ? { kind: "node", nodeId: first.process.node_id } : { kind: "none" });
        break;
      }
      case "dropbox":
        store.select({ kind: "dropbox" });
        break;
      case "console": {
        const node = store.selectedNode()?.process.node_id ?? store.dataNodes()[0]?.process.node_id;
        store.select({ kind: "console", nodeId: node });
        break;
      }
      case "ledger": {
        const node = store.selectedNode()?.process.node_id ?? store.dataNodes()[0]?.process.node_id;
        if (node) store.select({ kind: "ledger", nodeId: node });
        break;
      }
    }
  }

  // H1: check if search query matches a pinned nav item by label
  function matchesPinnedNav(q: string): string | undefined {
    if (!q) return undefined;
    const lower = q.toLowerCase();
    return PINNED_NAV.find((item) => item.label.toLowerCase().includes(lower))?.id;
  }

  function renderPinnedNav(): HTMLElement {
    const highlightedId = matchesPinnedNav(filterQuery);
    const items = PINNED_NAV.map((item) => {
      const isActive = isNavActive(item.id);
      const isHighlighted = highlightedId === item.id;
      const btn = el(
        "button",
        {
          class: "nav-item" + (isHighlighted ? " nav-item--highlighted" : ""),
          type: "button",
          "aria-selected": String(isActive),
        },
        icon(Icons[item.iconName], 14),
        el("span", { class: "nav-item__label", text: item.label }),
      );
      on(btn, "click", () => navAction(item.id));
      return btn;
    });
    return el("div", { class: "stack", style: "gap: 2px;" }, ...items);
  }

  function render(): void {
    replace(pinnedNav, renderPinnedNav());

    const groups = buildGroups();
    const tip = meshTip();
    const children: (Node | string)[] = [];

    children.push(...discoveredSection(onAddAsPeer));

    if (groups.length === 0 && store.state.discoveredCandidates.length === 0) {
      children.push(
        el(
          "div",
          { class: "empty" },
          el("p", { class: "empty__title", text: "No storage yet" }),
          el("p", {
            class: "empty__body",
            text: "Create a node to give this device somewhere to keep data, or plug in a drive to provision a portable one.",
          }),
        ),
      );
    }

    let lastKind: Group["kind"] | null = null;
    for (const group of groups) {
      const filteredNodes = group.nodes.filter((node) => {
        if (!filterQuery) return true;
        const q = filterQuery.toLowerCase();
        const name = (node.process.node_name || node.process.node_id).toLowerCase();
        const hasMatchingCol = (node.brain?.collections ?? []).some((c) =>
          c.name.toLowerCase().includes(q),
        );
        return name.includes(q) || node.process.data_dir.toLowerCase().includes(q) || hasMatchingCol;
      });

      const filteredPeers = group.peers.filter((p) => {
        if (!filterQuery) return true;
        const q = filterQuery.toLowerCase();
        return p.node_id.toLowerCase().includes(q) || p.host.toLowerCase().includes(q);
      });

      if (filterQuery && filteredNodes.length === 0 && filteredPeers.length === 0) {
        continue;
      }

      if (group.kind !== lastKind) {
        children.push(el("p", { class: "tree__group-label", text: GROUP_HEADINGS[group.kind] }));
        lastKind = group.kind;
      }

      children.push(groupRow(group));
      if (collapsed.has(group.id) && !filterQuery) continue;

      const sorted = [...filteredNodes].sort((a, b) =>
        (a.process.node_name || a.process.node_id).localeCompare(
          b.process.node_name || b.process.node_id,
        ),
      );
      for (const node of sorted) {
        children.push(...nodeRow(node, tip));
      }
      for (const peer of filteredPeers) children.push(peerRow(peer, tip));
      if (sorted.length === 0 && filteredPeers.length === 0 && group.kind === "mount") {
        children.push(emptyMountRow());
      }
    }

    replace(tree, ...children);
  }

  return { render, element, collapseBtn };
}
