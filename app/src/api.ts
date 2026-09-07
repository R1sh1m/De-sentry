/**
 * Typed client for a node's local REST API.
 *
 * Every type here mirrors a response shape produced in src/api/routes.cpp or
 * src/supervisor/supervisor_routes.cpp. When one of those handlers changes,
 * this file changes with it -- there is no schema generator in between,
 * because a generator would be a fetched dependency and a hand-written mirror
 * of ~30 endpoints is small enough to keep honest by reading.
 *
 * All requests go to loopback. The node API binds to 127.0.0.1 and attributes
 * every local call to the node's own identity (routes.cpp `self`), so there is
 * no auth header to send: "who is asking" is never taken from something a
 * caller could set.
 */

// -- error handling ----------------------------------------------------------

/** A non-2xx response. Carries the engine's own `{"error": "..."}` message. */
export class ApiError extends Error {
  constructor(
    readonly status: number,
    message: string,
    readonly url: string,
  ) {
    super(message);
    this.name = "ApiError";
  }

  /** True when the node answered but refused -- as opposed to being unreachable. */
  get isRefusal(): boolean {
    return this.status === 403 || this.status === 404 || this.status === 409;
  }
}

/** The node could not be reached at all: not started yet, or gone. */
export class UnreachableError extends Error {
  constructor(readonly baseUrl: string, cause: unknown) {
    super(`no response from ${baseUrl}`);
    this.name = "UnreachableError";
    this.cause = cause;
  }
}

// -- response shapes ---------------------------------------------------------

export interface QuotaStatus {
  limit_bytes: number;
  used_bytes: number;
  ledger_bytes: number;
  used_fraction: number;
  over_limit: boolean;
}

export interface EngineQuota {
  engine: string;
  used_bytes: number;
  limit_bytes: number;
}

export interface QuotaReport extends QuotaStatus {
  engines: EngineQuota[];
}

export interface BroadcastStats {
  sent: number;
  dropped: number;
  duplicates_suppressed: number;
  rate_limited: number;
  queued: number;
}

export interface NodeStatus {
  node_id: string;
  node_name: string;
  public_key_hex: string;
  uptime_seconds: number;
  supervisor: boolean;
  collections: number;
  known_peers: number;
  replication_factor: number;
  default_engine: string;
  api_port: number;
  p2p_port: number;
  quota: QuotaStatus;
  broadcast: BroadcastStats;
}

export type LifecycleState =
  | "discovered"
  | "allocated"
  | "provisioned"
  | "running"
  | "degraded"
  | "reclaimed";

export interface PeerFitness {
  latency_ms: number;
  success_rate: number;
  ledger_freshness_entry_id: number;
  free_quota_mb: number;
  probes: number;
  score: number;
}

export interface Peer {
  node_id: string;
  host: string;
  hostname: string;
  p2p_port: number;
  api_port: number;
  last_seen_ms: number;
  supervisor: boolean;
  state: LifecycleState;
  fitness: PeerFitness;
}

export interface LedgerTip {
  node_id: string;
  entry_id: number;
  entry_hash: string;
  signature: string;
  public_key_hex: string;
}

export type LedgerOp = "PUT" | "DEL" | "CHECKPOINT" | "TRANSIT_INTENT" | "TRANSIT_CLAIMED";

export interface LedgerEntry {
  entry_id: number;
  operation: LedgerOp | string;
  collection: string;
  key: string;
  key_hash: string;
  document_bytes: number;
  hlc: string;
  origin_node_id: string;
  origin_signature: string;
  prev_hash: string;
  entry_hash: string;
}

export interface LedgerEntriesPage {
  node_id: string;
  from: number;
  to: number;
  count: number;
  entries: LedgerEntry[];
}

export interface VerifyResult {
  node_id: string;
  verified: boolean;
  entries_checked: number;
  signed_entries: number;
  unsigned_entries: number;
  failed_at_entry_id?: number;
  reason?: string;
}

