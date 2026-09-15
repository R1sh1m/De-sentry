//! The adaptive engine advisor.
//!
//! Replaces "classify the description into one of seven prototypes and hope
//! the winner clears 0.35" -- a design that structurally cannot be confident
//! about real workloads, because real workloads are mixtures. "Users, their
//! orders, and a dashboard of daily revenue" is relational *and* analytical; a
//! softmax over seven exclusive shapes splits the probability between them and
//! the top-1 lands below the floor every time.
//!
//! The advisor instead reasons over **facets** -- independent dimensions of a
//! workload (data model, access pattern, write profile, consistency, retention,
//! scale) -- each holding its own probability distribution built from evidence:
//! embedding similarity to per-value exemplars, keyword matches, and answers.
//! Engines are then scored against the facet posteriors through a capability
//! matrix, so the recommendation is multi-engine by construction and every
//! engine's reasons point back at the evidence that produced them.
//!
//! When the decision is uncertain it does not stop and hand the user a picker.
//! It asks the single question whose answer is expected to raise decision
//! confidence the most (expected value of information over the facets), with
//! options limited to the values still plausible, phrased with the user's own
//! nouns. Answers update the facet posteriors directly -- not by appending text
//! and re-embedding, which could leave an answer with no effect. It stops
//! asking, and marks the state `ready`, as soon as confidence reaches the
//! target, no remaining question is informative, or MAX_QUESTIONS were asked.
//! `ready` is the window's signal to proceed automatically.
//!
//! SCAFFOLD CONTRACT (architecture-v3 §3). Types and command signatures are
//! fixed; bodies marked `TODO(advisor)` are filled in by the advisor
//! workstream. Until then the stub maps the existing `ai::Sizer` output onto
//! the new shape, so the window can be built against it.

use std::collections::HashMap;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, OnceLock};

use serde::{Deserialize, Serialize};
use tauri::State;

use crate::ai::{self, NodeSpec};
use crate::appstate::AppState;

/// Decision confidence at which the advisor stops asking and marks `ready`.
pub const TARGET_CONFIDENCE: f32 = 0.80;
/// Most questions asked in one session. Past this the best current answer wins.
pub const MAX_QUESTIONS: usize = 4;
/// Sessions kept in memory; the oldest is dropped past this.
pub const MAX_SESSIONS: usize = 32;
/// Option id that skips a question without answering it.
pub const SKIP_OPTION: &str = "skip";

// -- the shapes the window sees ---------------------------------------------

/// One possible value of a facet and its current probability.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct FacetValue {
    pub id: String,
    pub label: String,
    pub probability: f32,
}

/// One dimension of the workload.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct FacetState {
    /// "data_model" | "access" | "writes" | "consistency" | "retention" | "scale"
    pub id: String,
    pub label: String,
    /// Values ordered by probability, highest first.
    pub values: Vec<FacetValue>,
    /// 1 - normalised entropy: 0 knows nothing, 1 is certain.
    pub certainty: f32,
    /// Phrases that moved this facet: quoted spans of the description, or
    /// "you answered: ...".
    pub evidence: Vec<String>,
    /// True once the user answered a question about this facet.
    pub answered: bool,
}

/// One engine's standing.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct EngineRecommendation {
    pub engine: String,
    /// Display name, e.g. "Time series".
    pub label: String,
    /// "primary" | "secondary" | "not-needed"
    pub role: String,
    /// Expected utility under the facet posteriors, 0..1.
    pub utility: f32,
    /// Plain sentences citing evidence: "You mentioned 'readings every
    /// second', which is append-heavy time-ordered data."
    pub reasons: Vec<String>,
    /// False when this build of desentryd does not contain the engine.
    pub available: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct QuestionOption {
    pub id: String,
    pub label: String,
    /// One short line under the label.
    pub hint: String,
}

