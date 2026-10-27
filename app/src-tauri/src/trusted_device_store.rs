//! Trusted device persistence for De-Sentry.
//!
//! Stores a list of devices that this node trusts for automatic admission.
//! Each node maintains its own encrypted store in its data directory.
//!
//! The store is encrypted with a machine-local secret stored in the OS keychain,
//! so trusted devices do not sync across machines -- they are per-device policy.

use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::keychain;

/// File name for the trusted devices store.
const FILENAME: &str = "trusted_devices.json";

/// Keychain reference prefix for trusted device encryption keys.
const KEY_PREFIX: &str = "trusted_devices.";

/// A trusted device entry.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct TrustedDevice {
    /// The device's node id (Ed25519 public key in hex).
    pub node_id: String,
    /// Human-readable name for the device.
    pub node_name: String,
    /// P2P address (host:port) for direct connection.
    pub p2p_address: String,
    /// API address (host:port) for status checks.
    pub api_address: String,
    /// Public key in hex (for verification).
    pub public_key_hex: String,
    /// Unix timestamp in milliseconds when this device was added.
    pub added_ms: i64,
    /// Whether to auto-accept connection requests from this device.
    pub auto_accept: bool,
}

/// Internal store structure (serialized to JSON, then encrypted).
#[derive(Debug, Clone, Serialize, Deserialize)]
struct StoreData {
    version: u32,
    devices: HashMap<String, TrustedDevice>,
}

impl StoreData {
    fn new() -> Self {
        Self {
            version: 1,
            devices: HashMap::new(),
        }
    }
}

#[derive(Debug, thiserror::Error)]
pub enum TrustedDeviceError {
    #[error("could not access the OS keychain: {0}")]
    Keychain(String),
    #[error("could not read the trusted devices file: {0}")]
    Read(String),
    #[error("could not write the trusted devices file: {0}")]
    Write(String),
    #[error("could not encrypt/decrypt the store: {0}")]
    Crypto(String),
    #[error("the trusted devices store is corrupt")]
    Corrupt,
}

/// Get the keychain reference for a node's trusted devices.
fn keychain_ref(node_id: &str) -> String {
    format!("{}{}", KEY_PREFIX, node_id)
}

/// Get the path to the trusted devices file for a node.
fn store_path(data_dir: &Path) -> PathBuf {
    data_dir.join(FILENAME)
}

/// Load or generate the encryption key for a node's trusted devices.
fn get_or_create_key(node_id: &str) -> Result<[u8; 32], TrustedDeviceError> {
    let reference = keychain_ref(node_id);

    // Try to load existing key
    match keychain::load(&reference) {
        Ok(key_hex) => {
            // Decode hex key
            let bytes = hex::decode(&key_hex).map_err(|e| {
                TrustedDeviceError::Keychain(format!("stored key is not valid hex: {}", e))
            })?;
            if bytes.len() != 32 {
                return Err(TrustedDeviceError::Keychain(
                    "stored key is not 32 bytes".to_string()
                ));
            }
            let mut key = [0u8; 32];
            key.copy_from_slice(&bytes);
            Ok(key)
        }
        Err(keychain::KeychainError::Missing(_)) => {
            // Generate new key
            let mut key = [0u8; 32];
            getrandom::getrandom(&mut key).map_err(|e| {
                TrustedDeviceError::Keychain(format!("could not generate key: {}", e))
            })?;
            let key_hex = hex::encode(&key);

            // Store in keychain
            keychain::store(&reference, &key_hex).map_err(|e| {
                TrustedDeviceError::Keychain(e.to_string())
            })?;

            Ok(key)
        }
        Err(e) => Err(TrustedDeviceError::Keychain(e.to_string())),
    }
}

