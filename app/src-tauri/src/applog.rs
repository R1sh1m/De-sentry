//! One log, viewable in the app: the app's own records and every node's output.
//!
//! Before this module the sidecar called `log::warn!` and `log::error!`
//! throughout -- "the supervisor did not start", "a node did not restart" --
//! but no logger was ever installed, so every one of those records was
//! discarded. The failures most worth seeing were the ones nobody could see.
//!
//! What this provides:
//!
//!   * A `log::Log` implementation installed at startup, writing to an
//!     in-memory ring and to a size-rotated file in the app's log directory.
//!   * A single, globally ordered sequence number shared with node output
//!     (`nodes.rs` stamps each captured line with `next_seq()`), so the window
//!     can poll `since_seq` and receive app and node lines interleaved in the
//!     order they happened.
//!   * A query surface the Logs view and the assistant both use.
//!
//! SCAFFOLD CONTRACT (architecture-v3 §5). Types and command signatures are
//! fixed; the bodies marked `TODO(platform)` are filled in by the platform
//! workstream.

use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;

use serde::{Deserialize, Serialize};
use tauri::State;

use crate::appstate::AppState;

/// Records kept in memory for the app source. Node sources keep their own ring
/// (`nodes::LOG_CAPACITY`).
pub const APP_LOG_CAPACITY: usize = 5000;

/// Source id for the app's own records.
pub const SOURCE_APP: &str = "app";
/// Source id for the app's supervisor node.
pub const SOURCE_SUPERVISOR: &str = "supervisor";
/// Prefix for a data node's source id: `node:<node_id>`.
pub const SOURCE_NODE_PREFIX: &str = "node:";

static NEXT_SEQ: AtomicU64 = AtomicU64::new(1);

/// The next value of the one sequence shared by every log source.
pub fn next_seq() -> u64 {
    NEXT_SEQ.fetch_add(1, Ordering::Relaxed)
}

/// One record, from any source.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct AppLogEntry {
    /// Globally ordered across sources; strictly increasing.
    pub seq: u64,
    pub ts_ms: i64,
    /// "error" | "warn" | "info" | "debug" | "trace"
    pub level: String,
    /// "app" | "supervisor" | "node:<node_id>"
    pub source: String,
    /// Human name for the source: "App", "Supervisor", or the node's name.
    pub source_label: String,
    /// Rust module path for app records; engine subsystem ("wal", "tcp") for nodes.
    pub target: String,
    pub message: String,
}

/// A filter. Every field is optional; the empty query returns the newest
/// `limit` records from every source.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct LogQuery {
    /// Source ids to include. Empty means all.
    #[serde(default)]
    pub sources: Vec<String>,
    /// Lowest level to include: "error" < "warn" < "info" < "debug" < "trace".
    #[serde(default)]
    pub min_level: Option<String>,
    /// Case-insensitive substring over message and target.
    #[serde(default)]
    pub contains: Option<String>,
    /// Only records with `seq > since_seq` -- how the window follows a live log.
    #[serde(default)]
    pub since_seq: Option<u64>,
    /// Newest N after filtering. Defaults to 500, capped at 5000.
    #[serde(default)]
    pub limit: Option<usize>,
}

/// A source the window can filter by.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct LogSource {
    pub id: String,
    pub label: String,
    /// "app" | "supervisor" | "node"
    pub kind: String,
    pub running: bool,
}

/// Installs the logger. Called once, first thing in `setup`.
///
/// TODO(platform): install a `log::Log` writing to the ring and to
/// `<log_dir>/app.log`, rotated at 5 MiB keeping 3 files.
pub fn init(log_dir: PathBuf) {
    let _ = log_dir;
}

/// Records matching `query`, oldest first, across the app ring and every node.
///
/// TODO(platform): merge app ring + node rings, filter, order by seq, take the
/// newest `limit`.
pub fn search(state: &AppState, query: &LogQuery) -> Vec<AppLogEntry> {
    let _ = (state, query);
    Vec::new()
}

/// Every source that currently has, or could have, records.
///
/// TODO(platform): "app", "supervisor" when present, and one per managed node.
pub fn sources(state: &AppState) -> Vec<LogSource> {
    let _ = state;
    vec![LogSource {
        id: SOURCE_APP.to_owned(),
        label: "App".to_owned(),
        kind: SOURCE_APP.to_owned(),
        running: true,
    }]
}

#[tauri::command]
pub fn app_logs(state: State<'_, Arc<AppState>>, query: LogQuery) -> Result<Vec<AppLogEntry>, String> {
    Ok(search(state.inner(), &query))
}

#[tauri::command]
pub fn log_sources(state: State<'_, Arc<AppState>>) -> Result<Vec<LogSource>, String> {
    Ok(sources(state.inner()))
}

/// Writes the records matching `query` to `path` as plain text. Returns the
/// number of records written.
///
/// TODO(platform): validate the path came from the save dialog, write lines as
/// `ts level source target message`.
#[tauri::command]
pub fn export_logs(state: State<'_, Arc<AppState>>, path: String, query: LogQuery) -> Result<usize, String> {
    let _ = (state, path, query);
    Err("log export is not implemented yet".into())
}
