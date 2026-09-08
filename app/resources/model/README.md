# Sizing model lives here

This directory is intentionally almost empty in the source tree.

`npm run fetch-model` downloads all-MiniLM-L6-v2 (Apache-2.0) and the ONNX
Runtime into it as a **build step**; the app never fetches anything at run
time. The files are ~100 MB of binaries nobody can review in a diff, so they
are not committed — `.gitignore` keeps everything here except this README.

The directory itself has to exist even when the model does not, because
`src-tauri/tauri.conf.json` lists `resources/model/` as a bundled resource and
Tauri's build script fails outright on a missing resource path. Deleting this
file therefore breaks the build for anyone who has not run the download —
which is the opposite of the intent: **the app is designed to work without
it.** With no model present (or built with `--no-default-features`), the
creation wizard proposes engines using the deterministic keyword heuristic in
`src-tauri/src/ai.rs` and labels the result as the fallback rather than
presenting it as a model decision.

Expected contents after `npm run fetch-model`:

    model.onnx          quantized all-MiniLM-L6-v2, 384-dim embeddings
    tokenizer.json      word-piece vocabulary, 256-token truncation
    onnxruntime.<ext>   the runtime `ort` loads dynamically at startup
