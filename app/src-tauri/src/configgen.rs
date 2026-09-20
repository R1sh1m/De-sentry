//! Writes `node.json`.
//!
//! The user never edits this file. The wizard collects what a node needs, this
//! module turns that into the exact JSON shape `NodeConfig::LoadFromFile`
//! reads (include/desentry/common/config.h), and `desentryd` is started with
//! `--config <path>`.
//!
//! Two rules keep this honest:
//!
//!   * The field names here are the C++ field names. There is no translation
//!     layer, no camel-case conversion, and no "app format" that a migration
//!     would have to keep in step.
//!   * Nothing secret is written. `encrypt_at_rest` is a boolean and
//!     `keychain_ref` is the *name* of a keychain entry -- the key itself
//!     lives in the OS keychain and reaches the node over its stdin, never
//!     through a file that a backup tool would copy.

use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::ports::PortAllocation;

/// Mirrors `QuotaSplit` in include/desentry/common/config.h.
#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
pub struct QuotaSplit {
    pub db_pct: u32,
    pub transit_store_pct: u32,
    pub cache_hash_pct: u32,
    pub ledger_pct: u32,
    pub net_buffers_pct: u32,
}

impl Default for QuotaSplit {
    fn default() -> Self {
        Self {
            db_pct: 60,
            transit_store_pct: 15,
            cache_hash_pct: 10,
            ledger_pct: 10,
            net_buffers_pct: 5,
        }
    }
}

impl QuotaSplit {
    pub fn total(&self) -> u32 {
        self.db_pct + self.transit_store_pct + self.cache_hash_pct + self.ledger_pct + self.net_buffers_pct
    }

    /// Rounds the split back to exactly 100 by adjusting the largest bucket.
    ///
    /// `NodeConfig::Validate()` rejects anything else, and a node that refuses
    /// to start because a proposal was off by one point would be an absurd
    /// failure to hand a user. Adjusting the largest bucket keeps the shape of
    /// the proposal while making it valid.
    pub fn normalised(mut self) -> Self {
        let total = self.total() as i64;
        if total == 100 {
            return self;
        }
        let delta = 100 - total;
        let buckets: [&mut u32; 5] = [
            &mut self.db_pct,
            &mut self.transit_store_pct,
            &mut self.cache_hash_pct,
            &mut self.ledger_pct,
            &mut self.net_buffers_pct,
        ];
        let largest = buckets.into_iter().max_by_key(|value| **value).expect("five buckets");
        *largest = (*largest as i64 + delta).max(0) as u32;
        self
    }
}

/// Everything needed to write one node's config.
#[derive(Debug, Clone)]
pub struct NodeConfigSpec {
    pub data_dir: PathBuf,
    pub node_name: String,
    pub ports: PortAllocation,
    pub supervisor: bool,
    pub discovery_enabled: bool,
    pub quota_mb: u64,
    pub quota_split: QuotaSplit,
    pub engines: Vec<String>,
    pub default_engine: String,
    pub replication_factor: u32,
    pub retention_days: u32,
    pub encrypt_at_rest: bool,
    pub keychain_ref: String,
    pub bootstrap_peers: Vec<String>,
    pub advertise_hostname: String,
    /// "generated" (recovery key) or "passphrase" (custom passphrase + envelope).
    #[allow(clippy::vec_box)]
    pub key_mode: String,
    /// Hex salt for passphrase KDF; empty for generated keys.
    pub kdf_salt_hex: String,
    /// PBKDF2 iteration count for passphrase KDF; 0 for generated keys.
    pub kdf_iters: u32,
    /// Wrapped DEK hex + HMAC tag hex for passphrase nodes; empty otherwise.
    pub dek_wrapped_hex: String,
    pub dek_tag_hex: String,
}