/// Encrypt data with AES-256-GCM using the provided key.
fn encrypt(data: &[u8], key: &[u8; 32]) -> Result<Vec<u8>, TrustedDeviceError> {
    use aes_gcm::{
        aead::{Aead, KeyInit},
        Aes256Gcm, Nonce,
    };

    // Generate random nonce
    let mut nonce_bytes = [0u8; 12];
    getrandom::getrandom(&mut nonce_bytes).map_err(|e| {
        TrustedDeviceError::Crypto(format!("could not generate nonce: {}", e))
    })?;
    let nonce = Nonce::from_slice(&nonce_bytes);

    let cipher = Aes256Gcm::new_from_slice(key).map_err(|e| {
        TrustedDeviceError::Crypto(format!("could not initialize cipher: {}", e))
    })?;

    let mut ciphertext = cipher.encrypt(nonce, data).map_err(|e| {
        TrustedDeviceError::Crypto(format!("encryption failed: {}", e))
    })?;

    // Prepend nonce to ciphertext
    let mut result = Vec::with_capacity(nonce_bytes.len() + ciphertext.len());
    result.extend_from_slice(&nonce_bytes);
    result.append(&mut ciphertext);

    Ok(result)
}

/// Decrypt data with AES-256-GCM using the provided key.
fn decrypt(encrypted: &[u8], key: &[u8; 32]) -> Result<Vec<u8>, TrustedDeviceError> {
    use aes_gcm::{
        aead::{Aead, KeyInit},
        Aes256Gcm, Nonce,
    };

    if encrypted.len() < 12 {
        return Err(TrustedDeviceError::Crypto("ciphertext too short".to_string()));
    }

    let nonce_bytes = &encrypted[..12];
    let ciphertext = &encrypted[12..];
    let nonce = Nonce::from_slice(nonce_bytes);

    let cipher = Aes256Gcm::new_from_slice(key).map_err(|e| {
        TrustedDeviceError::Crypto(format!("could not initialize cipher: {}", e))
    })?;

    cipher.decrypt(nonce, ciphertext).map_err(|e| {
        TrustedDeviceError::Crypto(format!("decryption failed: {}", e))
    })
}

/// Load the trusted devices store for a node.
fn load_store(data_dir: &Path, node_id: &str) -> Result<StoreData, TrustedDeviceError> {
    let path = store_path(data_dir);

    if !path.exists() {
        return Ok(StoreData::new());
    }

    // Read encrypted file
    let encrypted = fs::read(&path).map_err(|e| {
        TrustedDeviceError::Read(format!("{}: {}", path.display(), e))
    })?;

    // Get decryption key
    let key = get_or_create_key(node_id)?;

    // Decrypt
    let plaintext = decrypt(&encrypted, &key)?;

    // Parse JSON
    let store: StoreData = serde_json::from_slice(&plaintext).map_err(|_| {
        TrustedDeviceError::Corrupt
    })?;

    Ok(store)
}

/// Save the trusted devices store for a node.
fn save_store(data_dir: &Path, node_id: &str, store: &StoreData) -> Result<(), TrustedDeviceError> {
    // Serialize to JSON
    let plaintext = serde_json::to_vec_pretty(store).map_err(|e| {
        TrustedDeviceError::Write(format!("serialization failed: {}", e))
    })?;

    // Get encryption key
    let key = get_or_create_key(node_id)?;

    // Encrypt
    let ciphertext = encrypt(&plaintext, &key)?;

    // Atomic write: tmp file + rename
    let path = store_path(data_dir);
    let tmp_path = path.with_extension("json.tmp");

    fs::write(&tmp_path, &ciphertext).map_err(|e| {
        TrustedDeviceError::Write(format!("{}: {}", tmp_path.display(), e))
    })?;

    fs::rename(&tmp_path, &path).map_err(|e| {
        TrustedDeviceError::Write(format!("{}: {}", path.display(), e))
    })?;

    Ok(())
}

/// The public API for managing trusted devices for a node.
pub struct TrustedDeviceStore {
    data_dir: PathBuf,
    node_id: String,
}

impl TrustedDeviceStore {
    /// Create a new trusted device store for a node.
    pub fn new(data_dir: PathBuf, node_id: String) -> Self {
        Self { data_dir, node_id }
    }

