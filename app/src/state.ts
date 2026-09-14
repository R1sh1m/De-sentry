/**
 * Application state, and the machinery that keeps it current.
 *
 * The store is a plain object plus a subscriber set. There is no reactivity
 * library: views read `store.state`, render, and re-render when told. With one
 * screen and a handful of panes that is less code than the wiring a framework
 * would need, and it keeps the app's whole dependency footprint at "Tauri and
 * the standard library".
 *
 * Freshness comes from two places, and only two:
 *
 *   * `GET /_changes?since=` -- one long poll per running node. When a write
 *     lands anywhere in the mesh and reaches this node's ledger, the poll
 *     returns and the node's snapshot is refreshed. No polling timer, so an
 *     idle mesh costs nothing and a busy one updates at its own rate.
 *   * Sidecar events -- process died, USB volume appeared, power source
 *     changed. Things the node's own API cannot report because they are about
 *     the node rather than from it.
 *
 * A `truncated: true` change response means the cursor predates a checkpoint
 * prune. The subscription then drops its cursor and re-reads from scratch
 * rather than pretending the gap did not happen.
 */

import {
  apiFor,
  ApiError,
  UnreachableError,
  type Brain,
  type CollectionDetail,
  type EngineInfo,
  type LedgerTip,
  type NodeStatus,
  type Peer,
  type QuotaReport,
  type Topology,
  type VerifyResult,
} from "./api.js";
import {
  onSidecarEvent,
  sidecar,
  NoSidecarError,
  type AppInfo,
  type DiscoveredCandidate,
  type SidecarEvent,
  type SupervisedNode,
} from "./bridge.js";

// -- view models -------------------------------------------------------------

/** How a node's ledger relates to the rest of the mesh, for the canvas colour. */
export type Convergence = "converged" | "lagging" | "diverged" | "offline" | "supervisor";

export interface NodeView {
  /** What the sidecar knows: process state, ports, paths. Always present. */
  process: SupervisedNode;
  /** What the node reports about itself. Null until it first answers. */
  status: NodeStatus | null;
  brain: Brain | null;
  tip: LedgerTip | null;
  quota: QuotaReport | null;
  peers: Peer[];
  /** Result of the last explicit Verify. Not run automatically -- it is not free. */
  lastVerify: VerifyResult | null;
  lastVerifyMs: number;
  /** The node answered its API within the last refresh. */
  reachable: boolean;
  /** Why it is not reachable, when it is not. */
  unreachableReason: string;
  /** Cursor for this node's change feed. -1 means "not yet synced". */
  changeCursor: number;
  /** Bumped whenever the feed reports a gap, so the UI can say so. */
  resyncs: number;
  lastUpdatedMs: number;
}

export interface Selection {
  kind: "none" | "device" | "mount" | "node" | "collection" | "ledger" | "console" | "dropbox";
  nodeId?: string;
  mountPath?: string;
  collection?: string;
}

export type CanvasMode = "tree" | "mesh";

export interface Toast {
  id: number;
  tone: "info" | "success" | "warning" | "error";
  title: string;
  detail: string;
  ms: number;
}

export type NodeAlertKind = "offline" | "diverged" | "lagging";

export interface NodeAlert {
  id: string;
  nodeId: string;
  nodeName: string;
  kind: NodeAlertKind;
  /** When the alert was first raised (ms since epoch). */
  since: number;
  dismissed: boolean;
}

export interface AppState {
  ready: boolean;
  /** Fatal startup problem, e.g. running outside the desktop shell. */
  bootError: string;
  appInfo: AppInfo | null;
  /** The app's own supervisor node, when it is up. */
  supervisorPort: number | null;
  topology: Topology | null;
  nodes: Map<string, NodeView>;
  /** Engine availability, read once from the supervisor: identical per build. */
  engines: EngineInfo[];
  selection: Selection;
  canvasMode: CanvasMode;
  /** Collection detail cache, keyed `nodeId::collection`. */
  collectionDetails: Map<string, CollectionDetail>;
  toasts: Toast[];
  /** Set while a long-running action (verify, checkpoint, scan) is in flight. */
  busy: string;
  /** Node directories found by the supervisor scan that we are not managing yet. */
  discoveredCandidates: DiscoveredCandidate[];
  /** Health alerts for nodes that have gone offline, diverged, or lagged too long. */
  nodeAlerts: NodeAlert[];
  /** Whether newly discovered unencrypted nodes/drives are automatically adopted. */
  autoConnectEnabled: boolean;
}