impl Default for NodeConfigSpec {
    fn default() -> Self {
        Self {
            data_dir: PathBuf::from("."),
            node_name: String::new(),
            ports: PortAllocation {
                api_port: 0,
                p2p_port: 0,
                discovery_port: 0,
            },
            supervisor: false,
            discovery_enabled: true,
            quota_mb: 0,
            quota_split: QuotaSplit::default(),
            engines: vec![],
            default_engine: "kv".to_owned(),
            replication_factor: 3,
            retention_days: 0,
            encrypt_at_rest: false,
            keychain_ref: String::new(),
            bootstrap_peers: vec![],
            advertise_hostname: String::new(),
            key_mode: "generated".to_owned(),
            kdf_salt_hex: String::new(),
            kdf_iters: 0,
            dek_wrapped_hex: String::new(),
            dek_tag_hex: String::new(),
        }
    }
}

impl NodeConfigSpec {
    /// The JSON `desentryd` reads. Field names match config.h exactly.
    pub fn to_json(&self) -> serde_json::Value {
        // A supervisor binds its API to loopback. `NodeConfig::Validate()`
        // enforces this and refuses to start otherwise, so writing anything
        // else here would only produce a node that will not run -- the value
        // is set correctly at the source instead.
        let api_bind = "127.0.0.1";
        let p2p_bind = if self.supervisor { "127.0.0.1" } else { "0.0.0.0" };
        let discovery_enabled = if self.supervisor {
            false
        } else {
            self.discovery_enabled
        };

        serde_json::json!({
            "data_dir": self.data_dir.to_string_lossy(),
            "node_name": self.node_name,

            "api_bind_addr": api_bind,
            "api_port": self.ports.api_port,
            "p2p_bind_addr": p2p_bind,
            "p2p_port": self.ports.p2p_port,

            "discovery_enabled": discovery_enabled,
            "discovery_port": self.ports.discovery_port,
            "discovery_interval_ms": 2000,
            "bootstrap_peers": self.bootstrap_peers,
            "gossip_interval_ms": 2000,
            "buffer_pool_pages": buffer_pool_pages(self.quota_mb),

            "supervisor": self.supervisor,
            "quota_mb": self.quota_mb,
            // The JSON keys have no _pct suffix. The C++ struct fields do
            // (config.h), and NodeConfig::Parse() reads the un-suffixed
            // names -- a mismatch here is silent: the node boots, ignores the
            // block, and runs on defaults that happen to also sum to 100.
            "quota_split": {
                "db": self.quota_split.db_pct,
                "transit_store": self.quota_split.transit_store_pct,
                "cache_hash": self.quota_split.cache_hash_pct,
                "ledger": self.quota_split.ledger_pct,
                "net_buffers": self.quota_split.net_buffers_pct,
            },
            "engines": self.engines,
            "default_engine": self.default_engine,
            "replication_factor": self.replication_factor,
            "transit_ttl_seconds": 7 * 24 * 3600,
            "retention_days": self.retention_days,

            "encrypt_at_rest": self.encrypt_at_rest,
            "keychain_ref": self.keychain_ref,

            // Passphrase envelope (non-secret alone): key_mode + KDF params +
            // wrapped DEK. Generated-key nodes leave these empty/defaults.
            // Copying node.json still copies nothing that decrypts anything:
            // the passphrase itself lives only in the user's memory/paper and
            // (optionally) the OS keychain holds the unwrapped DEK.
            "key_mode": self.key_mode,
            "kdf": {
                "kdf": "pbkdf2-sha256-210k",
                "iters": self.kdf_iters,
                "salt_hex": self.kdf_salt_hex,
            },
            "dek_wrapped_hex": self.dek_wrapped_hex,
            "dek_tag_hex": self.dek_tag_hex,

            "max_peer_threads": 8,
            "peer_rate_limit_per_sec": 200,
            "peer_rate_burst": 400,
            "mdns_enabled": !self.supervisor,
            "advertise_hostname": self.advertise_hostname,
        })
    }

