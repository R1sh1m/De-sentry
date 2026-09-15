//! Pairing: getting a second device into this mesh.
//!
//! The previous pairing sheet drew a QR code of the *supervisor's* details --
//! and nothing anywhere could consume it. There was no scan, no paste, no join
//! path; worse, the supervisor binds P2P to loopback, so its address is useless
//! to any other machine even if something had read it. The feature displayed a
//! code and went nowhere.
//!
//! What pairing is now:
//!
//!   * `pairing_code` describes a **data node** reachable on the LAN: its
//!     node_id, public key, LAN addresses with its P2P port, and the discovery
//!     port. Encoded as a short text code (`DSN1-...`) that is both the QR's
//!     content and something a person can copy and paste.
//!   * `join_mesh` takes that code on the other device, validates it, adds the
//!     advertised addresses as bootstrap peers to every local data node --
//!     persisted in node.json and applied live through the engine's
//!     `POST /_peers` -- and triggers discovery.
//!
//! A code carries addresses and a public key, never a secret. A photograph of
//! it lets someone *dial* the node; the secure-channel handshake still decides
//! whether they are trusted.
//!
//! SCAFFOLD CONTRACT (architecture-v3 §7). Types, the code format and command
//! signatures are fixed; bodies marked `TODO(platform)` are filled in by the
//! platform workstream. The engine route is the engine workstream's.

use std::sync::Arc;

use serde::{Deserialize, Serialize};
use tauri::{AppHandle, State};

use crate::appstate::AppState;

/// Every pairing code starts with this. The digit is the payload version.
pub const CODE_PREFIX: &str = "DSN1-";

/// Most addresses a code advertises. Keeps the QR at a comfortable version and
/// the pasted text short; a device with more interfaces lists its best four.
pub const MAX_ADDRESSES: usize = 4;

#[derive(Debug, thiserror::Error)]
pub enum PairingError {
    #[error("that is not a De-Sentry pairing code (it should start with {CODE_PREFIX})")]
    NotACode,
    #[error("the pairing code is damaged or incomplete: {0}")]
    Malformed(String),
    #[error("the pairing code is from a newer version of De-Sentry (version {0})")]
    UnsupportedVersion(u8),
    #[error("there is no running data node to pair with yet")]
    NoDataNode,
    #[error("this code points at this device")]
    SelfPairing,
}

/// The payload inside a code. Field names are short on purpose: they are
/// encoded into the QR.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct PairingPayload {
    /// Payload version. 1.
    pub v: u8,
    /// node_id of the advertised data node.
    pub id: String,
    /// Hex Ed25519 public key; the handshake checks the peer presents this key.
    pub pk: String,
    /// "ip:p2p_port" entries, best first, at most MAX_ADDRESSES.
    pub a: Vec<String>,
    /// UDP discovery port.
    pub d: u16,
    /// Human name of the node, truncated to 32 chars.
    #[serde(default)]
    pub n: String,
}

/// What the pairing sheet shows.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct PairingInfo {
    /// The full `DSN1-...` code. Also the QR content.
    pub code: String,
    pub node_id: String,
    pub node_name: String,
    pub addresses: Vec<String>,
    pub discovery_port: u16,
    pub created_ms: i64,
}

/// What joining did.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct JoinResult {
    pub mesh_node_id: String,
    pub mesh_node_name: String,
    /// Addresses added as bootstrap peers.
    pub peers_added: Vec<String>,
    /// Local node ids whose configuration was updated.
    pub nodes_updated: Vec<String>,
    /// Non-fatal problems, written for the user.
    pub warnings: Vec<String>,
}

/// Encodes a payload as `DSN1-<base64url(json), no padding>`.
///
/// TODO(platform)
pub fn encode(payload: &PairingPayload) -> String {
    let _ = payload;
    String::new()
}

/// Decodes and validates a code. Tolerates surrounding whitespace and line
/// breaks introduced by copy and paste.
///
/// TODO(platform)
pub fn decode(code: &str) -> Result<PairingPayload, PairingError> {
    let _ = code;
    Err(PairingError::NotACode)
}

#[tauri::command]
pub fn pairing_code(state: State<'_, Arc<AppState>>) -> Result<PairingInfo, String> {
    // TODO(platform): pick a running data node, read /_status for node_id,
    // public key and ports, enumerate LAN addresses, encode.
    let _ = state;
    Err(PairingError::NoDataNode.to_string())
}

#[tauri::command]
pub fn join_mesh(app: AppHandle, state: State<'_, Arc<AppState>>, code: String) -> Result<JoinResult, String> {
    // TODO(platform): decode, reject self-pairing, persist bootstrap peers into
    // each local data node's node.json, POST /_peers live, trigger discovery.
    let _ = (app, state, code);
    Err("joining a mesh is not implemented yet".into())
}
