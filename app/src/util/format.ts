/**
 * Formatters shared by every view.
 *
 * These exist as one module because a control room that renders the same
 * quantity two different ways in two different panes is a control room whose
 * operator has to stop and work out whether they are looking at the same
 * number. Bytes are formatted the same everywhere; so are hashes, durations
 * and node ids.
 */

const KIB = 1024;

/** Bytes as a human string. Binary units, because quotas are set in MiB. */
export function bytes(n: number | undefined | null): string {
  if (n === undefined || n === null || !Number.isFinite(n)) return "—";
  if (n < KIB) return `${n} B`;
  const units = ["KiB", "MiB", "GiB", "TiB", "PiB"];
  let value = n / KIB;
  let unit = 0;
  while (value >= KIB && unit < units.length - 1) {
    value /= KIB;
    unit += 1;
  }
  // One decimal below 100, none above -- "1.4 GiB" reads; "1.437 GiB" does not.
  const digits = value < 10 ? 1 : value < 100 ? 1 : 0;
  return `${value.toFixed(digits)} ${units[unit]}`;
}

/** Integer counts with thin separators. */
export function count(n: number | undefined | null): string {
  if (n === undefined || n === null || !Number.isFinite(n)) return "—";
  return n.toLocaleString(undefined, { maximumFractionDigits: 0 });
}

export function percent(fraction: number | undefined | null, digits = 0): string {
  if (fraction === undefined || fraction === null || !Number.isFinite(fraction)) return "—";
  return `${(fraction * 100).toFixed(digits)}%`;
}

/**
 * A duration in seconds as the coarsest unit that still says something.
 * An uptime of 90061s is "1d 1h", not "1 day, 1 hour, 1 minute and 1 second":
 * the operator wants to know it has been up about a day.
 */
export function duration(seconds: number | undefined | null): string {
  if (seconds === undefined || seconds === null || !Number.isFinite(seconds)) return "—";
  const s = Math.max(0, Math.floor(seconds));
  if (s < 60) return `${s}s`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m}m ${s % 60}s`;
  const h = Math.floor(m / 60);
  if (h < 24) return `${h}h ${m % 60}m`;
  const d = Math.floor(h / 24);
  return `${d}d ${h % 24}h`;
}

/**
 * "how long ago", from a millisecond epoch. Returns "—" for 0, which is the
 * engine's "never" -- rendering that as "56 years ago" would be worse than
 * useless.
 */
export function ago(epochMs: number | undefined | null, now = Date.now()): string {
  if (!epochMs) return "—";
  const delta = Math.max(0, now - epochMs);
  if (delta < 1500) return "just now";
  return `${duration(delta / 1000)} ago`;
}

export function timestamp(epochMs: number | undefined | null): string {
  if (!epochMs) return "—";
  const d = new Date(epochMs);
  return d.toLocaleString(undefined, {
    year: "numeric",
    month: "short",
    day: "numeric",
    hour: "2-digit",
    minute: "2-digit",
  });
}

/**
 * A hash or node id, shortened for a table cell. Keeps head and tail, because
 * a truncation that keeps only the head makes two different hashes with a
 * shared prefix look identical -- and shared prefixes are exactly what a
 * lagging-vs-diverged ledger comparison turns on.
 */
export function shortHash(hex: string | undefined | null, head = 8, tail = 6): string {
  if (!hex) return "—";
  if (hex.length <= head + tail + 1) return hex;
  return `${hex.slice(0, head)}…${hex.slice(-tail)}`;
}

/** Node ids are hashes too, but the head alone is the conventional handle. */
export function shortNode(nodeId: string | undefined | null, head = 10): string {
  if (!nodeId) return "—";
  return nodeId.length <= head ? nodeId : `${nodeId.slice(0, head)}…`;
}

/** Latency, with sub-millisecond values kept honest rather than rounded to 0. */
export function millis(ms: number | undefined | null): string {
  if (ms === undefined || ms === null || !Number.isFinite(ms)) return "—";
  if (ms <= 0) return "—";
  if (ms < 1) return "<1 ms";
  if (ms < 100) return `${ms.toFixed(1)} ms`;
  return `${Math.round(ms)} ms`;
}

/** Pretty JSON for the document preview pane. */
export function json(value: unknown): string {
  try {
    return JSON.stringify(value, null, 2);
  } catch {
    return String(value);
  }
}

/**
 * A one-line summary of a document for a table row: the first few scalar
 * fields, so the explorer's list is scannable without expanding every row.
 */
export function documentPreview(doc: unknown, maxFields = 4): string {
  if (doc === null || doc === undefined) return "null";
  if (typeof doc !== "object") return String(doc);
  if (Array.isArray(doc)) return `[${doc.length} items]`;
  const parts: string[] = [];
  for (const [key, value] of Object.entries(doc as Record<string, unknown>)) {
    if (parts.length >= maxFields) {
      parts.push("…");
      break;
    }
    if (value === null) parts.push(`${key}: null`);
    else if (typeof value === "object") parts.push(`${key}: ${Array.isArray(value) ? "[…]" : "{…}"}`);
    else parts.push(`${key}: ${truncate(String(value), 28)}`);
  }
  return parts.join("  ·  ") || "{}";
}

export function truncate(text: string, max: number): string {
  return text.length <= max ? text : `${text.slice(0, max - 1)}…`;
}

/** Title-cases an engine or state identifier for display: `ts_rollup` -> `Ts Rollup`. */
export function humanize(identifier: string | undefined | null): string {
  if (!identifier) return "—";
  return identifier
    .split(/[_\-\s]+/)
    .filter(Boolean)
    .map((word) => word.charAt(0).toUpperCase() + word.slice(1))
    .join(" ");
}

/** Engine identifiers keep their exact spelling, but get a friendlier label. */
export const EngineLabels: Record<string, string> = {
  kv: "Key–Value",
  columnar_lite: "Columnar",
  ts_rollup: "Time series",
  vector_hnsw_lite: "Vector",
  graph_adj: "Graph",
  sqlite: "SQLite",
  duckdb: "DuckDB",
  lmdb: "LMDB",
  sqlite_vec: "SQLite-vec",
};

export function engineLabel(name: string | undefined | null): string {
  if (!name) return "—";
  return EngineLabels[name] ?? humanize(name);
}
