//! De-Sentry control room -- the desktop shell around the `desentryd` engine.
//!
//! Startup, in order:
//!
//!   1. Resolve the bundled `desentryd` and the app's resource directory.
//!   2. Bring up the app-local **supervisor** node: an ordinary `desentryd`
//!      started with `"supervisor": true`, bound to loopback, holding no data
//!      and never elected. It is the control plane the window drives --
//!      hardware discovery, node lifecycle, quota checks, the quorum-gated
//!      checkpoint. The data plane underneath stays flat and leaderless; if
//!      every supervisor on the network is down, writes, reads and replication
//!      carry on unchanged.
//!   3. Restart the data nodes this app already manages.
//!   4. Load the sizing model (or record why it could not be loaded).
//!   5. Start the watchdog, the power/volume watcher, and the tray.
//!
//! Nothing here reaches the network. Every port bound is loopback or the LAN
//! the engine itself uses, and no code path in the app makes an outbound
//! request -- which is what lets the airplane-mode acceptance test cover the
//! app as well as the engine.

pub mod ai;
pub mod appstate;
pub mod commands;
pub mod configgen;
pub mod http;
pub mod keychain;
pub mod nodes;
pub mod notify;
pub mod ports;
pub mod power;
pub mod recovery;
pub mod tray;

use std::path::{Path, PathBuf};
use std::sync::atomic::Ordering;
use std::sync::Arc;
use std::time::Duration;

use tauri::{AppHandle, Manager, RunEvent, WindowEvent};
use tauri_plugin_autostart::{MacosLauncher, ManagerExt};

use appstate::AppState;
use configgen::{NodeConfigSpec, QuotaSplit};
use nodes::LaunchSpec;

/// Directory name for the app's own supervisor node.
const SUPERVISOR_DIR: &str = "supervisor";
/// The supervisor holds no replicated data, so its budget only has to cover
/// its own ledger and registry. 256 MiB is generous for that and small enough
/// that it never competes with the data nodes for disk.
const SUPERVISOR_QUOTA_MB: u64 = 256;

/// Resolves the bundled `desentryd`.
///
/// In a packaged app Tauri places sidecar binaries next to the executable with
/// the target triple appended. In development the CMake build output is used
/// instead, so `tauri dev` works against a locally built engine without
/// copying anything.
fn resolve_engine(app: &AppHandle) -> Result<PathBuf, String> {
    let exe_name = if cfg!(windows) { "desentryd.exe" } else { "desentryd" };

    // Packaged: alongside the app binary.
    if let Ok(dir) = std::env::current_exe().and_then(|exe| {
        exe.parent()
            .map(Path::to_path_buf)
            .ok_or_else(|| std::io::Error::other("no parent directory"))
    }) {
        let candidate = dir.join(exe_name);
        if candidate.exists() {
            return Ok(candidate);
        }
    }

    // Packaged, as a named resource.
    if let Ok(resource) = app.path().resource_dir() {
        let candidate = resource.join(exe_name);
        if candidate.exists() {
            return Ok(candidate);
        }
    }

    // Development: the usual CMake output locations, relative to src-tauri.
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    for relative in [
        "../../build/desentryd",
        "../../build/Debug/desentryd.exe",
        "../../build/Release/desentryd.exe",
        "../../build/RelWithDebInfo/desentryd.exe",
        "../../build/desentryd.exe",
    ] {
        let candidate = manifest.join(relative);
        if candidate.exists() {
            return Ok(candidate);
        }
    }

    Err(format!(
        "the {exe_name} engine binary was not found next to the app or in ../../build. \
         Build it with `cmake --build build --config RelWithDebInfo`, or reinstall the app."
    ))
}

/// The engine's own version string, for the About line and for spotting a
/// mismatch between a bundled app and a hand-built engine.
pub fn engine_version(binary: &Path) -> String {
    std::process::Command::new(binary)
        .arg("--version")
        .output()
        .ok()
        .map(|out| String::from_utf8_lossy(&out.stdout).trim().to_owned())
        .filter(|version| !version.is_empty())
        .unwrap_or_else(|| "unknown".to_owned())
}

