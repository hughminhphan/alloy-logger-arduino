import { cloudflareTest } from "@cloudflare/vitest-pool-workers";
import { defineConfig } from "vitest/config";

export default defineConfig({
  plugins: [
    cloudflareTest({
      wrangler: { configPath: "./wrangler.jsonc" },
      miniflare: {
        bindings: {
          ALLOY_DATA_URL: "https://alloy.mock",
          INACTIVITY_MS: "600000",
        },
      },
    }),
  ],
  test: {
    include: ["test/worker.test.ts"],
  },
});