export interface FullVerifyResult {
  node_id: string;
  ok: boolean;
  ledger: {
    verified: boolean;
    entries_checked: number;
    signed_entries: number;
    unsigned_entries: number;
    reason?: string;
  };
  backends_ok: boolean;
  backend_failure?: string;
}

export interface ChangeBatch {
  node_id: string;
  since: number;
  tip: number;
  /** The cursor predates a prune; the caller must re-read from scratch. */
  truncated: boolean;
  count: number;
  changes: LedgerEntry[];
}

export interface CollectionAcl {
  owner_node: string;
  private: boolean;
  readers: string[];
  parent: string;
}

export interface CollectionDetail {
  name: string;
  engine: string;
  document_count: number;
  checksum: string;
  created_at_ms: number;
  retention_days: number;
  shard_key: string;
  replication_factor: number;
  has_schema: boolean;
  acl: CollectionAcl;
  secondary_indexes: string[];
}

export interface EngineInfo {
  name: string;
  built_in: boolean;
  compiled_in: boolean;
  active_on_this_node: boolean;
  /** Present only when `compiled_in` is false: the CMake option that enables it. */
  enable_with?: string;
}

export interface DocumentRow {
  key: string;
  document: unknown;
}

export interface DocumentPage {
  collection: string;
  engine: string;
  count: number;
  documents: DocumentRow[];
}

export interface BrainCollection {
  name: string;
  engine: string;
  private: boolean;
  document_count: number;
  checksum: string;
}

export interface Brain {
  node_id: string;
  node_name: string;
  supervisor: boolean;
  generated_at_us: number;
  uptime_seconds: number;
  ledger_tip: { entry_id: number; entry_hash: string; signature: string };
  free_quota_mb: number;
  collections: BrainCollection[];
  known_peers: Peer[];
  transit_documents_held: number;
}

export interface TransitOwner {
  owner_node: string;
  documents: number;
  soonest_expiry_ms: number;
}

export interface TransitReport {
  holding_for: TransitOwner[];
  documents_held: number;
  bytes_held: number;
  ttl_seconds: number;
}

export interface ClaimReport {
  peers_asked: number;
  documents_claimed: number;
  failures: number;
}

export interface CheckpointResult {
  decision: {
    proceeded: boolean;
    agreeing: number;
    required: number;
    conflicting: number;
    unverified: number;
    checkpoint_lsn: number;
    reason?: string;
  };
  checkpoint_entry_id: number;
  entries_pruned: number;
  envelopes_released: number;
  previous_tip_hash: string;
  new_tip_hash: string;
}

export interface PlacementResult {
  collection: string;
  key: string;
  hash_input: string;
  shard_key: string;
  replicas: string[];
  primary: string;
  requested_rf: number;
  under_replicated: boolean;
  ring_nodes: number;
  skipped: string[];
}

export interface VectorHit {
  key: string;
  score: number;
}

export interface TsRollupBucket {
  bucket_start_ms: number;
  count: number;
  min: number;
  max: number;
  sum: number;
  first: number;
  last: number;
}

export interface GraphNeighbourhood {
  collection: string;
  key: string;
  out_edges: { to: string; label: string }[];
  in_edges: string[];
  descendants: string[];
}

// -- supervisor shapes -------------------------------------------------------

export interface DataDirCandidate {
  path: string;
  has_node_config: boolean;
  has_identity: boolean;
  has_data_file: boolean;
  has_manifest: boolean;
  removable: boolean;
  encrypted: boolean;
  node_name: string;
  node_id: string;
  free_bytes: number;
  used_bytes: number;
  /** Has an identity *and* config-or-data: a real node, not a lookalike folder. */
  existing_node: boolean;
  /** Empty enough to provision into without touching someone else's files. */
  adoptable: boolean;
}

export interface MountPoint {
  path: string;
  label: string;
  removable: boolean;
  total_bytes: number;
  free_bytes: number;
}

