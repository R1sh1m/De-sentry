import { defineConfig } from "vitest/config";

export default defineConfig({
  test: {
    environment: "jsdom",
    globals: true,
    include: ["tests/**/*.test.ts"],
    deps: {
      inline: ["@tauri-apps/api", "@tauri-apps/plugin-updater"],
    },
  },
});
