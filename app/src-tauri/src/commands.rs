//! The command surface the window invokes.
//!
//! Every name here matches a method on `sidecar` in app/src/bridge.ts. The
//! pairing is by name and checked by reading, not by a generated binding
//! layer -- there are eighteen of them, and a generator would be a dependency
//! that exists to save less typing than it costs to audit.
//!
//! Errors are returned as plain strings. The window renders them verbatim, so
//! they are written to be read by the person who hit the problem rather than
//! by whoever wrote the code: what failed, on what, and what to do.

use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};
use tauri::{AppHandle, State};
use tauri_plugin_dialog::DialogExt;

use crate::ai::{self, NodeSpec};
use crate::appstate::{self, AppState, DiscoveredCandidate, SidecarEvent};
use crate::configgen::{self, NodeConfigSpec, QuotaSplit};
use crate::keychain;
use crate::nodes::{LaunchSpec, LogLine, SupervisedNode};
use crate::ports::PortAllocation;
use crate::recovery;

type Reply<T> = Result<T, String>;

fn fail(error: impl std::fmt::Display) -> String {
    error.to_string()
}

// -- app information ---------------------------------------------------------

#[derive(Debug, Clone, Serialize)]
pub struct AppInfo {
    pub version: String,
    pub platform: String,
    pub supervisor_api_port: Option<u16>,
    pub autostart_enabled: bool,
    pub background_mode: bool,
    pub on_battery: bool,
    pub default_data_root: String,
    pub engine_version: String,
}

#[tauri::command]
pub fn app_info(app: AppHandle, state: State<'_, std::sync::Arc<AppState>>) -> Reply<AppInfo> {
    let supervisor_api_port = state
        .supervisor_id
        .lock()
        .map_err(|_| "the node registry is unavailable".to_string())?
        .as_ref()
        .and_then(|id| state.view_of(id))
        .map(|node| node.api_port);

    Ok(AppInfo {
        version: app.package_info().version.to_string(),
        platform: std::env::consts::OS.to_string(),
        supervisor_api_port,
        autostart_enabled: crate::autostart_enabled(&app),
        background_mode: state.background_mode.load(std::sync::atomic::Ordering::SeqCst),
        on_battery: state.on_battery.load(std::sync::atomic::Ordering::SeqCst),
        default_data_root: state.data_root.to_string_lossy().into_owned(),
        engine_version: crate::engine_version(&state.binary),
    })
}

// -- process supervision -----------------------------------------------------

#[tauri::command]
pub fn list_nodes(state: State<'_, std::sync::Arc<AppState>>) -> Reply<Vec<SupervisedNode>> {
    Ok(state.views())
}

#[tauri::command]
pub fn allocate_ports(state: State<'_, std::sync::Arc<AppState>>) -> Reply<PortAllocation> {
    state.allocate_ports().map_err(fail)
}

#[tauri::command]
pub fn node_logs(
    state: State<'_, std::sync::Arc<AppState>>,
    node_id: String,
    tail: Option<usize>,
) -> Reply<Vec<LogLine>> {
    Ok(state.logs(&node_id, tail.unwrap_or(400)))
}

#[tauri::command]
pub fn stop_node(
    app: AppHandle,
    state: State<'_, std::sync::Arc<AppState>>,
    node_id: String,
) -> Reply<()> {
    let view = state.stop_node(&node_id).map_err(fail)?;
    appstate::emit(&app, SidecarEvent::NodeState { node: view });
    Ok(())
}

#[tauri::command]
pub fn lock_node(
    app: AppHandle,
    state: State<'_, std::sync::Arc<AppState>>,
    node_id: String,
) -> Reply<SupervisedNode> {
    let view = state.lock_node(&node_id).map_err(fail)?;
    appstate::emit(&app, SidecarEvent::NodeState { node: view.clone() });
    Ok(view)
}

#[tauri::command]
pub fn restart_node(
    app: AppHandle,
    state: State<'_, std::sync::Arc<AppState>>,
    node_id: String,
) -> Reply<SupervisedNode> {
    let view = state.restart_node(&node_id).map_err(fail)?;
    appstate::emit(&app, SidecarEvent::NodeState { node: view.clone() });
    Ok(view)
}

