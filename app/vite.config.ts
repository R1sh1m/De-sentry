import { defineConfig } from "vite";

// Tauri serves the built assets from `dist/` over the custom `tauri://`
// protocol in production and from this dev server during `tauri dev`.
// `clearScreen: false` keeps the Rust compiler's output visible, which is the
// half of the log that actually matters while developing the sidecar.
export default defineConfig({
  clearScreen: false,
  server: {
    port: 5273,
    strictPort: true,
    watch: {
      // src-tauri is Rust; the Tauri CLI watches it. Having Vite watch it too
      // causes a reload storm on every cargo write.
      ignored: ["**/src-tauri/**"],
    },
  },
  build: {
    // Tauri v2 ships a webview per platform: WebView2 (Chromium) on Windows,
    // WKWebView (Safari) on macOS, WebKitGTK on Linux. WebKitGTK is the
    // laggard, so this is the floor that keeps all three working.
    target: ["es2021", "chrome100", "safari15"],
    minify: process.env.TAURI_DEBUG ? false : "esbuild",
    sourcemap: !!process.env.TAURI_DEBUG,
    outDir: "dist",
    emptyOutDir: true,
  },
  // Everything is bundled. No CDN, no runtime fetch -- the airplane-mode
  // acceptance test covers the UI too, not just the engine.
  envPrefix: ["VITE_", "TAURI_"],
});