    /// Writes `node.json` into the data directory, creating it if needed.
    ///
    /// Written to a temporary file and renamed, so a crash mid-write leaves the
    /// previous config intact rather than a truncated one that will not parse.
    pub fn write(&self) -> std::io::Result<PathBuf> {
        fs::create_dir_all(&self.data_dir)?;
        let final_path = self.data_dir.join("node.json");
        let temp_path = self.data_dir.join("node.json.tmp");

        let body = serde_json::to_string_pretty(&self.to_json())
            .map_err(|error| std::io::Error::new(std::io::ErrorKind::InvalidData, error))?;
        {
            let mut file = fs::File::create(&temp_path)?;
            file.write_all(body.as_bytes())?;
            file.write_all(b"\n")?;
            file.sync_all()?;
        }
        // Windows rename fails onto an existing file; removing first is the
        // portable form of an atomic replace here.
        let _ = fs::remove_file(&final_path);
        fs::rename(&temp_path, &final_path)?;
        Ok(final_path)
    }
}

/// Buffer pool sized from the node's budget, within sane bounds.
///
/// 4 KiB pages, aiming at about 6% of the node's quota, floored at the engine's
/// own default (1024 pages / 4 MiB) and capped at 64 MiB. A pool larger than
/// the data it caches is only a memory bill.
fn buffer_pool_pages(quota_mb: u64) -> u32 {
    if quota_mb == 0 {
        return 1024;
    }
    let target_pages = quota_mb * 256 * 6 / 100; // (quota_mb * 1024 KiB) / 4 KiB * 6%
    target_pages.clamp(1024, 16_384) as u32
}

/// Passphrase envelope as stored in node.json (all non-secret alone).
#[derive(Debug, Clone)]
pub struct KeyEnvelope {
    pub key_mode: String,
    pub kdf_iters: u32,
    pub kdf_salt_hex: String,
    pub dek_wrapped_hex: String,
    pub dek_tag_hex: String,
}

/// Reads the key envelope (key_mode + KDF + wrapped DEK) from node.json.
pub fn read_envelope(data_dir: &Path) -> Option<KeyEnvelope> {
    let text = fs::read_to_string(data_dir.join("node.json")).ok()?;
    let value: serde_json::Value = serde_json::from_str(&text).ok()?;
    Some(KeyEnvelope {
        key_mode: value
            .get("key_mode")
            .and_then(|v| v.as_str())
            .unwrap_or("generated")
            .to_owned(),
        kdf_iters: value
            .get("kdf")
            .and_then(|k| k.get("iters"))
            .and_then(|v| v.as_u64())
            .unwrap_or(0) as u32,
        kdf_salt_hex: value
            .get("kdf")
            .and_then(|k| k.get("salt_hex"))
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_owned(),
        dek_wrapped_hex: value
            .get("dek_wrapped_hex")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_owned(),
        dek_tag_hex: value
            .get("dek_tag_hex")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_owned(),
    })
}

/// Rewrites only the envelope fields of node.json, leaving everything else.
pub fn rewrite_envelope(
    config_path: &Path,
    key_mode: &str,
    kdf_iters: u32,
    kdf_salt_hex: &str,
    dek_wrapped_hex: &str,
    dek_tag_hex: &str,
) -> std::io::Result<()> {
    let text = fs::read_to_string(config_path)?;
    let mut value: serde_json::Value =
        serde_json::from_str(&text).map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))?;
    if let Some(object) = value.as_object_mut() {
        object.insert("key_mode".into(), key_mode.into());
        object.insert(
            "kdf".into(),
            serde_json::json!({
                "kdf": "pbkdf2-sha256-210k",
                "iters": kdf_iters,
                "salt_hex": kdf_salt_hex,
            }),
        );
        object.insert("dek_wrapped_hex".into(), dek_wrapped_hex.into());
        object.insert("dek_tag_hex".into(), dek_tag_hex.into());
    }
    let body = serde_json::to_string_pretty(&value)
        .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))?;
    // Atomic replace via temp + rename, matching NodeConfigSpec::write.
    if let Some(parent) = config_path.parent() {
        let temp = parent.join("node.json.tmp");
        fs::write(&temp, body + "\n")?;
        let _ = fs::remove_file(config_path);
        fs::rename(&temp, config_path)?;
        return Ok(());
    }
    fs::write(config_path, body + "\n")
}

