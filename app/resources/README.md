# App resources

Two things live here, and they behave differently.

## `prototypes.json` — checked in

The seven workload shapes the sizing step chooses between, with the sentence
that describes each one, the keyword list the deterministic fallback uses, and
the node shape each implies (engines, quota split, shard key, replication
factor, indexes, retention, draft collections).

It is small, it is text, and it is the part a person would want to argue with —
so it is in the repository rather than generated.

**There are no embedding vectors in it, deliberately.** The prototype sentences
are embedded at load time with whatever model is actually installed, which
guarantees the prototypes and the user's description live in the same vector
space. A checked-in vector would become quietly meaningless the day the model
was replaced or re-quantised, and nothing would report it.

## `model/` — not checked in

| File | What it is | Size |
| --- | --- | --- |
| `model.onnx` | all-MiniLM-L6-v2, int8-quantised (22.7M params, 384-dim, 256 word-piece truncation), Apache-2.0 | ~23 MB |
| `tokenizer.json` | its WordPiece vocabulary and normaliser | ~700 KB |
| `onnxruntime.dll` / `libonnxruntime.dylib` / `libonnxruntime.so` | the ONNX Runtime, loaded dynamically at startup | 10–20 MB |

Fetch them once, before building an installer:

```
cd app
npm run fetch-model
```

They are gitignored. Roughly 40 MB of binaries in a source repository is 40 MB
that every clone pays for and nobody can review, and the script that fetches
them is three URLs and a checksum-free extract — reproducible enough that
carrying the artefacts adds nothing.

### This does not make the app fetch anything at run time

`npm run fetch-model` is a **build** step. The files it downloads are bundled
into the installer by `tauri.conf.json`, and the shipped app reads them from
its own resource directory. No code path in the app makes an outbound request;
the airplane-mode acceptance test covers the sizing step along with everything
else.

### With the model absent

The app still runs, and sizing still works — through the deterministic keyword
heuristic in `src-tauri/src/ai.rs`. The wizard says so on screen, with the
reason the model could not be loaded, and the decision recorded into the node's
`manifest.json` carries `"method": "keyword"`.

That is the whole point of the fallback: a proposal whose provenance is hidden
is a proposal nobody can argue with. It is never presented as a model result.

### Building without ONNX at all

`cargo build --no-default-features` drops the `ort` and `tokenizers`
dependencies entirely. The app builds smaller and faster, sizing uses the
keyword path, and the UI reports `this build was compiled without the onnx
feature`. Useful on a machine where the native runtime will not build; not what
a release should ship.


## How these files reach the app

`src-tauri/tauri.conf.json` lists them under `bundle.resources`, and **its
source paths are relative to that file** -- i.e. to `src-tauri/`, not to
`app/`. These assets live one directory up, so the entries read:

    "../resources/prototypes.json": "resources/prototypes.json"
    "../resources/model/":          "resources/model/"

The `../` is load-bearing. Without it the build fails before compiling a line
of Rust, with `resource path 'resources\model' doesn't exist`.

The value of each entry is where the file lands *inside the bundle*, and that
is what `src-tauri/src/ai.rs` resolves against the resource directory at run
time -- so the targets stay `resources/...` even though the sources are
`../resources/...`. Do not "simplify" the two sides to match.

Note also that tauri.conf.json is validated against a strict schema: unlike
`config/node.example.json`, it rejects `"//"` comment keys outright. Notes
about that file belong here.
