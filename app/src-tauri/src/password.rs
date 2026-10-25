//! Per-node passwords with Argon2id verification and OS keychain storage.
//!
//! A password-protected node does not start until someone proves they know
//! the password -- or holds the node's recovery key. The app verifies the
//! secret against an Argon2id verifier stored in `<data_dir>/security.json`
//! and refuses to launch the node otherwise.
//!
//! "Remember on this device" stores the password in the OS keychain, so the
//! node starts automatically on this machine and nowhere else.
//!
//! **The engine does not yet encrypt data at rest** (`desentryd` logs
//! `encrypt_at_rest ... NOT YET ENFORCED`). A password is access control
//! inside the app, not protection against someone reading the data files
//! directly. `NodeSecurity::encrypted_at_rest` reports this honestly and the
//! UI must never call a password-protected node "encrypted".
//!
//! The verifier is not the password and cannot be turned back into it. The
//! recovery key gets its own verifier, so either secret unlocks, and changing
//! the password does not invalidate a recovery key the user already saved.
//!
//! The engine reads `security.json` through `routes.cpp`; this crate only
//! builds/reads/writes the record and exposes Tauri commands the window
//! calls.

use std::path::Path;
use std::sync::Arc;

use argon2::{
    password_hash::{PasswordHasher, PasswordVerifier, SaltString},
    Argon2,
};
use serde::{Deserialize, Serialize};
use tauri::State;

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

/// Creates a verifier for `secret` using Argon2id with OWASP 2024 minimum
/// parameters (m=19456 KiB, t=2, p=1) and a random 16-byte salt.
pub fn make_verifier(secret: &str) -> Result<Verifier, PasswordError> {
    let mut salt_bytes = [0u8; 16];
    getrandom::getrandom(&mut salt_bytes)
        .map_err(|e| PasswordError::Kdf(format!("could not generate salt: {e}")))?;
    let salt = SaltString::encode_b64(&salt_bytes)
        .map_err(|e| PasswordError::Kdf(format!("could not encode salt: {e}")))?;
    let argon2 = Argon2::default();
    argon2
        .hash_password(secret.as_bytes(), &salt)
        .map(|hash| Verifier {
            phc: hash.to_string(),
        })
        .map_err(|e| PasswordError::Kdf(e.to_string()))
}

/// True when `secret` matches `verifier`. Constant-time via the argon2 crate.
pub fn check(verifier: &Verifier, secret: &str) -> bool {
    let Ok(parsed) = argon2::PasswordHash::new(&verifier.phc) else {
        return false;
    };
    Argon2::default()
        .verify_password(secret.as_bytes(), &parsed)
        .is_ok()
}

/// Reads `<data_dir>/security.json`. `Ok(None)` when the node is unprotected.
pub fn load(data_dir: &Path) -> Result<Option<SecurityRecord>, PasswordError> {
    let path = data_dir.join(SECURITY_FILE);
    match std::fs::read_to_string(&path) {
        Ok(text) => serde_json::from_str(&text)
            .map(Some)
            .map_err(|e| PasswordError::Io(SECURITY_FILE.into(), e.to_string())),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(PasswordError::Io(SECURITY_FILE.into(), e.to_string())),
    }
}

/// Writes `<data_dir>/security.json` atomically (temp file + rename).
pub fn save(data_dir: &Path, record: &SecurityRecord) -> Result<(), PasswordError> {
    let path = data_dir.join(SECURITY_FILE);
    let tmp = path.with_extension("json.tmp");
    let body = serde_json::to_string_pretty(record)
        .map_err(|e| PasswordError::Io(SECURITY_FILE.into(), e.to_string()))?;
    std::fs::write(&tmp, body)
        .map_err(|e| PasswordError::Io(SECURITY_FILE.into(), e.to_string()))?;
    std::fs::rename(&tmp, &path)
        .map_err(|e| PasswordError::Io(SECURITY_FILE.into(), e.to_string()))
}

/// Verifies `secret` against either the password or the recovery verifier.
pub fn unlocks(record: &SecurityRecord, secret: &str) -> bool {
    check(&record.password, secret) || record.recovery.as_ref().is_some_and(|r| check(r, secret))
}

/// Returns the current unix time in milliseconds.
fn now_ms() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

