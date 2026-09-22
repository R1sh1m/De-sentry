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
use crate::passphrase;
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

/// The per-boot API bearer token (C-8): the webview attaches it as
/// `Authorization: Bearer` on every direct desentryd call. Empty when the
/// OS RNG was unavailable, in which case nodes run unauthenticated as before.
#[tauri::command]
pub fn api_token() -> Reply<String> {
    Ok(std::env::var("DESENTRY_API_TOKEN").unwrap_or_default())
}

/// Cluster-membership posture for pairing UX: whether the mesh is closed,
/// plus a fingerprint (first 8 hex of SHA-256 of the secret) so two machines
/// can confirm they will join the SAME mesh. The secret itself never leaves.
#[derive(Debug, Clone, serde::Serialize)]
pub struct MembershipStatus {
    pub closed: bool,
    pub fingerprint: String,
}

#[tauri::command]
pub fn membership_status() -> Reply<MembershipStatus> {
    let secret = std::env::var("DESENTRY_CLUSTER_SECRET").unwrap_or_default();
    if secret.is_empty() {
        return Ok(MembershipStatus { closed: false, fingerprint: String::new() });
    }
    use sha2::{Digest, Sha256};
    let mut hasher = Sha256::new();
    hasher.update(secret.as_bytes());
    let digest: [u8; 32] = hasher.finalize().into();
    let fingerprint: String = digest[..4].iter().map(|b| format!("{:02x}", b)).collect();
    Ok(MembershipStatus { closed: true, fingerprint })
}

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
    // Atomic rewrite (#7 fix): a crash between truncate and write must not
    // leave a half-written node.json that a later scan adopts as a node.
    let tmp = config_path.with_extension("json.tmp");
    std::fs::write(&tmp, body + "\n").map_err(fail)?;
    std::fs::rename(&tmp, config_path).map_err(fail)
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
    #[serde(default = "default_store_key_in_keychain")]
    pub store_key_in_keychain: bool,
    #[serde(default)]
    pub supervisor: bool,
    #[serde(default)]
    pub discovery_enabled: Option<bool>,
    #[serde(default)]
    pub removable: bool,
    #[serde(default)]
    pub bootstrap_peers: Vec<String>,
    #[serde(default)]
    pub description: String,
    #[serde(default)]
    pub preallocate: bool,
    /// "generated" (default) or "passphrase". Old frontends omit it.
    #[serde(default = "default_key_mode")]
    pub key_mode: String,
    /// User-chosen passphrase when key_mode == "passphrase".
    #[serde(default)]
    pub passphrase: Option<String>,
    /// Confirmation copy; must match `passphrase`.
    #[serde(default)]
    pub passphrase_confirm: Option<String>,
}

fn default_store_key_in_keychain() -> bool {
    true
}

fn default_key_mode() -> String {
    "generated".to_owned()
}

