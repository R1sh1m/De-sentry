#!/usr/bin/env node
/**
 * Wrapper for tauri CLI that ensures sidecar is staged before build commands
 */
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";
import { existsSync } from "node:fs";

const HERE = dirname(fileURLToPath(import.meta.url));
const APP = resolve(HERE, "..");

const args = process.argv.slice(2);

// If it's a build command, ensure staging happens
if (args[0] === "build" || args.includes("build")) {
  console.log("Staging sidecar before Tauri build...");
  const stageProc = spawn("node", ["scripts/stage-sidecar.mjs"], {
    cwd: APP,
    stdio: "inherit",
    shell: process.platform === "win32"
  });
  
  stageProc.on("close", (code) => {
    if (code !== 0) {
      console.error("Sidecar staging failed");
      process.exit(code);
    }
    // Run tauri build
    const tauriProc = spawn("tauri", args, {
      cwd: APP,
      stdio: "inherit",
      shell: process.platform === "win32"
    });
    tauriProc.on("close", (code) => process.exit(code));
  });
} else {
  // For other commands, just pass through
  const tauriProc = spawn("tauri", args, {
    cwd: APP,
    stdio: "inherit",
    shell: process.platform === "win32"
  });
  tauriProc.on("close", (code) => process.exit(code));
}
