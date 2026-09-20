//! At-rest keys in the operating system's keychain.
//!
//! Windows Credential Manager, macOS Keychain, Linux Secret Service -- one
//! crate over three platform APIs. What matters here is the shape of the
//! custody, not which store it lands in:
//!
//!   * The key never touches node.json. The config carries a `keychain_ref`,
//!     which is the *name* of an entry, so copying a config file (or a backup
//!     of one) copies nothing that decrypts anything.
//!   * The key reaches `desentryd` over its stdin, so it is not in a command
//!     line that every process listing on the machine can read.
//!   * Removable nodes get no keychain entry at all. A USB stick that unlocks
//!     itself from the keychain of the machine that made it is a stick that
//!     cannot be read anywhere else -- so those unlock from a password the
//!     user types, and the entry here is deliberately absent.

use keyring::Entry;

/// Keychain service name. One namespace for the whole app.
const SERVICE: &str = "dev.desentry.app";

#[derive(Debug, thiserror::Error)]
pub enum KeychainError {
    #[error("this system's keychain refused the request: {0}")]
    Backend(String),
    #[error("no at-rest key is stored for {0}. If this node was created on another machine, unlock it with its recovery key instead.")]
    Missing(String),
}

/// The entry name for a node. Recorded in node.json as `keychain_ref`.
pub fn reference_for(node_id: &str) -> String {
    format!("node.{node_id}")
}

/// A collision-free entry name for tests.
///
/// Tests share one persistent OS store (Credential Manager / Keychain / Secret
/// Service) with no per-process isolation — unlike temp dirs, which isolate by
/// pid. Fixed test refs race when two `cargo test` processes (or threads)
/// interleave `store → forget → load`. The pid + atomic nonce makes every
/// call unique across processes and threads; callers `forget` in a
/// scope-guard so a failure never leaks an entry into a later run.
#[cfg(test)]
pub fn unique_test_ref(prefix: &str) -> String {
    static NONCE: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
    let n = NONCE.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
    format!("node.test-{prefix}-{}-{n}", std::process::id())
}

fn entry(reference: &str) -> Result<Entry, KeychainError> {
    Entry::new(SERVICE, reference).map_err(|error| KeychainError::Backend(error.to_string()))
}

/// Stores a node's at-rest key.
pub fn store(reference: &str, key: &str) -> Result<(), KeychainError> {
    entry(reference)?
        .set_password(key)
        .map_err(|error| KeychainError::Backend(error.to_string()))
}

/// Reads a node's at-rest key back.
pub fn load(reference: &str) -> Result<String, KeychainError> {
    match entry(reference)?.get_password() {
        Ok(key) => Ok(key),
        Err(keyring::Error::NoEntry) => Err(KeychainError::Missing(reference.to_owned())),
        Err(error) => Err(KeychainError::Backend(error.to_string())),
    }
}

/// Removes a node's key. Called when a node is deliberately discarded.
///
/// A missing entry is success: the caller wanted the key gone, and it is.
pub fn forget(reference: &str) -> Result<(), KeychainError> {
    match entry(reference)?.delete_credential() {
        Ok(()) | Err(keyring::Error::NoEntry) => Ok(()),
        Err(error) => Err(KeychainError::Backend(error.to_string())),
    }
}

/// Whether the platform keychain is usable at all.
///
/// On a headless Linux box with no Secret Service running there is no
/// keychain, and the honest thing is to say so at the point the user chooses
/// encryption rather than to fail after the node has been created.
pub fn available() -> bool {
    // Probing with a read of a name that will not exist: a backend that is
    // present answers NoEntry, one that is absent fails to construct or errors
    // out at the platform layer.
    match Entry::new(SERVICE, "probe.availability") {
        Ok(entry) => !matches!(entry.get_password(), Err(keyring::Error::PlatformFailure(_))),
        Err(_) => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn references_are_namespaced_by_node() {
        assert_eq!(reference_for("abc123"), "node.abc123");
        assert_ne!(reference_for("a"), reference_for("b"));
    }

    /// Only meaningful where a keychain actually exists; skipped otherwise so
    /// CI on a headless box does not fail on the absence of a desktop service.
    #[test]
    fn a_stored_key_reads_back() {
        if !available() {
            return;
        }
        // Unique ref: the OS store is shared across test threads and across
        // concurrent `cargo test` processes — a fixed ref races.
        let reference = unique_test_ref("round-trip");
        struct Guard<'a>(&'a str);
        impl Drop for Guard<'_> {
            fn drop(&mut self) {
                let _ = forget(self.0);
            }
        }
        let _guard = Guard(&reference);
        let key = "ABCDE-FGHJK-MNPQR";
        if store(&reference, key).is_err() {
            return;
        }
        assert_eq!(load(&reference).unwrap(), key);
        forget(&reference).unwrap();
        assert!(matches!(load(&reference), Err(KeychainError::Missing(_))));
    }

    #[test]
    fn unique_test_refs_never_collide() {
        let mut seen = std::collections::HashSet::new();
        for _ in 0..100 {
            assert!(seen.insert(unique_test_ref("nonce")));
        }
    }
}