#[tauri::command]
pub fn forget_node(state: State<'_, std::sync::Arc<AppState>>, node_id: String) -> Reply<()> {
    state.forget_node(&node_id).map_err(fail)
}

#[tauri::command]
pub fn delete_node(
    state: State<'_, std::sync::Arc<AppState>>,
    node_id: String,
    delete_data: bool,
) -> Reply<()> {
    let (data_dir, keychain_ref) = {
        let nodes = state.nodes.lock().map_err(|_| "the node registry is unavailable".to_string())?;
        if let Some(handle) = nodes.get(&node_id) {
            (
                Some(handle.spec.data_dir.clone()),
                Some(crate::keychain::reference_for(&handle.node_id)),
            )
        } else {
            (None, None)
        }
    };

    let _ = state.forget_node(&node_id);

    if delete_data {
        if let Some(key_ref) = keychain_ref {
            let _ = crate::keychain::forget(&key_ref);
        }
        if let Some(dir) = data_dir {
            if dir.exists() {
                let _ = std::fs::remove_dir_all(&dir);
            }
        }
    }

    Ok(())
}

#[tauri::command]
pub fn delete_directory(path: String) -> Reply<()> {
    let dir = PathBuf::from(&path);
    if dir.exists() {
        std::fs::remove_dir_all(&dir).map_err(|e| format!("Could not delete folder: {e}"))?;
    }
    Ok(())
}

/// Adopts an existing data directory: reads its config, gives it a free port
/// pair if the one it names is taken, and starts it.
#[tauri::command]
pub fn start_existing_node(
    app: AppHandle,
    state: State<'_, std::sync::Arc<AppState>>,
    data_dir: String,
) -> Reply<SupervisedNode> {
    let dir = PathBuf::from(&data_dir);
    let config_path = dir.join("node.json");
    if !config_path.exists() {
        return Err(format!(
            "{} has no node.json, so there is no node here to start. Create one instead.",
            dir.display()
        ));
    }

    let node_name = configgen::read_node_name(&dir).unwrap_or_else(|| {
        dir.file_name()
            .map(|name| name.to_string_lossy().into_owned())
            .unwrap_or_else(|| "node".to_owned())
    });

    // Ports are re-allocated rather than trusted from the file: the machine
    // this directory was last run on is not necessarily this one, and a config
    // naming a port something else already holds would fail to start with a
    // message about binding rather than about ports.
    let allocation = state.allocate_ports().map_err(fail)?;
    let existing: serde_json::Value = std::fs::read_to_string(&config_path)
        .ok()
        .and_then(|text| serde_json::from_str(&text).ok())
        .unwrap_or_else(|| serde_json::json!({}));

    let encrypted = existing
        .get("encrypt_at_rest")
        .and_then(|v| v.as_bool())
        .unwrap_or(false);
    let keychain_ref = existing
        .get("keychain_ref")
        .and_then(|v| v.as_str())
        .unwrap_or_default()
        .to_owned();

    // An encrypted node needs its key. If this machine's keychain has it, the
    // node starts silently; if not, the window is told to ask for a password
    // rather than the node failing with a decryption error. Either early
    // return releases the allocation: ports handed out but never bound would
    // otherwise leak out of the range one pair at a time.
    let unlock_secret = if encrypted && !keychain_ref.is_empty() {
        match keychain::load(&keychain_ref) {
            Ok(key) => Some(key),
            Err(error) => {
                state.release_ports(allocation);
                return Err(error.to_string());
            }
        }
    } else {
        None
    };

    if let Err(error) = rewrite_ports(&config_path, &allocation) {
        state.release_ports(allocation);
        return Err(error);
    }

    let spec = LaunchSpec {
        node_name,
        data_dir: dir.clone(),
        config_path,
        api_port: allocation.api_port,
        p2p_port: allocation.p2p_port,
        discovery_port: allocation.discovery_port,
        supervisor: existing.get("supervisor").and_then(|v| v.as_bool()).unwrap_or(false),
        removable: false,
        encrypted,
        unlock_secret,
    };

    let view = state.start_node(spec).map_err(|error| {
        state.release_ports(allocation);
        fail(error)
    })?;
    appstate::emit(&app, SidecarEvent::NodeState { node: view.clone() });
    Ok(view)
}