#[derive(Debug, Clone, Serialize)]
pub struct CreateNodeResult {
    pub node: SupervisedNode,
    pub recovery_key: Option<String>,
    pub keychain_ref: String,
    /// Echoes the key mode ("generated" or "passphrase") so the wizard can
    /// render the matching step-5 screen.
    pub key_mode: String,
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
                key_mode: "generated".to_owned(),
            });
    }

    let allocation = state.allocate_ports().map_err(fail)?;

    // Key mode: "generated" (recovery key) or "passphrase" (custom passphrase).
    // The node key exists from the first write rather than converted after.
    let key_mode = if request.encrypt_at_rest
        && request.key_mode.trim().eq_ignore_ascii_case("passphrase")
    {
        "passphrase"
    } else {
        "generated"
    };
    let wants_passphrase = request.encrypt_at_rest && key_mode == "passphrase";

    // For passphrase nodes: validate, derive KEK, mint a random DEK and wrap
    // it. The DEK (encoded) is the node's unlock secret; the passphrase itself
    // is never stored. For generated nodes: mint the recovery key directly.
    let passphrase_salt: Option<[u8; passphrase::SALT_BYTES]>;
    let passphrase_wrapped: Option<(String, String)>;
    let recovery_key: Option<String>;
    if wants_passphrase {
        let pw = request.passphrase.clone().unwrap_or_default();
        let confirm = request.passphrase_confirm.clone().unwrap_or_default();
        // Trim only leading/trailing whitespace: interior spaces are significant.
        let pw_trimmed = pw.trim().to_owned();
        let confirm_trimmed = confirm.trim().to_owned();
        passphrase::validate(&pw_trimmed, &confirm_trimmed).map_err(|e| match e {
            passphrase::PassphraseError::TooShort(n) => {
                format!("choose a passphrase with at least {n} characters, then confirm it matches")
            }
            passphrase::PassphraseError::Mismatch => {
                "the two passphrases do not match — retype both".to_owned()
            }
            other => other.to_string(),
        })?;
        let (score, _) = passphrase::strength(&pw_trimmed);
        if score <= 1 {
            log::warn!("weak custom passphrase accepted with explicit user ack");
        }
        let salt = passphrase::generate_salt().map_err(fail)?;
        let kek = passphrase::derive_kek(&pw_trimmed, &salt, passphrase::ITERS);
        let dek = passphrase::generate_dek().map_err(fail)?;
        let (wrapped, tag) = passphrase::wrap_dek(&dek, &kek, &salt);
        // Zeroize copies on the stack promptly (best-effort, no crate).
        let mut kek_zero = kek;
        for b in kek_zero.iter_mut() {
            *b = 0;
        }
        passphrase_salt = Some(salt);
        passphrase_wrapped = Some((wrapped, tag));
        // unlock_secret is the DEK in the same Crockford shape as a recovery
        // key, so the engine / keychain plumbing is unchanged.
        recovery_key = Some(recovery::encode(&dek));
        let mut dek_zero = dek;
        for b in dek_zero.iter_mut() {
            *b = 0;
        }
    } else {
        passphrase_salt = None;
        passphrase_wrapped = None;
        recovery_key = if request.encrypt_at_rest {
            Some(recovery::generate().map_err(fail)?)
        } else {
            None
        };
    }

    let (kdf_salt_hex, kdf_iters, dek_wrapped_hex, dek_tag_hex) = match (&passphrase_salt, &passphrase_wrapped) {
        (Some(salt), Some((wrapped, tag))) => (
            hex_of(salt),
            passphrase::ITERS,
            wrapped.clone(),
            tag.clone(),
        ),
        _ => (String::new(), 0, String::new(), String::new()),
    };

    let discovery_enabled = request.discovery_enabled.unwrap_or(!request.supervisor);
    let replication_factor = if !discovery_enabled {
        1
    } else {
        request.spec.replication_factor.max(1)
    };

    let config = NodeConfigSpec {
        data_dir: data_dir.clone(),
        node_name: request.node_name.trim().to_owned(),
        ports: allocation,
        supervisor: request.supervisor,
        discovery_enabled,
        quota_mb: request.quota_mb,
        quota_split: normalise_split(request.spec.quota_split),
        engines: request.spec.engines.clone(),
        default_engine: request.spec.default_engine.clone(),
        replication_factor,
        retention_days: request.spec.retention_days,
        encrypt_at_rest: request.encrypt_at_rest,
        // Filled in once the node reports its id: the keychain entry is named
        // after the node, and the node's identity does not exist until it has
        // generated one.
        keychain_ref: String::new(),
        bootstrap_peers: request.bootstrap_peers.clone(),
        advertise_hostname: hostname(),
        key_mode: key_mode.to_owned(),
        kdf_salt_hex,
        kdf_iters,
        dek_wrapped_hex,
        dek_tag_hex,
    };
    // Whether the data directory predates this attempt: rollback removes a
    // directory only when this attempt created it (#7 ghost-dir fix).
    let dir_preexisted = data_dir.exists();
    // If upfront space reservation was requested, allocate storage.reserved now.
    let reserved_path = if request.preallocate && request.quota_mb > 0 {
        let path = data_dir.join("storage.reserved");
        if let Err(error) = preallocate_reservation_file(&path, request.quota_mb * 1024 * 1024) {
            rollback_create(
                &state,
                allocation,
                None,
                None,
                None,
                None,
                &data_dir,
                dir_preexisted,
            );
            return Err(format!("could not pre-allocate storage reservation file: {error}"));
        }
        Some(path)
    } else {
        None
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
            rollback_create(
                &state,
                allocation,
                None,
                None,
                None,
                reserved_path.as_deref(),
                &data_dir,
                dir_preexisted,
            );
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
            rollback_create(
                &state,
                allocation,
                None,
                None,
                Some(&config_path),
                reserved_path.as_deref(),
                &data_dir,
                dir_preexisted,
            );
            return Err(fail(error));
        }
    };

    // Now that the node has an identity, the key gets a home.
    //
    // Removable nodes deliberately get none: a stick that unlocks from this
    // machine's keychain is a stick that cannot be read on any other machine,
    // which defeats the point of putting a node on a stick. Those unlock from
    // the recovery key or passphrase the user keeps.
    //
    // Passphrase nodes return recovery_key: None — the user already knows the
    // secret, and showing the wrapped DEK would present two secrets for one
    // node. The DEK itself still reaches the child as unlock_secret and the
    // keychain (when requested) exactly like a generated key.
    let mut keychain_ref = String::new();
    if let Some(key) = recovery_key.as_ref() {
        if !request.removable && request.store_key_in_keychain {
            keychain_ref = keychain::reference_for(&node.node_id);
            if let Err(error) = keychain::store(&keychain_ref, key) {
                let message = fail(error);
                rollback_create(
                    &state,
                    allocation,
                    Some(&node.node_id),
                    None,
                    Some(&config_path),
                    reserved_path.as_deref(),
                    &data_dir,
                    dir_preexisted,
                );
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
                    reserved_path.as_deref(),
                    &data_dir,
                    dir_preexisted,
                );
                return Err(message);
            }
        }
        // Only generated keys are held for one-time export. A passphrase is
        // already in the user's memory; holding the DEK for display would
        // defeat the choice they just made.
        if key_mode == "generated" {
            state.hold_recovery_key(&node.node_id, key.clone());
        }
    }

    // The sizing decision travels with the node, so a stick carried to another
    // machine says why it is shaped the way it is.
    let manifest = serde_json::json!({
        "version": 2,
        "node_id": node.node_id,
        "node_name": node.node_name,
        "created_ms": crate::nodes::now_ms(),
        "encrypted": request.encrypt_at_rest,
        "key_mode": key_mode,
        "removable": request.removable,
        "quota_mb": request.quota_mb,
        "preallocate": request.preallocate,
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

    // Passphrase mode: the DEK served as unlock_secret + keychain, but the
    // wizard must not display it. Return None so step 5 renders the
    // "passphrase set" screen instead of the recovery-key screen.
    let shown_key = if key_mode == "passphrase" {
        None
    } else {
        recovery_key
    };
    Ok(CreateNodeResult {
        node,
        recovery_key: shown_key,
        keychain_ref,
        key_mode: key_mode.to_owned(),
    })
}

fn normalise_split(split: QuotaSplit) -> QuotaSplit {
    split.normalised()
}

fn hex_of(bytes: &[u8]) -> String {
    const H: &[u8] = b"0123456789abcdef";
    let mut s = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        s.push(H[(b >> 4) as usize] as char);
        s.push(H[(b & 0xf) as usize] as char);
    }
    s
}