function emptyState(): AppState {
  let autoConnect = true;
  try {
    const val = localStorage.getItem("desentry.autoconnect");
    if (val !== null) autoConnect = val === "true";
  } catch {
    // localStorage not accessible
  }
  return {
    ready: false,
    bootError: "",
    appInfo: null,
    supervisorPort: null,
    topology: null,
    nodes: new Map(),
    engines: [],
    selection: { kind: "none" },
    canvasMode: "tree",
    collectionDetails: new Map(),
    toasts: [],
    busy: "",
    discoveredCandidates: [],
    nodeAlerts: [],
    autoConnectEnabled: autoConnect,
  };
}

// -- store -------------------------------------------------------------------

type Subscriber = (state: AppState) => void;

class Store {
  state: AppState = emptyState();
  private subscribers = new Set<Subscriber>();
  private frame = 0;
  private nextToastId = 1;

  subscribe(subscriber: Subscriber): () => void {
    this.subscribers.add(subscriber);
    return () => this.subscribers.delete(subscriber);
  }

  /**
   * Marks the state dirty. Notification is coalesced into the next animation
   * frame: a fifty-node mesh answering at once would otherwise trigger fifty
   * full re-renders in the same tick.
   */
  notify(): void {
    if (this.frame !== 0) return;
    this.frame = requestAnimationFrame(() => {
      this.frame = 0;
      for (const subscriber of this.subscribers) subscriber(this.state);
    });
  }

  toast(tone: Toast["tone"], title: string, detail = "", ms = 5000): void {
    const toast: Toast = { id: this.nextToastId++, tone, title, detail, ms };
    this.state.toasts = [...this.state.toasts, toast];
    this.notify();
    if (ms > 0) window.setTimeout(() => this.dismissToast(toast.id), ms);
  }

  dismissToast(id: number): void {
    this.state.toasts = this.state.toasts.filter((t) => t.id !== id);
    this.notify();
  }

  select(selection: Selection): void {
    this.state.selection = selection;
    this.notify();
  }

  setCanvasMode(mode: CanvasMode): void {
    this.state.canvasMode = mode;
    this.notify();
  }

  setBusy(label: string): void {
    this.state.busy = label;
    this.notify();
  }

  node(nodeId: string): NodeView | undefined {
    return this.state.nodes.get(nodeId);
  }

  /** The node the user is looking at, following a collection selection up to its node. */
  selectedNode(): NodeView | undefined {
    const id = this.state.selection.nodeId;
    return id ? this.state.nodes.get(id) : undefined;
  }

  /** Data nodes only -- the supervisor is control plane and never charted. */
  dataNodes(): NodeView[] {
    return [...this.state.nodes.values()].filter((n) => !n.process.supervisor);
  }

  dismissCandidate(path: string): void {
    dismissedPaths.add(path);
    this.state.discoveredCandidates = this.state.discoveredCandidates.filter((c) => c.path !== path);
    this.notify();
  }

  dismissAlert(id: string): void {
    const alert = this.state.nodeAlerts.find((a) => a.id === id);
    if (alert) alert.dismissed = true;
    this.state.nodeAlerts = this.state.nodeAlerts.filter((a) => !a.dismissed);
    this.notify();
  }

  setAutoConnect(enabled: boolean): void {
    this.state.autoConnectEnabled = enabled;
    try {
      localStorage.setItem("desentry.autoconnect", String(enabled));
    } catch {
      // ignore
    }
    this.notify();
    if (enabled) void refreshDiscovered();
  }
}

export const store = new Store();

// -- session state for discovery dismissals ----------------------------------

/**
 * Paths the user has dismissed for this session. Re-plugging a USB clears this
 * via the volumes-changed event, which calls refreshDiscovered() from scratch.
 */
const dismissedPaths = new Set<string>();
const connectingPaths = new Set<string>();

// -- convergence -------------------------------------------------------------

/**
 * Classifies a node against the mesh's furthest-ahead ledger.
 *
 * The distinction that matters is lagging vs diverged: a node behind on entry
 * count is catching up and needs no attention, while a node at the same height
 * with a different hash has a history that does not match and needs a human.
 * Colouring both amber would hide the second case inside the first.
 */
