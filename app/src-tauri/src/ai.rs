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
    text: String,
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
                let texts: Vec<&str> = self.prototypes.iter().map(|p| p.text.as_str()).collect();
                match model.embed_all(&texts) {
                    Ok(embeddings) => {
                        self.embeddings = embeddings;
                        *self.model.lock().expect("model mutex") = Some(model);
                    }
                    Err(error) => {
                        self.fallback_reason = format!("the prototypes could not be embedded: {error}");
                    }
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
                Ok(scores) => (scores, "onnx", String::new()),
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
        };

        self.spec_from(prototype, quota_mb, available_engines, decision)
    }

    /// Turns a chosen prototype into a spec, filtered to what this build has.
    fn spec_from(
        &self,
        prototype: &Prototype,
        quota_mb: u64,
        available: &[String],
        decision: SizingDecision,
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
            quota_split: prototype.quota_split.normalised(),
            shard_key: prototype.shard_key.clone(),
            replication_factor: prototype.replication_factor,
            secondary_indexes: prototype.secondary_indexes.clone(),
            retention_days: prototype.retention_days,
            collections,
            decision,
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
            let mut types = vec![0i64; batch * width];
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
}