fn hex_to_bytes(s: &str) -> Result<Vec<u8>, ()> {
    if s.len() % 2 != 0 {
        return Err(());
    }
    let bytes = s.as_bytes();
    let val = |c: u8| match c {
        b'0'..=b'9' => Ok(c - b'0'),
        b'a'..=b'f' => Ok(c - b'a' + 10),
        b'A'..=b'F' => Ok(c - b'A' + 10),
        _ => Err(()),
    };
    let mut out = Vec::with_capacity(s.len() / 2);
    for pair in bytes.chunks_exact(2) {
        out.push((val(pair[0])? << 4) | val(pair[1])?);
    }
    Ok(out)
}

/// Creates and resizes a reservation file to guarantee space upfront.
fn preallocate_reservation_file(path: &Path, bytes: u64) -> std::io::Result<()> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let file = std::fs::OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .open(path)?;
    file.set_len(bytes)?;
    file.sync_all()?;
    Ok(())
}

/// Dynamically adjusts the reservation file so that
/// `reserved_file_size + used_bytes == quota_bytes`.
#[tauri::command]
pub fn sync_storage_reservation(data_dir: String, used_bytes: u64, quota_mb: u64) -> Reply<bool> {
    if quota_mb == 0 {
        return Ok(false);
    }
    let path = PathBuf::from(data_dir).join("storage.reserved");
    if !path.exists() {
        return Ok(false);
    }
    let quota_bytes = quota_mb * 1024 * 1024;
    let target = quota_bytes.saturating_sub(used_bytes);
    if target == 0 {
        let _ = std::fs::remove_file(&path);
        return Ok(true);
    }
    if let Ok(file) = std::fs::OpenOptions::new().write(true).open(&path) {
        let _ = file.set_len(target);
        return Ok(true);
    }
    Ok(false)
}

