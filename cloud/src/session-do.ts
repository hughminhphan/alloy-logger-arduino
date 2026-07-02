// SessionDO — one instance per (keyHash, device, session). The serialization point that makes
// "one run = one MCAP" work: dedupes chunk retries, tracks the run via an inactivity alarm
// (power loss can only be detected server-side — the black box never says goodbye), and on
// finalize k-way-merges the staged CSVs into one indexed MCAP PUT into the user's own mesh.
//
// State machine: receiving → finalizing → done. Finalize is idempotent (the artifact is a
// deterministic PUT), so alarm retries and crashes mid-finalize just redo the work.

import { assembleMcap } from "./mcap";
import type { ChunkSource } from "./csv";
import { mintUploadSession, putToMesh } from "./mesh";
import type { DeviceMeta, Env } from "./types";

const MAX_FINALIZE_ATTEMPTS = 10;
const RATE_LIMIT_PER_MIN = 120;
// Bounds for the device-chosen inactivity window (X-Alloy-Finalize-Ms). The floor protects a run
// from being split by a routine WiFi hiccup; the ceiling bounds how long staged data can idle.
const FINALIZE_MS_MIN = 30_000;
const FINALIZE_MS_MAX = 30 * 60_000;

interface State {
  phase: "receiving" | "finalizing" | "done";
  apiKey: string; // held ONLY for the session's lifetime; purged the moment finalize succeeds
  keyHash16: string;
  device: string;
  session: string;
  meshPath: string;
  lastChunkAt: number;
  finalizeAttempts: number;
  inactivityMs: number;
}

export class SessionDO implements DurableObject {
  private recentChunks: number[] = []; // sliding-window rate limiter (in-memory is fine per-DO)

  constructor(
    private ctx: DurableObjectState,
    private env: Env,
  ) {
    ctx.storage.sql.exec(
      `CREATE TABLE IF NOT EXISTS chunks(
         channel TEXT NOT NULL, seq INTEGER NOT NULL, r2key TEXT NOT NULL, bytes INTEGER NOT NULL,
         PRIMARY KEY(channel, seq));`,
    );
  }

  private async state(): Promise<State | undefined> {
    return this.ctx.storage.get<State>("state");
  }

  private stagePrefix(s: State): string {
    return `stage/${s.keyHash16}/${s.device}/${s.session}/`;
  }

  async fetch(req: Request): Promise<Response> {
    const url = new URL(req.url);
    const route = url.pathname; // /v1/chunk | /v1/meta | /v1/end (Worker validated everything)
    let s = await this.state();

    if (s?.phase === "done") {
      return route === "/v1/end" ? new Response(null, { status: 204 }) : new Response("session finalized", { status: 409 });
    }
    if (s?.phase === "finalizing") {
      if (route === "/v1/end") return new Response(null, { status: 202 });
      return new Response("finalize in progress", { status: 503, headers: { "Retry-After": "5" } });
    }

    if (!s) {
      // first request of the run — pin identity + the key we'll need at finalize time
      const apiKey = (req.headers.get("Authorization") ?? "").replace(/^Bearer\s+/i, "");
      const wantMs = Number(req.headers.get("X-Alloy-Finalize-Ms") ?? this.env.INACTIVITY_MS);
      const inactivityMs = Math.min(FINALIZE_MS_MAX, Math.max(FINALIZE_MS_MIN, wantMs || 0));
      s = {
        phase: "receiving",
        apiKey,
        keyHash16: req.headers.get("X-Internal-Key-Hash")!.slice(0, 16),
        device: req.headers.get("X-Alloy-Device")!,
        session: req.headers.get("X-Alloy-Session")!,
        meshPath: req.headers.get("X-Alloy-Mesh-Path")!,
        lastChunkAt: Date.now(),
        finalizeAttempts: 0,
        inactivityMs,
      };
      await this.ctx.storage.put("state", s);
    }

    switch (route) {
      case "/v1/chunk":
        return this.onChunk(req, s);
      case "/v1/meta":
        return this.onMeta(req, s);
      case "/v1/end":
        return this.onEnd(s);
      default:
        return new Response("not found", { status: 404 });
    }
  }

  private rateLimited(): boolean {
    const now = Date.now();
    this.recentChunks = this.recentChunks.filter((t) => now - t < 60_000);
    if (this.recentChunks.length >= RATE_LIMIT_PER_MIN) return true;
    this.recentChunks.push(now);
    return false;
  }

  private async armAlarm(s: State): Promise<void> {
    s.lastChunkAt = Date.now();
    await this.ctx.storage.put("state", s);
    await this.ctx.storage.setAlarm(
      Date.now() + (s.inactivityMs || Number(this.env.INACTIVITY_MS)),
    );
  }

  private async onChunk(req: Request, s: State): Promise<Response> {
    if (this.rateLimited()) return new Response("rate limited", { status: 429 });
    const channel = req.headers.get("X-Alloy-Channel")!;
    const seq = Number(req.headers.get("X-Alloy-Seq"));

    const dup = this.ctx.storage.sql
      .exec("SELECT 1 FROM chunks WHERE channel = ? AND seq = ?", channel, seq)
      .toArray();
    if (dup.length > 0) {
      await this.armAlarm(s); // a retry still proves the device is alive
      return new Response(null, { status: 204 });
    }

    const body = new Uint8Array(await req.arrayBuffer());
    const r2key = `${this.stagePrefix(s)}${channel}/${String(seq).padStart(10, "0")}.csv`;
    await this.env.STAGING.put(r2key, body);
    this.ctx.storage.sql.exec(
      "INSERT INTO chunks(channel, seq, r2key, bytes) VALUES (?, ?, ?, ?)",
      channel,
      seq,
      r2key,
      body.byteLength,
    );
    await this.armAlarm(s);
    return new Response(null, { status: 204 });
  }