export function convergenceOf(node: NodeView, meshTip: LedgerTip | null): Convergence {
  if (node.process.supervisor) return "supervisor";
  if (!node.reachable || node.process.process !== "running") return "offline";
  const tip = node.tip ?? (node.brain ? { ...node.brain.ledger_tip } : null);
  if (tip === null || meshTip === null) return "lagging";
  if (tip.entry_id === meshTip.entry_id) {
    return tip.entry_hash === meshTip.entry_hash ? "converged" : "diverged";
  }
  return tip.entry_id > meshTip.entry_id ? "converged" : "lagging";
}

/** The furthest-ahead tip among reachable data nodes; the yardstick for the above. */
export function meshTip(): LedgerTip | null {
  let best: LedgerTip | null = null;
  for (const node of store.dataNodes()) {
    if (!node.reachable || node.tip === null) continue;
    if (best === null || node.tip.entry_id > best.entry_id) best = node.tip;
  }
  return best;
}

// -- refresh -----------------------------------------------------------------

function blankView(process: SupervisedNode): NodeView {
  return {
    process,
    status: null,
    brain: null,
    tip: null,
    quota: null,
    peers: [],
    lastVerify: null,
    lastVerifyMs: 0,
    reachable: false,
    unreachableReason: "",
    changeCursor: -1,
    resyncs: 0,
    lastUpdatedMs: 0,
  };
}

function describeError(error: unknown): string {
  if (error instanceof UnreachableError) return "not answering";
  if (error instanceof ApiError) return error.message;
  if (error instanceof Error) return error.message;
  return String(error);
}

/**
 * Refreshes one node's snapshot.
 *
 * `/_brain` already carries collections, peers and the tip, so the common case
 * is two requests, not six. Status is fetched alongside it for the ports,
 * uptime and broadcast counters the brain omits.
 */
export async function refreshNode(nodeId: string, options: { quota?: boolean } = {}): Promise<void> {
  const view = store.state.nodes.get(nodeId);
  if (view === undefined) return;
  if (view.process.process === "stopped" || view.process.process === "failed") {
    view.reachable = false;
    view.unreachableReason = view.process.last_error || "not running";
    checkNodeHealth(nodeId, view);
    store.notify();
    return;
  }

  const api = apiFor(view.process.api_port);
  try {
    const [status, brain] = await Promise.all([api.status(), api.brain()]);
    view.status = status;
    view.brain = brain;
    view.peers = brain.known_peers;
    view.tip = {
      node_id: brain.node_id,
      entry_id: brain.ledger_tip.entry_id,
      entry_hash: brain.ledger_tip.entry_hash,
      signature: brain.ledger_tip.signature,
      public_key_hex: status.public_key_hex,
    };
    if (options.quota !== false) {
      // Per-engine quota is a separate call and only the inspector shows it,
      // so it is refreshed with the rest rather than on its own timer.
      view.quota = await api.quota().catch(() => view.quota);
    }
    view.reachable = true;
    view.unreachableReason = "";
    view.lastUpdatedMs = Date.now();
  } catch (error) {
    view.reachable = false;
    view.unreachableReason = describeError(error);
  }
  checkNodeHealth(nodeId, view);
  store.notify();
}

/** Reconciles the node map with what the sidecar currently supervises. */
export async function refreshNodeList(): Promise<void> {
  const processes = await sidecar.listNodes();
  const seen = new Set<string>();

  for (const process of processes) {
    seen.add(process.node_id);
    const existing = store.state.nodes.get(process.node_id);
    if (existing === undefined) {
      store.state.nodes.set(process.node_id, blankView(process));
    } else {
      existing.process = process;
    }
    if (process.supervisor && process.process === "running") {
      store.state.supervisorPort = process.api_port;
    }
  }

  for (const nodeId of [...store.state.nodes.keys()]) {
    if (!seen.has(nodeId)) {
      stopSubscription(nodeId);
      store.state.nodes.delete(nodeId);
      if (store.state.selection.nodeId === nodeId) store.state.selection = { kind: "none" };
    }
  }
  store.notify();

  await Promise.all([...seen].map((nodeId) => refreshNode(nodeId)));
  syncSubscriptions();
}