    /// Add a trusted device.
    pub fn add(&self, device: TrustedDevice) -> Result<(), TrustedDeviceError> {
        let mut store = load_store(&self.data_dir, &self.node_id)?;
        store.devices.insert(device.node_id.clone(), device);
        save_store(&self.data_dir, &self.node_id, &store)
    }

    /// Remove a trusted device by node id.
    pub fn remove(&self, node_id: &str) -> Result<(), TrustedDeviceError> {
        let mut store = load_store(&self.data_dir, &self.node_id)?;
        store.devices.remove(node_id);
        save_store(&self.data_dir, &self.node_id, &store)
    }

    /// List all trusted devices.
    pub fn list(&self) -> Result<Vec<TrustedDevice>, TrustedDeviceError> {
        let store = load_store(&self.data_dir, &self.node_id)?;
        let mut devices: Vec<TrustedDevice> = store.devices.into_values().collect();
        // Sort by added_ms descending (newest first)
        devices.sort_by(|a, b| b.added_ms.cmp(&a.added_ms));
        Ok(devices)
    }

    /// Check if a device is trusted.
    pub fn contains(&self, node_id: &str) -> Result<bool, TrustedDeviceError> {
        let store = load_store(&self.data_dir, &self.node_id)?;
        Ok(store.devices.contains_key(node_id))
    }

    /// Set auto-accept for a device.
    pub fn set_auto_accept(&self, node_id: &str, auto_accept: bool) -> Result<(), TrustedDeviceError> {
        let mut store = load_store(&self.data_dir, &self.node_id)?;
        if let Some(device) = store.devices.get_mut(node_id) {
            device.auto_accept = auto_accept;
            save_store(&self.data_dir, &self.node_id, &store)
        } else {
            Err(TrustedDeviceError::Write(format!("device {} not found", node_id)))
        }
    }

    /// Get a specific device.
    pub fn get(&self, node_id: &str) -> Result<Option<TrustedDevice>, TrustedDeviceError> {
        let store = load_store(&self.data_dir, &self.node_id)?;
        Ok(store.devices.get(node_id).cloned())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn now_ms() -> i64 {
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|d| d.as_millis() as i64)
            .unwrap_or(0)
    }

    fn test_device(id: &str) -> TrustedDevice {
        TrustedDevice {
            node_id: id.to_string(),
            node_name: format!("Device {}", id),
            p2p_address: format!("127.0.0.1:{}", 7800 + id.parse::<u16>().unwrap_or(0)),
            api_address: format!("127.0.0.1:{}", 7700 + id.parse::<u16>().unwrap_or(0)),
            public_key_hex: format!("{:064x}", id.parse::<u64>().unwrap_or(0)),
            added_ms: now_ms(),
            auto_accept: true,
        }
    }

    #[test]
    fn add_and_list_devices() {
        let dir = std::env::temp_dir().join(format!("desentry-trusted-test-{}", std::process::id()));
        let _ = std::fs::create_dir_all(&dir);

        let store = TrustedDeviceStore::new(dir.clone(), "test-node".to_string());

        // Add devices
        store.add(test_device("1")).unwrap();
        store.add(test_device("2")).unwrap();

        // List
        let devices = store.list().unwrap();
        assert_eq!(devices.len(), 2);

        // Contains
        assert!(store.contains("1").unwrap());
        assert!(store.contains("2").unwrap());
        assert!(!store.contains("3").unwrap());

        // Remove
        store.remove("1").unwrap();
        assert!(!store.contains("1").unwrap());
        assert!(store.contains("2").unwrap());

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn set_auto_accept() {
        let dir = std::env::temp_dir().join(format!("desentry-autoaccept-test-{}", std::process::id()));
        let _ = std::fs::create_dir_all(&dir);

        let store = TrustedDeviceStore::new(dir.clone(), "test-node".to_string());
        store.add(test_device("1")).unwrap();

        // Initially true
        let device = store.get("1").unwrap().unwrap();
        assert!(device.auto_accept);

        // Set to false
        store.set_auto_accept("1", false).unwrap();
        let device = store.get("1").unwrap().unwrap();
        assert!(!device.auto_accept);

        let _ = std::fs::remove_dir_all(&dir);
    }
}