/// Undoes a half-finished `create_node`: stops the child (if started),
/// releases the port reservation, forgets a keychain entry this attempt
/// stored, and removes the node.json this attempt wrote so a later directory
/// scan does not adopt a half-created node.
///
/// Only artifacts named here are touched, as is any key the window already
/// held. Cleanup failures are logged and ignored -- the caller reports the
/// original error.
///
/// Ghost-directory rule (#7 fix): files this attempt created (node.json,
/// storage.reserved, manifest.json) are always removed. The data directory
/// itself is removed only when it did not exist before this attempt AND is
/// empty afterwards -- a user-chosen directory that already held files is
/// never deleted, but a directory we created for a failed attempt does not
/// linger as a ghost.
fn rollback_create(
    state: &AppState,
    allocation: PortAllocation,
    node_id: Option<&str>,
    keychain_ref: Option<&str>,
    config_path: Option<&Path>,
    reserved_path: Option<&Path>,
    data_dir: &Path,
    dir_preexisted: bool,
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
    if let Some(path) = reserved_path {
        let _ = std::fs::remove_file(path);
    }
    let _ = std::fs::remove_file(data_dir.join("manifest.json"));
    if !dir_preexisted {
        // Only an empty directory we created: remove_dir fails on non-empty,
        // which is exactly the guard for engine files a started child wrote
        // or user files that appeared concurrently.
        if let Err(error) = std::fs::remove_dir(data_dir) {
            log::warn!("rollback: could not remove new data dir {}: {error}", data_dir.display());
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

/// Resolves a typed secret to the node's unlock secret (DEK-encoded).
///
/// Generated nodes: the typed text must decode as a recovery key and is used
/// directly. Passphrase nodes: the typed text derives a KEK with the stored
/// salt, which must unwrap the stored DEK (HMAC-verified). A single error is
/// returned for both paths so a stranger learns nothing about which mode the
/// node uses.
fn resolve_unlock_secret(data_dir: &Path, typed: &str) -> Reply<String> {
    let secret = typed.trim();
    if secret.is_empty() {
        return Err("enter the recovery key or passphrase for this node".to_owned());
    }
    // Envelope first: passphrase-mode nodes accept ONLY the passphrase. This
    // is what makes rotation and generated→passphrase migration actually
    // invalidate the old secret — a pasted DEK or old recovery key must not
    // keep working after the wrap changes. Generated nodes (or legacy configs
    // without an envelope) fall through to the decode path below.
    if let Some(envelope) = configgen::read_envelope(data_dir) {
        if envelope.key_mode == "passphrase" && !envelope.dek_wrapped_hex.is_empty() {
            if envelope.kdf_salt_hex.is_empty() {
                return Err("this node's key envelope is corrupt — restore from backup".to_owned());
            }
            let salt = hex_to_bytes(&envelope.kdf_salt_hex)
                .map_err(|_| "that passphrase did not unlock this node".to_owned())?;
            let iters = if envelope.kdf_iters > 0 {
                envelope.kdf_iters
            } else {
                passphrase::ITERS
            };
            let kek = passphrase::derive_kek(secret, &salt, iters);
            return match passphrase::unwrap_dek(
                &envelope.dek_wrapped_hex,
                &envelope.dek_tag_hex,
                &kek,
                &salt,
            ) {
                Ok(dek) => Ok(recovery::encode(&dek)),
                Err(_) => Err(
                    "that passphrase did not unlock this node — check for typos".to_owned(),
                ),
            };
        }
    }
    // Generated-key path.
    if recovery::decode(secret).is_ok() {
        return Ok(secret.to_owned());
    }
    Err("that is not a valid recovery key for this node — check for typos".to_owned())
}

/// Unlocks an encrypted node with its recovery key or passphrase, for USB
/// nodes and for nodes created on a different machine.
#[tauri::command]
pub fn unlock_node(
    app: AppHandle,
    state: State<'_, std::sync::Arc<AppState>>,
    node_id: String,
    password: String,
    data_dir: Option<String>,
) -> Reply<SupervisedNode> {
    // Resolve the data directory first (no lock held across file IO): for a
    // supervised node it comes from the registry, otherwise from the caller
    // or the discovery scan. The secret is verified against the envelope
    // before anything is restarted, so a mistype never produces a node that
    // fails to start.
    let (existing_spec, known_dir) = {
        let nodes = state.nodes.lock().map_err(|_| "the node registry is unavailable".to_string())?;
        let spec = nodes.get(&node_id).map(|handle| handle.spec.clone());
        let dir = spec.as_ref().map(|s| s.data_dir.clone());
        (spec, dir)
    };

    let spec = if let Some(mut handle_spec) = existing_spec {
        let dir = known_dir.clone().unwrap_or_else(|| PathBuf::from(&handle_spec.data_dir));
        let secret = resolve_unlock_secret(&dir, &password)?;
        handle_spec.unlock_secret = Some(secret);
        let _ = state.forget_node(&node_id);
        handle_spec
    } else {
        // Node is not currently supervised. Find its data directory.
        let target_dir = if let Some(dir_str) = data_dir.filter(|s| !s.is_empty()) {
            PathBuf::from(dir_str)
        } else {
            let candidates = appstate::scan_for_candidates(&state);
            candidates
                .into_iter()
                .find(|c| c.node_id == node_id)
                .map(|c| PathBuf::from(c.path))
                .ok_or_else(|| format!("there is no node with id {node_id}"))?
        };

        let config_path = target_dir.join("node.json");
        if !config_path.exists() {
            return Err(format!("{} has no node.json to adopt or unlock", target_dir.display()));
        }

        let node_name = configgen::read_node_name(&target_dir).unwrap_or_else(|| {
            target_dir
                .file_name()
                .map(|name| name.to_string_lossy().into_owned())
                .unwrap_or_else(|| "node".to_owned())
        });

        let allocation = state.allocate_ports().map_err(fail)?;
        let existing: serde_json::Value = std::fs::read_to_string(&config_path)
            .ok()
            .and_then(|text| serde_json::from_str(&text).ok())
            .unwrap_or_else(|| serde_json::json!({}));

        if let Err(error) = rewrite_ports(&config_path, &allocation) {
            state.release_ports(allocation);
            return Err(error);
        }

        // Verify the typed secret against the envelope before binding ports.
        let secret = resolve_unlock_secret(&target_dir, &password)?;

        LaunchSpec {
            node_name,
            data_dir: target_dir.clone(),
            config_path,
            api_port: allocation.api_port,
            p2p_port: allocation.p2p_port,
            discovery_port: allocation.discovery_port,
            supervisor: existing.get("supervisor").and_then(|v| v.as_bool()).unwrap_or(false),
            removable: false,
            encrypted: true,
            unlock_secret: Some(secret.clone()),
        }
    };

    let node_secret = spec.unlock_secret.clone().unwrap_or_default();
    let view = state.start_node(spec).map_err(fail)?;

    let config_path = PathBuf::from(&view.data_dir).join("node.json");
    // Preserve the node's storage choice. A node created without a keychain
    // reference must continue requiring its secret after restarts. The stored
    // value is always the DEK-encoded secret, never the raw passphrase, so a
    // passphrase unlock and a recovery-key unlock converge on the same entry.
    let keychain_ref = std::fs::read_to_string(&config_path)
        .ok()
        .and_then(|text| serde_json::from_str::<serde_json::Value>(&text).ok())
        .and_then(|value| {
            value
                .get("keychain_ref")
                .and_then(|reference| reference.as_str())
                .filter(|reference| !reference.is_empty())
                .map(str::to_owned)
        });
    if let Some(keychain_ref) = keychain_ref {
        if let Err(error) = keychain::store(&keychain_ref, &node_secret) {
            log::warn!("could not refresh keychain entry {keychain_ref}: {error}");
        }
    }

    appstate::emit(&app, SidecarEvent::NodeState { node: view.clone() });
    Ok(view)
}

/// Strength of a candidate passphrase for the wizard meter (no secret leaves
/// the machine; this only scores what the window already holds).
#[tauri::command]
pub fn passphrase_strength(passphrase: String) -> Reply<PassphraseStrength> {
    let (score, label) = passphrase::strength(&passphrase);
    Ok(PassphraseStrength {
        score,
        label: label.to_owned(),
        min_len: passphrase::MIN_LEN,
    })
}

#[derive(Debug, Clone, Serialize)]
pub struct PassphraseStrength {
    pub score: u8,
    pub label: String,
    pub min_len: usize,
}

/// Rotates a passphrase-wrapped DEK: verifies the old secret, re-wraps the
/// same DEK under a fresh salt, updates node.json + keychain. The old secret
/// stops working; no data is re-encrypted. Generated-key nodes can migrate to
/// a passphrase this way (old = recovery key); passphrase nodes can rotate
/// (old = previous passphrase).
#[tauri::command]
pub fn change_passphrase(
    state: State<'_, std::sync::Arc<AppState>>,
    node_id: String,
    old_secret: String,
    new_passphrase: String,
    new_confirm: String,
) -> Reply<()> {
    let data_dir = {
        let nodes = state.nodes.lock().map_err(|_| "the node registry is unavailable".to_string())?;
        nodes
            .get(&node_id)
            .map(|handle| handle.spec.data_dir.clone())
            .or_else(|| {
                appstate::scan_for_candidates(&state)
                    .into_iter()
                    .find(|c| c.node_id == node_id)
                    .map(|c| PathBuf::from(c.path))
            })
            .ok_or_else(|| format!("there is no node with id {node_id}"))?
    };

    let new_trimmed = new_passphrase.trim().to_owned();
    let confirm_trimmed = new_confirm.trim().to_owned();
    passphrase::validate(&new_trimmed, &confirm_trimmed).map_err(|e| match e {
        passphrase::PassphraseError::TooShort(n) => {
            format!("choose a passphrase with at least {n} characters, then confirm it matches")
        }
        passphrase::PassphraseError::Mismatch => {
            "the two new passphrases do not match — retype both".to_owned()
        }
        other => other.to_string(),
    })?;

    // Verify the old secret. Passphrase-mode nodes require the old *passphrase*
    // (exclusively — a pasted DEK must not rotate the secret, which is what
    // makes rotation actually invalidate the old value). Generated nodes
    // migrating accept the old recovery key, which IS the DEK being wrapped.
    let current_dek: [u8; passphrase::KEY_BYTES] = {
        let typed = old_secret.trim();
        if typed.is_empty() {
            return Err("enter the current recovery key or passphrase first".to_owned());
        }
        let envelope = configgen::read_envelope(&data_dir);
        let is_passphrase_node = envelope.as_ref().is_some_and(|env| {
            env.key_mode == "passphrase" && !env.dek_wrapped_hex.is_empty()
        });
        if is_passphrase_node {
            let env = envelope.expect("checked above");
            let salt = hex_to_bytes(&env.kdf_salt_hex)
                .map_err(|_| "that current passphrase is not valid for this node".to_owned())?;
            let iters = if env.kdf_iters > 0 { env.kdf_iters } else { passphrase::ITERS };
            let kek = passphrase::derive_kek(typed, &salt, iters);
            passphrase::unwrap_dek(&env.dek_wrapped_hex, &env.dek_tag_hex, &kek, &salt)
                .map_err(|_| "that current passphrase is not valid for this node".to_owned())?
        } else if let Ok(bytes) = recovery::decode(typed) {
            if bytes.len() != passphrase::KEY_BYTES {
                return Err("that current secret is not valid for this node".to_owned());
            }
            let mut dek = [0u8; passphrase::KEY_BYTES];
            dek.copy_from_slice(&bytes);
            dek
        } else {
            return Err("that current secret is not valid for this node".to_owned());
        }
    };

    // Re-wrap the same DEK under a fresh salt.
    let new_salt = passphrase::generate_salt().map_err(fail)?;
    let new_kek = passphrase::derive_kek(&new_trimmed, &new_salt, passphrase::ITERS);
    let (wrapped, tag) = passphrase::wrap_dek(&current_dek, &new_kek, &new_salt);
    let config_path = data_dir.join("node.json");
    configgen::rewrite_envelope(
        &config_path,
        "passphrase",
        passphrase::ITERS,
        &hex_of(&new_salt),
        &wrapped,
        &tag,
    )
    .map_err(|e| format!("could not update {}: {e}", config_path.display()))?;

    // Refresh the keychain entry to the same DEK (unchanged value, still the
    // unlock secret) so auto-unlock keeps working after rotation.
    if let Ok(text) = std::fs::read_to_string(&config_path) {
        if let Ok(value) = serde_json::from_str::<serde_json::Value>(&text) {
            if let Some(reference) = value
                .get("keychain_ref")
                .and_then(|r| r.as_str())
                .filter(|r| !r.is_empty())
            {
                let _ = keychain::store(reference, &recovery::encode(&current_dek));
            }
        }
    }
    log::info!("passphrase rotated for node {node_id}");
    Ok(())
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
    #[serial_test::serial]
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
        let reserved_path = state.data_root.join(".reserved");
        std::fs::write(&reserved_path, "{}").expect("temp reserved writes");
        assert!(reserved_path.exists());

        // Unique ref: the OS keychain is shared across test threads and
        // processes — a fixed ref races with concurrent runs (see keychain.rs).
        let stored = if crate::keychain::available() {
            let reference = crate::keychain::unique_test_ref("rollback");
            if crate::keychain::store(&reference, "secret").is_ok() {
                Some(reference)
            } else {
                None
            }
        } else {
            None
        };

        assert_eq!(state.reserved_count(), 2, "one pair is reserved before rollback");
        rollback_create(
            &state,
            allocation,
            None,
            stored.as_deref(),
            Some(&config_path),
            Some(&reserved_path),
            &state.data_root,
            true, // data_root predates the attempt: never delete it here
        );

        assert_eq!(state.reserved_count(), 0, "rollback hands the reservation back");
        assert!(!config_path.exists(), "half-written node.json is removed");
        assert!(!reserved_path.exists(), "half-written reservation file is removed");
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

    /// The exact production unlock dispatch, headless: generated-key nodes
    /// resolve recovery text directly; passphrase nodes derive + unwrap to
    /// the DEK and reject everything else (including a valid-shaped key that
    /// is not the passphrase -- the fail-closed rotation semantics).
    #[test]
    fn unlock_resolution_accepts_passphrase_and_rejects_the_rest() {
        static NONCE: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let n = NONCE.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
        let dir = std::env::temp_dir().join(format!("desentry-unlock-{}-{n}", std::process::id()));
        let _ = std::fs::create_dir_all(&dir);
        std::fs::write(dir.join("node.json"), r#"{"node_name":"t"}"#).expect("node.json writes");

        // Generated mode (no envelope): recovery-shaped text resolves as-is.
        let key = crate::recovery::generate().expect("OS entropy");
        assert_eq!(super::resolve_unlock_secret(&dir, &key).unwrap(), key);
        assert!(super::resolve_unlock_secret(&dir, "not a valid key!!").is_err());
        assert!(super::resolve_unlock_secret(&dir, "   ").is_err());

        // Passphrase mode: wrap a DEK under a KDF salt, store the envelope.
        // Reduced iterations: KDF correctness is iters-agnostic (covered in
        // passphrase.rs) and debug-mode PBKDF2-210k costs ~15s per derive;
        // the dispatch logic under test does not depend on the count.
        let dek = crate::passphrase::generate_dek().expect("OS entropy");
        let salt = crate::passphrase::generate_salt().expect("OS entropy");
        let pw = "a long enough test phrase!";
        let kek = crate::passphrase::derive_kek(pw, &salt, 2000);
        let (wrapped, tag) = crate::passphrase::wrap_dek(&dek, &kek, &salt);
        let salt_hex: String = salt.iter().map(|b| format!("{b:02x}")).collect();
        crate::configgen::rewrite_envelope(
            &dir.join("node.json"),
            "passphrase",
            2000,
            &salt_hex,
            &wrapped,
            &tag,
        )
        .expect("envelope writes");

        // The passphrase resolves to the DEK in recovery-key shape (what the
        // engine receives on stdin); surrounding whitespace is tolerated.
        assert_eq!(
            super::resolve_unlock_secret(&dir, &format!("  {pw}  ")).unwrap(),
            crate::recovery::encode(&dek)
        );
        // A valid-shaped recovery key is NOT the passphrase: rejected.
        assert!(super::resolve_unlock_secret(&dir, &key).is_err());
        // A wrong passphrase fails with no oracle detail.
        assert!(super::resolve_unlock_secret(&dir, "a totally different phrase here").is_err());

        let _ = std::fs::remove_dir_all(&dir);
    }
}