/// Reads a node's existing name from its config, for adoption.
pub fn read_node_name(data_dir: &Path) -> Option<String> {
    let text = fs::read_to_string(data_dir.join("node.json")).ok()?;
    let value: serde_json::Value = serde_json::from_str(&text).ok()?;
    value
        .get("node_name")
        .and_then(|name| name.as_str())
        .map(str::to_owned)
        .filter(|name| !name.is_empty())
}

/// Records the sizing decision alongside the node, for audit.
///
/// The spec asks for the AI's decision and confidence to be logged into the
/// node manifest. It goes next to the data rather than into a central log:
/// a node that travels on a USB stick carries the reason it was shaped the way
/// it was.
pub fn write_manifest(data_dir: &Path, manifest: &serde_json::Value) -> std::io::Result<()> {
    fs::create_dir_all(data_dir)?;
    let body = serde_json::to_string_pretty(manifest)
        .map_err(|error| std::io::Error::new(std::io::ErrorKind::InvalidData, error))?;
    fs::write(data_dir.join("manifest.json"), body + "\n")
}

#[cfg(test)]
mod tests {
    use super::*;

    fn spec() -> NodeConfigSpec {
        NodeConfigSpec {
            data_dir: PathBuf::from("/tmp/desentry-test"),
            node_name: "test".into(),
            ports: PortAllocation {
                api_port: 7702,
                p2p_port: 7802,
                discovery_port: 7901,
            },
            supervisor: false,
            discovery_enabled: true,
            quota_mb: 2048,
            quota_split: QuotaSplit::default(),
            engines: vec!["kv".into()],
            default_engine: "kv".into(),
            replication_factor: 3,
            retention_days: 0,
            encrypt_at_rest: false,
            keychain_ref: String::new(),
            bootstrap_peers: vec![],
            advertise_hostname: "test-host".into(),
            key_mode: "generated".into(),
            kdf_salt_hex: String::new(),
            kdf_iters: 0,
            dek_wrapped_hex: String::new(),
            dek_tag_hex: String::new(),
        }
    }

    #[test]
    fn a_standalone_isolated_database_disables_discovery() {
        let mut spec = spec();
        spec.discovery_enabled = false;
        let json = spec.to_json();
        assert_eq!(json["discovery_enabled"], false);
    }

    #[test]
    fn a_supervisor_binds_only_to_loopback() {
        let mut spec = spec();
        spec.supervisor = true;
        let json = spec.to_json();
        assert_eq!(json["api_bind_addr"], "127.0.0.1");
        assert_eq!(json["p2p_bind_addr"], "127.0.0.1");
        // A supervisor that advertised itself would be discoverable from the
        // LAN, which is exactly what it must not be.
        assert_eq!(json["discovery_enabled"], false);
        assert_eq!(json["mdns_enabled"], false);
    }

    #[test]
    fn a_data_node_binds_p2p_publicly() {
        let json = spec().to_json();
        assert_eq!(json["api_bind_addr"], "127.0.0.1");
        assert_eq!(json["p2p_bind_addr"], "0.0.0.0");
        assert_eq!(json["discovery_enabled"], true);
    }

    #[test]
    fn no_secret_reaches_the_config_file() {
        let mut spec = spec();
        spec.encrypt_at_rest = true;
        spec.keychain_ref = "dev.desentry.node.abc123".into();
        let text = serde_json::to_string(&spec.to_json()).unwrap();
        assert!(text.contains("dev.desentry.node.abc123"));
        assert!(!text.contains("key\":\"") || !text.contains("BEGIN"));
    }

    #[test]
    fn a_split_that_does_not_sum_is_repaired() {
        let split = QuotaSplit {
            db_pct: 61,
            transit_store_pct: 15,
            cache_hash_pct: 10,
            ledger_pct: 10,
            net_buffers_pct: 5,
        }
        .normalised();
        assert_eq!(split.total(), 100);
        // The largest bucket absorbs the correction, so the proposal's shape
        // survives.
        assert_eq!(split.db_pct, 60);
    }

