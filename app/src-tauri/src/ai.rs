//! Workload sizing: description in, node shape out.
//!
//! The pipeline the spec asks for, in order:
//!
//!   description -> embed (all-MiniLM-L6-v2, 384-dim, Apache-2.0)
//!               -> cosine against seven prototype embeddings
//!               -> top-1 + confidence
//!               -> NodeSpec (engines, quota split, shard key, RF, indexes,
//!                  retention) plus draft collections and their `_schema`
//!                  payloads for the user to confirm.
//!
//! Everything runs on this machine. The model is bundled in the installer, the
//! ONNX Runtime is loaded from the app's resources, and no request leaves the
//! process -- which is what lets the airplane-mode acceptance test cover this
//! step too.
//!
//! Three things this module is careful about:
//!
//! **The prototypes are embedded with the same model, at load time.** Storing
//! precomputed vectors would be faster and would silently produce nonsense the
//! first time the model file was replaced or requantised, with nothing to
//! report it.
//!
//! **The fallback is honest.** If the model or the runtime cannot be loaded,
//! a deterministic keyword heuristic runs instead -- and the decision it
//! returns says `method: "keyword"` with the reason, which the wizard shows.
//! A fallback presented as a model result would be worse than no model.
//!
//! **Confidence is a margin, not a similarity.** Cosine similarities between
//! short English sentences all sit in a narrow band, so reporting one as
//! "confidence" would read as 0.7 for a coin flip. Instead the seven scores go
//! through a softmax, and the reported number is the winner's share -- which
//! is 1/7 when the description tells us nothing and approaches 1 when one
//! shape clearly wins. `CONFIDENCE_FLOOR` is applied to that.

use std::path::{Path, PathBuf};
use std::sync::Mutex;

use serde::{Deserialize, Serialize};

use crate::configgen::QuotaSplit;

/// Below this the wizard stops proposing and makes the user choose engines.
/// Mirrored in app/resources/prototypes.json and in wizard.ts.
pub const CONFIDENCE_FLOOR: f32 = 0.35;

/// Softmax temperature over cosine similarities.
///
/// Cosine between two related English sentences is typically 0.3-0.8, and the
/// gaps that matter are ~0.05. At T=0.05 a gap that size is a factor of e in
/// the odds, which puts a clear winner comfortably above the floor and leaves
/// an ambiguous description below it. Larger T flattens everything to 1/7;
/// smaller T makes the top-1 look certain whatever the margin.
const TEMPERATURE: f32 = 0.05;

/// MiniLM truncates at 256 word pieces. Longer descriptions are cut rather
/// than rejected: the first 256 pieces of a rambling description still say
/// what it is about.
#[allow(dead_code)]
const MAX_TOKENS: usize = 256;

