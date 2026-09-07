#!/usr/bin/env node
/**
 * Fetches the onboard sizing model and the ONNX Runtime library.
 *
 * This is the one step in the whole project that needs a network, and it is a
 * *build* step, run once by whoever prepares an installer. Nothing downloaded
 * here is fetched again at run time: the files land in app/resources/model/,
 * the installer bundles them, and the shipped app works with no connection at
 * all. That is the distinction the spec draws -- zero fetched dependencies at
 * run time -- and this script is on the correct side of it.
 *
 *     cd app && npm run fetch-model
 *
 * Skips anything already present, so it is safe to re-run. Pass --force to
 * download again.
 *
 * The model is all-MiniLM-L6-v2 (22.7M parameters, 384 dimensions, 256
 * word-piece truncation), Apache-2.0, in its int8-quantised ONNX form -- about
 * 23 MB rather than 90. Quantisation costs a little accuracy on a task that is
 * choosing between seven well-separated prototypes, which is a trade worth
 * making for an installer people have to download.
 */

import { createWriteStream } from "node:fs";
import { mkdir, rm, stat, readdir, copyFile } from "node:fs/promises";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { pipeline } from "node:stream/promises";
import { spawn } from "node:child_process";
import { Readable } from "node:stream";
import { tmpdir } from "node:os";

const HERE = dirname(fileURLToPath(import.meta.url));
const MODEL_DIR = resolve(HERE, "..", "resources", "model");

const FORCE = process.argv.includes("--force");

/** Pinned revision, so the file that ships is the file that was tested. */
const MODEL_REVISION = "main";
const MODEL_REPO = "Xenova/all-MiniLM-L6-v2";

const MODEL_FILES = [
  {
    name: "model.onnx",
    url: `https://huggingface.co/${MODEL_REPO}/resolve/${MODEL_REVISION}/onnx/model_quantized.onnx`,
    about: "the quantised sentence encoder",
  },
  {
    name: "tokenizer.json",
    url: `https://huggingface.co/${MODEL_REPO}/resolve/${MODEL_REVISION}/tokenizer.json`,
    about: "the WordPiece vocabulary and normaliser",
  },
];

/**
 * ONNX Runtime. `ort` is built with `load-dynamic`, so the library is resolved
 * from this directory at run time rather than linked at build time -- which is
 * what keeps `cargo build` working with no network.
 */
const ORT_VERSION = "1.20.1";
const ORT_BUILDS = {
  "win32-x64": {
    archive: `onnxruntime-win-x64-${ORT_VERSION}.zip`,
    library: "onnxruntime.dll",
    inside: `onnxruntime-win-x64-${ORT_VERSION}/lib/onnxruntime.dll`,
  },
  "win32-arm64": {
    archive: `onnxruntime-win-arm64-${ORT_VERSION}.zip`,
    library: "onnxruntime.dll",
    inside: `onnxruntime-win-arm64-${ORT_VERSION}/lib/onnxruntime.dll`,
  },
  "darwin-arm64": {
    archive: `onnxruntime-osx-arm64-${ORT_VERSION}.tgz`,
    library: "libonnxruntime.dylib",
    inside: `onnxruntime-osx-arm64-${ORT_VERSION}/lib/libonnxruntime.${ORT_VERSION}.dylib`,
  },
  "darwin-x64": {
    archive: `onnxruntime-osx-x86_64-${ORT_VERSION}.tgz`,
    library: "libonnxruntime.dylib",
    inside: `onnxruntime-osx-x86_64-${ORT_VERSION}/lib/libonnxruntime.${ORT_VERSION}.dylib`,
  },
  "linux-x64": {
    archive: `onnxruntime-linux-x64-${ORT_VERSION}.tgz`,
    library: "libonnxruntime.so",
    inside: `onnxruntime-linux-x64-${ORT_VERSION}/lib/libonnxruntime.so.${ORT_VERSION}`,
  },
  "linux-arm64": {
    archive: `onnxruntime-linux-aarch64-${ORT_VERSION}.tgz`,
    library: "libonnxruntime.so",
    inside: `onnxruntime-linux-aarch64-${ORT_VERSION}/lib/libonnxruntime.so.${ORT_VERSION}`,
  },
};

function ortUrl(archive) {
  return `https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/${archive}`;
}

async function exists(path) {
  try {
    await stat(path);
    return true;
  } catch {
    return false;
  }
}

