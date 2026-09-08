#!/usr/bin/env node
/**
 * Copies the built `desentryd` into src-tauri/binaries/ under the name Tauri
 * expects for a sidecar: `desentryd-<target-triple>`.
 *
 * Tauri appends the Rust target triple so one bundle definition can ship the
 * right binary per platform. Getting the name wrong produces a build error
 * late in `tauri build` that reads like a missing file rather than a naming
 * convention, so this does it rather than a README asking someone to.
 *
 *     cd app && node scripts/stage-sidecar.mjs [path/to/desentryd]
 *
 * With no argument it looks in the usual CMake output directories.
 */

import { copyFile, mkdir, chmod, stat } from "node:fs/promises";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { execFileSync } from "node:child_process";

const HERE = dirname(fileURLToPath(import.meta.url));
const APP = resolve(HERE, "..");
const REPO = resolve(APP, "..");
const OUT = join(APP, "src-tauri", "binaries");

const EXE = process.platform === "win32" ? "desentryd.exe" : "desentryd";

const CANDIDATES = [
  join(REPO, "build", EXE),
  join(REPO, "build", "RelWithDebInfo", EXE),
  join(REPO, "build", "Release", EXE),
  join(REPO, "build", "Debug", EXE),
  join(REPO, "build", "apps", "desentry_node", EXE),
];

async function exists(path) {
  try {
    await stat(path);
    return true;
  } catch {
    return false;
  }
}

/** The host target triple, straight from rustc -- the only authority on it. */
function targetTriple() {
  const cargoRustc = join(process.env.USERPROFILE || "", ".cargo", "bin", "rustc.exe");
  const bin = process.platform === "win32" && exists(cargoRustc) ? cargoRustc : "rustc";
  let output;
  try {
    output = execFileSync("rustc", ["-vV"], { encoding: "utf8" });
  } catch {
    output = execFileSync(cargoRustc, ["-vV"], { encoding: "utf8" });
  }
  const line = output.split("\n").find((l) => l.startsWith("host:"));
  if (line === undefined) throw new Error("rustc -vV did not report a host triple");
  return line.slice("host:".length).trim();
}

async function main() {
  const explicit = process.argv[2];
  let source = explicit ? resolve(explicit) : null;

  if (source === null) {
    for (const candidate of CANDIDATES) {
      if (await exists(candidate)) {
        source = candidate;
        break;
      }
    }
  }

  if (source === null || !(await exists(source))) {
    throw new Error(
      `no ${EXE} found. Build it first:\n` +
        `  cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo\n` +
        `  cmake --build build --config RelWithDebInfo\n` +
        `or pass its path: node scripts/stage-sidecar.mjs path/to/${EXE}`,
    );
  }

  const triple = targetTriple();
  const suffix = process.platform === "win32" ? ".exe" : "";
  const destination = join(OUT, `desentryd-${triple}${suffix}`);

  await mkdir(OUT, { recursive: true });
  await copyFile(source, destination);
  if (process.platform !== "win32") {
    // The copy loses the executable bit on some filesystems, and a sidecar
    // that cannot be executed fails at run time rather than at bundle time.
    await chmod(destination, 0o755);
  }

  const info = await stat(destination);
  process.stdout.write(
    `staged ${source}\n     -> ${destination} (${(info.size / 1024 / 1024).toFixed(1)} MiB)\n`,
  );
}

main().catch((error) => {
  process.stderr.write(`stage-sidecar failed: ${error.message}\n`);
  process.exit(1);
});