/** Re-reads the sidebar's whole tree from the supervisor in one call. */
export async function refreshTopology(): Promise<void> {
  const port = store.state.supervisorPort;
  if (port === null) return;
  try {
    store.state.topology = await apiFor(port).topology();
  } catch (error) {
    // A supervisor that is up but not answering topology is worth saying out
    // loud: the sidebar silently going stale is the confusing failure.
    store.toast("warning", "Could not read the device topology", describeError(error));
  }
  store.notify();
}

export async function refreshCollection(nodeId: string, collection: string): Promise<void> {
  const view = store.state.nodes.get(nodeId);
  if (view === undefined) return;
  try {
    const detail = await apiFor(view.process.api_port).collection(collection);
    store.state.collectionDetails.set(`${nodeId}::${collection}`, detail);
    store.notify();
  } catch (error) {
    store.toast("error", `Could not read ${collection}`, describeError(error));
  }
}

export function collectionDetail(nodeId: string, collection: string): CollectionDetail | undefined {
  return store.state.collectionDetails.get(`${nodeId}::${collection}`);
}

// -- change-feed subscriptions ----------------------------------------------

interface Subscription {
  nodeId: string;
  controller: AbortController;
  stopped: boolean;
}

const subscriptions = new Map<string, Subscription>();

/** Backoff for a node that is not answering. Capped so recovery is prompt. */
const RETRY_MIN_MS = 1000;
const RETRY_MAX_MS = 15_000;

async function runSubscription(subscription: Subscription): Promise<void> {
  let retryMs = RETRY_MIN_MS;

  while (!subscription.stopped) {
    const view = store.state.nodes.get(subscription.nodeId);
    if (view === undefined) return;
    const api = apiFor(view.process.api_port);

    try {
      const batch = await api.changes(view.changeCursor, {}, { signal: subscription.controller.signal });
      if (subscription.stopped) return;
      retryMs = RETRY_MIN_MS;

      if (batch.truncated) {
        // The gap is real: entries between our cursor and the batch were
        // pruned by a checkpoint. Re-read rather than continue from a cursor
        // whose continuity we cannot vouch for.
        view.resyncs += 1;
        view.changeCursor = batch.tip;
        await refreshNode(subscription.nodeId);
        store.toast(
          "info",
          `${view.process.node_name || view.process.node_id.slice(0, 8)} re-synced`,
          "Its change cursor predated a checkpoint, so the snapshot was re-read from the current tip.",
        );
        continue;
      }

      view.changeCursor = batch.tip;
      if (batch.count > 0) {
        // Only the tip moved -- refresh the node's own snapshot, and the
        // topology if a lifecycle-shaped entry landed.
        await refreshNode(subscription.nodeId, { quota: false });
        if (batch.changes.some((c) => c.operation === "CHECKPOINT")) await refreshTopology();
      } else {
        // A poll that timed out with no changes still proves the node is alive.
        if (!view.reachable) await refreshNode(subscription.nodeId);
      }
    } catch (error) {
      if (subscription.stopped || subscription.controller.signal.aborted) return;
      const view2 = store.state.nodes.get(subscription.nodeId);
      if (view2 !== undefined && view2.reachable) {
        view2.reachable = false;
        view2.unreachableReason = describeError(error);
        store.notify();
      }
      await sleep(retryMs);
      retryMs = Math.min(RETRY_MAX_MS, retryMs * 2);
      // A node that has come back needs its cursor re-established from its own
      // tip, since it may have been restarted from a pruned ledger.
      if (!subscription.stopped) await refreshNode(subscription.nodeId).catch(() => undefined);
    }
  }
}

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => window.setTimeout(resolve, ms));
}

function startSubscription(nodeId: string): void {
  if (subscriptions.has(nodeId)) return;
  const subscription: Subscription = { nodeId, controller: new AbortController(), stopped: false };
  subscriptions.set(nodeId, subscription);
  void runSubscription(subscription);
}

function stopSubscription(nodeId: string): void {
  const subscription = subscriptions.get(nodeId);
  if (subscription === undefined) return;
  subscription.stopped = true;
  subscription.controller.abort();
  subscriptions.delete(nodeId);
}

/** One live subscription per running node, and none for the others. */
export function syncSubscriptions(): void {
  for (const [nodeId, view] of store.state.nodes) {
    const shouldRun = view.process.process === "running" || view.process.process === "starting";
    if (shouldRun) startSubscription(nodeId);
    else stopSubscription(nodeId);
  }
  for (const nodeId of [...subscriptions.keys()]) {
    if (!store.state.nodes.has(nodeId)) stopSubscription(nodeId);
  }
}