/// Where nodes live by default: a per-user application data directory.
fn default_data_root(app: &AppHandle) -> PathBuf {
    app.path()
        .app_data_dir()
        .unwrap_or_else(|_| PathBuf::from("."))
        .join("nodes")
}

// -- autostart ---------------------------------------------------------------

pub fn autostart_enabled(app: &AppHandle) -> bool {
    app.autolaunch().is_enabled().unwrap_or(false)
}

pub fn set_autostart(app: &AppHandle, enabled: bool) -> Result<(), String> {
    let manager = app.autolaunch();
    if enabled {
        manager.enable().map_err(|error| error.to_string())
    } else {
        manager.disable().map_err(|error| error.to_string())
    }
}

// -- startup -----------------------------------------------------------------

/// Starts the app's own supervisor, creating its data directory on first run.
fn start_supervisor(state: &AppState) -> Result<(), String> {
    let data_dir = state.data_root.join(SUPERVISOR_DIR);
    let allocation = state.allocate_ports().map_err(|error| error.to_string())?;

    // The config is rewritten on every launch rather than reused. Ports change
    // between runs, and a supervisor is pure control plane -- there is nothing
    // in its config worth preserving across restarts except the data directory
    // itself.
    let config = NodeConfigSpec {
        data_dir: data_dir.clone(),
        node_name: "supervisor".to_owned(),
        ports: allocation,
        supervisor: true,
        quota_mb: SUPERVISOR_QUOTA_MB,
        quota_split: QuotaSplit::default(),
        engines: vec!["kv".to_owned()],
        default_engine: "kv".to_owned(),
        replication_factor: 3,
        retention_days: 0,
        encrypt_at_rest: false,
        keychain_ref: String::new(),
        bootstrap_peers: Vec::new(),
        advertise_hostname: String::new(),
    };
    let config_path = config
        .write()
        .map_err(|error| format!("could not write the supervisor's config: {error}"))?;

    state
        .start_node(LaunchSpec {
            node_name: "supervisor".to_owned(),
            data_dir,
            config_path,
            api_port: allocation.api_port,
            p2p_port: allocation.p2p_port,
            discovery_port: allocation.discovery_port,
            supervisor: true,
            removable: false,
            encrypted: false,
            unlock_secret: None,
        })
        .map(|_| ())
        .map_err(|error| error.to_string())
}

/// Restarts every data node found under the data root.
///
/// A directory with a node.json is a node this app created; starting them all
/// is what makes "close the laptop, open it tomorrow, everything is where you
/// left it" true. Failures are collected rather than fatal: one node with a
/// corrupt data file must not stop the other nine from coming up.
fn restore_nodes(app: &AppHandle, state: &Arc<AppState>) -> Vec<String> {
    let mut problems = Vec::new();
    let Ok(entries) = std::fs::read_dir(&state.data_root) else {
        return problems;
    };

    for entry in entries.flatten() {
        let dir = entry.path();
        if !dir.is_dir() || dir.file_name().map(|n| n == SUPERVISOR_DIR).unwrap_or(false) {
            continue;
        }
        if !dir.join("node.json").exists() {
            continue;
        }

        let path = dir.to_string_lossy().into_owned();
        // Reuses the same adoption path the window uses, so a node restored at
        // startup and one adopted by hand behave identically.
        match commands::start_existing_node(app.clone(), app.state(), path.clone()) {
            Ok(_) => {}
            Err(error) => problems.push(format!("{path}: {error}")),
        }
    }
    problems
}