export interface ManagedNode {
  node_id: string;
  node_name: string;
  data_dir: string;
  api_port: number;
  p2p_port: number;
  state: LifecycleState;
  removable: boolean;
  encrypted: boolean;
  recovery_key_exported: boolean;
  recovery_key_exported_ms: number;
  keychain_ref: string;
  created_ms: number;
  last_seen_ms: number;
  last_error: string;
}

export interface TopologyPeer {
  node_id: string;
  host: string;
  hostname: string;
  api_port: number;
  p2p_port: number;
  state: LifecycleState;
  supervisor: boolean;
  score: number;
  ledger_entry_id: number;
}

export interface Topology {
  supervisor_node_id: string;
  mounts: MountPoint[];
  managed_nodes: ManagedNode[];
  lan_peers: TopologyPeer[];
  replication_factor: number;
}

export interface PairingPayload {
  version: number;
  node_id: string;
  public_key_hex: string;
  discovery_port: number;
  bootstrap_peers: string[];
}

export interface SupervisorPassReport {
  peers_marked_degraded: number;
  transit_expired: number;
  nodes_over_quota: number;
  checkpoint_attempted: boolean;
  checkpoint_proceeded: boolean;
  checkpoint_reason: string;
  entries_pruned: number;
}

// -- client ------------------------------------------------------------------

export interface RequestOptions {
  /** Aborts the request. Long polls pass the subscription's signal here. */
  signal?: AbortSignal;
  /**
   * Per-request timeout in ms. Defaults to 10s; the change feed overrides it,
   * because a long poll is *supposed* to hang for its full timeout.
   */
  timeoutMs?: number;
}

const DEFAULT_TIMEOUT_MS = 10_000;

/**
 * One node's API. Construct with the node's API port; the host is always
 * loopback, which is the property that lets `index.html` restrict `connect-src`
 * to 127.0.0.1 and nothing else.
 */
export class NodeApi {
  readonly baseUrl: string;

  constructor(readonly port: number, host = "127.0.0.1") {
    this.baseUrl = `http://${host}:${port}`;
  }

  // -- transport -------------------------------------------------------------

