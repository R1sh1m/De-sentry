//! Per-node passwords.
//!
//! What a password does, stated exactly, because the previous UI overstated it:
//!
//!   * A password-protected node does not start until someone proves they know
//!     the password -- or holds the node's recovery key. The app verifies the
//!     secret against an Argon2id verifier stored in `<data_dir>/security.json`
//!     and refuses to launch the node otherwise.
//!   * "Remember on this device" stores the password in the OS keychain, so the
//!     node starts automatically on this machine and nowhere else.
//!   * **The engine does not yet encrypt data at rest** (`desentryd` logs
//!     `encrypt_at_rest ... NOT YET ENFORCED`). A password is access control
//!     inside the app, not protection against someone reading the data files
//!     directly. `NodeSecurity::encrypted_at_rest` reports this honestly and the
//!     UI must never call a password-protected node "encrypted".
//!
//! The verifier is not the password and cannot be turned back into it. The
//! recovery key gets its own verifier, so either secret unlocks, and changing
//! the password does not invalidate a recovery key the user already saved.
//!
//! SCAFFOLD CONTRACT (architecture-v3 §6). Types and command signatures are
//! fixed; bodies marked `TODO(platform)` are filled in by the platform
//! workstream.

use std::path::Path;
use std::sync::Arc;

use serde::{Deserialize, Serialize};
use tauri::{AppHandle, State};

use crate::appstate::AppState;

/// File in a node's data directory holding its verifiers. Never holds a secret.
pub const SECURITY_FILE: &str = "security.json";

/// Shortest password accepted. Long enough to rule out the trivially guessable,
/// short enough not to push people toward writing it on the laptop.
pub const MIN_PASSWORD_LEN: usize = 8;

#[derive(Debug, thiserror::Error)]
pub enum PasswordError {
    #[error("a password needs at least {MIN_PASSWORD_LEN} characters")]
    TooShort,
    #[error("that password or recovery key is not correct for this node")]
    Mismatch,
    #[error("this node is not password protected")]
    NotProtected,
    #[error("could not read or write {0}: {1}")]
    Io(String, String),
    #[error("could not derive a key: {0}")]
    Kdf(String),
}

/// One Argon2id verifier. PHC-format string, e.g. `$argon2id$v=19$m=...`.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct Verifier {
    pub phc: String,
}

/// Contents of `security.json`.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct SecurityRecord {
    pub version: u32,
    /// Verifier for the user's password.
    pub password: Verifier,
    /// Verifier for the recovery key, when one was issued.
    #[serde(default)]
    pub recovery: Option<Verifier>,
    pub created_ms: i64,
    #[serde(default)]
    pub changed_ms: i64,
}

/// What the window needs to know about a node's protection.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct NodeSecurity {
    pub node_id: String,
    /// A password is required to start this node.
    pub protected: bool,
    /// The password is in this machine's keychain, so the node auto-starts.
    pub remembered: bool,
    /// Protected and not currently unlocked in this session.
    pub locked: bool,
    pub has_recovery_key: bool,
    /// Always false until the engine enforces encryption at rest. Reported so
    /// the UI can say so rather than imply otherwise.
    pub encrypted_at_rest: bool,
}

/// Creates a verifier for `secret`.
///
/// TODO(platform): Argon2id, OWASP 2024 minimum (m=19456 KiB, t=2, p=1),
/// random 16-byte salt from `getrandom`.
pub fn make_verifier(secret: &str) -> Result<Verifier, PasswordError> {
    let _ = secret;
    Err(PasswordError::Kdf("not implemented yet".into()))
}

/// True when `secret` matches `verifier`. Constant-time via the argon2 crate.
///
/// TODO(platform)
pub fn check(verifier: &Verifier, secret: &str) -> bool {
    let _ = (verifier, secret);
    false
}

/// Reads `<data_dir>/security.json`. `Ok(None)` when the node is unprotected.
///
/// TODO(platform)
pub fn load(data_dir: &Path) -> Result<Option<SecurityRecord>, PasswordError> {
    let _ = data_dir;
    Ok(None)
}

/// Writes `<data_dir>/security.json` atomically (temp file + rename).
///
/// TODO(platform)
pub fn save(data_dir: &Path, record: &SecurityRecord) -> Result<(), PasswordError> {
    let _ = (data_dir, record);
    Err(PasswordError::Io(SECURITY_FILE.into(), "not implemented yet".into()))
}

/// Verifies `secret` against either the password or the recovery verifier.
///
/// TODO(platform)
pub fn unlocks(record: &SecurityRecord, secret: &str) -> bool {
    let _ = (record, secret);
    false
}

#[tauri::command]
pub fn node_security(state: State<'_, Arc<AppState>>, node_id: String) -> Result<NodeSecurity, String> {
    // TODO(platform): read the node's security.json and keychain entry.
    let _ = state;
    Ok(NodeSecurity {
        node_id,
        protected: false,
        remembered: false,
        locked: false,
        has_recovery_key: false,
        encrypted_at_rest: false,
    })
}

/// Sets, changes or removes a node's password.
///
///   * set:    `current_secret = None`, `new_password = Some(..)` on an unprotected node
///   * change: `current_secret = Some(password or recovery key)`, `new_password = Some(..)`
///   * remove: `current_secret = Some(..)`, `new_password = None`
///
/// `remember` stores (or clears) the new password in the OS keychain.
///
/// TODO(platform)
#[tauri::command]
pub fn set_node_password(
    app: AppHandle,
    state: State<'_, Arc<AppState>>,
    node_id: String,
    current_secret: Option<String>,
    new_password: Option<String>,
    remember: bool,
) -> Result<NodeSecurity, String> {
    let _ = (app, state, node_id, current_secret, new_password, remember);
    Err("setting a node password is not implemented yet".into())
}