/// Rewrites only the port fields of an existing config, leaving the rest alone.
fn rewrite_ports(config_path: &Path, allocation: &PortAllocation) -> Reply<()> {
    let text = std::fs::read_to_string(config_path).map_err(fail)?;
    let mut value: serde_json::Value = serde_json::from_str(&text).map_err(|error| {
        format!("{} is not valid JSON: {error}", config_path.display())
    })?;
    if let Some(object) = value.as_object_mut() {
        object.insert("api_port".into(), allocation.api_port.into());
        object.insert("p2p_port".into(), allocation.p2p_port.into());
        object.insert("discovery_port".into(), allocation.discovery_port.into());
    }
    let body = serde_json::to_string_pretty(&value).map_err(fail)?;
    std::fs::write(config_path, body + "\n").map_err(fail)
}

// -- creation ----------------------------------------------------------------

#[derive(Debug, Clone, Deserialize)]
pub struct CreateNodeRequest {
    pub node_name: String,
    pub data_dir: String,
    #[serde(default)]
    pub adopt: bool,
    pub quota_mb: u64,
    pub spec: NodeSpec,
    pub encrypt_at_rest: bool,
    #[serde(default)]
    pub supervisor: bool,
    #[serde(default)]
    pub removable: bool,
    #[serde(default)]
    pub bootstrap_peers: Vec<String>,
    #[serde(default)]
    pub description: String,
}

#[derive(Debug, Clone, Serialize)]
pub struct CreateNodeResult {
    pub node: SupervisedNode,
    pub recovery_key: Option<String>,
    pub keychain_ref: String,
}

#[tauri::command]
pub fn create_node(
    app: AppHandle,
    state: State<'_, std::sync::Arc<AppState>>,
    request: CreateNodeRequest,
) -> Reply<CreateNodeResult> {
    let data_dir = PathBuf::from(&request.data_dir);
    if request.node_name.trim().is_empty() {
        return Err("a node needs a name".into());
    }
    if request.adopt {
        return start_existing_node(app, state, request.data_dir)
            .map(|node| CreateNodeResult {
                node,
                recovery_key: None,
                keychain_ref: String::new(),
            });
    }

    let allocation = state.allocate_ports().map_err(fail)?;

    // The key is generated before the node starts, so the node is encrypted
    // from its first write rather than converted afterwards.
    let recovery_key = if request.encrypt_at_rest {
        Some(recovery::generate().map_err(fail)?)
    } else {
        None
    };

    let config = NodeConfigSpec {
        data_dir: data_dir.clone(),
        node_name: request.node_name.trim().to_owned(),
        ports: allocation,
        supervisor: request.supervisor,
        quota_mb: request.quota_mb,
        quota_split: normalise_split(request.spec.quota_split),
        engines: request.spec.engines.clone(),
        default_engine: request.spec.default_engine.clone(),
        replication_factor: request.spec.replication_factor.max(1),
        retention_days: request.spec.retention_days,
        encrypt_at_rest: request.encrypt_at_rest,
        // Filled in once the node reports its id: the keychain entry is named
        // after the node, and the node's identity does not exist until it has
        // generated one.
        keychain_ref: String::new(),
        bootstrap_peers: request.bootstrap_peers.clone(),
        advertise_hostname: hostname(),
    };
    // Every step below can fail after earlier steps have already had effects
    // (reserved ports, a node.json on disk, a started child, a keychain
    // entry). There is exactly one rollback path for all of them --
    // `rollback_create` -- so a failed creation never leaks a process, a port
    // reservation, a keychain entry, or a half-written node.json that a later
    // scan would mistake for a real node. The original error is always what
    // the window sees; cleanup failures are logged, never substituted.
    let config_path = match config.write() {
        Ok(path) => path,
        Err(error) => {
            state.release_ports(allocation);
            return Err(format!("could not write {}: {error}", data_dir.join("node.json").display()));
        }
    };

    let spec = LaunchSpec {
        node_name: config.node_name.clone(),
        data_dir: data_dir.clone(),
        config_path: config_path.clone(),
        api_port: allocation.api_port,
        p2p_port: allocation.p2p_port,
        discovery_port: allocation.discovery_port,
        supervisor: request.supervisor,
        removable: request.removable,
        encrypted: request.encrypt_at_rest,
        unlock_secret: recovery_key.clone(),
    };

    let node = match state.start_node(spec) {
        Ok(node) => node,
        Err(error) => {
            rollback_create(&state, allocation, None, None, Some(&config_path));
            return Err(fail(error));
        }
    };

    // Now that the node has an identity, the key gets a home.
    //
    // Removable nodes deliberately get none: a stick that unlocks from this
    // machine's keychain is a stick that cannot be read on any other machine,
    // which defeats the point of putting a node on a stick. Those unlock from
    // the recovery key the user is about to export.
    let mut keychain_ref = String::new();
    if let Some(key) = recovery_key.as_ref() {
        if !request.removable {
            keychain_ref = keychain::reference_for(&node.node_id);
            if let Err(error) = keychain::store(&keychain_ref, key) {
                let message = fail(error);
                rollback_create(&state, allocation, Some(&node.node_id), None, Some(&config_path));
                return Err(message);
            }
            let mut updated = config.clone();
            updated.keychain_ref = keychain_ref.clone();
            if let Err(error) = updated.write() {
                let message = fail(error);
                rollback_create(
                    &state,
                    allocation,
                    Some(&node.node_id),
                    Some(keychain_ref.as_str()),
                    Some(&config_path),
                );
                return Err(message);
            }
        }
        state.hold_recovery_key(&node.node_id, key.clone());
    }

    // The sizing decision travels with the node, so a stick carried to another
    // machine says why it is shaped the way it is.
    let manifest = serde_json::json!({
        "version": 2,
        "node_id": node.node_id,
        "node_name": node.node_name,
        "created_ms": crate::nodes::now_ms(),
        "encrypted": request.encrypt_at_rest,
        "removable": request.removable,
        "quota_mb": request.quota_mb,
        "engines": request.spec.engines,
        "default_engine": request.spec.default_engine,
        "replication_factor": request.spec.replication_factor,
        "description": request.description,
        "sizing": request.spec.decision,
    });
    if let Err(error) = configgen::write_manifest(&data_dir, &manifest) {
        // A missing manifest costs auditability, not function. Saying so beats
        // failing a node creation that otherwise succeeded.
        log::warn!("could not write the node manifest: {error}");
    }

    appstate::emit(&app, SidecarEvent::NodeState { node: node.clone() });

    Ok(CreateNodeResult {
        node,
        recovery_key,
        keychain_ref,
    })
}