  private async request<T>(
    method: string,
    path: string,
    body?: unknown,
    options: RequestOptions = {},
  ): Promise<T> {
    const url = `${this.baseUrl}${path}`;
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), options.timeoutMs ?? DEFAULT_TIMEOUT_MS);
    // A caller's own signal must also abort us -- otherwise closing a view
    // leaves its long poll running until the server's timeout.
    const onAbort = () => controller.abort();
    options.signal?.addEventListener("abort", onAbort, { once: true });

    let response: Response;
    try {
      response = await fetch(url, {
        method,
        signal: controller.signal,
        headers: body === undefined ? undefined : { "Content-Type": "application/json" },
        body: body === undefined ? undefined : JSON.stringify(body),
      });
    } catch (cause) {
      if (options.signal?.aborted) throw cause;
      throw new UnreachableError(this.baseUrl, cause);
    } finally {
      clearTimeout(timeout);
      options.signal?.removeEventListener("abort", onAbort);
    }

    const text = await response.text();
    let parsed: unknown = null;
    if (text.length > 0) {
      try {
        parsed = JSON.parse(text);
      } catch {
        // A non-JSON body from a JSON API means something is badly wrong;
        // surface the raw text rather than a parse error about it.
        if (!response.ok) throw new ApiError(response.status, text.slice(0, 200), url);
        parsed = text;
      }
    }

    if (!response.ok) {
      const message =
        parsed && typeof parsed === "object" && "error" in parsed
          ? String((parsed as { error: unknown }).error)
          : `HTTP ${response.status}`;
      throw new ApiError(response.status, message, url);
    }
    return parsed as T;
  }

  private get<T>(path: string, options?: RequestOptions): Promise<T> {
    return this.request<T>("GET", path, undefined, options);
  }

  // -- node state ------------------------------------------------------------

  status(options?: RequestOptions): Promise<NodeStatus> {
    return this.get<NodeStatus>("/_status", options);
  }

  brain(options?: RequestOptions): Promise<Brain> {
    return this.get<Brain>("/_brain", options);
  }

  config(options?: RequestOptions): Promise<Record<string, unknown>> {
    return this.get<Record<string, unknown>>("/_config", options);
  }

  peers(options?: RequestOptions): Promise<Peer[]> {
    return this.get<Peer[]>("/_peers", options);
  }

  quota(options?: RequestOptions): Promise<QuotaReport> {
    return this.get<QuotaReport>("/_quota", options);
  }

  engines(options?: RequestOptions): Promise<EngineInfo[]> {
    return this.get<EngineInfo[]>("/_engines", options);
  }

  /** Only collections this node is permitted to read; `_transit` is never listed. */
  collections(options?: RequestOptions): Promise<string[]> {
    return this.get<string[]>("/_collections", options);
  }

  collection(name: string, options?: RequestOptions): Promise<CollectionDetail> {
    return this.get<CollectionDetail>(`/_collection/${encodeURIComponent(name)}`, options);
  }

  // -- documents -------------------------------------------------------------

  listDocuments(
    collection: string,
    params: { start?: string; limit?: number } = {},
    options?: RequestOptions,
  ): Promise<DocumentPage> {
    const query = new URLSearchParams();
    if (params.start) query.set("start", params.start);
    query.set("limit", String(params.limit ?? 100));
    return this.get<DocumentPage>(
      `/db/${encodeURIComponent(collection)}?${query.toString()}`,
      options,
    );
  }

  getDocument(collection: string, key: string, options?: RequestOptions): Promise<unknown> {
    return this.get<unknown>(
      `/db/${encodeURIComponent(collection)}/${encodeURIComponent(key)}`,
      options,
    );
  }

  putDocument(
    collection: string,
    key: string,
    document: unknown,
    options?: RequestOptions,
  ): Promise<{ ok: boolean; collection: string; key: string; entry_id: number }> {
    return this.request(
      "PUT",
      `/db/${encodeURIComponent(collection)}/${encodeURIComponent(key)}`,
      document,
      options,
    );
  }

  deleteDocument(collection: string, key: string, options?: RequestOptions): Promise<{ ok: boolean }> {
    return this.request(
      "DELETE",
      `/db/${encodeURIComponent(collection)}/${encodeURIComponent(key)}`,
      undefined,
      options,
    );
  }

  // -- collection administration ---------------------------------------------

  putSchema(collection: string, schema: unknown, options?: RequestOptions): Promise<{ ok: boolean }> {
    return this.request("PUT", `/_schema/${encodeURIComponent(collection)}`, schema, options);
  }

  getSchema(collection: string, options?: RequestOptions): Promise<unknown | null> {
    return this.get<unknown | null>(`/_schema/${encodeURIComponent(collection)}`, options);
  }

  bindEngine(collection: string, engine: string, options?: RequestOptions): Promise<{ ok: boolean }> {
    return this.request(
      "PUT",
      `/_collection/${encodeURIComponent(collection)}/engine`,
      { engine },
      options,
    );
  }

  setAcl(
    collection: string,
    acl: { owner_node?: string; private?: boolean; readers?: string[]; parent?: string },
    options?: RequestOptions,
  ): Promise<{ ok: boolean }> {
    return this.request("PUT", `/_collection/${encodeURIComponent(collection)}/acl`, acl, options);
  }

  setPlacement(
    collection: string,
    placement: { shard_key?: string; replication_factor?: number; retention_days?: number },
    options?: RequestOptions,
  ): Promise<{ ok: boolean }> {
    return this.request(
      "PUT",
      `/_collection/${encodeURIComponent(collection)}/placement`,
      placement,
      options,
    );
  }

  placementFor(collection: string, key: string, options?: RequestOptions): Promise<PlacementResult> {
    return this.get<PlacementResult>(
      `/_placement/${encodeURIComponent(collection)}/${encodeURIComponent(key)}`,
      options,
    );
  }

  // -- ledger ----------------------------------------------------------------

  ledgerTip(options?: RequestOptions): Promise<LedgerTip> {
    return this.get<LedgerTip>("/_ledger/tip", options);
  }

  ledgerEntries(from: number, to?: number, options?: RequestOptions): Promise<LedgerEntriesPage> {
    const query = new URLSearchParams({ from: String(from) });
    if (to !== undefined) query.set("to", String(to));
    return this.get<LedgerEntriesPage>(`/_ledger/entries?${query.toString()}`, options);
  }

  /** Re-derives the hash chain and checks every origin signature. */
  verifyLedger(options?: RequestOptions): Promise<VerifyResult> {
    // Verification walks the whole ledger, so it gets a longer default budget
    // than an ordinary read.
    return this.request<VerifyResult>("POST", "/_ledger/verify", undefined, {
      timeoutMs: 60_000,
      ...options,
    });
  }

  /** Ledger chain *and* every storage backend's own structural check. */
  verifyEverything(options?: RequestOptions): Promise<FullVerifyResult> {
    return this.request<FullVerifyResult>("POST", "/_verify", undefined, {
      timeoutMs: 120_000,
      ...options,
    });
  }

  /**
   * Long poll for ledger changes. `since` is the last entry_id the caller has
   * applied; -1 means "tell me the tip without waiting".
   *
   * The default timeout is deliberately above the server's 25s poll window:
   * aborting at exactly the server's timeout races it and shows spurious
   * disconnects.
   */
  changes(
    since: number,
    params: { timeoutMs?: number; limit?: number } = {},
    options: RequestOptions = {},
  ): Promise<ChangeBatch> {
    const serverTimeout = params.timeoutMs ?? 25_000;
    const query = new URLSearchParams({
      since: String(since),
      timeout_ms: String(serverTimeout),
      limit: String(params.limit ?? 256),
    });
    return this.get<ChangeBatch>(`/_changes?${query.toString()}`, {
      timeoutMs: serverTimeout + 10_000,
      ...options,
    });
  }

  // -- transit ---------------------------------------------------------------

  transit(options?: RequestOptions): Promise<TransitReport> {
    return this.get<TransitReport>("/_transit", options);
  }

  /** Asks peers for anything they are holding on this node's behalf. */
  claimTransit(options?: RequestOptions): Promise<ClaimReport> {
    return this.request<ClaimReport>("POST", "/_transit/claim", undefined, {
      timeoutMs: 60_000,
      ...options,
    });
  }

  expireTransit(options?: RequestOptions): Promise<{ expired: number }> {
    return this.request("POST", "/_transit/expire", undefined, options);
  }

  /** Supervisor-only; a data node answers 403. */
  checkpoint(options?: RequestOptions): Promise<CheckpointResult> {
    return this.request<CheckpointResult>("POST", "/_checkpoint", undefined, {
      timeoutMs: 120_000,
      ...options,
    });
  }

  // -- engine-specific queries -----------------------------------------------

  vectorSearch(
    collection: string,
    vector: number[],
    k = 10,
    options?: RequestOptions,
  ): Promise<{ collection: string; count: number; hits: VectorHit[] }> {
    return this.request(
      "POST",
      `/_search/vector/${encodeURIComponent(collection)}`,
      { vector, k },
      options,
    );
  }

  tsRollups(
    collection: string,
    params: { series?: string; from_ms?: number; to_ms?: number; bucket_ms?: number } = {},
    options?: RequestOptions,
  ): Promise<Record<string, TsRollupBucket[]>> {
    const query = new URLSearchParams();
    for (const [key, value] of Object.entries(params)) {
      if (value !== undefined) query.set(key, String(value));
    }
    return this.get<Record<string, TsRollupBucket[]>>(
      `/_ts/${encodeURIComponent(collection)}/rollups?${query.toString()}`,
      options,
    );
  }

  graphNeighbourhood(
    collection: string,
    key: string,
    depth = 1,
    options?: RequestOptions,
  ): Promise<GraphNeighbourhood> {
    return this.get<GraphNeighbourhood>(
      `/_graph/${encodeURIComponent(collection)}/${encodeURIComponent(key)}?depth=${depth}`,
      options,
    );
  }

  // -- supervisor surface ----------------------------------------------------
  //
  // Present only on a node started with "supervisor": true. On any other node
  // these 404, which is how the app decides whether to show control-plane UI.

  scan(root?: string, options?: RequestOptions): Promise<{ count: number; candidates: DataDirCandidate[] }> {
    const query = root ? `?root=${encodeURIComponent(root)}` : "";
    return this.get(`/_supervisor/scan${query}`, { timeoutMs: 60_000, ...options });
  }

  mounts(options?: RequestOptions): Promise<MountPoint[]> {
    return this.get<MountPoint[]>("/_supervisor/mounts", options);
  }

  inspect(path: string, options?: RequestOptions): Promise<DataDirCandidate> {
    return this.get<DataDirCandidate>(
      `/_supervisor/inspect?path=${encodeURIComponent(path)}`,
      options,
    );
  }

  managedNodes(options?: RequestOptions): Promise<ManagedNode[]> {
    return this.get<ManagedNode[]>("/_supervisor/nodes", options);
  }

  upsertManagedNode(node: Partial<ManagedNode> & { node_id: string }, options?: RequestOptions): Promise<ManagedNode> {
    return this.request("PUT", `/_supervisor/nodes/${encodeURIComponent(node.node_id)}`, node, options);
  }

  transitionNode(nodeId: string, state: LifecycleState, options?: RequestOptions): Promise<ManagedNode> {
    return this.request(
      "POST",
      `/_supervisor/nodes/${encodeURIComponent(nodeId)}/transition`,
      { state },
      options,
    );
  }

  /**
   * Records *that* the user exported a recovery key -- never the key. There is
   * no escrow: if this is the only record, the key exists solely wherever the
   * user put it.
   */
  recordRecoveryKeyExport(nodeId: string, options?: RequestOptions): Promise<boolean> {
    return this.request(
      "POST",
      `/_supervisor/nodes/${encodeURIComponent(nodeId)}/recovery-key-exported`,
      undefined,
      options,
    );
  }

  removeManagedNode(nodeId: string, options?: RequestOptions): Promise<{ ok: boolean }> {
    return this.request("DELETE", `/_supervisor/nodes/${encodeURIComponent(nodeId)}`, undefined, options);
  }

  writeManifest(dir: string, manifest: unknown, options?: RequestOptions): Promise<{ ok: boolean }> {
    return this.request("POST", "/_supervisor/manifest", { dir, manifest }, options);
  }

  supervisorPass(options?: RequestOptions): Promise<SupervisorPassReport> {
    return this.request<SupervisorPassReport>("POST", "/_supervisor/pass", undefined, {
      timeoutMs: 60_000,
      ...options,
    });
  }

  topology(options?: RequestOptions): Promise<Topology> {
    return this.get<Topology>("/_supervisor/topology", { timeoutMs: 30_000, ...options });
  }

  pairing(options?: RequestOptions): Promise<PairingPayload> {
    return this.get<PairingPayload>("/_supervisor/pairing", options);
  }
}

/** Cached clients, keyed by port -- views ask for a node's API by port freely. */
const clients = new Map<number, NodeApi>();

export function apiFor(port: number): NodeApi {
  let client = clients.get(port);
  if (client === undefined) {
    client = new NodeApi(port);
    clients.set(port, client);
  }
  return client;
}
