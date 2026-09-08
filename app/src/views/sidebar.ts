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
import { convergenceOf, meshTip, store, type Convergence, type NodeView } from "../state.js";
import { bytes, engineLabel, shortNode } from "../util/format.js";
import { el, icon, Icons, on, replace } from "../util/dom.js";

/** Collapsed groups, remembered for the session only. */
const collapsed = new Set<string>();
/** Expanded nodes showing their collections. */
const expandedNodes = new Set<string>();
/** Current search query for filtering sidebar items. */
let filterQuery = "";

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
  const normalized = path.replace(/[\\/]+$/, "");
  const cut = Math.max(normalized.lastIndexOf("/"), normalized.lastIndexOf("\\"));
  return cut <= 0 ? normalized : normalized.slice(0, cut);
}

function baseName(path: string): string {
  const normalized = path.replace(/[\\/]+$/, "");
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

function statusLabel(status: Convergence): string {
  switch (status) {
    case "converged": return "In sync";
    case "lagging": return "Catching up";
    case "diverged": return "Diverged";
    case "offline": return "Offline";
    case "supervisor": return "Supervisor";
  }
}

function dot(status: Convergence): HTMLElement {
  const node = el("span", { class: "dot", "data-status": status, role: "img" });
  node.setAttribute("aria-label", statusLabel(status));
  return node;
}

function nodeRow(node: NodeView, tip: ReturnType<typeof meshTip>): HTMLElement[] {
  const selection = store.state.selection;
  const isNodeSelected = selection.kind === "node" && selection.nodeId === node.process.node_id;
  const status = convergenceOf(node, tip);
  const name = node.process.node_name || shortNode(node.process.node_id);
  const collections = node.brain?.collections ?? [];
  const isExpanded = expandedNodes.has(node.process.node_id) || (filterQuery !== "" && collections.length > 0);

  const meta =
    status === "offline"
      ? node.unreachableReason || node.process.last_error || "not running"
      : `${collections.length} coll`;

  const chevron = collections.length > 0
    ? (() => {
        const c = icon(Icons.chevronRight);
        c.setAttribute("class", "tree__chevron");
        c.setAttribute("data-expanded", String(isExpanded));
        return c;
      })()
    : el("span", { style: "width: 10px; flex: none;" });

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
    dot(status),
    el("span", { class: "tree__label", text: name }),
    el("span", { class: "tree__meta", text: meta }),
  );

  const toggleOrSelect = (e: MouseEvent | KeyboardEvent) => {
    // If clicking the chevron, toggle expansion; otherwise select node
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

  const rows: HTMLElement[] = [row];

  // If expanded, render collections under this node
  if (isExpanded && collections.length > 0) {
    for (const c of collections) {
      if (filterQuery && !c.name.toLowerCase().includes(filterQuery.toLowerCase()) && !name.toLowerCase().includes(filterQuery.toLowerCase())) {
        continue;
      }
      const isColSelected = selection.kind === "collection" && selection.nodeId === node.process.node_id && selection.collection === c.name;
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
        el("span", { class: "badge", text: `${c.document_count}d` }),
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

function peerRow(peer: TopologyPeer): HTMLElement {
  const status: Convergence = peer.state === "running" ? "converged" : "offline";
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
    el("span", { class: "tree__meta", text: `entry ${peer.ledger_entry_id}` }),
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
    el("span", { class: "tree__label muted", text: "No node here yet" }),
  );
}

export interface SidebarHandles {
  render(): void;
  element: HTMLElement;
}

export function createSidebar(onNewNode: () => void): SidebarHandles {
  const tree = el("div", { class: "stack", role: "tree", "aria-label": "Devices and nodes" });

  const searchInput = el("input", {
    class: "sidebar__search-input",
    type: "search",
    placeholder: "Filter nodes or collections…",
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

  const element = el(
    "aside",
    { class: "sidebar", "data-open": "false" },
    el(
      "div",
      { class: "row row--between", style: "margin-bottom: var(--space-xs);" },
      el("span", { class: "tree__group-label", text: "Topology" }),
      (() => {
        const button = el("button", { class: "btn btn--sm btn--ghost", type: "button" }, icon(Icons.plus, 13), "New");
        on(button, "click", onNewNode);
        return button;
      })(),
    ),
    searchBox,
    tree,
  );

  function render(): void {
    const groups = buildGroups();
    const tip = meshTip();
    const children: (Node | string)[] = [];

    if (groups.length === 0) {
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
      // Filter logic
      const filteredNodes = group.nodes.filter((node) => {
        if (!filterQuery) return true;
        const q = filterQuery.toLowerCase();
        const name = (node.process.node_name || node.process.node_id).toLowerCase();
        const hasMatchingCol = (node.brain?.collections ?? []).some((c) => c.name.toLowerCase().includes(q));
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
        (a.process.node_name || a.process.node_id).localeCompare(b.process.node_name || b.process.node_id),
      );
      for (const node of sorted) {
        children.push(...nodeRow(node, tip));
      }
      for (const peer of filteredPeers) children.push(peerRow(peer));
      if (sorted.length === 0 && filteredPeers.length === 0 && group.kind === "mount") {
        children.push(emptyMountRow());
      }
    }

    replace(tree, ...children);
  }

  return { render, element };
}