fn normalise_split(split: QuotaSplit) -> QuotaSplit {
    split.normalised()
}

/// Undoes a half-finished `create_node`: stops the child (if started),
/// releases the port reservation, forgets a keychain entry this attempt
/// stored, and removes the node.json this attempt wrote so a later directory
/// scan does not adopt a half-created node.
///
/// Only artifacts named here are touched: the data directory itself is left
/// alone (the user may have pointed creation at a directory that already held
/// other files), as is any key the window already held. Cleanup failures are
/// logged and ignored -- the caller reports the original error.
fn rollback_create(
    state: &AppState,
    allocation: PortAllocation,
    node_id: Option<&str>,
    keychain_ref: Option<&str>,
    config_path: Option<&Path>,
) {
    if let Some(id) = node_id {
        // Stops the child, releases its ports, and drops its pending key.
        if let Err(error) = state.forget_node(id) {
            log::warn!("rollback: could not stop half-created node {id}: {error}");
        }
    } else {
        state.release_ports(allocation);
    }
    if let Some(reference) = keychain_ref {
        if let Err(error) = keychain::forget(reference) {
            log::warn!("rollback: could not forget keychain entry {reference}: {error}");
        }
    }
    if let Some(path) = config_path {
        if let Err(error) = std::fs::remove_file(path) {
            log::warn!("rollback: could not remove {}: {error}", path.display());
        }
    }
}

fn hostname() -> String {
    // No hostname crate for one string: every platform exposes it as an
    // environment variable or a file, and an empty value simply means the
    // sidebar shows an address instead of a name.
    std::env::var("COMPUTERNAME")
        .or_else(|_| std::env::var("HOSTNAME"))
        .ok()
        .or_else(|| std::fs::read_to_string("/etc/hostname").ok().map(|s| s.trim().to_owned()))
        .filter(|name| !name.is_empty())
        .unwrap_or_default()
}