/// The next thing worth asking.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct AdvisorQuestion {
    pub id: String,
    /// The facet this question resolves.
    pub facet: String,
    pub prompt: String,
    /// Why it matters, in one sentence: "This decides between the relational
    /// and the document engine."
    pub why: String,
    /// Only values still plausible, most likely first. 2 to 5 entries.
    pub options: Vec<QuestionOption>,
    /// Expected rise in decision confidence if answered, 0..1.
    pub expected_gain: f32,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct AdvisorAnswer {
    pub question_id: String,
    pub facet: String,
    /// An option id, or SKIP_OPTION.
    pub option_id: String,
}

/// Everything the window renders for a sizing session.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AdvisorState {
    pub session_id: String,
    pub description: String,
    /// "onnx" when the embedding model contributed evidence, "keyword" otherwise.
    pub method: String,
    /// Why the model was not used. Empty when it was.
    pub fallback_reason: String,
    pub facets: Vec<FacetState>,
    /// Every known engine, primary first, then by utility.
    pub engines: Vec<EngineRecommendation>,
    /// Probability that the primary engine choice is right, 0..1.
    pub confidence: f32,
    pub target_confidence: f32,
    /// None when nothing is worth asking.
    pub next_question: Option<AdvisorQuestion>,
    pub answered: Vec<AdvisorAnswer>,
    pub questions_remaining: usize,
    /// Proceed without further input: confident, out of useful questions, or
    /// out of question budget. The window auto-advances on this.
    pub ready: bool,
    /// Two or three sentences a person can read: what was understood and why
    /// these engines.
    pub summary: String,
    /// A node name suggested from the description, e.g. "sensor-readings".
    pub suggested_name: String,
    /// Drop-in for `create_node`: engines, split, collections, and the decision
    /// record written to the node manifest.
    pub spec: NodeSpec,
}

// -- sessions ----------------------------------------------------------------

#[derive(Debug, Clone)]
struct Session {
    description: String,
    quota_mb: u64,
    answers: Vec<AdvisorAnswer>,
    /// Engines forced by the user, if any.
    engines_override: Option<Vec<String>>,
    last: Option<AdvisorState>,
    touched: u64,
}

fn sessions() -> &'static Mutex<HashMap<String, Session>> {
    static SESSIONS: OnceLock<Mutex<HashMap<String, Session>>> = OnceLock::new();
    SESSIONS.get_or_init(|| Mutex::new(HashMap::new()))
}

static CLOCK: AtomicU64 = AtomicU64::new(1);

