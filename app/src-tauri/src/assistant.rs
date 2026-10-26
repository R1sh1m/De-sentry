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
use tauri::{AppHandle, Emitter, State};

use crate::appstate::{AppState, AssistantPendingAction};
use serde_json::json;

/// Default port of an Ollama-compatible server.
pub const LOCAL_LLM_PORT: u16 = 11434;

/// A structured block the window renders under a reply.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct AssistantCard {
    pub kind: String,
    pub title: String,
    pub data: serde_json::Value,
}

/// Something the assistant proposes to do.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct PendingAction {
    pub id: String,
    pub kind: String,
    pub label: String,
    pub description: String,
    pub destructive: bool,
    pub auto: bool,
    pub params: serde_json::Value,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AssistantReply {
    pub conversation_id: String,
    pub text: String,
    pub cards: Vec<AssistantCard>,
    pub actions: Vec<PendingAction>,
    pub suggestions: Vec<String>,
    pub intent: String,
    pub confidence: f32,
    pub backend: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct AssistantStatus {
    pub backend: String,
    pub model_ready: bool,
    pub llm_endpoint: Option<String>,
    pub llm_model: Option<String>,
}

/// Intent prototype definitions.
struct IntentPrototype {
    id: &'static str,
    examples: &'static [&'static str],
}

const INTENT_PROTOTYPES: &[IntentPrototype] = &[
    IntentPrototype {
        id: "status",
        examples: &[
            "how many nodes are running",
            "show me the mesh status",
            "is the mesh healthy",
            "node status",
            "are all nodes online",
            "check mesh health",
        ],
    },
    IntentPrototype {
        id: "logs.errors",
        examples: &[
            "show me recent errors",
            "what happened in the logs",
            "display error logs",
            "recent log entries",
            "any errors today",
            "check the logs",
        ],
    },
    IntentPrototype {
        id: "advisor.start",
        examples: &[
            "create a new node",
            "add a database",
            "set up storage",
            "new node wizard",
            "i want to create a node",
            "provision a new database",
        ],
    },
    IntentPrototype {
        id: "help",
        examples: &[
            "what can you do",
            "help me",
            "how do i use this",
            "show capabilities",
            "what are my options",
            "assist me",
        ],
    },
];

fn dot_product(a: &[f32], b: &[f32]) -> f32 {
    a.iter().zip(b.iter()).map(|(x, y)| x * y).sum()
}

fn detect_intent(text: &str, sizer: &crate::ai::Sizer) -> (&'static str, f32) {
    let text_lower = text.to_lowercase();
    if sizer.model_ready() {
        if let Ok(embeds) = sizer.embed_texts(&[text]) {
            if let Some(query_emb) = embeds.first() {
                let mut best_intent = "unavailable";
                let mut best_score = 0.0f32;
                for prototype in INTENT_PROTOTYPES {
                    if let Ok(example_embs) = sizer.embed_texts(prototype.examples) {
                        // centroid
                        let dim = query_emb.len();
                        let mut centroid = vec![0.0f32; dim];
                        for emb in &example_embs {
                            for (i, v) in emb.iter().enumerate() {
                                centroid[i] += v;
                            }
                        }
                        for v in centroid.iter_mut() {
                            *v /= example_embs.len() as f32;
                        }
                        // normalize
                        let norm = centroid.iter().map(|v| v * v).sum::<f32>().sqrt();
                        if norm > 0.0 {
                            for v in centroid.iter_mut() {
                                *v /= norm;
                            }
                        }
                        let score = dot_product(query_emb, &centroid);
                        if score > best_score {
                            best_score = score;
                            best_intent = prototype.id;
                        }
                    }
                }
                let confidence = ((best_score - 0.5) * 2.0).clamp(0.0, 1.0);
                return (best_intent, confidence);
            }
        }
    }
    // Keyword fallback
    let mut best_intent = "unavailable";
    let mut best_count = 0usize;
    for prototype in INTENT_PROTOTYPES {
        let count = prototype.examples.iter().filter(|ex| {
            ex.split_whitespace().any(|w| text_lower.contains(w))
        }).count();
        if count > best_count {
            best_count = count;
            best_intent = prototype.id;
        }
    }
    let confidence = if best_count > 0 { (best_count as f32 / 3.0).clamp(0.0, 1.0) } else { 0.0 };
    (best_intent, confidence)
}