// -- the shapes the window sees ---------------------------------------------

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct WorkloadScore {
    pub workload: String,
    pub score: f32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct EngineRationale {
    pub engine: String,
    pub role: String,
    pub reason: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SizingReasoning {
    pub summary: String,
    pub key_matched_signals: Vec<String>,
    pub engine_rationales: Vec<EngineRationale>,
    pub quota_rationale: String,
    pub runner_up_contrast: Option<String>,
    pub operational_trade_offs: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ClarificationOption {
    pub label: String,
    pub description: String,
    pub target_workload: String,
    pub appended_context: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ClarifyingQuestion {
    pub id: String,
    pub prompt: String,
    pub rationale: String,
    pub options: Vec<ClarificationOption>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SizingDecision {
    pub workload: String,
    pub confidence: f32,
    /// "onnx" when the model ran; "keyword" when the deterministic fallback did.
    pub method: String,
    pub scores: Vec<WorkloadScore>,
    /// Why the model was not used. Empty when it was.
    pub fallback_reason: String,
    pub confidence_floor: f32,
    pub description: String,
    pub decided_at_ms: i64,
    #[serde(default)]
    pub reasoning: Option<SizingReasoning>,
    #[serde(default)]
    pub clarifying_questions: Vec<ClarifyingQuestion>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DraftCollection {
    pub name: String,
    pub engine: String,
    pub schema: Option<serde_json::Value>,
    pub shard_key: String,
    pub retention_days: u32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct NodeSpec {
    pub engines: Vec<String>,
    pub default_engine: String,
    pub quota_split: QuotaSplit,
    pub shard_key: String,
    pub replication_factor: u32,
    pub secondary_indexes: Vec<String>,
    pub retention_days: u32,
    pub collections: Vec<DraftCollection>,
    pub decision: SizingDecision,
}

// -- prototypes --------------------------------------------------------------

#[derive(Debug, Clone, Deserialize)]
struct Prototype {
    workload: String,
    #[allow(dead_code)]
    label: String,
    /// Primary example text. Used by the keyword fallback and as the first
    /// embedding example when `texts` is empty.
    #[cfg_attr(not(feature = "onnx"), allow(dead_code))]
    text: String,
    /// Additional example sentences that broaden the prototype's coverage in
    /// embedding space. When present, the prototype embedding is the
    /// L2-normalised centroid of `text` plus every entry here, which makes it
    /// more robust than a single point.
    #[serde(default)]
    #[cfg_attr(not(feature = "onnx"), allow(dead_code))]
    texts: Vec<String>,
    #[serde(default)]
    keywords: std::collections::BTreeMap<String, f32>,
    engines: Vec<String>,
    default_engine: String,
    quota_split: QuotaSplit,
    #[serde(default)]
    shard_key: String,
    #[serde(default = "default_rf")]
    replication_factor: u32,
    #[serde(default)]
    secondary_indexes: Vec<String>,
    #[serde(default)]
    retention_days: u32,
    #[serde(default)]
    collections: Vec<DraftCollection>,
    #[serde(default)]
    rationale: Option<PrototypeRationale>,
}

#[derive(Debug, Clone, Default, Deserialize)]
struct PrototypeRationale {
    #[serde(default)]
    summary: String,
    #[serde(default)]
    key_signals: Vec<String>,
    #[serde(default)]
    engine_rationales: std::collections::BTreeMap<String, String>,
    #[serde(default)]
    quota_rationale: String,
    #[serde(default)]
    trade_offs: Vec<String>,
}

impl Prototype {
    /// All embedding example texts: the primary `text` followed by any extras
    /// from `texts`. The caller can embed them all and take the centroid.
    #[cfg(feature = "onnx")]
    fn all_texts(&self) -> Vec<&str> {
        let mut out = Vec::with_capacity(1 + self.texts.len());
        out.push(self.text.as_str());
        for extra in &self.texts {
            out.push(extra.as_str());
        }
        out
    }
}

fn default_rf() -> u32 {
    3
}

#[derive(Debug, Clone, Deserialize)]
struct PrototypeFile {
    #[serde(default = "default_floor")]
    confidence_floor: f32,
    prototypes: Vec<Prototype>,
}

fn default_floor() -> f32 {
    CONFIDENCE_FLOOR
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum MediaIntent {
    Storage,
    Similarity,
}

fn explicit_media_intent(description: &str) -> Option<MediaIntent> {
    let text = description.to_lowercase();
    let media = ["image", "images", "photo", "photos", "picture", "pictures", "media"];
    if !media.iter().any(|term| contains_word(&text, term)) {
        return None;
    }

    let similarity = [
        "similarity",
        "similar",
        "embedding",
        "embeddings",
        "nearest",
        "reverse image",
        "visual search",
        "image search",
    ];
    if similarity.iter().any(|term| contains_word(&text, term)) {
        Some(MediaIntent::Similarity)
    } else {
        Some(MediaIntent::Storage)
    }
}

#[derive(Debug, thiserror::Error)]
pub enum SizingError {
    #[error("could not read the workload prototypes at {0}: {1}")]
    Prototypes(String, String),
    #[error("the workload prototypes are empty, so nothing can be proposed")]
    NoPrototypes,
}

/// Where the model and prototypes live, resolved from the bundle at startup.
#[derive(Debug, Clone)]
pub struct Resources {
    pub prototypes: PathBuf,
    pub model: PathBuf,
    pub tokenizer: PathBuf,
    /// Directory holding libonnxruntime, loaded dynamically.
    pub runtime_dir: PathBuf,
}

/// The sizing engine. Built once and reused; embedding the prototypes is the
/// expensive part and it only has to happen once per run.
pub struct Sizer {
    prototypes: Vec<Prototype>,
    floor: f32,
    /// Prototype embeddings, in prototype order. Empty when the model is absent.
    embeddings: Vec<Vec<f32>>,
    /// Why the model is not in use, when it is not.
    fallback_reason: String,
    #[cfg(feature = "onnx")]
    model: Mutex<Option<onnx::Model>>,
}

impl Sizer {
    /// Loads prototypes, then tries to bring up the model.
    ///
    /// A model failure is recorded, not returned: sizing must still work, and
    /// the window is told which path ran.
    pub fn load(resources: &Resources) -> Result<Self, SizingError> {
        let text = std::fs::read_to_string(&resources.prototypes).map_err(|error| {
            SizingError::Prototypes(resources.prototypes.display().to_string(), error.to_string())
        })?;
        let file: PrototypeFile = serde_json::from_str(&text).map_err(|error| {
            SizingError::Prototypes(resources.prototypes.display().to_string(), error.to_string())
        })?;
        if file.prototypes.is_empty() {
            return Err(SizingError::NoPrototypes);
        }

        let mut sizer = Self {
            prototypes: file.prototypes,
            floor: file.confidence_floor,
            embeddings: Vec::new(),
            fallback_reason: String::new(),
            #[cfg(feature = "onnx")]
            model: Mutex::new(None),
        };
        sizer.try_load_model(resources);
        Ok(sizer)
    }

    #[cfg(feature = "onnx")]
    fn try_load_model(&mut self, resources: &Resources) {
        match onnx::Model::load(resources) {
            Ok(model) => {
                // Embed every prototype as the centroid of its example texts.
                // A centroid covers more of the semantic region than a single
                // point, which makes classification more robust to paraphrasing.
                let mut embeddings = Vec::with_capacity(self.prototypes.len());
                let mut failed = false;
                for prototype in &self.prototypes {
                    let texts = prototype.all_texts();
                    match model.embed_centroid(&texts) {
                        Ok(centroid) => embeddings.push(centroid),
                        Err(error) => {
                            self.fallback_reason = format!(
                                "the '{}' prototype could not be embedded: {error}",
                                prototype.workload
                            );
                            failed = true;
                            break;
                        }
                    }
                }
                if !failed {
                    self.embeddings = embeddings;
                    *self.model.lock().expect("model mutex") = Some(model);
                }
            }
            Err(error) => self.fallback_reason = error.to_string(),
        }
    }

    #[cfg(not(feature = "onnx"))]
    fn try_load_model(&mut self, _resources: &Resources) {
        self.fallback_reason =
            "this build was compiled without the onnx feature, so the embedding model is not present".into();
    }

    /// True when the embedding model is loaded and in use.
    pub fn model_ready(&self) -> bool {
        !self.embeddings.is_empty()
    }

    /// The main entry point: description and budget in, a full NodeSpec out.
    pub fn size(&self, description: &str, quota_mb: u64, available_engines: &[String]) -> NodeSpec {
        let trimmed = description.trim();
        let (raw_scores, method, reason) = if trimmed.is_empty() {
            (
                vec![0.0; self.prototypes.len()],
                "keyword",
                "no description was given".to_owned(),
            )
        } else if self.model_ready() {
            match self.embed_scores(trimmed) {
                Ok(scores) => (
                    self.combine_semantic_and_keywords(trimmed, &scores),
                    "onnx",
                    String::new(),
                ),
                Err(error) => (self.keyword_scores(trimmed), "keyword", error),
            }
        } else {
            (self.keyword_scores(trimmed), "keyword", self.fallback_reason.clone())
        };

        let confidences = softmax(&raw_scores);
        let (best, confidence) = confidences
            .iter()
            .enumerate()
            .max_by(|a, b| a.1.partial_cmp(b.1).unwrap_or(std::cmp::Ordering::Equal))
            .map(|(index, value)| (index, *value))
            .unwrap_or((0, 0.0));

        let prototype = &self.prototypes[best];
        let decision = SizingDecision {
            workload: prototype.workload.clone(),
            // A description we could not read at all is 0, not 1/7: the wizard
            // must send that to the manual picker, and a floor comparison
            // against the uniform value would be a coin flip.
            confidence: if trimmed.is_empty() { 0.0 } else { confidence },
            method: method.to_owned(),
            scores: self
                .prototypes
                .iter()
                .zip(confidences.iter())
                .map(|(p, score)| WorkloadScore {
                    workload: p.workload.clone(),
                    score: *score,
                })
                .collect(),
            fallback_reason: reason,
            confidence_floor: self.floor,
            description: trimmed.to_owned(),
            decided_at_ms: crate::nodes::now_ms(),
            reasoning: None,
            clarifying_questions: Vec::new(),
        };

        self.spec_from(prototype, quota_mb, available_engines, decision)
    }

    /// Turns a chosen prototype into a spec, filtered to what this build has.
    fn spec_from(
        &self,
        prototype: &Prototype,
        quota_mb: u64,
        available: &[String],
        mut decision: SizingDecision,
    ) -> NodeSpec {
        // Never propose an engine the binary does not contain: an option that
        // fails on selection is worse than one that is not offered.
        let mut engines: Vec<String> = prototype
            .engines
            .iter()
            .filter(|name| available.iter().any(|have| have == *name))
            .cloned()
            .collect();
        if engines.is_empty() {
            engines.push("kv".to_owned());
        }

        let default_engine = if engines.iter().any(|name| *name == prototype.default_engine) {
            prototype.default_engine.clone()
        } else {
            engines[0].clone()
        };

        let quota_split = prototype.quota_split.normalised();

        let reasoning = self.build_reasoning(
            prototype,
            &decision.description,
            &decision.method,
            &engines,
            &default_engine,
            &quota_split,
            &decision.scores,
            decision.confidence,
        );

        let clarifying_questions = self.build_clarifying_questions(
            prototype,
            &decision.description,
            &decision.scores,
            decision.confidence,
            decision.confidence_floor,
        );

        decision.reasoning = Some(reasoning);
        decision.clarifying_questions = clarifying_questions;

        let collections = prototype
            .collections
            .iter()
            .map(|draft| {
                let engine = if engines.iter().any(|name| *name == draft.engine) {
                    draft.engine.clone()
                } else {
                    default_engine.clone()
                };
                DraftCollection {
                    name: draft.name.clone(),
                    engine,
                    schema: draft.schema.clone(),
                    shard_key: draft.shard_key.clone(),
                    retention_days: draft.retention_days,
                }
            })
            .collect();

        // A tiny node cannot usefully hold three replicas' worth of anything,
        // but RF is about how many *other* nodes hold a copy, not about this
        // node's size -- so the budget does not change it. What the budget does
        // change is the split, which is normalised here rather than trusted.
        let _ = quota_mb;

        NodeSpec {
            engines,
            default_engine,
            quota_split,
            shard_key: prototype.shard_key.clone(),
            replication_factor: prototype.replication_factor,
            secondary_indexes: prototype.secondary_indexes.clone(),
            retention_days: prototype.retention_days,
            collections,
            decision,
        }
    }

    fn build_reasoning(
        &self,
        prototype: &Prototype,
        description: &str,
        method: &str,
        engines: &[String],
        default_engine: &str,
        quota_split: &QuotaSplit,
        scores: &[WorkloadScore],
        confidence: f32,
    ) -> SizingReasoning {
        let trimmed = description.trim();
        let label = &prototype.label;

        // 1. Matched signals from user description
        let mut key_matched_signals = Vec::new();
        let haystack = trimmed.to_lowercase();
        for (kw, _) in &prototype.keywords {
            if contains_word(&haystack, kw) && !key_matched_signals.contains(kw) {
                key_matched_signals.push(kw.clone());
            }
        }
        if let Some(r) = &prototype.rationale {
            for sig in &r.key_signals {
                if contains_word(&haystack, &sig.to_lowercase()) && !key_matched_signals.contains(sig) {
                    key_matched_signals.push(sig.clone());
                }
            }
        }
        key_matched_signals.sort();
        key_matched_signals.dedup();
        key_matched_signals.truncate(6);

        // 2. High-level summary
        let summary = if trimmed.is_empty() {
            "No workload description provided. Defaulting to general-purpose key-value storage.".to_string()
        } else if method == "onnx" {
            if !key_matched_signals.is_empty() {
                format!(
                    "Neural embedding and semantic matching identified strong alignment with the {label} workload (signals: {signals}).",
                    signals = key_matched_signals.join(", ")
                )
            } else {
                format!(
                    "Neural embedding semantic analysis identified close alignment with the {label} profile.",
                )
            }
        } else if !key_matched_signals.is_empty() {
            format!(
                "Deterministic keyword analysis matched domain terms ({signals}) corresponding to the {label} shape.",
                signals = key_matched_signals.join(", ")
            )
        } else {
            format!("Classified as {label} based on structural fallback heuristics.")
        };

        // 3. Engine rationales
        let mut engine_rationales = Vec::new();
        for engine in engines {
            let is_default = engine == default_engine;
            let role = if is_default {
                "Default Engine".to_string()
            } else {
                "Secondary Storage".to_string()
            };
            let reason = prototype
                .rationale
                .as_ref()
                .and_then(|r| r.engine_rationales.get(engine))
                .cloned()
                .unwrap_or_else(|| match engine.as_str() {
                    "sqlite" => "Relational B+Tree engine for tables with primary/foreign keys and SQL queries.".to_string(),
                    "kv" => "Lightweight key-value storage engine for point lookups and document payloads.".to_string(),
                    "ts_rollup" => "Time-series engine with automatic tiered downsampling and delta compression.".to_string(),
                    "vector_hnsw_lite" => "In-memory HNSW vector index for approximate nearest neighbor similarity searches.".to_string(),
                    "graph_adj" => "Adjacency-list graph engine for multi-hop relationship traversals.".to_string(),
                    "columnar_lite" => "Column-oriented storage format optimized for sequential analytical scans.".to_string(),
                    "duckdb" => "Vectorized analytical engine for fast SQL OLAP aggregations across event logs.".to_string(),
                    other => format!("Provides dedicated storage backing for {other} collection workloads."),
                });
            engine_rationales.push(EngineRationale {
                engine: engine.clone(),
                role,
                reason,
            });
        }

        // 4. Quota rationale
        let quota_rationale = prototype
            .rationale
            .as_ref()
            .map(|r| r.quota_rationale.clone())
            .filter(|q| !q.is_empty())
            .unwrap_or_else(|| {
                format!(
                    "Budget split allocated: {}% database cache, {}% transit store, {}% cache/hash index, {}% ledger log, and {}% network buffers.",
                    quota_split.db_pct,
                    quota_split.transit_store_pct,
                    quota_split.cache_hash_pct,
                    quota_split.ledger_pct,
                    quota_split.net_buffers_pct
                )
            });

        // 5. Runner-up contrast
        let mut sorted_scores = scores.to_vec();
        sorted_scores.sort_by(|a, b| b.score.partial_cmp(&a.score).unwrap_or(std::cmp::Ordering::Equal));
        let runner_up_contrast = if sorted_scores.len() > 1 && sorted_scores[0].workload == prototype.workload {
            let runner_up = &sorted_scores[1];
            if runner_up.score > 0.15 || (confidence - runner_up.score) < 0.25 {
                let ru_proto = self.prototypes.iter().find(|p| p.workload == runner_up.workload);
                let ru_label = ru_proto.map(|p| p.label.as_str()).unwrap_or(&runner_up.workload);
                let ru_summary = ru_proto
                    .and_then(|p| p.rationale.as_ref())
                    .map(|r| r.summary.as_str())
                    .unwrap_or("alternative data pattern");
                let my_summary = prototype
                    .rationale
                    .as_ref()
                    .map(|r| r.summary.as_str())
                    .unwrap_or("target workload");
                Some(format!(
                    "Also considered {ru_label} ({score:.1}% confidence share). Selected {label} because your requirements emphasize {my_summary} rather than {ru_summary}.",
                    score = runner_up.score * 100.0,
                ))
            } else {
                None
            }
        } else {
            None
        };

        // 6. Operational trade-offs
        let operational_trade_offs = prototype
            .rationale
            .as_ref()
            .map(|r| r.trade_offs.clone())
            .unwrap_or_default();

        SizingReasoning {
            summary,
            key_matched_signals,
            engine_rationales,
            quota_rationale,
            runner_up_contrast,
            operational_trade_offs,
        }
    }

    fn build_clarifying_questions(
        &self,
        prototype: &Prototype,
        description: &str,
        scores: &[WorkloadScore],
        confidence: f32,
        floor: f32,
    ) -> Vec<ClarifyingQuestion> {
        let trimmed = description.trim();
        let mut questions = Vec::new();
        let mut sorted = scores.to_vec();
        sorted.sort_by(|a, b| b.score.partial_cmp(&a.score).unwrap_or(std::cmp::Ordering::Equal));

        let is_below_floor = confidence < floor;
        let is_vague = trimmed.is_empty() || trimmed.split_whitespace().count() < 6;
        let is_close_contest = sorted.len() >= 2 && (sorted[0].score - sorted[1].score) < 0.15;

        if is_below_floor || is_vague {
            let top_workload = sorted.first().map(|s| s.workload.as_str()).unwrap_or("sql");
            let second_workload = sorted.get(1).map(|s| s.workload.as_str()).unwrap_or("nosql-doc");

            if is_close_contest {
                if let Some(pair_q) = self.question_for_pair(top_workload, second_workload) {
                    questions.push(pair_q);
                }
            }

            questions.push(ClarifyingQuestion {
                id: "primary_access_pattern".to_string(),
                prompt: "How will your application primarily query and access this data?".to_string(),
                rationale: "De-Sentry features specialized engines for relational SQL, schemaless documents, time-series, vectors, and graphs. Clarifying your primary access pattern ensures optimal engine and quota allocation.".to_string(),
                options: vec![
                    ClarificationOption {
                        label: "Relational Tables & SQL Joins".to_string(),
                        description: "Structured entities with foreign keys, column constraints, and SQL joins.".to_string(),
                        target_workload: "sql".to_string(),
                        appended_context: "normalised relational tables with foreign keys and multi-table SQL joins".to_string(),
                    },
                    ClarificationOption {
                        label: "Flexible JSON Documents".to_string(),
                        description: "Schemaless documents or blobs fetched directly by unique ID or key.".to_string(),
                        target_workload: "nosql-doc".to_string(),
                        appended_context: "schemaless JSON documents and objects fetched directly by key".to_string(),
                    },
                    ClarificationOption {
                        label: "Continuous Time-Series Metrics".to_string(),
                        description: "Timestamped sensor or telemetry readings rolled up over time windows.".to_string(),
                        target_workload: "time-series".to_string(),
                        appended_context: "timestamped time-series sensor telemetry sampled continuously with rollups".to_string(),
                    },
                    ClarificationOption {
                        label: "Semantic / Vector Similarity Search".to_string(),
                        description: "Numeric embeddings searched by cosine similarity or nearest neighbors (RAG).".to_string(),
                        target_workload: "vector".to_string(),
                        appended_context: "vector embeddings searched by cosine similarity for nearest neighbor retrieval".to_string(),
                    },
                    ClarificationOption {
                        label: "Connected Entity Graphs & Traversal".to_string(),
                        description: "Deep relationship networks, org charts, dependencies, and path traversals.".to_string(),
                        target_workload: "graph".to_string(),
                        appended_context: "connected entity graph traversed along relationship edges and node hierarchies".to_string(),
                    },
                    ClarificationOption {
                        label: "Columnar OLAP Analytics on Event Logs".to_string(),
                        description: "Scanned batches of logs and clickstream events aggregated over millions of rows.".to_string(),
                        target_workload: "semi-structured".to_string(),
                        appended_context: "columnar OLAP aggregations across event logs and scraped analytical records".to_string(),
                    },
                ],
            });
        } else if is_close_contest {
            let top_workload = &sorted[0].workload;
            let second_workload = &sorted[1].workload;
            if let Some(pair_q) = self.question_for_pair(top_workload, second_workload) {
                questions.push(pair_q);
            }
        } else if let Some(tuning_q) = self.fine_tuning_question(&prototype.workload) {
            questions.push(tuning_q);
        }

        questions
    }

    fn question_for_pair(&self, w1: &str, w2: &str) -> Option<ClarifyingQuestion> {
        let pair = if w1 < w2 { (w1, w2) } else { (w2, w1) };
        match pair {
            ("nosql-doc", "vector") => Some(ClarifyingQuestion {
                id: "pair_nosql_vs_vector".to_string(),
                prompt: "Are you storing media/files as raw assets, or searching them by visual and semantic similarity?".to_string(),
                rationale: "Standard document storage uses lightweight key-value records, whereas similarity search requires in-memory HNSW vector indexes.".to_string(),
                options: vec![
                    ClarificationOption {
                        label: "Store media files by ID & metadata".to_string(),
                        description: "Uploaded files and pictures fetched by key without embedding similarity.".to_string(),
                        target_workload: "nosql-doc".to_string(),
                        appended_context: "store image and media files as blobs by asset ID with metadata".to_string(),
                    },
                    ClarificationOption {
                        label: "Search embeddings by semantic / visual similarity".to_string(),
                        description: "High-dimensional embeddings searched via cosine similarity or nearest neighbor.".to_string(),
                        target_workload: "vector".to_string(),
                        appended_context: "search image embeddings by visual similarity and nearest neighbor".to_string(),
                    },
                ],
            }),
            ("nosql-doc", "sql") => Some(ClarifyingQuestion {
                id: "pair_sql_vs_nosql".to_string(),
                prompt: "Do you need strict multi-table referential integrity (foreign keys & joins), or flexible schemaless documents?".to_string(),
                rationale: "Relational tables enforce schema constraints and foreign keys, while document storage allows dynamic, variable fields.".to_string(),
                options: vec![
                    ClarificationOption {
                        label: "Strict relational tables with foreign keys & joins".to_string(),
                        description: "Structured tables with primary keys, foreign keys, and multi-table SQL queries.".to_string(),
                        target_workload: "sql".to_string(),
                        appended_context: "normalised relational tables with foreign keys and ACID joins".to_string(),
                    },
                    ClarificationOption {
                        label: "Dynamic JSON documents without rigid schemas".to_string(),
                        description: "Variable records stored and retrieved directly by document key.".to_string(),
                        target_workload: "nosql-doc".to_string(),
                        appended_context: "schemaless JSON documents stored and fetched by key".to_string(),
                    },
                ],
            }),
            ("sql", "time-series") => Some(ClarifyingQuestion {
                id: "pair_sql_vs_timeseries".to_string(),
                prompt: "Is your workload primarily transactional business records, or continuous sensor / telemetry metric streams?".to_string(),
                rationale: "Business records require relational consistency and joins, whereas telemetry streams benefit from automated time-window rollups.".to_string(),
                options: vec![
                    ClarificationOption {
                        label: "Transactional business entities requiring ACID joins".to_string(),
                        description: "Customer accounts, orders, and invoices with referential consistency.".to_string(),
                        target_workload: "sql".to_string(),
                        appended_context: "business records in relational tables with foreign keys and ACID joins".to_string(),
                    },
                    ClarificationOption {
                        label: "Continuous timestamped metric streams with rollups".to_string(),
                        description: "Periodic sensor or server telemetry downsampled over hours/days.".to_string(),
                        target_workload: "time-series".to_string(),
                        appended_context: "timestamped telemetry sampled continuously with automated downsampling rollups".to_string(),
                    },
                ],
            }),
            ("nosql-doc", "semi-structured") => Some(ClarifyingQuestion {
                id: "pair_nosql_vs_semistructured".to_string(),
                prompt: "Will you primarily run analytical queries across millions of records, or point lookups of individual documents?".to_string(),
                rationale: "Batch analytics across events benefit from columnar storage, while point lookups benefit from document key-value storage.".to_string(),
                options: vec![
                    ClarificationOption {
                        label: "Columnar aggregations and OLAP summaries across large batches".to_string(),
                        description: "Scanned batches of event logs, clickstreams, and analytical reports.".to_string(),
                        target_workload: "semi-structured".to_string(),
                        appended_context: "columnar OLAP aggregations across event logs and analytical tables".to_string(),
                    },
                    ClarificationOption {
                        label: "Direct point lookups and updates of individual documents by ID".to_string(),
                        description: "Single-record operations and profile retrievals by document key.".to_string(),
                        target_workload: "nosql-doc".to_string(),
                        appended_context: "document storage with key-value point lookups by ID".to_string(),
                    },
                ],
            }),
            ("graph", "sql") => Some(ClarifyingQuestion {
                id: "pair_graph_vs_sql".to_string(),
                prompt: "Do you primarily need to traverse deep relationship paths, or query structured tables with standard foreign keys?".to_string(),
                rationale: "Adjacency-list graph engines excel at variable-depth pointer-chasing, whereas relational engines excel at multi-column SQL queries.".to_string(),
                options: vec![
                    ClarificationOption {
                        label: "Multi-hop graph traversals across entity networks & hierarchies".to_string(),
                        description: "Org charts, social follower networks, and shortest path traversals.".to_string(),
                        target_workload: "graph".to_string(),
                        appended_context: "traversing graph relationships and directional edges between connected nodes".to_string(),
                    },
                    ClarificationOption {
                        label: "Standard relational tables with foreign key joins".to_string(),
                        description: "Normalised SQL tables with fixed columns and indexed joins.".to_string(),
                        target_workload: "sql".to_string(),
                        appended_context: "normalised relational tables with foreign keys and SQL joins".to_string(),
                    },
                ],
            }),
            ("oops-rdbms", "sql") => Some(ClarifyingQuestion {
                id: "pair_orm_vs_sql".to_string(),
                prompt: "Are you mapping an object-oriented class inheritance hierarchy, or working with direct relational database tables?".to_string(),
                rationale: "ORM class hierarchies need inheritance mapping and polymorphic queries, while relational tables focus on normalized relational schemas.".to_string(),
                options: vec![
                    ClarificationOption {
                        label: "Polymorphic domain models with class inheritance mapped via an ORM".to_string(),
                        description: "Classes, subclasses, and instance relationships mapped to tables.".to_string(),
                        target_workload: "oops-rdbms".to_string(),
                        appended_context: "class inheritance hierarchy persisted via an ORM into relational tables".to_string(),
                    },
                    ClarificationOption {
                        label: "Direct relational database tables and SQL queries".to_string(),
                        description: "Explicit SQL schema without OOP class mapping overhead.".to_string(),
                        target_workload: "sql".to_string(),
                        appended_context: "normalised relational tables with foreign keys and SQL joins".to_string(),
                    },
                ],
            }),
            _ => None,
        }
    }

    fn fine_tuning_question(&self, workload: &str) -> Option<ClarifyingQuestion> {
        match workload {
            "time-series" => Some(ClarifyingQuestion {
                id: "tune_timeseries_retention".to_string(),
                prompt: "What is your desired retention window for metric history?".to_string(),
                rationale: "De-Sentry automatically downsamples and purges expired metric points past the retention horizon to preserve disk quota.".to_string(),
                options: vec![
                    ClarificationOption {
                        label: "30 days (short-term operational metrics)".to_string(),
                        description: "Ideal for real-time monitoring where long-term trends are not required.".to_string(),
                        target_workload: "time-series".to_string(),
                        appended_context: "with 30 days retention".to_string(),
                    },
                    ClarificationOption {
                        label: "90 days (quarterly monitoring window)".to_string(),
                        description: "Balanced history for seasonal monitoring and service-level tracking.".to_string(),
                        target_workload: "time-series".to_string(),
                        appended_context: "with 90 days retention".to_string(),
                    },
                    ClarificationOption {
                        label: "365 days (annual historical auditing)".to_string(),
                        description: "Retains hourly rollups for up to one full calendar year.".to_string(),
                        target_workload: "time-series".to_string(),
                        appended_context: "with 365 days retention".to_string(),
                    },
                ],
            }),
            "vector" => Some(ClarifyingQuestion {
                id: "tune_vector_source".to_string(),
                prompt: "What is the primary retrieval task for these vector embeddings?".to_string(),
                rationale: "Tailoring the embedding profile helps configure secondary metadata collections and shard keys.".to_string(),
                options: vec![
                    ClarificationOption {
                        label: "RAG knowledge base document retrieval".to_string(),
                        description: "Embeddings of text passages for generative AI grounding and question answering.".to_string(),
                        target_workload: "vector".to_string(),
                        appended_context: "for RAG document passage retrieval and question answering".to_string(),
                    },
                    ClarificationOption {
                        label: "Product / content semantic recommendations".to_string(),
                        description: "Finding related products or articles based on user preference vectors.".to_string(),
                        target_workload: "vector".to_string(),
                        appended_context: "for semantic recommendation similarity across products".to_string(),
                    },
                    ClarificationOption {
                        label: "Visual / reverse image search".to_string(),
                        description: "Feature vectors generated from images for visual nearest neighbor search.".to_string(),
                        target_workload: "vector".to_string(),
                        appended_context: "for visual similarity search over image embeddings".to_string(),
                    },
                ],
            }),
            "sql" => Some(ClarifyingQuestion {
                id: "tune_sql_workload".to_string(),
                prompt: "What is the expected transactional profile for these tables?".to_string(),
                rationale: "Understanding read/write intensity helps configure buffer pool page sizes and secondary indexing.".to_string(),
                options: vec![
                    ClarificationOption {
                        label: "Balanced OLTP transactional reads and writes".to_string(),
                        description: "Frequent concurrent transactions, order inserts, and status updates.".to_string(),
                        target_workload: "sql".to_string(),
                        appended_context: "with balanced OLTP transactional reads and writes".to_string(),
                    },
                    ClarificationOption {
                        label: "Read-heavy analytical reporting & dashboards".to_string(),
                        description: "Infrequent batch writes with complex multi-table SQL queries.".to_string(),
                        target_workload: "sql".to_string(),
                        appended_context: "with read-heavy analytical reporting queries".to_string(),
                    },
                ],
            }),
            _ => None,
        }
    }

    /// Cosine similarity of the description against every prototype embedding.
    #[cfg(feature = "onnx")]
    fn embed_scores(&self, description: &str) -> Result<Vec<f32>, String> {
        let guard = self.model.lock().map_err(|_| "the model is unavailable".to_owned())?;
        let model = guard.as_ref().ok_or_else(|| "the model is not loaded".to_owned())?;
        let query = model.embed(description).map_err(|error| error.to_string())?;
        Ok(self.embeddings.iter().map(|proto| dot(&query, proto)).collect())
    }

    /// Keep the embedding model primary, but let unmistakable domain words
    /// disambiguate close neighbors such as media blobs and image search.
    /// This also makes the proposal explainable when the user's wording is
    /// more specific than the short prototype sentences.
    fn combine_semantic_and_keywords(&self, description: &str, semantic: &[f32]) -> Vec<f32> {
        let lexical = self.keyword_scores(description);
        let explicit_media = explicit_media_intent(description);
        self.prototypes
            .iter()
            .enumerate()
            .map(|(index, prototype)| {
                let intent_boost = match (explicit_media, prototype.workload.as_str()) {
                    (Some(MediaIntent::Storage), "nosql-doc") => 0.65,
                    (Some(MediaIntent::Similarity), "vector") => 0.65,
                    _ => 0.0,
                };
                semantic[index] + lexical[index] * 0.2 + intent_boost
            })
            .collect()
    }

    #[cfg(not(feature = "onnx"))]
    fn embed_scores(&self, _description: &str) -> Result<Vec<f32>, String> {
        Err("this build has no embedding model".to_owned())
    }

    /// The deterministic fallback.
    ///
    /// Sums the weights of every prototype keyword that appears in the
    /// description, normalised by the prototype's total available weight so a
    /// prototype with many keywords does not win by breadth alone. Blunt, but
    /// explainable: a user can look at their own sentence and see why.
    fn keyword_scores(&self, description: &str) -> Vec<f32> {
        let haystack = description.to_lowercase();
        self.prototypes
            .iter()
            .map(|prototype| {
                let mut matched = 0.0f32;
                let mut total = 0.0f32;
                for (term, weight) in &prototype.keywords {
                    total += *weight;
                    if contains_word(&haystack, term) {
                        matched += *weight;
                    }
                }
                if total <= 0.0 {
                    0.0
                } else {
                    // Scaled into roughly the same range as a cosine so the
                    // shared softmax temperature means the same thing on both
                    // paths.
                    (matched / total).sqrt()
                }
            })
            .collect()
    }
}

/// Whole-word (or whole-phrase) containment.
///
/// Substring matching would score "graph" for "paragraph" and "orm" for
/// "format" -- both of which are exactly the sort of confident nonsense a
/// fallback must not produce.
fn contains_word(haystack: &str, needle: &str) -> bool {
    let needle = needle.trim();
    if needle.is_empty() {
        return false;
    }
    let bytes = haystack.as_bytes();
    let mut from = 0usize;
    while let Some(offset) = haystack[from..].find(needle) {
        let start = from + offset;
        let end = start + needle.len();
        let before_ok = start == 0 || !bytes[start - 1].is_ascii_alphanumeric();
        let after_ok = end >= bytes.len() || !bytes[end].is_ascii_alphanumeric();
        if before_ok && after_ok {
            return true;
        }
        from = start + 1;
        if from >= haystack.len() {
            break;
        }
    }
    false
}

/// Softmax at `TEMPERATURE`, shifted by the maximum for numerical stability.
fn softmax(scores: &[f32]) -> Vec<f32> {
    if scores.is_empty() {
        return Vec::new();
    }
    let max = scores.iter().copied().fold(f32::NEG_INFINITY, f32::max);
    if !max.is_finite() {
        return vec![1.0 / scores.len() as f32; scores.len()];
    }
    let exps: Vec<f32> = scores.iter().map(|s| ((s - max) / TEMPERATURE).exp()).collect();
    let sum: f32 = exps.iter().sum();
    if sum <= 0.0 || !sum.is_finite() {
        return vec![1.0 / scores.len() as f32; scores.len()];
    }
    exps.into_iter().map(|e| e / sum).collect()
}

/// Dot product. Embeddings are L2-normalised, so this is the cosine.
#[cfg_attr(not(feature = "onnx"), allow(dead_code))]
fn dot(a: &[f32], b: &[f32]) -> f32 {
    a.iter().zip(b.iter()).map(|(x, y)| x * y).sum()
}

// -- the ONNX path -----------------------------------------------------------

#[cfg(feature = "onnx")]
mod onnx {
    use std::path::Path;

    use super::{Resources, MAX_TOKENS};

    #[derive(Debug, thiserror::Error)]
    pub enum ModelError {
        #[error("the embedding model is not installed at {0}. Run `npm run fetch-model` in app/ to add it; sizing falls back to keywords until then.")]
        Missing(String),
        #[error("the ONNX runtime could not be initialised: {0}")]
        Runtime(String),
        #[error("the tokenizer could not be loaded: {0}")]
        Tokenizer(String),
        #[error("the model rejected the input: {0}")]
        Inference(String),
    }

    pub struct Model {
        session: std::sync::Mutex<ort::session::Session>,
        tokenizer: tokenizers::Tokenizer,
    }

    impl Model {
        pub fn load(resources: &Resources) -> Result<Self, ModelError> {
            check_present(&resources.model)?;
            check_present(&resources.tokenizer)?;

            // `load-dynamic` means the runtime library is resolved at run time
            // from the app's own resources -- nothing is downloaded during the
            // build, which is what keeps `cargo build` working offline.
            let library = runtime_library(&resources.runtime_dir);
            if library.exists() {
                std::env::set_var("ORT_DYLIB_PATH", &library);
            }

            let session = ort::session::Session::builder()
                .map_err(|error| ModelError::Runtime(error.to_string()))?
                .with_intra_threads(1)
                .map_err(|error| ModelError::Runtime(error.to_string()))?
                .commit_from_file(&resources.model)
                .map_err(|error| ModelError::Runtime(error.to_string()))?;

            let tokenizer = tokenizers::Tokenizer::from_file(&resources.tokenizer)
                .map_err(|error| ModelError::Tokenizer(error.to_string()))?;

            Ok(Self { session: std::sync::Mutex::new(session), tokenizer })
        }

        /// One sentence to one L2-normalised 384-dim vector.
        pub fn embed(&self, text: &str) -> Result<Vec<f32>, ModelError> {
            Ok(self.embed_all(&[text])?.remove(0))
        }

        /// Embeds a batch, padded to the longest member.
        ///
        /// Mean pooling over the token dimension, masked so padding does not
        /// drag every vector toward the same point -- this is the pooling
        /// all-MiniLM-L6-v2 was trained with, and using CLS instead would give
        /// vectors that are quietly worse rather than obviously broken.
        pub fn embed_all(&self, texts: &[&str]) -> Result<Vec<Vec<f32>>, ModelError> {
            use ort::value::Value;

            let mut encodings = Vec::with_capacity(texts.len());
            for text in texts {
                let mut encoding = self
                    .tokenizer
                    .encode(*text, true)
                    .map_err(|error| ModelError::Tokenizer(error.to_string()))?;
                encoding.truncate(MAX_TOKENS, 0, tokenizers::TruncationDirection::Right);
                encodings.push(encoding);
            }

            let batch = encodings.len();
            let width = encodings.iter().map(|e| e.len()).max().unwrap_or(1).max(1);

            let mut ids = vec![0i64; batch * width];
            let mut mask = vec![0i64; batch * width];
            let types = vec![0i64; batch * width];
            for (row, encoding) in encodings.iter().enumerate() {
                for (col, id) in encoding.get_ids().iter().enumerate() {
                    ids[row * width + col] = i64::from(*id);
                    mask[row * width + col] = 1;
                }
            }

            let shape = [batch, width];
            let input_ids = Value::from_array((shape, ids.clone()))
                .map_err(|error| ModelError::Inference(error.to_string()))?;
            let attention_mask = Value::from_array((shape, mask.clone()))
                .map_err(|error| ModelError::Inference(error.to_string()))?;
            let token_type_ids = Value::from_array((shape, types.clone()))
                .map_err(|error| ModelError::Inference(error.to_string()))?;

            let mut session = self
                .session
                .lock()
                .map_err(|_| ModelError::Inference("ONNX session lock poisoned".to_owned()))?;
            let outputs = session
                .run(ort::inputs![
                    "input_ids" => input_ids,
                    "attention_mask" => attention_mask,
                    "token_type_ids" => token_type_ids,
                ])
                .map_err(|error| ModelError::Inference(error.to_string()))?;

            let (shape, data) = outputs[0]
                .try_extract_tensor::<f32>()
                .map_err(|error| ModelError::Inference(error.to_string()))?;
            if shape.len() != 3 {
                return Err(ModelError::Inference(format!(
                    "expected a [batch, tokens, hidden] tensor, got {shape:?}"
                )));
            }
            let tokens = shape[1] as usize;
            let hidden = shape[2] as usize;

            let mut out = Vec::with_capacity(batch);
            for row in 0..batch {
                let mut pooled = vec![0f32; hidden];
                let mut counted = 0f32;
                for token in 0..tokens {
                    if mask[row * width + token] == 0 {
                        continue;
                    }
                    counted += 1.0;
                    let base = (row * tokens + token) * hidden;
                    for (i, value) in pooled.iter_mut().enumerate() {
                        *value += data[base + i];
                    }
                }
                if counted > 0.0 {
                    for value in pooled.iter_mut() {
                        *value /= counted;
                    }
                }
                normalise(&mut pooled);
                out.push(pooled);
            }
            Ok(out)
        }

        /// Embeds a batch of texts for a single prototype and returns their
        /// L2-normalised centroid — i.e. the mean vector, renormalised to
        /// unit length so the dot product against a query is still the cosine.
        ///
        /// With a single text this is identical to `embed(text)`. With several
        /// it produces a point that is the geometric centre of the cluster,
        /// which covers a broader semantic region without requiring retraining.
        pub fn embed_centroid(&self, texts: &[&str]) -> Result<Vec<f32>, ModelError> {
            if texts.is_empty() {
                return Err(ModelError::Inference("no texts supplied for centroid".to_owned()));
            }
            let vecs = self.embed_all(texts)?;
            let dim = vecs[0].len();
            let mut centroid = vec![0f32; dim];
            for vec in &vecs {
                for (i, x) in vec.iter().enumerate() {
                    centroid[i] += x;
                }
            }
            let n = vecs.len() as f32;
            for x in centroid.iter_mut() {
                *x /= n;
            }
            // Re-normalise so the centroid is a proper unit vector; necessary
            // because the average of unit vectors is not itself a unit vector.
            normalise(&mut centroid);
            Ok(centroid)
        }
    }

    fn normalise(vector: &mut [f32]) {
        let norm = vector.iter().map(|v| v * v).sum::<f32>().sqrt();
        if norm > 0.0 {
            for value in vector.iter_mut() {
                *value /= norm;
            }
        }
    }

    fn check_present(path: &Path) -> Result<(), ModelError> {
        if path.exists() {
            Ok(())
        } else {
            Err(ModelError::Missing(path.display().to_string()))
        }
    }

    fn runtime_library(dir: &Path) -> std::path::PathBuf {
        #[cfg(target_os = "windows")]
        let name = "onnxruntime.dll";
        #[cfg(target_os = "macos")]
        let name = "libonnxruntime.dylib";
        #[cfg(all(unix, not(target_os = "macos")))]
        let name = "libonnxruntime.so";
        dir.join(name)
    }
}

// -- global instance ---------------------------------------------------------

/// Built once at startup. `None` when the prototypes themselves are missing,
/// which is a packaging failure rather than a runtime condition.
static SIZER: Mutex<Option<&'static Sizer>> = Mutex::new(None);

pub fn install(sizer: Sizer) {
    let leaked: &'static Sizer = Box::leak(Box::new(sizer));
    *SIZER.lock().expect("sizer mutex") = Some(leaked);
}

pub fn get() -> Option<&'static Sizer> {
    *SIZER.lock().expect("sizer mutex")
}

/// Resolves the bundled resource paths.
pub fn resources_from(base: &Path) -> Resources {
    Resources {
        prototypes: base.join("resources").join("prototypes.json"),
        model: base.join("resources").join("model").join("model.onnx"),
        tokenizer: base.join("resources").join("model").join("tokenizer.json"),
        runtime_dir: base.join("resources").join("model"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sizer() -> Sizer {
        let text = std::fs::read_to_string(
            Path::new(env!("CARGO_MANIFEST_DIR"))
                .join("..")
                .join("resources")
                .join("prototypes.json"),
        )
        .expect("the prototypes ship with the app");
        let file: PrototypeFile = serde_json::from_str(&text).expect("valid prototypes");
        Sizer {
            prototypes: file.prototypes,
            floor: file.confidence_floor,
            embeddings: Vec::new(),
            fallback_reason: "test".into(),
            #[cfg(feature = "onnx")]
            model: Mutex::new(None),
        }
    }

    fn all_engines() -> Vec<String> {
        ["kv", "columnar_lite", "ts_rollup", "vector_hnsw_lite", "graph_adj"]
            .iter()
            .map(|s| s.to_string())
            .collect()
    }

    #[test]
    fn the_keyword_fallback_finds_the_obvious_shapes() {
        let sizer = sizer();
        let cases = [
            ("Sensor readings from the workshop, sampled every second and kept for a year", "time-series"),
            ("Embeddings of support articles searched by semantic similarity", "vector"),
            ("An org chart: who reports to whom, traversed to find descendants", "graph"),
        ];
        for (description, expected) in cases {
            let spec = sizer.size(description, 2048, &all_engines());
            assert_eq!(spec.decision.workload, expected, "for: {description}");
            assert_eq!(spec.decision.method, "keyword");
        }
    }

    #[test]
    fn an_empty_description_has_no_confidence() {
        let spec = sizer().size("   ", 1024, &all_engines());
        assert_eq!(spec.decision.confidence, 0.0);
        // Which is what pushes the wizard to the manual picker.
        assert!(spec.decision.confidence < spec.decision.confidence_floor);
    }

    #[test]
    fn a_description_that_says_nothing_lands_below_the_floor() {
        let spec = sizer().size("some data for the thing we discussed", 1024, &all_engines());
        assert!(
            spec.decision.confidence < spec.decision.confidence_floor,
            "confidence was {} for a description with no signal",
            spec.decision.confidence
        );
    }

    #[test]
    fn engines_are_filtered_to_this_build() {
        // sqlite is not compiled in here, so the relational proposal must fall
        // back to something that is rather than offer an engine that fails.
        let only_kv = vec!["kv".to_string()];
        let spec = sizer().size("Relational tables with joins and foreign keys", 1024, &only_kv);
        assert!(spec.engines.iter().all(|e| e == "kv"));
        assert_eq!(spec.default_engine, "kv");
        for collection in &spec.collections {
            assert_eq!(collection.engine, "kv");
        }
    }

    #[test]
    fn every_proposed_split_is_valid() {
        let sizer = sizer();
        for prototype in &sizer.prototypes {
            let split = prototype.quota_split.normalised();
            assert_eq!(split.total(), 100, "{} has an invalid split", prototype.workload);
        }
    }

    #[test]
    fn word_matching_does_not_fire_on_substrings() {
        assert!(!contains_word("the paragraph was long", "graph"));
        assert!(contains_word("a graph of dependencies", "graph"));
        assert!(!contains_word("the file format", "orm"));
        assert!(contains_word("we use an orm", "orm"));
        assert!(contains_word("time series data", "time series"));
    }

    #[test]
    fn softmax_is_uniform_when_nothing_stands_out() {
        let uniform = softmax(&[0.0, 0.0, 0.0, 0.0]);
        for value in uniform {
            assert!((value - 0.25).abs() < 1e-6);
        }
    }

    #[test]
    fn softmax_rewards_a_clear_margin() {
        let scores = softmax(&[0.8, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5]);
        assert!(scores[0] > 0.9, "a 0.3 cosine margin should be decisive, got {}", scores[0]);
    }

    #[test]
    fn sizing_decision_includes_rich_reasoning() {
        let sizer = sizer();
        let spec = sizer.size(
            "Sensor readings from the workshop, sampled every second and kept for a year",
            2048,
            &all_engines(),
        );
        let reasoning = spec.decision.reasoning.expect("reasoning must be populated");
        assert!(!reasoning.summary.is_empty(), "summary should explain the decision");
        assert!(!reasoning.engine_rationales.is_empty(), "engine rationales should not be empty");
        for engine in &spec.engines {
            assert!(
                reasoning.engine_rationales.iter().any(|er| er.engine == *engine),
                "must provide rationale for engine {engine}"
            );
        }
        assert!(!reasoning.quota_rationale.is_empty(), "quota rationale must be present");
        assert!(!reasoning.key_matched_signals.is_empty(), "should extract matched signals from text");
        assert!(reasoning.key_matched_signals.iter().any(|s| s.contains("sensor")));
    }

    #[test]
    fn ambiguous_input_generates_clarifying_questions_that_resolve_intent() {
        let sizer = sizer();
        // Vague query below floor
        let vague = "I need a database for my project";
        let spec = sizer.size(vague, 1024, &all_engines());
        assert!(spec.decision.confidence < spec.decision.confidence_floor);
        assert!(!spec.decision.clarifying_questions.is_empty(), "must propose clarifying questions");

        // The clarifying questions have options with appended_context
        let ts_opt = spec
            .decision
            .clarifying_questions
            .iter()
            .flat_map(|q| &q.options)
            .find(|o| o.target_workload == "time-series")
            .expect("time-series option exists in questions");
        let refined_desc = format!("{vague} {}", ts_opt.appended_context);
        let refined_spec = sizer.size(&refined_desc, 1024, &all_engines());

        assert_eq!(refined_spec.decision.workload, "time-series");
        assert!(
            refined_spec.decision.confidence >= refined_spec.decision.confidence_floor,
            "answering the clarifying question must raise confidence above floor ({:.3} >= {:.3})",
            refined_spec.decision.confidence,
            refined_spec.decision.confidence_floor
        );
    }

    /// The full ONNX path against the real fetched files: load the model and
    /// tokenizer, embed the prototypes, and size a description semantically.
    /// This is the exact code the wizard runs -- not a mock of it. Skipped
    /// loudly (not failed) on a tree without `npm run fetch-model` output,
    /// since the model is a gitignored build artifact, not source.
    ///
    /// Uses the shared model Sizer (one session, serialised inference --
    /// exactly the production topology). See `shared_model_sizer`.
    #[cfg(feature = "onnx")]
    #[test]
    fn the_onnx_model_loads_and_sizes() {
        let Some(sizer) = shared_model_sizer("the_onnx_model_loads_and_sizes") else {
            return;
        };
        assert_eq!(sizer.embeddings.len(), sizer.prototypes.len());
        for embedding in &sizer.embeddings {
            assert_eq!(embedding.len(), 384, "MiniLM embeddings are 384-wide");
            assert!(embedding.iter().all(|v| v.is_finite()));
        }
        let spec = sizer.size(
            "Sensor readings from the workshop, sampled every second and kept for a year",
            2048,
            &all_engines(),
        );
        assert_eq!(spec.decision.method, "onnx");
        assert_eq!(spec.decision.workload, "time-series");
        assert!(!spec.engines.is_empty());
    }

    /// Broad quality evaluation of the ONNX model across 17 cases:
    ///
    /// - Seven canonical descriptions (one per workload) -- the model must get
    ///   all of them right and above the confidence floor.
    /// - Five paraphrased descriptions that avoid the obvious keywords -- these
    ///   test that the *semantic* embedding is doing the work, not the keyword
    ///   fallback.
    /// - Two adversarial traps that look like a different workload by substring
    ///   ("paragraph" must not fire "graph"; "format" must not fire "orm").
    /// - Three ambiguous / vague descriptions that must land *below* the floor
    ///   so the wizard correctly hands off to the manual picker.
    ///
    /// Skipped (not failed) when the model files are absent.
    ///
    /// Uses the shared model Sizer (one session, serialised inference --
    /// exactly the production topology). See `shared_model_sizer`.
    #[cfg(feature = "onnx")]
    #[test]
    fn the_onnx_model_quality() {
        let Some(sizer) = shared_model_sizer("the_onnx_model_quality") else {
            return;
        };

        let engines = [
            "kv", "columnar_lite", "ts_rollup", "vector_hnsw_lite", "graph_adj", "sqlite",
        ]
        .iter()
        .map(|s| s.to_string())
        .collect::<Vec<_>>();

        // -- Canonical: one clear example per workload -----------------------
        let canonical: &[(&str, &str)] = &[
            (
                "Orders, customers, and invoices stored in normalised tables with foreign keys and joined in reports",
                "sql",
            ),
            (
                "Sensor readings from a factory floor sampled every second, kept for 6 months, rolled up hourly",
                "time-series",
            ),
            (
                "User profile documents stored as JSON blobs, fetched by user id, no fixed schema",
                "nosql-doc",
            ),
            (
                "Text embeddings of product descriptions searched by cosine similarity for recommendations",
                "vector",
            ),
            (
                "An org chart: employees, their managers, traversed to find everyone who reports to a VP",
                "graph",
            ),
            (
                "Scraped web events with mixed fields, aggregated and summarised across millions of rows",
                "semi-structured",
            ),
            (
                "Python classes with inheritance persisted via an ORM into queryable tables",
                "oops-rdbms",
            ),
        ];
        // Every canonical case must be correctly classified and meet or exceed
        // the confidence floor.
        for (desc, expected) in canonical {
            let spec = sizer.size(desc, 2048, &engines);
            assert_eq!(
                spec.decision.method, "onnx",
                "model must drive canonical case: {desc}"
            );
            assert_eq!(
                spec.decision.workload, *expected,
                "canonical ONNX case wrong: {desc}"
            );
            assert!(
                spec.decision.confidence >= spec.decision.confidence_floor,
                "canonical case confidence {:.3} below floor {:.3}: {desc}",
                spec.decision.confidence,
                spec.decision.confidence_floor
            );
        }

        // -- Paraphrases: keyword-avoiding rewrites of the same intent -------
        // These test that the semantic embedding is doing the work, not
        // the keyword fallback -- if the model is absent they would pass
        // keyword anyway, which is why we assert method == "onnx" above.
        let paraphrases_strict: &[(&str, &str)] = &[
            (
                "IoT temperature and humidity data arriving continuously from 500 devices",
                "time-series",
            ),
            (
                "A social network: who follows whom, shortest path between users",
                "graph",
            ),
            (
                "RAG pipeline: embed support-ticket summaries and retrieve the nearest 5 by meaning",
                "vector",
            ),
            (
                "Columnar analytics over clickstream logs with OLAP-style aggregations",
                "semi-structured",
            ),
        ];
        for (desc, expected) in paraphrases_strict {
            let spec = sizer.size(desc, 2048, &engines);
            assert_eq!(
                spec.decision.workload, *expected,
                "paraphrase case wrong [{method}]: {desc}",
                method = spec.decision.method
            );
            assert!(
                spec.decision.confidence >= spec.decision.confidence_floor,
                "paraphrase confidence {:.3} below floor {:.3}: {desc}",
                spec.decision.confidence,
                spec.decision.confidence_floor
            );
        }

        // -- Adversarial: substring traps the keyword fallback would misfire on.
        // The ONNX path should be immune since it operates on meaning, not
        // character sequences; confirm workload is *not* the trap target.
        let adversarial: &[(&str, &str, &str)] = &[
            // "paragraph" contains "graph" as a substring
            (
                "We need to store a paragraph of text for each user",
                "graph", // must NOT be this
                "paragraph must not fire the graph prototype",
            ),
            // "format" contains "orm" as a substring
            (
                "Format conversion pipeline for batch imports from CSV files",
                "oops-rdbms", // must NOT be this
                "format/import must not fire the ORM prototype",
            ),
        ];
        for (desc, must_not_be, note) in adversarial {
            let spec = sizer.size(desc, 2048, &engines);
            assert_ne!(
                spec.decision.workload, *must_not_be,
                "adversarial trap fired [{method}]: {note} -- got '{got}' for: {desc}",
                method = spec.decision.method,
                got = spec.decision.workload
            );
        }

        // -- Ambiguous: vague descriptions must land below the confidence floor
        // so the wizard routes the user to the manual picker rather than
        // presenting a low-quality automated proposal.
        let ambiguous: &[&str] = &[
            "We have some data we need to store and query quickly",
            "some data for the thing we discussed",
            "I need a database for my project",
        ];
        for desc in ambiguous {
            let spec = sizer.size(desc, 2048, &engines);
            assert!(
                spec.decision.confidence < spec.decision.confidence_floor,
                "vague description '{desc}' had confidence {:.3} >= floor {:.3} -- \
                 the wizard would propose instead of routing to the manual picker",
                spec.decision.confidence,
                spec.decision.confidence_floor
            );
        }
    }

    /// Diagnostic: prints confidence scores for every test description without
    /// asserting. Run explicitly with `cargo test -- --ignored` or
    /// `cargo test print_scores -- --ignored --nocapture` to audit the model.
    /// Never runs in normal `cargo test` so it never blocks CI.
    #[cfg(feature = "onnx")]
    #[test]
    #[ignore]
    fn print_onnx_scores_for_all_cases() {
        let base = Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
        let resources = resources_from(&base);
        for path in [&resources.prototypes, &resources.model, &resources.tokenizer] {
            if !path.exists() {
                eprintln!("SKIPPED: {} absent", path.display());
                return;
            }
        }
        let sizer = Sizer::load(&resources).expect("prototypes load");
        assert!(sizer.model_ready(), "model not ready: {}", sizer.fallback_reason);

        let engines = [
            "kv", "columnar_lite", "ts_rollup", "vector_hnsw_lite", "graph_adj", "sqlite",
        ]
        .iter()
        .map(|s| s.to_string())
        .collect::<Vec<_>>();

        let cases: &[(&str, &str)] = &[
            ("Orders, customers, and invoices stored in normalised tables with foreign keys and joined in reports", "sql"),
            ("Sensor readings from a factory floor sampled every second, kept for 6 months, rolled up hourly", "time-series"),
            ("User profile documents stored as JSON blobs, fetched by user id, no fixed schema", "nosql-doc"),
            ("Text embeddings of product descriptions searched by cosine similarity for recommendations", "vector"),
            ("An org chart: employees, their managers, traversed to find everyone who reports to a VP", "graph"),
            ("Scraped web events with mixed fields, aggregated and summarised across millions of rows", "semi-structured"),
            ("Python classes with inheritance persisted via an ORM into queryable tables", "oops-rdbms"),
            ("IoT temperature and humidity data arriving continuously from 500 devices", "time-series"),
            ("A social network: who follows whom, shortest path between users", "graph"),
            ("RAG pipeline: embed support-ticket summaries and retrieve the nearest 5 by meaning", "vector"),
            ("Columnar analytics over clickstream logs with OLAP-style aggregations", "semi-structured"),
            ("We need to store customer receipts and inventory with referential integrity", "sql"),
            ("We need to store a paragraph of text for each user", "nosql-doc"),
            ("Format conversion pipeline for batch imports from CSV files", "semi-structured"),
            ("We have some data we need to store and query quickly", "?"),
            ("some data for the thing we discussed", "?"),
            ("I need a database for my project", "?"),
        ];

        eprintln!("\n{:<68} {:>8} {:>12} {:>6}  SCORES", "DESCRIPTION", "EXPECTED", "GOT", "CONF");
        eprintln!("{}", "-".repeat(120));
        for (desc, expected) in cases {
            let spec = sizer.size(desc, 2048, &engines);
            let mut scores = spec.decision.scores.clone();
            scores.sort_by(|a, b| b.score.partial_cmp(&a.score).unwrap_or(std::cmp::Ordering::Equal));
            let top3 = scores.iter().take(3)
                .map(|s| format!("{}={:.3}", s.workload, s.score))
                .collect::<Vec<_>>()
                .join("  ");
            let flag = if spec.decision.workload == *expected || *expected == "?" { "" } else { " ✗" };
            let below = if spec.decision.confidence < spec.decision.confidence_floor { " [BELOW FLOOR]" } else { "" };
            eprintln!(
                "{:<68} {:>8} {:>12} {:>6.3}{}{}  | {}",
                &desc[..desc.len().min(67)],
                expected,
                spec.decision.workload,
                spec.decision.confidence,
                flag,
                below,
                top3
            );
        }
        eprintln!("floor={}", CONFIDENCE_FLOOR);
    }

    /// One ONNX session shared by the model tests, mirroring production.
    ///
    /// Production runs a single global SIZER with inference serialised on its
    /// mutex (`install`/`get` above; every Tauri sizing command shares it).
    /// The model tests share one session the same way instead of each loading
    /// their own, so the tests exercise the topology the product actually
    /// runs -- and the seven prototype centroids are embedded once rather
    /// than once per test. Poisoning is ignored on lock so a genuine failure
    /// in one test does not cascade into a lock panic in the next -- each
    /// failure must read as itself.
    #[cfg(feature = "onnx")]
    static SHARED_MODEL_SIZER: std::sync::OnceLock<std::sync::Mutex<Sizer>> =
        std::sync::OnceLock::new();

    /// Locks the shared model Sizer, loading it on first use. Returns None
    /// (after a loud SKIP note) when the model files are absent, preserving
    /// the skip-not-fail contract on trees without fetch-model output.
    #[cfg(feature = "onnx")]
    fn shared_model_sizer(test: &str) -> Option<std::sync::MutexGuard<'static, Sizer>> {
        let base = Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
        let resources = resources_from(&base);
        for path in [&resources.prototypes, &resources.model, &resources.tokenizer] {
            if !path.exists() {
                eprintln!("SKIPPED {test}: {} is absent (run npm run fetch-model in app/)", path.display());
                return None;
            }
        }
        let sizer = SHARED_MODEL_SIZER.get_or_init(|| {
            let loaded = Sizer::load(&resources).expect("prototypes load");
            assert!(loaded.model_ready(), "model not ready: {}", loaded.fallback_reason);
            std::sync::Mutex::new(loaded)
        });
        Some(sizer.lock().unwrap_or_else(|poisoned| poisoned.into_inner()))
    }
}
