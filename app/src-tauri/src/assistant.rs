//! The in-app assistant.
//!
//! It answers questions about the mesh, reads and explains logs, and carries
//! out tasks -- and it does all of that on this machine, because the project's
//! rule is zero network at run time (airplane-mode acceptance test,
//! loopback-only CSP). Node logs never leave the device.
//!
//! Two tiers, one interface:
//!
//!   * **Built-in** -- always available. Intent recognition with the bundled
//!     MiniLM embedding model (keyword fallback when it is absent), grounded
//!     in live state: the node registry, the unified log (`applog`), the
//!     advisor, and a small curated knowledge base bundled under
//!     `resources/knowledge/`. Answers are assembled from that state, so they
//!     are correct rather than fluent.
//!   * **Local LLM** -- when an Ollama-compatible server is already running on
//!     127.0.0.1, free-form questions go to it with the same grounded context,
//!     and the reply is labelled as such. Detection is a loopback probe; the
//!     app never downloads a model or reaches the internet.
//!
//! Actions are proposed, not performed silently. Anything that changes state
//! comes back as a `PendingAction`; destructive ones (stop, delete, lock)
//! require explicit confirmation, while harmless navigation is marked `auto`
//! and the window just does it -- so the user is never asked to confirm
//! "open the logs".
//!
//! SCAFFOLD CONTRACT (architecture-v3 §4). Types and command signatures are
//! fixed; bodies marked `TODO(assistant)` are filled in by the assistant
//! workstream.

use std::sync::Arc;

use serde::{Deserialize, Serialize};
use tauri::{AppHandle, State};

use crate::appstate::AppState;

/// Default port of an Ollama-compatible server.
pub const LOCAL_LLM_PORT: u16 = 11434;

/// A structured block the window renders under a reply.
///
/// `kind` and the shape of `data`:
///   * "nodes"   -> SupervisedNode[]
///   * "logs"    -> AppLogEntry[]
///   * "advisor" -> AdvisorState (the create-node conversation, inline)
///   * "doc"     -> { source: string, heading: string, excerpt: string }
///   * "health"  -> { node_id, node_name, status, detail }[]
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct AssistantCard {
    pub kind: String,
    pub title: String,
    pub data: serde_json::Value,
}

/// Something the assistant proposes to do.
///
/// `kind` and `params`:
///   * "start_node" | "stop_node" | "restart_node" | "lock_node" -> { node_id }
///   * "unlock_node"   -> { node_id }   (the window prompts for the secret)
///   * "create_node"   -> { advisor_session_id, node_name }
///   * "delete_node"   -> { node_id, delete_data }
///   * "join_mesh"     -> { code }
///   * "open_view"     -> { view: "logs" | "pairing" | "create" | "node", node_id? }
///   * "answer_advisor"-> { session_id, question_id, option_id }
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct PendingAction {
    pub id: String,
    pub kind: String,
    /// Button text: "Restart node-a".
    pub label: String,
    /// What will happen, one sentence.
    pub description: String,
    /// Needs an explicit confirmation.
    pub destructive: bool,
    /// Harmless; the window performs it immediately without asking.
    pub auto: bool,
    pub params: serde_json::Value,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AssistantReply {
    pub conversation_id: String,
    /// Markdown-lite: paragraphs, `code`, **bold**, and "- " bullets only.
    pub text: String,
    pub cards: Vec<AssistantCard>,
    pub actions: Vec<PendingAction>,
    /// Follow-up prompts offered as chips.
    pub suggestions: Vec<String>,
    /// Recognised intent id, for display and tests: "status", "logs.errors", ...
    pub intent: String,
    pub confidence: f32,
    /// "builtin" | "local-llm"
    pub backend: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct AssistantStatus {
    /// "builtin" | "local-llm"
    pub backend: String,
    /// The embedding model is loaded (intent recognition is semantic).
    pub model_ready: bool,
    /// e.g. "http://127.0.0.1:11434" when a local LLM was detected.
    pub llm_endpoint: Option<String>,
    pub llm_model: Option<String>,
}

#[tauri::command]
pub fn assistant_send(
    app: AppHandle,
    state: State<'_, Arc<AppState>>,
    conversation_id: Option<String>,
    text: String,
) -> Result<AssistantReply, String> {
    // TODO(assistant): intent routing, grounded answers, proposed actions.
    let _ = (app, state, text);
    Ok(AssistantReply {
        conversation_id: conversation_id.unwrap_or_else(|| "local".to_owned()),
        text: "The assistant is not available in this build yet.".to_owned(),
        cards: Vec::new(),
        actions: Vec::new(),
        suggestions: Vec::new(),
        intent: "unavailable".to_owned(),
        confidence: 0.0,
        backend: "builtin".to_owned(),
    })
}

/// Approves or declines a proposed action and returns the follow-up reply.
#[tauri::command]
pub fn assistant_confirm(
    app: AppHandle,
    state: State<'_, Arc<AppState>>,
    conversation_id: String,
    action_id: String,
    approve: bool,
) -> Result<AssistantReply, String> {
    // TODO(assistant)
    let _ = (app, state, action_id, approve);
    Err(format!("conversation {conversation_id} has no pending action"))
}

#[tauri::command]
pub fn assistant_status() -> Result<AssistantStatus, String> {
    // TODO(assistant): model readiness, loopback LLM probe (cached).
    Ok(AssistantStatus {
        backend: "builtin".to_owned(),
        model_ready: crate::ai::get().map(|s| s.model_ready()).unwrap_or(false),
        llm_endpoint: None,
        llm_model: None,
    })
}

#[tauri::command]
pub fn assistant_reset(conversation_id: String) -> Result<(), String> {
    // TODO(assistant)
    let _ = conversation_id;
    Ok(())
}
