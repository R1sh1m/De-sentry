//! The sidecar's shared state, and the watchdog that keeps it true.
//!
//! One structure owns every supervised node, the ports handed out so far, and
//! the recovery keys that have been generated but not yet exported. A single
//! background thread polls for process exits, restarts what should be
//! restarted, and pushes every change to the window -- so the UI never has to
//! poll for "is it still running", and a node that dies while nobody is
//! looking still produces a notification.
//!
//! Pending recovery keys deserve a note. Between `create_node` returning a key
//! and the user exporting it, the key is held in memory here and nowhere else:
//! not on disk, not in a log, not in the node's config. `export_recovery_key`
//! writes it to the file the user chose and immediately drops it. If the app
//! exits in between, the key is gone -- which is the correct behaviour for a
//! system with no escrow, and the wizard says so before it gets there.

use std::collections::{HashMap, HashSet};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use serde::Serialize;
use tauri::{AppHandle, Emitter, Manager};

use crate::nodes::{self, LaunchSpec, NodeHandle, ProcessState, SupervisedNode};
use crate::ports::{self, PortAllocation};

/// How often the watchdog checks for exited children.
const WATCH_INTERVAL: Duration = Duration::from_millis(750);

/// The channel every sidecar-originated update travels on.
pub const EVENT: &str = "desentry://sidecar";

#[derive(Debug, Clone, Serialize)]
#[serde(tag = "kind", rename_all = "kebab-case")]
pub enum SidecarEvent {
    NodeState { node: SupervisedNode },
    VolumesChanged,
    PowerChanged { on_battery: bool },
    NetworkChanged,
}

pub struct AppState {
    /// The bundled `desentryd`, resolved once at startup.
    pub binary: PathBuf,
    /// Where new nodes are created by default.
    pub data_root: PathBuf,
    /// Bundled resources: prototypes, model, ONNX runtime.
    pub resource_dir: PathBuf,
    pub nodes: Mutex<HashMap<String, NodeHandle>>,
    /// Ports handed out this session, including to nodes still starting.
    reserved: Mutex<HashSet<u16>>,
    /// Recovery keys generated but not yet exported. Never persisted.
    pending_keys: Mutex<HashMap<String, String>>,
    /// Node id of the app's own supervisor.
    pub supervisor_id: Mutex<Option<String>>,
    pub background_mode: AtomicBool,
    pub on_battery: AtomicBool,
    /// Set when the app is shutting down, so the watchdog stops restarting.
    pub shutting_down: AtomicBool,
}

impl AppState {
    pub fn new(binary: PathBuf, data_root: PathBuf, resource_dir: PathBuf) -> Self {
        Self {
            binary,
            data_root,
            resource_dir,
            nodes: Mutex::new(HashMap::new()),
            reserved: Mutex::new(HashSet::new()),
            pending_keys: Mutex::new(HashMap::new()),
            supervisor_id: Mutex::new(None),
            background_mode: AtomicBool::new(false),
            on_battery: AtomicBool::new(false),
            shutting_down: AtomicBool::new(false),
        }
    }

    /// Reserves and returns the next free port triple.
    pub fn allocate_ports(&self) -> Result<PortAllocation, ports::PortError> {
        let mut reserved = self.reserved.lock().expect("port mutex");
        let allocation = ports::allocate(&reserved)?;
        reserved.insert(allocation.api_port);
        reserved.insert(allocation.p2p_port);
        Ok(allocation)
    }

    /// Releases ports when a node is forgotten, so a long session does not
    /// slowly exhaust the range.
    pub fn release_ports(&self, allocation: PortAllocation) {
        let mut reserved = self.reserved.lock().expect("port mutex");
        reserved.remove(&allocation.api_port);
        reserved.remove(&allocation.p2p_port);
    }

    pub fn views(&self) -> Vec<SupervisedNode> {
        let nodes = self.nodes.lock().expect("node mutex");
        let mut out: Vec<SupervisedNode> = nodes.values().map(NodeHandle::view).collect();
        // Stable order: the supervisor first, then by name. The window sorts
        // for display anyway, but a stable list keeps diffs meaningful.
        out.sort_by(|a, b| b.supervisor.cmp(&a.supervisor).then(a.node_name.cmp(&b.node_name)));
        out
    }

    pub fn view_of(&self, node_id: &str) -> Option<SupervisedNode> {
        self.nodes.lock().expect("node mutex").get(node_id).map(NodeHandle::view)
    }

    /// Starts a node and registers it under the id it reports.
    pub fn start_node(&self, spec: LaunchSpec) -> Result<SupervisedNode, nodes::NodeError> {
        let handle = nodes::start(&self.binary, spec)?;
        let view = handle.view();
        if handle.spec.supervisor {
            *self.supervisor_id.lock().expect("supervisor mutex") = Some(handle.node_id.clone());
        }
        self.nodes
            .lock()
            .expect("node mutex")
            .insert(handle.node_id.clone(), handle);
        Ok(view)
    }

    pub fn stop_node(&self, node_id: &str) -> Result<SupervisedNode, nodes::NodeError> {
        let mut nodes = self.nodes.lock().expect("node mutex");
        let handle = nodes
            .get_mut(node_id)
            .ok_or_else(|| nodes::NodeError::Unknown(node_id.to_owned()))?;
        nodes::stop(handle);
        Ok(handle.view())
    }

