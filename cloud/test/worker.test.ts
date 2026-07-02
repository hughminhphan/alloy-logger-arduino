// Integration tests: Worker routing/auth + SessionDO state machine, against mocked Alloy
// endpoints (upload-session + R2 PUT) so the FULL finalize path runs, SigV4 signing included.

import { beforeEach, describe, expect, it } from "vitest";
import {
  env,
  runDurableObjectAlarm,
  runInDurableObject,
  SELF,
} from "cloudflare:test";

const GOOD_KEY = "test-key-good";
const BAD_KEY = "test-key-bad";
const DEVICE = "esp32-abc123";
const MESH = "robots/test";
// storage is shared across tests within this file — a unique session per test
// keeps DO instances and staging keys from colliding
let sessionCounter = 1782000000;
let SESSION = "";

const uploadSessionBody = () => ({
  bucket: "test-bucket",
  endpoint_url: "https://r2.mock",
  region: "auto",
  prefix: `uploads/sdk-uploads/${MESH}/${SESSION}/`,
  expires_at: "2099-01-01T00:00:00Z",
  credentials: {
    access_key_id: "AKIATEST",
    secret_access_key: "secret",
    session_token: "token",
  },
});

// PUTs into the mocked mesh, keyed by path — lets tests assert what landed
let meshPuts: string[] = [];

// the mock fetch persists across tests, so it is installed once and consults
// module-level knobs (fetchMock was removed in vitest-pool-workers 0.13; the main
// worker + DO run in the same isolate as tests, so patching globalThis.fetch applies)
let failsLeft = 0;
let mocksRegistered = false;

function mockAlloy(opts: { failPuts?: number } = {}) {
  failsLeft = opts.failPuts ?? 0;
  if (mocksRegistered) return;
  mocksRegistered = true;
  globalThis.fetch = (async (input: RequestInfo | URL, init?: RequestInit) => {
    const req = new Request(input, init);
    const url = new URL(req.url);
    if (url.origin === "https://alloy.mock") {
      if (url.pathname === "/mesh/storage/upload-session" && req.method === "POST") {
        if (req.headers.get("Authorization") === `Bearer ${GOOD_KEY}`) {
          return Response.json(uploadSessionBody());
        }
        return new Response("forbidden", { status: 403 });
      }
      return new Response("not found", { status: 404 });
    }
    if (url.origin === "https://r2.mock" && req.method === "PUT") {
      if (failsLeft > 0) {
        failsLeft--;
        return new Response("injected failure", { status: 500 });
      }
      meshPuts.push(url.pathname);
      return new Response("", { status: 200 });
    }
    throw new Error(`unmocked outbound fetch: ${req.method} ${req.url}`);
  }) as typeof fetch;
}

function post(
  path: string,
  body: string | null,
  headers: Record<string, string> = {},
): Promise<Response> {
  return SELF.fetch(`https://ingest.alloylogger.com${path}`, {
    method: "POST",
    headers: {
      Authorization: `Bearer ${GOOD_KEY}`,
      "X-Alloy-Device": DEVICE,
      "X-Alloy-Session": SESSION,
      "X-Alloy-Mesh-Path": MESH,
      ...headers,
    },
    body,
  });
}

function chunk(channel: string, seq: number, csv: string): Promise<Response> {
  return post("/v1/chunk", csv, {
    "X-Alloy-Channel": channel,
    "X-Alloy-Seq": String(seq),
    "Content-Type": "text/csv",
  });
}

interface StoredState {
  phase: string;
  finalizeAttempts: number;
  apiKey: string;
}

// /v1/end schedules setAlarm(now) and workerd auto-fires it in the background, so tests must
// await the resulting state rather than fire the alarm themselves.
async function waitForState(
  pred: (st: StoredState) => boolean,
  ms = 3000,
): Promise<StoredState> {
  const stub = await sessionStub();
  const t0 = Date.now();
  for (;;) {
    const st = (await runInDurableObject(stub, (_i: unknown, state: DurableObjectState) =>
      state.storage.get("state"),
    )) as StoredState | undefined;
    if (st && pred(st)) return st;
    if (Date.now() - t0 > ms) {
      throw new Error(`timeout waiting for DO state, last: ${JSON.stringify(st)}`);
    }
    await new Promise((r) => setTimeout(r, 50));
  }
}

async function sessionStub() {
  const digest = await crypto.subtle.digest(
    "SHA-256",
    new TextEncoder().encode(GOOD_KEY),
  );
  const keyHash = [...new Uint8Array(digest)]
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("");
  const id = env.SESSION_DO.idFromName(`${keyHash}:${DEVICE}:${SESSION}`);
  return env.SESSION_DO.get(id);
}

beforeEach(() => {
  SESSION = String(sessionCounter++);
  meshPuts = [];
});