pub fn node_security(state: State<'_, Arc<AppState>>, node_id: String) -> Result<NodeSecurity, String> {
    let nodes = state.nodes.lock().map_err(|_| "the node registry is unavailable")?;
    let handle = nodes.get(&node_id).ok_or_else(|| format!("no node with id {node_id}"))?;
    let data_dir = handle.spec.data_dir.clone();

    // Load security.json if present.
    let record = match load(&data_dir) {
        Ok(Some(r)) => r,
        Ok(None) => {
            return Ok(NodeSecurity {
                node_id,
                protected: false,
                remembered: false,
                locked: false,
                has_recovery_key: false,
                encrypted_at_rest: false,
            });
        }
        Err(e) => return Err(format!("could not read node security: {e}")),
    };

    // Is the password in this machine's keychain?
    let reference = crate::keychain::reference_for(&node_id);
    let remembered = crate::keychain::load(&reference).is_ok();

    Ok(NodeSecurity {
        node_id,
        protected: true,
        remembered,
        locked: false, // the sidecar does not hold in-memory unlock state here
        has_recovery_key: record.recovery.is_some(),
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
pub fn set_node_password(
    state: State<'_, Arc<AppState>>,
    node_id: String,
    current_secret: Option<String>,
    new_password: Option<String>,
    remember: bool,
) -> Result<NodeSecurity, String> {
    let nodes = state.nodes.lock().map_err(|_| "the node registry is unavailable")?;
    let handle = nodes.get(&node_id).ok_or_else(|| format!("no node with id {node_id}"))?;
    let data_dir = handle.spec.data_dir.clone();

    let record = match load(&data_dir) {
        Ok(Some(r)) => r,
        Ok(None) => SecurityRecord {
            version: 1,
            password: Verifier { phc: String::new() },
            recovery: None,
            created_ms: now_ms(),
            changed_ms: 0,
        },
        Err(e) => return Err(format!("could not read node security: {e}")),
    };

    // For a change, verify the current secret (password or recovery key).
    if let Some(current) = &current_secret {
        if !record.password.phc.is_empty() && !check(&record.password, current) {
            if record.recovery.as_ref().is_none_or(|r| !check(r, current)) {
                return Err("current password or recovery key is not correct".into());
            }
        }
    }

    let reference = crate::keychain::reference_for(&node_id);

    match new_password {
        Some(password) => {
            if password.chars().count() < MIN_PASSWORD_LEN {
                return Err(format!("a password needs at least {MIN_PASSWORD_LEN} characters").into());
            }
            let verifier = make_verifier(&password)
                .map_err(|e| format!("could not hash the password: {e}"))?;
            let mut new_record = record.clone();
            new_record.password = verifier;
            new_record.changed_ms = now_ms();
            if new_record.created_ms == 0 {
                new_record.created_ms = now_ms();
            }

            save(&data_dir, &new_record).map_err(|e| format!("could not save security: {e}"))?;

            if remember {
                crate::keychain::store(&reference, &password)
                    .map_err(|e| format!("could not store the password in the keychain: {e}"))?;
            } else {
                // "Remember me" was cleared: drop any stored password.
                let _ = crate::keychain::forget(&reference);
            }

            drop(nodes);
            node_security(state, node_id)
        }
        None => {
            // Removing the password. Verify current secret first.
            if let Some(current) = &current_secret {
                if !record.password.phc.is_empty() && !check(&record.password, current) {
                    if record.recovery.as_ref().is_none_or(|r| !check(r, current)) {
                        return Err("current password or recovery key is not correct".into());
                    }
                }
            }
            let mut new_record = record.clone();
            new_record.password = Verifier { phc: String::new() };
            new_record.changed_ms = now_ms();
            save(&data_dir, &new_record).map_err(|e| format!("could not save security: {e}"))?;
            let _ = crate::keychain::forget(&reference);

            drop(nodes);
            node_security(state, node_id)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn verifier_round_trips_and_rejects_wrong() {
        let v = make_verifier("correct horse battery staple").unwrap();
        assert!(check(&v, "correct horse battery staple"));
        assert!(!check(&v, "wrong password"));
        assert_ne!(v, Verifier { phc: String::new() });
    }

    #[test]
    fn unlocks_accepts_password_or_recovery() {
        let record = SecurityRecord {
            version: 1,
            password: Verifier { phc: make_verifier("the-password").unwrap().phc },
            recovery: Some(Verifier { phc: make_verifier("the-recovery-key").unwrap().phc }),
            created_ms: 1,
            changed_ms: 0,
        };
        assert!(unlocks(&record, "the-password"));
        assert!(unlocks(&record, "the-recovery-key"));
        assert!(!unlocks(&record, "nonsense"));
    }

    #[test]
    fn save_and_load_round_trip_in_temp_dir() {
        let dir = tempfile::tempdir().unwrap();
        let record = SecurityRecord {
            version: 1,
            password: Verifier { phc: "$argon2id$v=19$m=19456,t=2,p=1$salt$hash".to_owned() },
            recovery: None,
            created_ms: 1,
            changed_ms: 2,
        };
        save(dir.path(), &record).unwrap();
        let loaded = load(dir.path()).unwrap().unwrap();
        assert_eq!(loaded, record);
    }
}
