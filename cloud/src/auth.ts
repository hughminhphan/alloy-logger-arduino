// Bearer-key validation. Alloy has no whoami endpoint, so a successful upload-session mint IS the
// proof the key is live. Verdicts are cached in the Cache API keyed by the key's sha256 — the raw
// key never appears in a cache key, log line, or error.

import { mintUploadSession } from "./mesh";
import type { Env } from "./types";

export async function sha256Hex(s: string): Promise<string> {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(s));
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

const POSITIVE_TTL = 300; // 5 min — bounds auth-oracle calls to Alloy
const NEGATIVE_TTL = 60; // blunts brute force without locking out a fixed typo for long

export async function authenticate(
  env: Env,
  apiKey: string,
  meshPath: string,
): Promise<boolean> {
  const keyHash = await sha256Hex(apiKey);
  const cacheKey = new Request(`https://auth.alloylogger.internal/${keyHash}`);
  const cache = caches.default;

  const hit = await cache.match(cacheKey);
  if (hit) return (await hit.text()) === "1";

  const sess = await mintUploadSession(env.ALLOY_DATA_URL, apiKey, meshPath);
  const ok = sess !== null;
  await cache.put(
    cacheKey,
    new Response(ok ? "1" : "0", {
      headers: { "Cache-Control": `max-age=${ok ? POSITIVE_TTL : NEGATIVE_TTL}` },
    }),
  );
  return ok;
}
