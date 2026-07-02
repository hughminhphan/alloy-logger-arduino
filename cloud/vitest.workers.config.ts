import { defineWorkersConfig } from "@cloudflare/vitest-pool-workers/config";

export default defineWorkersConfig({
  test: {
    include: ["test/worker.test.ts"],
    poolOptions: {
      workers: {
        // Isolated-storage stacking breaks with SQLite-backed DO alarms; tests use unique
        // session ids instead so shared storage never collides.
        isolatedStorage: false,
        singleWorker: true,
        wrangler: { configPath: "./wrangler.jsonc" },
        miniflare: {
          bindings: {
            ALLOY_DATA_URL: "https://alloy.mock",
            INACTIVITY_MS: "600000",
          },
        },
      },
    },
  },
});
