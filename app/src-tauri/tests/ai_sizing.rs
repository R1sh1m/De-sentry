use std::path::PathBuf;

use de_sentry_app::ai::{Resources, Sizer};

#[test]
fn test_ai_sizing_engines_and_confidence() {
    // Point to the bundled prototypes; force model load failure to use keyword fallback.
    let resources = Resources {
        prototypes: PathBuf::from("../resources/prototypes.json"),
        model: PathBuf::from("nonexistent/model.onnx"),
        tokenizer: PathBuf::from("nonexistent/tokenizer.json"),
        runtime_dir: PathBuf::from("nonexistent"),
    };
    let sizer = Sizer::load(&resources).expect("Failed to load Sizer");

    // Available built‑in engines (the ones compiled into the binary).
    let available = vec![
        "kv".to_string(),
        "columnar_lite".to_string(),
        "ts_rollup".to_string(),
        "vector_hnsw_lite".to_string(),
        "graph_adj".to_string(),
    ];

    // Helper closure to assert basic expectations.
    let check = |description: &str, expected_engine: &str| {
        let spec = sizer.size(description, 1024, &available);
        // The prototype may request an engine that is not compiled; the sizing code falls back to the first available engine.
        // For the built‑in set we expect the fallback to be "kv" for relational and OOP workloads.
        let contains = spec.engines.iter().any(|e| e == expected_engine);
        assert!(
            contains,
            "Engines {:?} should contain {} for description: {}",
            spec.engines,
            expected_engine,
            description
        );
        assert!(
            spec.decision.confidence > 0.35,
            "Confidence should be above floor for description: {}",
            description
        );
    };

    // Relational (SQL) – falls back to kv because sqlite is not compiled.
    check("Relational tables with joins and foreign keys", "kv");
    // Document store – uses kv directly.
    check("Flexible JSON documents stored by key", "kv");
    // Time‑series – expects ts_rollup, which is compiled.
    check("Continuous sensor readings stored as time series", "ts_rollup");
    // Vector similarity – expects vector_hnsw_lite, which is compiled.
    check("Embeddings of text searched by similarity", "vector_hnsw_lite");
    // Graph – expects graph_adj, which is compiled.
    check("Entity relationships traversed as a graph", "graph_adj");
    // Semi‑structured – expects columnar_lite, which is compiled.
    check("Event logs with mixed schema that need OLAP analysis", "columnar_lite");
    // OOP relational – falls back to kv.
    check("Objects with inheritance persisted via an ORM", "kv");
}