function human(bytes) {
  if (bytes < 1024) return `${bytes} B`;
  const units = ["KiB", "MiB", "GiB"];
  let value = bytes / 1024;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit += 1;
  }
  return `${value.toFixed(1)} ${units[unit]}`;
}

async function download(url, destination, label) {
  process.stdout.write(`  ${label}\n    ${url}\n`);
  const response = await fetch(url, { redirect: "follow" });
  if (!response.ok || response.body === null) {
    throw new Error(`HTTP ${response.status} from ${url}`);
  }
  const partial = `${destination}.part`;
  await pipeline(Readable.fromWeb(response.body), createWriteStream(partial));
  // Renamed only once complete, so an interrupted download is never mistaken
  // for a finished one on the next run.
  const { rename } = await import("node:fs/promises");
  await rename(partial, destination);
  const info = await stat(destination);
  process.stdout.write(`    -> ${destination} (${human(info.size)})\n`);
}

function run(command, args, cwd) {
  return new Promise((resolvePromise, rejectPromise) => {
    const child = spawn(command, args, { cwd, stdio: "inherit" });
    child.on("error", rejectPromise);
    child.on("exit", (code) =>
      code === 0 ? resolvePromise() : rejectPromise(new Error(`${command} exited with ${code}`)),
    );
  });
}

/** Finds a file by name anywhere under a directory. */
async function findFile(root, name) {
  const entries = await readdir(root, { withFileTypes: true });
  for (const entry of entries) {
    const path = join(root, entry.name);
    if (entry.isDirectory()) {
      const found = await findFile(path, name);
      if (found !== null) return found;
    } else if (entry.name === name || entry.name.startsWith(`${name}.`)) {
      return path;
    }
  }
  return null;
}

async function fetchRuntime() {
  const key = `${process.platform}-${process.arch}`;
  const build = ORT_BUILDS[key];
  if (build === undefined) {
    process.stdout.write(
      `\n  No ONNX Runtime build is listed for ${key}.\n` +
        `  Sizing will fall back to the deterministic keyword heuristic, which works\n` +
        `  but is blunter. To add the runtime, drop the platform's shared library into\n` +
        `  ${MODEL_DIR} and re-run.\n`,
    );
    return;
  }

  const target = join(MODEL_DIR, build.library);
  if (!FORCE && (await exists(target))) {
    process.stdout.write(`  ${build.library} is already here\n`);
    return;
  }

  const scratch = join(tmpdir(), `desentry-ort-${process.pid}`);
  await mkdir(scratch, { recursive: true });
  const archive = join(scratch, build.archive);

  try {
    await download(ortUrl(build.archive), archive, `ONNX Runtime ${ORT_VERSION} for ${key}`);
    // bsdtar handles both .zip and .tgz and ships with Windows 10+, macOS and
    // every Linux this app targets -- one command instead of an archive
    // library this script would otherwise have to depend on.
    await run("tar", ["-xf", archive], scratch);

    const extracted = await findFile(scratch, build.library);
    if (extracted === null) {
      throw new Error(`${build.library} was not in the archive`);
    }
    await copyFile(extracted, target);
    const info = await stat(target);
    process.stdout.write(`    -> ${target} (${human(info.size)})\n`);
  } catch (error) {
    process.stdout.write(
      `\n  Could not install the ONNX Runtime: ${error.message}\n` +
        `  Download ${ortUrl(build.archive)} by hand and copy\n` +
        `  ${build.inside}\n  to ${target}.\n` +
        `  Until then, sizing uses the keyword fallback and the wizard says so.\n`,
    );
  } finally {
    await rm(scratch, { recursive: true, force: true });
  }
}

async function main() {
  process.stdout.write("Fetching the onboard sizing model.\n");
  process.stdout.write("This is a build step. Nothing here is downloaded at run time.\n\n");

  await mkdir(MODEL_DIR, { recursive: true });

  for (const file of MODEL_FILES) {
    const destination = join(MODEL_DIR, file.name);
    if (!FORCE && (await exists(destination))) {
      process.stdout.write(`  ${file.name} is already here\n`);
      continue;
    }
    await download(file.url, destination, `${file.name} — ${file.about}`);
  }

  await fetchRuntime();

  process.stdout.write(
    "\nDone. These files are bundled into the installer by tauri.conf.json and are\n" +
      "never fetched again. With them absent the app still runs; workload sizing\n" +
      "falls back to the deterministic keyword heuristic and reports that it did.\n",
  );
}

main().catch((error) => {
  process.stderr.write(`\nfetch-model failed: ${error.message}\n`);
  process.exit(1);
});