    pub fn restart_node(&self, node_id: &str) -> Result<SupervisedNode, nodes::NodeError> {
        // The handle is taken out of the map before the restart and put back
        // afterwards. A restart waits up to twenty seconds for the node to
        // answer, and holding the map across that would block every other
        // command the window issues in the meantime.
        let mut handle = {
            let mut nodes = self.nodes.lock().expect("node mutex");
            nodes
                .remove(node_id)
                .ok_or_else(|| nodes::NodeError::Unknown(node_id.to_owned()))?
        };

        nodes::stop(&mut handle);
        let result = nodes::restart(&self.binary, &mut handle);
        let view = handle.view();
        // A restart can come back with a different node_id if the data
        // directory was replaced; re-keying by the reported id keeps the map
        // honest either way.
        let key = if handle.node_id.is_empty() {
            node_id.to_owned()
        } else {
            handle.node_id.clone()
        };
        self.nodes.lock().expect("node mutex").insert(key, handle);
        result.map(|()| view)
    }

    /// Forgets a node without touching its data directory.
    pub fn forget_node(&self, node_id: &str) -> Result<(), nodes::NodeError> {
        let mut handle = {
            let mut nodes = self.nodes.lock().expect("node mutex");
            nodes
                .remove(node_id)
                .ok_or_else(|| nodes::NodeError::Unknown(node_id.to_owned()))?
        };
        nodes::stop(&mut handle);
        self.release_ports(PortAllocation {
            api_port: handle.spec.api_port,
            p2p_port: handle.spec.p2p_port,
            discovery_port: handle.spec.discovery_port,
        });
        self.pending_keys.lock().expect("key mutex").remove(node_id);
        Ok(())
    }

    pub fn logs(&self, node_id: &str, tail: usize) -> Vec<crate::nodes::LogLine> {
        self.nodes
            .lock()
            .expect("node mutex")
            .get(node_id)
            .map(|handle| handle.logs(tail))
            .unwrap_or_default()
    }

    // -- pending recovery keys -------------------------------------------------

    pub fn hold_recovery_key(&self, node_id: &str, key: String) {
        self.pending_keys
            .lock()
            .expect("key mutex")
            .insert(node_id.to_owned(), key);
    }

    pub fn peek_recovery_key(&self, node_id: &str) -> Option<String> {
        self.pending_keys.lock().expect("key mutex").get(node_id).cloned()
    }

    /// Returns the key and forgets it in the same step, so the "shown once"
    /// promise is enforced by the code rather than by the caller remembering.
    pub fn take_recovery_key(&self, node_id: &str) -> Option<String> {
        self.pending_keys.lock().expect("key mutex").remove(node_id)
    }

    pub fn has_unexported_keys(&self) -> bool {
        !self.pending_keys.lock().expect("key mutex").is_empty()
    }
}

/// Emits a sidecar event to every window.
pub fn emit(app: &AppHandle, event: SidecarEvent) {
    // A failed emit means no window is listening -- background mode, or a
    // window closing as the event fires. It is not an error worth propagating.
    let _ = app.emit(EVENT, event);
}

/// Starts the watchdog thread.
///
/// It does three things on each pass: reap exited children, restart the ones
/// that should come back, and tell the window about anything that changed. It
/// is the only place a restart is initiated automatically, so the restart
/// budget in `nodes.rs` is enough to bound them.
pub fn start_watchdog(app: AppHandle, state: Arc<AppState>) {
    std::thread::spawn(move || loop {
        std::thread::sleep(WATCH_INTERVAL);
        if state.shutting_down.load(Ordering::SeqCst) {
            return;
        }

        // The work is done in two phases so the node mutex is never held
        // across a restart -- a restart waits up to twenty seconds for the
        // node to answer, and holding the map for that long would freeze every
        // command the window issues.
        let exited: Vec<String> = {
            let mut nodes = state.nodes.lock().expect("node mutex");
            let mut exited = Vec::new();
            for (node_id, handle) in nodes.iter_mut() {
                if nodes::poll_exit(handle).is_some() {
                    exited.push(node_id.clone());
                }
            }
            exited
        };

        for node_id in exited {
            let (should_restart, view) = {
                let nodes = state.nodes.lock().expect("node mutex");
                match nodes.get(&node_id) {
                    Some(handle) => (handle.state == ProcessState::Failed, Some(handle.view())),
                    None => (false, None),
                }
            };

            if let Some(view) = view {
                crate::notify::node_stopped(&app, &view);
                emit(&app, SidecarEvent::NodeState { node: view });
            }

            if !should_restart || state.shutting_down.load(Ordering::SeqCst) {
                continue;
            }

            // Same discipline as `restart_node`: out of the map, restarted,
            // back in -- never restarted with the map held.
            let handle = state.nodes.lock().expect("node mutex").remove(&node_id);
            if let Some(mut handle) = handle {
                let _ = nodes::restart(&state.binary, &mut handle);
                let view = handle.view();
                let key = if handle.node_id.is_empty() {
                    node_id.clone()
                } else {
                    handle.node_id.clone()
                };
                state.nodes.lock().expect("node mutex").insert(key, handle);
                emit(&app, SidecarEvent::NodeState { node: view });
            }
        }
    });
}

/// Stops every node. Called on shutdown so nothing is left orphaned.
pub fn shutdown(state: &AppState) {
    state.shutting_down.store(true, Ordering::SeqCst);
    let mut nodes = state.nodes.lock().expect("node mutex");
    for handle in nodes.values_mut() {
        nodes::stop(handle);
    }
}

/// Convenience for commands: the state out of a Tauri handle.
pub fn from(app: &AppHandle) -> Arc<AppState> {
    app.state::<Arc<AppState>>().inner().clone()
}
