import { defineConfig } from "vitest/config";

// Pure-node tests (assembler, csv). Worker/DO tests use vitest.workers.config.ts.
export default defineConfig({
  test: {
    include: ["test/mcap.test.ts", "test/csv.test.ts"],
    environment: "node",
  },
});