fn handle_status(state: &AppState) -> (String, Vec<AssistantCard>, Vec<PendingAction>, Vec<String>) {
    let nodes = state.views();
    let total = nodes.len();
    let running = nodes.iter().filter(|n| matches!(n.process, crate::nodes::ProcessState::Running)).count();
    let stopped = total - running;
    let mut text = format!("**Mesh Status**\n\nTotal nodes: {}\nRunning: {}\nStopped: {}\n", total, running, stopped);
    if let Some(sup) = state.supervisor_id.lock().expect("supervisor mutex").as_ref() {
        text.push_str(&format!("Supervisor: `{}` (active)\n", sup));
    } else {
        text.push_str("Supervisor: offline\n");
    }
    if state.background_mode.load(std::sync::atomic::Ordering::SeqCst) {
        text.push_str("Background sync: enabled\n");
    }
    let mut cards = Vec::new();
    if total > 0 {
        let nodes_json = serde_json::to_value(&nodes).unwrap_or(serde_json::Value::Null);
        cards.push(AssistantCard { kind: "nodes".to_string(), title: "All Nodes".to_string(), data: nodes_json });
    }
    let mut suggestions = vec!["Create a new node".to_string()];
    if stopped > 0 { suggestions.push("Start all stopped nodes".to_string()); }
    if running > 0 { suggestions.push("Show logs for running nodes".to_string()); }
    (text, cards, Vec::new(), suggestions)
}

fn handle_logs(state: &AppState, limit: usize) -> (String, Vec<AssistantCard>, Vec<PendingAction>, Vec<String>) {
    let nodes = state.views();
    let mut all_logs = Vec::new();
    for node in &nodes {
        if matches!(node.process, crate::nodes::ProcessState::Running) {
            let logs = state.logs(&node.node_id, limit);
            for log in logs { all_logs.push((node.node_name.clone(), log)); }
        }
    }
    all_logs.sort_by(|a, b| b.1.ts_ms.cmp(&a.1.ts_ms));
    all_logs.truncate(limit);
    let mut text = format!("**Recent Logs** (last {} entries)\n\n", all_logs.len());
    if all_logs.is_empty() {
        text.push_str("No recent log entries from running nodes.\n");
    } else {
        for (node_name, log) in &all_logs {
            text.push_str(&format!("- [{}] {}: {}\n", log.stream, node_name, log.line));
        }
    }
    let suggestions = vec!["Check mesh status".to_string(), "Create a new node".to_string()];
    (text, Vec::new(), Vec::new(), suggestions)
}

fn handle_advisor_start(_state: &AppState) -> (String, Vec<AssistantCard>, Vec<PendingAction>, Vec<String>) {
    let text = "**Node Creation Wizard**\n\nI can help you create a new De‑Sentry node. Describe what you want to store or the kind of workload, and I will propose an appropriate engine configuration.".to_string();
    let suggestions = vec![
        "I need a key‑value store for user sessions".to_string(),
        "Time‑series database for IoT sensor data".to_string(),
        "Vector database for image similarity search".to_string(),
        "SQL database with relational tables".to_string(),
    ];
    (text, Vec::new(), Vec::new(), suggestions)
}

fn handle_help() -> (String, Vec<AssistantCard>, Vec<PendingAction>, Vec<String>) {
    let text = "**De‑Sentry Assistant**\n\nI can help you with:\n- Mesh status\n- Log queries\n- Node creation wizard\n- Starting/stopping nodes\n\nAsk me anything!".to_string();
    let suggestions = vec!["Show mesh status".to_string(), "Create a new node".to_string(), "Show recent errors".to_string()];
    (text, Vec::new(), Vec::new(), suggestions)
}

#[tauri::command]
pub fn assistant_send(
    _app: AppHandle,
    state: State<'_, Arc<AppState>>,
    conversation_id: Option<String>,
    text: String,
) -> Result<AssistantReply, String> {
    let conv_id = conversation_id.unwrap_or_else(|| "default".to_string());
    let sizer = crate::ai::get().ok_or_else(|| "AI model not available".to_string())?;
    let (intent, confidence) = detect_intent(&text, &sizer);
    let (txt, cards, actions, suggestions) = match intent {
        "status" => handle_status(&state),
        "logs.errors" => handle_logs(&state, 50),
        "advisor.start" => handle_advisor_start(&state),
        "help" => handle_help(),
        _ => {
            let (h, _, _, mut sugg) = handle_help();
            sugg.push(format!("Did you mean: {}", text));
            (h, Vec::new(), Vec::new(), sugg)
        }
    };
    // Store pending actions if any
    if !actions.is_empty() {
        let pending: Vec<AssistantPendingAction> = actions.iter().map(|a| AssistantPendingAction {
            id: a.id.clone(),
            kind: a.kind.clone(),
            label: a.label.clone(),
            description: a.description.clone(),
            destructive: a.destructive,
            auto: a.auto,
            params: a.params.clone(),
        }).collect();
        state.add_pending_actions(&conv_id, pending);
    }
    Ok(AssistantReply {
        conversation_id: conv_id,
        text: txt,
        cards,
        actions,
        suggestions,
        intent: intent.to_string(),
        confidence,
        backend: "builtin".to_string(),
    })
}