// -- sizing ------------------------------------------------------------------

#[tauri::command]
pub fn size_workload(
    state: State<'_, std::sync::Arc<AppState>>,
    description: String,
    quota_mb: u64,
) -> Reply<NodeSpec> {
    let sizer = ai::get().ok_or_else(|| {
        "the workload prototypes are missing from this installation, so nothing can be proposed. \
         Choose the engines by hand."
            .to_string()
    })?;
    let engines = engine_names(state.inner());
    Ok(sizer.size(&description, quota_mb, &engines))
}

#[tauri::command]
pub fn available_engines(state: State<'_, std::sync::Arc<AppState>>) -> Reply<Vec<String>> {
    Ok(engine_names(state.inner()))
}

/// Asks the running supervisor which engines this build actually has.
///
/// The binary is the authority, not a list compiled into the app: a build with
/// SQLite vendored in and one without are the same app with different engines,
/// and only the binary knows which it is.
fn engine_names(state: &AppState) -> Vec<String> {
    let port = state
        .supervisor_id
        .lock()
        .ok()
        .and_then(|id| id.clone())
        .and_then(|id| state.view_of(&id))
        .map(|node| node.api_port);

    let Some(port) = port else {
        return vec!["kv".to_owned()];
    };
    let Ok(body) = crate::http::get(port, "/_engines", std::time::Duration::from_secs(3)) else {
        return vec!["kv".to_owned()];
    };
    let Ok(value): Result<serde_json::Value, _> = serde_json::from_str(&body) else {
        return vec!["kv".to_owned()];
    };
    value
        .as_array()
        .map(|engines| {
            engines
                .iter()
                .filter(|engine| engine.get("compiled_in").and_then(|v| v.as_bool()).unwrap_or(false))
                .filter_map(|engine| engine.get("name").and_then(|v| v.as_str()).map(str::to_owned))
                .collect()
        })
        .unwrap_or_else(|| vec!["kv".to_owned()])
}

// -- keys --------------------------------------------------------------------

#[tauri::command]
pub fn pending_recovery_key(
    state: State<'_, std::sync::Arc<AppState>>,
    node_id: String,
) -> Reply<Option<String>> {
    Ok(state.peek_recovery_key(&node_id))
}

/// Writes the key to the file the user chose, then drops it from memory.
///
/// Taking the key out of the map in the same step is what makes "shown once"
/// a property of the code rather than a promise in the UI.
#[tauri::command]
pub fn export_recovery_key(
    state: State<'_, std::sync::Arc<AppState>>,
    node_id: String,
    path: String,
) -> Reply<()> {
    let node = state
        .view_of(&node_id)
        .ok_or_else(|| format!("there is no node with id {node_id}"))?;
    let key = state
        .peek_recovery_key(&node_id)
        .ok_or_else(|| "that key has already been exported and is no longer held in memory".to_string())?;

    recovery::export(Path::new(&path), &node.node_name, &node.node_id, &key).map_err(fail)?;
    state.take_recovery_key(&node_id);
    Ok(())
}

/// Unlocks an encrypted node with a typed recovery key, for USB nodes and for
/// nodes created on a different machine.
#[tauri::command]
pub fn unlock_node(
    app: AppHandle,
    state: State<'_, std::sync::Arc<AppState>>,
    node_id: String,
    password: String,
) -> Reply<SupervisedNode> {
    // Parsed before anything is restarted, so a mistyped key produces "that is
    // not a valid recovery key" rather than a node that fails to start.
    recovery::decode(&password).map_err(fail)?;

    let mut handle_spec = {
        let nodes = state.nodes.lock().map_err(|_| "the node registry is unavailable".to_string())?;
        nodes
            .get(&node_id)
            .map(|handle| handle.spec.clone())
            .ok_or_else(|| format!("there is no node with id {node_id}"))?
    };
    handle_spec.unlock_secret = Some(password);

    state.forget_node(&node_id).map_err(fail)?;
    let view = state.start_node(handle_spec).map_err(fail)?;
    appstate::emit(&app, SidecarEvent::NodeState { node: view.clone() });
    Ok(view)
}

