//! Integration tests for the AI sizing system.

use std::path::PathBuf;

use de_sentry_app::ai::{Sizer, Resources, NodeSpec};

#[test]
fn test_sizing_engine_selection_and_quota_adjustments() {
    // Setup resources pointing at the bundled prototype and model files.
    let resources = Resources {
        prototypes: PathBuf::from("../resources/prototypes.json"),
        model: PathBuf::from("../resources/model/model.onnx"),
        tokenizer: PathBuf::from("../resources/model/tokenizer.json"),
        runtime_dir: PathBuf::from("../resources/model"),
    };

    let sizer = Sizer::load(&resources).expect("failed to load sizer");

    // Simulate a tiny node (64 MiB) with the installed engines.
    let available = vec!["kv".to_string(), "ts_rollup".to_string(), "sqlite".to_string()];
    let description = "Store sensor readings every second, query by time range and keep one year of history.";
    let spec: NodeSpec = sizer.size(description, 64, &available);

    // Expect the time‑series engine to be selected.
    assert!(spec.engines.iter().any(|e| e == "ts_rollup"), "time-series engine not selected");
    // Verify quota adjustments: tiny nodes should boost DB cache.
    assert!(spec.quota_split.db_pct > 60, "quota split not adjusted for tiny node");
    // The total should still sum to 100 after normalisation.
    assert_eq!(spec.quota_split.total(), 100);

    // Now test a large node (5 GiB) – DB cache should still be boosted but less aggressively.
    let large_spec: NodeSpec = sizer.size(description, 5120, &available);
    assert!(large_spec.quota_split.db_pct >= spec.quota_split.db_pct, "large node should not reduce DB pct");
    assert_eq!(large_spec.quota_split.total(), 100);
}