export function stopAllSubscriptions(): void {
  for (const nodeId of [...subscriptions.keys()]) stopSubscription(nodeId);
}

// -- health alerts -----------------------------------------------------------

/** When a node went offline (not-reachable), keyed by nodeId. */
const offlineSince = new Map<string, number>();

/** When a node entered the lagging convergence state, keyed by nodeId. */
const laggingSince = new Map<string, number>();

/** 60 seconds before we surface an offline alert. */
const OFFLINE_ALERT_MS = 60_000;

/** 5 minutes of sustained lag before we surface a lagging alert. */
const LAGGING_ALERT_MS = 5 * 60_000;

function upsertAlert(nodeId: string, kind: NodeAlertKind, name: string): void {
  const id = `${nodeId}:${kind}`;
  const existing = store.state.nodeAlerts.find((a) => a.id === id);
  if (existing === undefined) {
    store.state.nodeAlerts = [
      ...store.state.nodeAlerts,
      { id, nodeId, nodeName: name, kind, since: Date.now(), dismissed: false },
    ];
  }
}

function clearAlert(nodeId: string, kind: NodeAlertKind): void {
  const id = `${nodeId}:${kind}`;
  store.state.nodeAlerts = store.state.nodeAlerts.filter((a) => a.id !== id);
}

/**
 * Called after every refreshNode. Checks reachability and convergence, raises
 * or clears health alerts, and updates the offline/lagging timers.
 *
 * Supervisors are excluded: they are control-plane nodes and their offline
 * state is surfaced differently (the whole sidebar stops updating).
 */
function checkNodeHealth(nodeId: string, view: NodeView): void {
  if (view.process.supervisor) return;

  const name = view.process.node_name || nodeId.slice(0, 8);
  const now = Date.now();

  // -- offline -----------------------------------------------------------------
  if (!view.reachable) {
    if (!offlineSince.has(nodeId)) offlineSince.set(nodeId, now);
    const since = offlineSince.get(nodeId)!;
    if (now - since >= OFFLINE_ALERT_MS) upsertAlert(nodeId, "offline", name);
  } else {
    offlineSince.delete(nodeId);
    clearAlert(nodeId, "offline");
  }

  // -- diverged ----------------------------------------------------------------
  const tip = meshTip();
  const convergence = view.reachable ? convergenceOf(view, tip) : null;
  if (convergence === "diverged") {
    upsertAlert(nodeId, "diverged", name);
  } else {
    clearAlert(nodeId, "diverged");
  }

  // -- lagging ----------------------------------------------------------------
  if (convergence === "lagging") {
    if (!laggingSince.has(nodeId)) laggingSince.set(nodeId, now);
    const since = laggingSince.get(nodeId)!;
    if (now - since >= LAGGING_ALERT_MS) upsertAlert(nodeId, "lagging", name);
  } else {
    laggingSince.delete(nodeId);
    clearAlert(nodeId, "lagging");
  }
}

// -- discovery ---------------------------------------------------------------

function autoAdoptCandidates(candidates: DiscoveredCandidate[]): void {
  if (!store.state.autoConnectEnabled) return;
  for (const candidate of candidates) {
    if (candidate.adoptable && !candidate.encrypted && !connectingPaths.has(candidate.path)) {
      connectingPaths.add(candidate.path);
      sidecar
        .startExistingNode(candidate.path)
        .then((node) => {
          store.toast(
            "success",
            `Connected node ${node.node_name || node.node_id.slice(0, 8)}`,
            candidate.path,
            3000,
          );
          void refreshNodeList();
          void refreshTopology();
        })
        .catch(() => {
          connectingPaths.delete(candidate.path);
        });
    }
  }
}

/**
 * Fetches unmanaged node candidates from the sidecar and updates the store.
 *
 * Candidates that the user has dismissed this session are filtered out. The
 * scan is also de-duplicated against already-managed nodes (the sidecar does
 * this too, but the frontend re-checks in case the sidecar's managed-set is
 * slightly stale during a race).
 */
