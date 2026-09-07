/**
 * The boundary between the web view and the Rust sidecar.
 *
 * Everything the UI cannot do itself lives behind an `invoke` call: spawning
 * and supervising `desentryd` processes, allocating ports, writing node.json,
 * talking to the OS keychain, generating recovery keys, running the ONNX
 * sizing model, autostart and tray state. The web view never touches the
 * filesystem and never spawns anything -- that separation is what makes the
 * capability allowlist in src-tauri/capabilities/default.json meaningful.
 *
 * Command names here must match the `#[tauri::command]` functions in
 * src-tauri/src/. There is no generated binding layer; the pairing is by name,
 * and both sides list the same set.
 */

import { invoke as tauriInvoke } from "@tauri-apps/api/core";

// -- shapes shared with the sidecar -----------------------------------------

/** A `desentryd` process this app owns. */
export interface SupervisedNode {
  node_id: string;
  node_name: string;
  data_dir: string;
  config_path: string;
  api_port: number;
  p2p_port: number;
  discovery_port: number;
  supervisor: boolean;
  /** Process state as the supervisor sees it, independent of the node's own API. */
  process: "starting" | "running" | "restarting" | "stopped" | "failed";
  pid: number | null;
  /** Restarts performed since the app started. A climbing count is a symptom. */
  restarts: number;
  last_exit_code: number | null;
  last_error: string;
  started_ms: number;
  removable: boolean;
  encrypted: boolean;
}

export interface PortAllocation {
  api_port: number;
  p2p_port: number;
  discovery_port: number;
}

/** Quota division, mirroring `QuotaSplit` in include/desentry/common/config.h. */
export interface QuotaSplit {
  db_pct: number;
  transit_store_pct: number;
  cache_hash_pct: number;
  ledger_pct: number;
  net_buffers_pct: number;
}

/**
 * What the sizing step proposes and the wizard confirms. Mirrors `NodeSpec` in
 * src-tauri/src/ai.rs.
 */
export interface NodeSpec {
  engines: string[];
  default_engine: string;
  quota_split: QuotaSplit;
  shard_key: string;
  replication_factor: number;
  secondary_indexes: string[];
  retention_days: number;
  /** Draft collections the wizard offers to create, with `_schema` payloads. */
  collections: DraftCollection[];
  /** The sizing decision itself, recorded into the node manifest for audit. */
  decision: SizingDecision;
}

export interface DraftCollection {
  name: string;
  engine: string;
  schema: unknown | null;
  shard_key: string;
  retention_days: number;
}

export interface SizingDecision {
  /** The winning prototype: sql, nosql-doc, time-series, vector, graph, semi-structured, oops-rdbms. */
  workload: string;
  confidence: number;
  /** "onnx" when the model ran, "keyword" when the deterministic fallback did. */
  method: "onnx" | "keyword";
  /** Cosine similarity against every prototype, for the confidence bar chart. */
  scores: { workload: string; score: number }[];
  /** Set when the model could not be loaded; shown, not hidden. */
  fallback_reason: string;
  /** Below this, the wizard forces a manual engine choice. */
  confidence_floor: number;
  description: string;
  decided_at_ms: number;
}

export interface CreateNodeRequest {
  node_name: string;
  data_dir: string;
  /** Empty for a fresh node; set when adopting an existing data directory. */
  adopt: boolean;
  quota_mb: number;
  spec: NodeSpec;
  encrypt_at_rest: boolean;
  supervisor: boolean;
  removable: boolean;
  bootstrap_peers: string[];
  description: string;
}

export interface CreateNodeResult {
  node: SupervisedNode;
  /**
   * The recovery key, in plain text, exactly once. It is not stored anywhere
   * the app can read back: this response is the only time it exists outside
   * the OS keychain. The wizard cannot advance until the user has exported it.
   */
  recovery_key: string | null;
  /** Keychain entry name, so the node config can reference it. */
  keychain_ref: string;
}

export interface AppInfo {
  version: string;
  platform: string;
  /** API port of the app's own supervisor node, or null while it is starting. */
  supervisor_api_port: number | null;
  autostart_enabled: boolean;
  background_mode: boolean;
  /** True when the machine is on battery; the app throttles gossip then. */
  on_battery: boolean;
  /** Where the app keeps node data directories by default. */
  default_data_root: string;
  /** Version string of the bundled `desentryd`, so the UI can show a mismatch. */
  engine_version: string;
}

export interface LogLine {
  ts_ms: number;
  stream: "stdout" | "stderr";
  line: string;
}

// -- invoke ------------------------------------------------------------------

/** True when running inside the Tauri shell rather than a plain browser tab. */
export function isTauri(): boolean {
  return typeof window !== "undefined" && "__TAURI_INTERNALS__" in window;
}

/**
 * Raised when a sidecar command is called outside the desktop shell -- which
 * happens during `vite dev` in a browser. Reported rather than papered over
 * with fake data: a control room showing invented nodes is worse than one
 * showing an honest error.
 */
export class NoSidecarError extends Error {
  constructor(command: string) {
    super(`'${command}' needs the desktop shell; this window is running without the Tauri sidecar`);
    this.name = "NoSidecarError";
  }
}

async function invoke<T>(command: string, args?: Record<string, unknown>): Promise<T> {
  if (!isTauri()) throw new NoSidecarError(command);
  return tauriInvoke<T>(command, args);
}