/// Loads the sizing model. Never fatal -- the keyword fallback always exists.
fn load_sizer(state: &AppState) {
    let resources = ai::resources_from(&state.resource_dir);
    match ai::Sizer::load(&resources) {
        Ok(sizer) => {
            if !sizer.model_ready() {
                log::warn!(
                    "the workload sizing model is not available; the deterministic keyword \
                     fallback will be used and the wizard will say so"
                );
            }
            ai::install(sizer);
        }
        Err(error) => {
            // The prototypes missing is a packaging fault, not a runtime state.
            // Sizing then refuses rather than guessing, and the wizard sends
            // the user to the manual engine picker.
            log::error!("workload sizing is unavailable: {error}");
        }
    }
}

/// Keeps the tray's status line current.
fn start_tray_updater(app: AppHandle, state: Arc<AppState>) {
    std::thread::spawn(move || loop {
        std::thread::sleep(Duration::from_secs(2));
        if state.shutting_down.load(Ordering::SeqCst) {
            return;
        }
        let views = state.views();
        let running = views
            .iter()
            .filter(|node| matches!(node.process, nodes::ProcessState::Running))
            .count();
        tray::update_status(
            &app,
            running,
            views.len(),
            state.on_battery.load(Ordering::SeqCst),
        );
    });
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_notification::init())
        .plugin(tauri_plugin_autostart::init(
            MacosLauncher::LaunchAgent,
            // No arguments: the app decides for itself whether to open a window
            // based on whether it was launched at login, rather than trusting a
            // flag that a user could also pass by hand.
            None,
        ))
        .invoke_handler(tauri::generate_handler![
            commands::app_info,
            commands::list_nodes,
            commands::allocate_ports,
            commands::node_logs,
            commands::stop_node,
            commands::restart_node,
            commands::forget_node,
            commands::start_existing_node,
            commands::create_node,
            commands::size_workload,
            commands::available_engines,
            commands::pending_recovery_key,
            commands::export_recovery_key,
            commands::unlock_node,
            commands::pick_directory,
            commands::pick_save_file,
            commands::reveal_path,
            commands::set_autostart,
            commands::set_background_mode,
            commands::notify,
        ])
        .setup(|app| {
            let handle = app.handle().clone();

            let binary = resolve_engine(&handle)?;
            let data_root = default_data_root(&handle);
            std::fs::create_dir_all(&data_root)?;
            let resource_dir = handle.path().resource_dir().unwrap_or_else(|_| {
                // Development: resources sit in app/resources rather than in a
                // bundle.
                PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("..")
            });

            let state = Arc::new(AppState::new(binary, data_root, resource_dir));
            handle.manage(Arc::clone(&state));

            load_sizer(&state);

            // The supervisor comes up first: the window's whole sidebar reads
            // from it, and a node started before it would not be in its
            // registry.
            if let Err(error) = start_supervisor(&state) {
                log::error!("the supervisor did not start: {error}");
            }

            let problems = restore_nodes(&handle, &state);
            for problem in &problems {
                log::warn!("a node did not restart: {problem}");
            }

            appstate::start_watchdog(handle.clone(), Arc::clone(&state));
            power::start(handle.clone(), Arc::clone(&state));
            start_tray_updater(handle.clone(), Arc::clone(&state));
            tray::build(&handle, Arc::clone(&state))?;

            Ok(())
        })
        .on_window_event(|window, event| {
            if let WindowEvent::CloseRequested { api, .. } = event {
                let state = appstate::from(window.app_handle());
                if state.background_mode.load(Ordering::SeqCst) {
                    // Hide rather than exit: the nodes keep syncing, which is
                    // the entire reason background mode exists.
                    api.prevent_close();
                    let _ = window.hide();
                }
            }
        })
        .build(tauri::generate_context!())
        .expect("the app could not be built")
        .run(|app, event| {
            if let RunEvent::ExitRequested { .. } | RunEvent::Exit = event {
                // Nodes are stopped here rather than left to the OS: a parent
                // exiting does not reap its children on any of the three
                // platforms, and orphans would hold their ports into the next
                // launch.
                let state = appstate::from(app);
                appstate::shutdown(&state);
            }
        });
}