// -- shell / OS --------------------------------------------------------------

#[tauri::command]
pub async fn pick_directory(app: AppHandle, title: String) -> Reply<Option<String>> {
    let folder = app
        .dialog()
        .file()
        .set_title(&title)
        .blocking_pick_folder();
    Ok(folder.map(|path| path.to_string()))
}

#[tauri::command]
pub async fn pick_save_file(
    app: AppHandle,
    title: String,
    default_name: String,
) -> Reply<Option<String>> {
    let file = app
        .dialog()
        .file()
        .set_title(&title)
        .set_file_name(&default_name)
        .add_filter("Text", &["txt"])
        .blocking_save_file();
    Ok(file.map(|path| path.to_string()))
}

/// Opens a node's data directory in the platform's file manager.
///
/// Takes a node id, never a path: the directory is resolved server-side from
/// the sidecar's own registry and checked against the allowlisted roots (the
/// app data root plus every known node's data directory, which covers
/// removable nodes living outside the root). A raw frontend path is not
/// accepted -- collection names and keys arrive from the mesh, and a path
/// that reaches the opener must never be influenced by them.
///
/// Spawned directly rather than through the shell plugin, so there is no
/// general-purpose "open anything" capability granted to the web view.
#[tauri::command]
pub fn reveal_node_files(
    state: State<'_, std::sync::Arc<AppState>>,
    node_id: String,
) -> Reply<()> {
    // Clone out from under the lock first; canonicalization and the spawn
    // below must never run with the node-map mutex held.
    let (data_dir, approved): (Option<PathBuf>, Vec<PathBuf>) = {
        let nodes = state.nodes.lock().expect("node mutex");
        let dir = nodes
            .get(&node_id)
            .map(|handle| handle.spec.data_dir.clone());
        let mut dirs: Vec<PathBuf> = nodes.values().map(|handle| handle.spec.data_dir.clone()).collect();
        dirs.push(state.data_root.clone());
        (dir, dirs)
    };
    let Some(data_dir) = data_dir else {
        return Err(format!("there is no node {node_id} here any more"));
    };
    let target = std::fs::canonicalize(&data_dir)
        .map_err(|_| format!("{} is not there any more", data_dir.display()))?;
    let mut allowed = false;
    for root in &approved {
        if let Ok(canonical) = std::fs::canonicalize(root) {
            if path_within_root(&target, &canonical) {
                allowed = true;
                break;
            }
        }
    }
    if !allowed {
        return Err(format!(
            "{} is outside the folders this app manages, so it will not be opened",
            target.display()
        ));
    }

    #[cfg(target_os = "windows")]
    let result = std::process::Command::new("explorer").arg(&target).spawn();
    #[cfg(target_os = "macos")]
    let result = std::process::Command::new("open").arg(&target).spawn();
    #[cfg(all(unix, not(target_os = "macos")))]
    let result = std::process::Command::new("xdg-open").arg(&target).spawn();

    result
        .map(|_| ())
        .map_err(|error| format!("could not open {}: {error}", target.display()))
}

/// True when `target` is `root` itself or something underneath it. Both sides
/// must already be canonicalized (symlinks resolved, `..` eliminated):
/// `Path::starts_with` is lexical, so calling it on un-canonicalized input
/// would accept `root/node/../../etc`. Component-wise comparison also means a
/// sibling whose name merely shares a prefix (`node2` vs `node`) is rejected.
fn path_within_root(target: &Path, root: &Path) -> bool {
    target.starts_with(root)
}

#[tauri::command]
pub fn set_autostart(app: AppHandle, enabled: bool) -> Reply<()> {
    crate::set_autostart(&app, enabled)
}

#[tauri::command]
pub fn set_background_mode(state: State<'_, std::sync::Arc<AppState>>, enabled: bool) -> Reply<()> {
    state
        .background_mode
        .store(enabled, std::sync::atomic::Ordering::SeqCst);
    Ok(())
}

#[tauri::command]
pub fn notify(app: AppHandle, title: String, body: String) -> Reply<()> {
    use tauri_plugin_notification::NotificationExt;
    app.notification()
        .builder()
        .title(title)
        .body(body)
        .show()
        .map_err(fail)
}