// -- process supervision -----------------------------------------------------

export const sidecar = {
  appInfo(): Promise<AppInfo> {
    return invoke<AppInfo>("app_info");
  },

  /** Every `desentryd` this app owns, supervisor included. */
  listNodes(): Promise<SupervisedNode[]> {
    return invoke<SupervisedNode[]>("list_nodes");
  },

  /** Writes node.json, allocates ports, spawns the process, returns once it answers. */
  createNode(request: CreateNodeRequest): Promise<CreateNodeResult> {
    return invoke<CreateNodeResult>("create_node", { request });
  },

  /** Adopts an existing data directory: reads its config, allocates a free port, spawns. */
  startExistingNode(dataDir: string): Promise<SupervisedNode> {
    return invoke<SupervisedNode>("start_existing_node", { dataDir });
  },

  stopNode(nodeId: string): Promise<void> {
    return invoke<void>("stop_node", { nodeId });
  },

  restartNode(nodeId: string): Promise<SupervisedNode> {
    return invoke<SupervisedNode>("restart_node", { nodeId });
  },

  /** Forgets a node without deleting its data directory. */
  forgetNode(nodeId: string): Promise<void> {
    return invoke<void>("forget_node", { nodeId });
  },

  /** Captured stdout/stderr, most recent last. */
  nodeLogs(nodeId: string, tail = 400): Promise<LogLine[]> {
    return invoke<LogLine[]>("node_logs", { nodeId, tail });
  },

  allocatePorts(): Promise<PortAllocation> {
    return invoke<PortAllocation>("allocate_ports");
  },

  // -- sizing ----------------------------------------------------------------

  /**
   * Embeds the description with the bundled MiniLM model and compares it to the
   * seven prototype embeddings. Falls back to a deterministic keyword heuristic
   * when the model cannot load, and says so in `decision.method`.
   */
  sizeWorkload(description: string, quotaMb: number): Promise<NodeSpec> {
    return invoke<NodeSpec>("size_workload", { description, quotaMb });
  },

  /** Engine names this build of `desentryd` can offer, for the manual picker. */
  availableEngines(): Promise<string[]> {
    return invoke<string[]>("available_engines");
  },

  // -- keys ------------------------------------------------------------------

  /**
   * Re-renders a recovery key for printing. Returns null once the key has been
   * committed to the keychain and dropped from memory -- there is no escrow, so
   * a key not exported at creation cannot be recovered here.
   */
  pendingRecoveryKey(nodeId: string): Promise<string | null> {
    return invoke<string | null>("pending_recovery_key", { nodeId });
  },

  /** Writes the key to a file the user chose, then clears it from memory. */
  exportRecoveryKey(nodeId: string, path: string): Promise<void> {
    return invoke<void>("export_recovery_key", { nodeId, path });
  },

  /** Unlocks an encrypted node with a password (USB nodes have no keychain entry). */
  unlockNode(nodeId: string, password: string): Promise<SupervisedNode> {
    return invoke<SupervisedNode>("unlock_node", { nodeId, password });
  },

  // -- shell / OS ------------------------------------------------------------

  pickDirectory(title: string): Promise<string | null> {
    return invoke<string | null>("pick_directory", { title });
  },

  pickSaveFile(title: string, defaultName: string): Promise<string | null> {
    return invoke<string | null>("pick_save_file", { title, defaultName });
  },

  revealPath(path: string): Promise<void> {
    return invoke<void>("reveal_path", { path });
  },

  setAutostart(enabled: boolean): Promise<void> {
    return invoke<void>("set_autostart", { enabled });
  },

  /** Keeps nodes syncing with the window closed, from the tray. */
  setBackgroundMode(enabled: boolean): Promise<void> {
    return invoke<void>("set_background_mode", { enabled });
  },

  notify(title: string, body: string): Promise<void> {
    return invoke<void>("notify", { title, body });
  },
};

// -- events ------------------------------------------------------------------

/**
 * Sidecar-pushed events. The node data itself comes from `/_changes` over HTTP;
 * these carry only what the sidecar knows and the node API cannot report --
 * a process dying, a USB volume appearing, the machine switching to battery.
 */
export type SidecarEvent =
  | { kind: "node-state"; node: SupervisedNode }
  | { kind: "node-log"; node_id: string; line: LogLine }
  | { kind: "volumes-changed" }
  | { kind: "power-changed"; on_battery: boolean }
  | { kind: "network-changed" };

type Listener = (event: SidecarEvent) => void;

const listeners = new Set<Listener>();
let unlistenAll: (() => void) | null = null;

/** Subscribes to sidecar events. Returns an unsubscribe function. */
export function onSidecarEvent(listener: Listener): () => void {
  listeners.add(listener);
  void ensureSubscribed();
  return () => {
    listeners.delete(listener);
  };
}

async function ensureSubscribed(): Promise<void> {
  if (unlistenAll !== null || !isTauri()) return;
  // Imported lazily so a browser-only dev session does not fail at module load.
  const { listen } = await import("@tauri-apps/api/event");
  const unlisten = await listen<SidecarEvent>("desentry://sidecar", (event) => {
    for (const listener of listeners) listener(event.payload);
  });
  unlistenAll = unlisten;
}