export async function refreshDiscovered(): Promise<void> {
  try {
    const candidates = await sidecar.scanForNodes();
    const managedIds = new Set([...store.state.nodes.keys()]);
    const filtered = candidates.filter(
      (c) => !dismissedPaths.has(c.path) && !managedIds.has(c.node_id),
    );
    store.state.discoveredCandidates = filtered;
    store.notify();
    autoAdoptCandidates(filtered);
  } catch {
    // Scan failure is silent: the supervisor may not be up yet on first boot.
  }
}

// -- boot --------------------------------------------------------------------

function handleSidecarEvent(event: SidecarEvent): void {
  switch (event.kind) {
    case "node-state": {
      const existing = store.state.nodes.get(event.node.node_id);
      if (existing === undefined) store.state.nodes.set(event.node.node_id, blankView(event.node));
      else existing.process = event.node;

      if (event.node.supervisor && event.node.process === "running") {
        store.state.supervisorPort = event.node.api_port;
      }
      if (event.node.process === "failed") {
        store.toast(
          "error",
          `${event.node.node_name || event.node.node_id.slice(0, 8)} stopped`,
          event.node.last_error || `exit code ${event.node.last_exit_code ?? "unknown"}`,
          0,
        );
      }
      syncSubscriptions();
      void refreshNode(event.node.node_id);
      break;
    }
    case "nodes-discovered":
      // The watchdog pushes a candidate list; apply it directly so the sidebar
      // updates without a round-trip to the sidecar.
      {
        const managedIds = new Set([...store.state.nodes.keys()]);
        const filtered = event.candidates.filter(
          (c) => !dismissedPaths.has(c.path) && !managedIds.has(c.node_id),
        );
        store.state.discoveredCandidates = filtered;
        store.notify();
        autoAdoptCandidates(filtered);
      }
      break;
    case "volumes-changed":
      // A USB node appearing or vanishing changes the sidebar's whole shape,
      // and may surface a new node to adopt. Re-scan immediately.
      void refreshTopology();
      void refreshNodeList();
      void refreshDiscovered();
      // Also clear any dismissed paths for removable media: the user unplugged
      // and re-plugged something, which is a distinct event from a prior dismiss.
      for (const path of dismissedPaths) {
        // Only clear paths that look like removable-media mount points.
        // Non-removable dismissed paths persist for the whole session.
        const candidate = store.state.discoveredCandidates.find((c) => c.path === path);
        if (candidate?.removable) dismissedPaths.delete(path);
      }
      break;
    case "power-changed":
      if (store.state.appInfo !== null) store.state.appInfo.on_battery = event.on_battery;
      store.notify();
      break;
    case "network-changed":
      // Peers found over a previous WiFi network are stale; re-read every node.
      // A network change can also bring new peers into range, so run discovery.
      for (const nodeId of store.state.nodes.keys()) void refreshNode(nodeId);
      void refreshTopology();
      void refreshDiscovered();
      break;
    case "node-log":
      // Log lines are pulled on demand by the log pane rather than buffered
      // here; the event exists so an open pane can tail without polling.
      break;
  }
}

/** Brings the store up: sidecar info, node list, topology, engines, subscriptions. */
export async function boot(): Promise<void> {
  onSidecarEvent(handleSidecarEvent);

  try {
    store.state.appInfo = await sidecar.appInfo();
  } catch (error) {
    store.state.bootError =
      error instanceof NoSidecarError
        ? "This window is running outside the De-Sentry desktop shell, so it cannot reach any nodes. Launch the app rather than opening the dev server directly."
        : describeError(error);
    store.state.ready = true;
    store.notify();
    return;
  }

  await refreshNodeList();
  await refreshTopology();

  const port = store.state.supervisorPort;
  if (port !== null) {
    store.state.engines = await apiFor(port).engines().catch(() => []);
  }

  // Initial discovery scan: surfaces any unmanaged nodes immediately on open.
  void refreshDiscovered();

  // Select something so the app does not open on an empty canvas when there
  // is in fact a node to look at.
  if (store.state.selection.kind === "none") {
    const first = store.dataNodes()[0];
    if (first !== undefined) store.state.selection = { kind: "node", nodeId: first.process.node_id };
  }

  store.state.ready = true;
  store.notify();

  // 30-second auto-poll for newly discovered nodes (covers LAN peers that
  // appear without a volume or network event, e.g. a node started by hand on
  // another machine while the app is open).
  window.setInterval(() => {
    void refreshDiscovered();
  }, 30_000);
}