describe("worker routing + auth", () => {
  it("health check", async () => {
    const res = await SELF.fetch("https://ingest.alloylogger.com/v1/health");
    expect(res.status).toBe(200);
  });

  it("rejects missing bearer", async () => {
    const res = await SELF.fetch("https://ingest.alloylogger.com/v1/chunk", {
      method: "POST",
      body: "x",
    });
    expect(res.status).toBe(401);
  });

  it("rejects an invalid key via the Alloy auth oracle", async () => {
    mockAlloy();
    const res = await post("/v1/chunk", "t_ns,x\n0000000000000000001,1\n", {
      Authorization: `Bearer ${BAD_KEY}`,
      "X-Alloy-Channel": "io",
      "X-Alloy-Seq": "0",
    });
    expect(res.status).toBe(401);
  });

  it("rejects malformed headers", async () => {
    mockAlloy();
    expect(
      (await post("/v1/chunk", "x", { "X-Alloy-Channel": "bad channel!", "X-Alloy-Seq": "0" }))
        .status,
    ).toBe(400);
    expect(
      (
        await post("/v1/chunk", "x", {
          "X-Alloy-Channel": "io",
          "X-Alloy-Seq": "0",
          "X-Alloy-Session": "not-digits",
        })
      ).status,
    ).toBe(400);
  });

  it("rejects oversized chunks", async () => {
    mockAlloy();
    const res = await post("/v1/chunk", "x".repeat(70 * 1024), {
      "X-Alloy-Channel": "io",
      "X-Alloy-Seq": "0",
    });
    expect(res.status).toBe(413);
  });
});

describe("session lifecycle", () => {
  it("stages chunks, dedupes retries, finalizes on /v1/end, purges, then 409s", async () => {
    mockAlloy();
    expect((await chunk("io", 0, "t_ns,btn\n0000000000000000100,1\n")).status).toBe(204);
    expect((await chunk("io", 1, "t_ns,btn\n0000000000000000200,0\n")).status).toBe(204);
    expect((await chunk("io", 1, "t_ns,btn\n0000000000000000200,0\n")).status).toBe(204); // retry
    expect((await post("/v1/meta", JSON.stringify({ device: DEVICE, fields: [] }))).status).toBe(
      204,
    );

    // dedupe: exactly 2 staged chunk objects (+1 meta)
    const staged = await env.STAGING.list();
    const mine = staged.objects.filter((o: R2Object) => o.key.includes(`/${SESSION}/`));
    expect(mine.filter((o: R2Object) => o.key.endsWith(".csv")).length).toBe(2);

    expect((await post("/v1/end", null)).status).toBe(202);
    await runDurableObjectAlarm(await sessionStub()); // no-op if it already auto-fired
    await waitForState((st) => st.phase === "done");

    // one mcap + the meta sidecar landed in the (mocked) user mesh
    expect(meshPuts).toContainEqual(
      `/test-bucket/uploads/sdk-uploads/${MESH}/${SESSION}/${DEVICE}_${SESSION}.mcap`,
    );
    expect(meshPuts).toContainEqual(
      `/test-bucket/uploads/sdk-uploads/${MESH}/${SESSION}/${DEVICE}_meta.json`,
    );

    // staging purged, tombstone written, key gone
    const after = await env.STAGING.list();
    expect(after.objects.filter((o: R2Object) => o.key.includes(`/${SESSION}/`)).length).toBe(0);
    const stub = await sessionStub();
    const { tombstone, apiKey } = (await runInDurableObject(
      stub,
      async (_i: unknown, state: DurableObjectState) => ({
        tombstone: await state.storage.get("tombstone"),
        apiKey: ((await state.storage.get("state")) as { apiKey: string }).apiKey,
      }),
    )) as { tombstone: { mcapBytes: number }; apiKey: string };
    expect(tombstone.mcapBytes).toBeGreaterThan(0);
    expect(apiKey).toBe("");

    // late chunk → 409; late end → 204
    expect((await chunk("io", 2, "t_ns,btn\n0000000000000000300,1\n")).status).toBe(409);
    expect((await post("/v1/end", null)).status).toBe(204);
  });

  it("finalizes on the inactivity alarm (power-loss path)", async () => {
    mockAlloy();
    await chunk("adc", 0, "t_ns,v\n0000000000000000100,3.3\n");
    const fired = await runDurableObjectAlarm(await sessionStub());
    expect(fired).toBe(true);
    expect(meshPuts.some((p) => p.endsWith(".mcap"))).toBe(true);
  });

  it("accepts out-of-order seqs and still assembles", async () => {
    mockAlloy();
    await chunk("io", 5, "t_ns,btn\n0000000000000000500,1\n");
    await chunk("io", 2, "t_ns,btn\n0000000000000000200,0\n");
    await post("/v1/end", null);
    await runDurableObjectAlarm(await sessionStub());
    await waitForState((st) => st.phase === "done");
    const stub = await sessionStub();
    const tomb = (await runInDurableObject(stub, (_i: unknown, state: DurableObjectState) =>
      state.storage.get("tombstone"),
    )) as { mcapBytes: number };
    expect(tomb.mcapBytes).toBeGreaterThan(0);
  });

  it("retries finalize with backoff after an upstream failure, then succeeds", async () => {
    mockAlloy({ failPuts: 1 }); // first mesh PUT 500s
    await chunk("io", 0, "t_ns,btn\n0000000000000000100,1\n");
    await post("/v1/end", null);

    // attempt 1 auto-fires and fails against the injected 500; backoff alarm gets set
    const st = await waitForState((s) => s.finalizeAttempts === 1);
    expect(st.phase).toBe("finalizing");

    // chunks are refused mid-finalize
    expect((await chunk("io", 1, "t_ns,btn\n0000000000000000200,0\n")).status).toBe(503);

    // fast-forward the backoff alarm — retry succeeds
    const fired = await runDurableObjectAlarm(await sessionStub());
    expect(fired).toBe(true);
    await waitForState((s) => s.phase === "done");
    expect(meshPuts.some((p) => p.endsWith(".mcap"))).toBe(true);
  });
});