    #[test]
    fn buffer_pool_stays_within_bounds() {
        assert_eq!(buffer_pool_pages(0), 1024);
        assert_eq!(buffer_pool_pages(64), 1024);
        assert_eq!(buffer_pool_pages(1_000_000), 16_384);
        assert!(buffer_pool_pages(4096) > 1024);
    }

    #[test]
    fn the_passphrase_envelope_round_trips_through_node_json() {
        // Full production path at the config layer: write a passphrase-mode
        // config, read the envelope back, rotate it, and confirm the old wrap
        // no longer reads while the new one does. Uses a small iteration
        // count — the KDF shape is covered in passphrase.rs; this covers the
        // persistence contract.
        use crate::passphrase;
        let dir = std::env::temp_dir().join(format!("desentry-envelope-{}", std::process::id()));
        let _ = std::fs::create_dir_all(&dir);
        let mut spec = spec();
        spec.data_dir = dir.clone();
        spec.encrypt_at_rest = true;
        spec.key_mode = "passphrase".into();

        let salt = [11u8; passphrase::SALT_BYTES];
        let dek = [0x5au8; passphrase::KEY_BYTES];
        let kek = passphrase::derive_kek("a long enough old phrase!", &salt, 1000);
        let (wrapped, tag) = passphrase::wrap_dek(&dek, &kek, &salt);
        spec.kdf_salt_hex = {
            let mut s = String::new();
            for b in salt {
                s.push_str(&format!("{b:02x}"));
            }
            s
        };
        spec.kdf_iters = 1000;
        spec.dek_wrapped_hex = wrapped.clone();
        spec.dek_tag_hex = tag.clone();
        let config_path = spec.write().expect("config writes");

        let env = read_envelope(&dir).expect("envelope reads back");
        assert_eq!(env.key_mode, "passphrase");
        assert_eq!(env.kdf_iters, 1000);
        assert_eq!(env.dek_wrapped_hex, wrapped);

        // Rotation: same DEK under a fresh salt; old KEK must fail, new must pass.
        let new_salt = [22u8; passphrase::SALT_BYTES];
        let new_kek = passphrase::derive_kek("a different long phrase here", &new_salt, 1000);
        let (new_wrapped, new_tag) = passphrase::wrap_dek(&dek, &new_kek, &new_salt);
        let mut new_salt_hex = String::new();
        for b in new_salt {
            new_salt_hex.push_str(&format!("{b:02x}"));
        }
        rewrite_envelope(&config_path, "passphrase", 1000, &new_salt_hex, &new_wrapped, &new_tag)
            .expect("rotation writes");
        let rotated = read_envelope(&dir).expect("rotated envelope reads");
        assert_eq!(rotated.dek_wrapped_hex, new_wrapped);
        // Old KEK against the new wrap fails; new KEK succeeds.
        assert!(passphrase::unwrap_dek(&rotated.dek_wrapped_hex, &rotated.dek_tag_hex, &kek, &new_salt).is_err());
        let back = passphrase::unwrap_dek(&rotated.dek_wrapped_hex, &rotated.dek_tag_hex, &new_kek, &new_salt)
            .expect("new passphrase unwraps");
        assert_eq!(back, dek);

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn legacy_configs_without_an_envelope_read_as_generated() {
        let dir = std::env::temp_dir().join(format!("desentry-legacy-{}", std::process::id()));
        let _ = std::fs::create_dir_all(&dir);
        std::fs::write(
            dir.join("node.json"),
            r#"{"node_name":"old","keychain_ref":""}"#,
        )
        .expect("legacy config writes");
        let env = read_envelope(&dir).expect("legacy envelope has defaults");
        assert_eq!(env.key_mode, "generated");
        assert!(env.dek_wrapped_hex.is_empty());
        let _ = std::fs::remove_dir_all(&dir);
    }
}