// -- discovery ---------------------------------------------------------------

/// Returns all node directories the supervisor knows about that this app is
/// not yet managing. Safe to call at any time; an absent supervisor returns an
/// empty list rather than an error.
///
/// This is the pull-side complement to the watchdog's push: the frontend
/// calls this on boot and on volumes-changed / network-changed, so discoveries
/// arrive promptly instead of waiting for the 30-second watchdog pass.
#[tauri::command]
pub fn scan_for_nodes(state: State<'_, std::sync::Arc<AppState>>) -> Reply<Vec<DiscoveredCandidate>> {
    Ok(appstate::scan_for_candidates(&state))
}

/// Same as `scan_for_nodes`, but also emits a `NodesDiscovered` event to the
/// window so any open sidebar re-renders without the caller having to handle
/// the returned list. Used by power-change and volume-change hooks that need
/// to fire-and-forget.
#[tauri::command]
pub fn trigger_discovery_scan(
    app: AppHandle,
    state: State<'_, std::sync::Arc<AppState>>,
) -> Reply<()> {
    let candidates = appstate::scan_for_candidates(&state);
    appstate::emit(&app, SidecarEvent::NodesDiscovered { candidates });
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn allowlist_accepts_the_root_itself_and_children() {
        let root = PathBuf::from(if cfg!(windows) { r"C:\data\nodes" } else { "/data/nodes" });
        assert!(path_within_root(&root, &root));
        assert!(path_within_root(&root.join("node-a"), &root));
        assert!(path_within_root(&root.join("node-a").join("node.json"), &root));
    }

    #[test]
    fn allowlist_rejects_outside_and_prefix_siblings() {
        let root = PathBuf::from(if cfg!(windows) { r"C:\data\nodes" } else { "/data/nodes" });
        // Outside the root entirely.
        assert!(!path_within_root(
            &PathBuf::from(if cfg!(windows) { r"C:\Windows\System32" } else { "/etc" }),
            &root
        ));
        // A sibling whose name merely shares a prefix is NOT inside.
        assert!(!path_within_root(&root.with_extension("backup"), &root));
        // Lexical `..` must never reach the predicate: callers canonicalize
        // first, and an un-canonicalized traversal is rejected by failing
        // canonicalization, not by this check. Documented here so a future
        // caller does not "simplify" the canonicalize away.
        let traversal = root.join("node-a").join("..").join("..").join("etc");
        assert!(traversal.starts_with(&root), "lexical starts_with accepts ..; canonicalize first");
    }

    #[test]
    fn rollback_releases_ports_and_cleans_keychain_and_config() {
        let dir = std::env::temp_dir().join(format!("desentry-rollback-{}", std::process::id()));
        let _ = std::fs::create_dir_all(&dir);
        let state = AppState::new(PathBuf::from("desentryd"), dir, PathBuf::from("."));

        // Reserve a real allocation, then roll back a failure that happened
        // before any node started: ports must be handed back.
        let allocation = state.allocate_ports().expect("a free port pair exists");
        let config_path = state.data_root.join("node.json");
        std::fs::write(&config_path, "{}").expect("temp config writes");
        assert!(config_path.exists());

        let stored = if crate::keychain::available() {
            let reference = crate::keychain::reference_for("rollback-test");
            if crate::keychain::store(&reference, "secret").is_ok() {
                Some(reference)
            } else {
                None
            }
        } else {
            None
        };

        assert_eq!(state.reserved_count(), 2, "one pair is reserved before rollback");
        rollback_create(&state, allocation, None, stored.as_deref(), Some(&config_path));

        assert_eq!(state.reserved_count(), 0, "rollback hands the reservation back");
        assert!(!config_path.exists(), "half-written node.json is removed");
        if let Some(reference) = stored {
            assert!(
                matches!(crate::keychain::load(&reference), Err(crate::keychain::KeychainError::Missing(_))),
                "keychain entry from the failed attempt is gone"
            );
        }
        // A fresh allocation still succeeds (the range was not leaked dry).
        let again = state.allocate_ports().expect("ports were released by rollback");
        state.release_ports(again);
        let _ = std::fs::remove_dir_all(&state.data_root);
    }
}