fn new_session_id() -> String {
    let mut bytes = [0u8; 8];
    let _ = getrandom::getrandom(&mut bytes);
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn remember(id: &str, mut session: Session) {
    let mut map = sessions().lock().expect("advisor sessions");
    session.touched = CLOCK.fetch_add(1, Ordering::Relaxed);
    map.insert(id.to_owned(), session);
    if map.len() > MAX_SESSIONS {
        if let Some(oldest) = map.iter().min_by_key(|(_, s)| s.touched).map(|(k, _)| k.clone()) {
            map.remove(&oldest);
        }
    }
}

fn session(id: &str) -> Result<Session, String> {
    sessions()
        .lock()
        .expect("advisor sessions")
        .get(id)
        .cloned()
        .ok_or_else(|| "that sizing session has expired; describe the workload again".to_owned())
}

// -- evaluation ---------------------------------------------------------------

/// Runs the advisor for a session's current inputs.
///
/// TODO(advisor): facet posteriors from description + answers, engine
/// utilities, decision confidence, next question by expected gain.
fn evaluate(
    state: &AppState,
    session_id: &str,
    session: &Session,
) -> Result<AdvisorState, String> {
    let sizer = ai::get().ok_or_else(|| "the workload prototypes are missing from this installation".to_owned())?;
    let available = crate::commands::engine_names(state);
    let mut spec = sizer.size(&session.description, session.quota_mb, &available);
    if let Some(engines) = &session.engines_override {
        if !engines.is_empty() {
            spec.engines = engines.clone();
            if !spec.engines.contains(&spec.default_engine) {
                spec.default_engine = spec.engines[0].clone();
            }
        }
    }
    let decision = spec.decision.clone();
    let engines = spec
        .engines
        .iter()
        .map(|engine| EngineRecommendation {
            engine: engine.clone(),
            label: engine.clone(),
            role: if *engine == spec.default_engine { "primary".into() } else { "secondary".into() },
            utility: decision.confidence,
            reasons: Vec::new(),
            available: available.iter().any(|a| a == engine),
        })
        .collect();
    Ok(AdvisorState {
        session_id: session_id.to_owned(),
        description: session.description.clone(),
        method: decision.method.clone(),
        fallback_reason: decision.fallback_reason.clone(),
        facets: Vec::new(),
        engines,
        confidence: decision.confidence,
        target_confidence: TARGET_CONFIDENCE,
        next_question: None,
        answered: session.answers.clone(),
        questions_remaining: MAX_QUESTIONS.saturating_sub(session.answers.len()),
        ready: session.engines_override.is_some() || decision.confidence >= decision.confidence_floor,
        summary: decision.reasoning.as_ref().map(|r| r.summary.clone()).unwrap_or_default(),
        suggested_name: String::new(),
        spec,
    })
}

fn run(state: &AppState, id: &str, mut session: Session) -> Result<AdvisorState, String> {
    let result = evaluate(state, id, &session)?;
    session.last = Some(result.clone());
    remember(id, session);
    Ok(result)
}

// -- public API (also used by the assistant) ----------------------------------

pub fn start(state: &AppState, description: &str, quota_mb: u64) -> Result<AdvisorState, String> {
    let id = new_session_id();
    let session = Session {
        description: description.trim().to_owned(),
        quota_mb,
        answers: Vec::new(),
        engines_override: None,
        last: None,
        touched: 0,
    };
    run(state, &id, session)
}

pub fn answer(state: &AppState, session_id: &str, question_id: &str, option_id: &str) -> Result<AdvisorState, String> {
    let mut session = session(session_id)?;
    let facet = session
        .last
        .as_ref()
        .and_then(|last| last.next_question.as_ref())
        .filter(|question| question.id == question_id)
        .map(|question| question.facet.clone())
        .unwrap_or_default();
    session.answers.retain(|a| a.question_id != question_id);
    session.answers.push(AdvisorAnswer {
        question_id: question_id.to_owned(),
        facet,
        option_id: option_id.to_owned(),
    });
    run(state, session_id, session)
}

pub fn refine(state: &AppState, session_id: &str, description: &str) -> Result<AdvisorState, String> {
    let mut session = session(session_id)?;
    session.description = description.trim().to_owned();
    run(state, session_id, session)
}

pub fn set_engines(state: &AppState, session_id: &str, engines: Vec<String>) -> Result<AdvisorState, String> {
    let mut session = session(session_id)?;
    session.engines_override = if engines.is_empty() { None } else { Some(engines) };
    run(state, session_id, session)
}

// -- commands ------------------------------------------------------------------

#[tauri::command]
pub fn advisor_start(state: State<'_, Arc<AppState>>, description: String, quota_mb: u64) -> Result<AdvisorState, String> {
    start(state.inner(), &description, quota_mb)
}

#[tauri::command]
pub fn advisor_answer(
    state: State<'_, Arc<AppState>>,
    session_id: String,
    question_id: String,
    option_id: String,
) -> Result<AdvisorState, String> {
    answer(state.inner(), &session_id, &question_id, &option_id)
}

/// The user edited the description. Earlier answers are kept.
#[tauri::command]
pub fn advisor_refine(state: State<'_, Arc<AppState>>, session_id: String, description: String) -> Result<AdvisorState, String> {
    refine(state.inner(), &session_id, &description)
}

/// Manual override. An empty list clears it and returns to the recommendation.
#[tauri::command]
pub fn advisor_set_engines(
    state: State<'_, Arc<AppState>>,
    session_id: String,
    engines: Vec<String>,
) -> Result<AdvisorState, String> {
    set_engines(state.inner(), &session_id, engines)
}
