// Worker entry — validates, authenticates, and routes device traffic to the session DO.
// One POST per sealed device buffer; the DO does everything stateful.

import { authenticate, sha256Hex } from "./auth";
import type { Env } from "./types";

export { SessionDO } from "./session-do";

const MAX_CHUNK_BYTES = 64 * 1024; // device buffers are 12KB — generous headroom
const MAX_META_BYTES = 16 * 1024;
const NAME_RE = /^[A-Za-z0-9_-]{1,64}$/; // device + channel (device sanitizes these already)
const SESSION_RE = /^\d{1,12}$/; // epoch seconds
const MESH_RE = /^[A-Za-z0-9_\/-]{1,128}$/;

function bad(msg: string): Response {
  return new Response(msg, { status: 400 });
}

export default {
  async fetch(req: Request, env: Env): Promise<Response> {
    const url = new URL(req.url);

    if (url.pathname === "/v1/health") return Response.json({ ok: true });

    if (!["/v1/chunk", "/v1/meta", "/v1/end"].includes(url.pathname)) {
      return new Response("not found", { status: 404 });
    }
    if (req.method !== "POST") return new Response("method not allowed", { status: 405 });

    const apiKey = (req.headers.get("Authorization") ?? "").replace(/^Bearer\s+/i, "");
    if (!apiKey) return new Response("missing bearer", { status: 401 });

    const device = req.headers.get("X-Alloy-Device") ?? "";
    const session = req.headers.get("X-Alloy-Session") ?? "";
    const meshPath = (req.headers.get("X-Alloy-Mesh-Path") ?? "").replace(/^\/+|\/+$/g, "");
    if (!NAME_RE.test(device)) return bad("bad X-Alloy-Device");
    if (!SESSION_RE.test(session)) return bad("bad X-Alloy-Session");
    if (!MESH_RE.test(meshPath)) return bad("bad X-Alloy-Mesh-Path");

    const len = Number(req.headers.get("Content-Length") ?? "0");
    if (url.pathname === "/v1/chunk") {
      const channel = req.headers.get("X-Alloy-Channel") ?? "";
      const seq = req.headers.get("X-Alloy-Seq") ?? "";
      if (!NAME_RE.test(channel)) return bad("bad X-Alloy-Channel");
      if (!/^\d{1,9}$/.test(seq)) return bad("bad X-Alloy-Seq");
      if (len > MAX_CHUNK_BYTES) return new Response("chunk too large", { status: 413 });
    } else if (url.pathname === "/v1/meta" && len > MAX_META_BYTES) {
      return new Response("meta too large", { status: 413 });
    }

    if (!(await authenticate(env, apiKey, meshPath))) {
      return new Response("invalid api key", { status: 401 });
    }

    const keyHash = await sha256Hex(apiKey);
    const id = env.SESSION_DO.idFromName(`${keyHash}:${device}:${session}`);
    const stub = env.SESSION_DO.get(id);
    const fwd = new Request(req, { headers: new Headers(req.headers) });
    fwd.headers.set("X-Internal-Key-Hash", keyHash);
    return stub.fetch(fwd);
  },
} satisfies ExportedHandler<Env>;