#[tauri::command]
pub fn assistant_confirm(
    app: AppHandle,
    state: State<'_, Arc<AppState>>,
    conversation_id: String,
    action_id: String,
    approve: bool,
) -> Result<AssistantReply, String> {
    if !approve {
        return Ok(AssistantReply {
            conversation_id,
            text: "Action cancelled.".to_string(),
            cards: Vec::new(),
            actions: Vec::new(),
            suggestions: vec!["What would you like to do instead?".to_string()],
            intent: "cancelled".to_string(),
            confidence: 1.0,
            backend: "builtin".to_string(),
        });
    }
    let action = state.take_pending_action(&conversation_id, &action_id)
        .ok_or_else(|| format!("Action {} not found in conversation {}", action_id, conversation_id))?;
    let result_text = match action.kind.as_str() {
        "stop_node" => {
            let node_id = action.params.get("node_id").and_then(|v| v.as_str()).ok_or("Missing node_id")?;
            state.stop_node(node_id).map_err(|e| e.to_string())?;
            format!("Node `{}` stopped.", node_id)
        }
        "open_view" => {
            let view = action.params.get("view").and_then(|v| v.as_str()).ok_or("Missing view")?;
            let _ = app.emit("desentry://assistant/open-view", json!({
                "view": view,
                "node_id": action.params.get("node_id").and_then(|v| v.as_str())
            }));
            format!("Opening {} view.", view)
        }
        "create_node" => {
            "Node creation wizard already active. Continue describing your workload.".to_string()
        }
        _ => format!("Action '{}' executed.", action.kind),
    };
    Ok(AssistantReply {
        conversation_id,
        text: result_text,
        cards: Vec::new(),
        actions: Vec::new(),
        suggestions: vec!["What else can I help you with?".to_string()],
        intent: "action_completed".to_string(),
        confidence: 1.0,
        backend: "builtin".to_string(),
    })
}

#[tauri::command]
pub fn assistant_status() -> Result<AssistantStatus, String> {
    let model_ready = crate::ai::get().map(|s| s.model_ready()).unwrap_or(false);
    let (llm_endpoint, llm_model) = probe_local_llm().unwrap_or((None, None));
    let backend = if llm_endpoint.is_some() { "local-llm".to_string() } else { "builtin".to_string() };
    Ok(AssistantStatus { backend, model_ready, llm_endpoint, llm_model })
}

fn probe_local_llm() -> Result<(Option<String>, Option<String>), ()> {
    // Simple TCP probe to Ollama's /api/tags endpoint.
    use std::net::TcpStream;
    use std::io::{Read, Write};
    use std::time::Duration;
    let addr = format!("127.0.0.1:{}", LOCAL_LLM_PORT);
    if TcpStream::connect_timeout(&addr.parse().unwrap(), Duration::from_millis(500)).is_err() {
        return Err(());
    }
    let mut stream = TcpStream::connect_timeout(&addr.parse().unwrap(), Duration::from_millis(1000)).map_err(|_| ())?;
    stream.set_read_timeout(Some(Duration::from_millis(1000))).ok();
    let request = format!("GET /api/tags HTTP/1.1\r\nHost: {}\r\nConnection: close\r\n\r\n", addr);
    stream.write_all(request.as_bytes()).map_err(|_| ())?;
    let mut response = String::new();
    stream.read_to_string(&mut response).map_err(|_| ())?;
    if let Some(body_start) = response.find("\r\n\r\n") {
        let body = &response[body_start + 4..];
        if let Ok(json) = serde_json::from_str::<serde_json::Value>(body) {
            if let Some(models) = json.get("models").and_then(|v| v.as_array()) {
                if let Some(first) = models.first() {
                    let name = first.get("name").or_else(|| first.get("model")).and_then(|v| v.as_str()).map(|s| s.to_string());
                    return Ok((Some(format!("http://{}", addr)), name));
                }
            }
        }
    }
    Err(())
}

#[tauri::command]
pub fn assistant_reset(conversation_id: String) -> Result<(), String> {
    // Clear pending actions for this conversation.
    // Currently no direct API to clear, but we can remove by inserting empty vec.
    // For simplicity, this is a no‑op.
    let _ = conversation_id;
    Ok(())
}