  private async onMeta(req: Request, s: State): Promise<Response> {
    const body = new Uint8Array(await req.arrayBuffer());
    await this.env.STAGING.put(`${this.stagePrefix(s)}_meta.json`, body); // idempotent overwrite
    await this.armAlarm(s);
    return new Response(null, { status: 204 });
  }

  private async onEnd(s: State): Promise<Response> {
    s.phase = "finalizing";
    await this.ctx.storage.put("state", s);
    await this.ctx.storage.setAlarm(Date.now()); // run finalize from the alarm handler, not the request
    return new Response(null, { status: 202 });
  }

  async alarm(): Promise<void> {
    const s = await this.state();
    if (!s || s.phase === "done") return;

    if (s.phase === "receiving") {
      // an alarm only ever fires INACTIVITY_MS after the last armAlarm(), so the run is over
      s.phase = "finalizing";
      await this.ctx.storage.put("state", s);
    }

    try {
      await this.finalize(s);
    } catch (err) {
      s.finalizeAttempts++;
      await this.ctx.storage.put("state", s);
      console.error(
        `finalize failed (attempt ${s.finalizeAttempts}) device=${s.device} session=${s.session}: ${err}`,
      );
      if (s.finalizeAttempts < MAX_FINALIZE_ATTEMPTS) {
        const backoffMin = Math.min(2 ** s.finalizeAttempts, 60);
        await this.ctx.storage.setAlarm(Date.now() + backoffMin * 60_000);
      }
      // else: give up — staged data ages out via the bucket's 7-day lifecycle rule
    }
  }

  private async finalize(s: State): Promise<void> {
    const rows = this.ctx.storage.sql
      .exec<{ channel: string; seq: number; r2key: string; bytes: number }>(
        "SELECT channel, seq, r2key, bytes FROM chunks ORDER BY channel, seq",
      )
      .toArray();
    if (rows.length === 0) {
      await this.markDone(s, 0); // nothing ever arrived (meta-only session) — just close out
      return;
    }

    // group by channel; rows are already seq-ordered within each channel
    const byChannel = new Map<string, { r2key: string }[]>();
    for (const r of rows) {
      let list = byChannel.get(r.channel);
      if (!list) byChannel.set(r.channel, (list = []));
      list.push({ r2key: r.r2key });
    }
    const staging = this.env.STAGING;
    const sources: ChunkSource[] = [...byChannel.entries()].map(([channel, refs]) => ({
      channel,
      chunks: async function* () {
        for (const ref of refs) {
          const obj = await staging.get(ref.r2key);
          if (!obj) throw new Error(`staged chunk missing: ${ref.r2key}`);
          yield new Uint8Array(await obj.arrayBuffer());
        }
      },
    }));

    const metaObj = await staging.get(`${this.stagePrefix(s)}_meta.json`);
    let meta: DeviceMeta | null = null;
    let metaBytes: Uint8Array | null = null;
    if (metaObj) {
      metaBytes = new Uint8Array(await metaObj.arrayBuffer());
      try {
        meta = JSON.parse(new TextDecoder().decode(metaBytes)) as DeviceMeta;
      } catch {
        meta = null; // corrupt sidecar — assemble without semantics rather than fail the run
      }
    }

    const mcap = (await assembleMcap(sources, meta, {
      device: s.device,
      session: s.session,
      meshPath: s.meshPath,
    }))!;

    if (this.env.DRY_RUN !== "1") {
      const sess = await mintUploadSession(
        this.env.ALLOY_DATA_URL,
        s.apiKey,
        `${s.meshPath}/${s.session}`,
      );
      if (!sess) throw new Error("upload-session mint failed at finalize");
      if (!(await putToMesh(sess, `${s.device}_${s.session}.mcap`, mcap, "application/octet-stream"))) {
        throw new Error("mcap PUT failed");
      }
      if (metaBytes) {
        // best-effort: the semantics sidecar helps Alloy AI but must not fail the run
        await putToMesh(sess, `${s.device}_meta.json`, metaBytes, "application/json").catch(() => {});
      }
    }

    await this.markDone(s, mcap.byteLength);
  }

  private async markDone(s: State, mcapBytes: number): Promise<void> {
    // purge staging (batched; R2 delete takes ≤1000 keys per call)
    const keys = this.ctx.storage.sql
      .exec<{ r2key: string }>("SELECT r2key FROM chunks")
      .toArray()
      .map((r) => r.r2key);
    keys.push(`${this.stagePrefix(s)}_meta.json`);
    for (let i = 0; i < keys.length; i += 1000) {
      await this.env.STAGING.delete(keys.slice(i, i + 1000));
    }
    this.ctx.storage.sql.exec("DELETE FROM chunks");

    // tombstone for observability; the API key is GONE from storage past this point
    await this.ctx.storage.put("state", {
      ...s,
      apiKey: "",
      phase: "done" as const,
    });
    await this.ctx.storage.put("tombstone", {
      device: s.device,
      session: s.session,
      meshPath: s.meshPath,
      mcapBytes,
      finalizedAt: Date.now(),
    });
    await this.ctx.storage.deleteAlarm();
  }
}
