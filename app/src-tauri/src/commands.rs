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
use tauri::{AppHandle, Manager, State};
use tauri_plugin_dialog::DialogExt;

use crate::ai::{self, NodeSpec};
use crate::appstate::{self, AppState, SidecarEvent};
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
    // rather than the node failing with a decryption error.
    let unlock_secret = if encrypted && !keychain_ref.is_empty() {
        match keychain::load(&keychain_ref) {
            Ok(key) => Some(key),
            Err(error) => return Err(error.to_string()),
        }
    } else {
        None
    };

    rewrite_ports(&config_path, &allocation)?;

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

    let view = state.start_node(spec).map_err(fail)?;
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
    let config_path = config.write().map_err(|error| {
        format!("could not write {}: {error}", data_dir.join("node.json").display())
    })?;

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

    let node = state.start_node(spec).map_err(fail)?;

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
            keychain::store(&keychain_ref, key).map_err(fail)?;
            let mut updated = config.clone();
            updated.keychain_ref = keychain_ref.clone();
            updated.write().map_err(fail)?;
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

/// Opens a folder in the platform's file manager.
///
/// Spawned directly rather than through the shell plugin, so there is no
/// general-purpose "open anything" capability granted to the web view -- the
/// only path this can ever open is one the sidecar already knows about.
#[tauri::command]
pub fn reveal_path(path: String) -> Reply<()> {
    let target = PathBuf::from(&path);
    if !target.exists() {
        return Err(format!("{path} is not there any more"));
    }

    #[cfg(target_os = "windows")]
    let result = std::process::Command::new("explorer").arg(&target).spawn();
    #[cfg(target_os = "macos")]
    let result = std::process::Command::new("open").arg(&target).spawn();
    #[cfg(all(unix, not(target_os = "macos")))]
    let result = std::process::Command::new("xdg-open").arg(&target).spawn();

    result
        .map(|_| ())
        .map_err(|error| format!("could not open {path}: {error}"))
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
