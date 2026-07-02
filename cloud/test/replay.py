#!/usr/bin/env python3
"""Replay a captured AlloyLogger session against an ingest endpoint (wrangler dev or prod).

Usage:
  ALLOY_API_KEY=... python3 test/replay.py <ingestUrl> [fixturesDir] [--end]

Posts every <dev>_<chan>_<seq>.csv as /v1/chunk (+ meta.json as /v1/meta), then optionally
/v1/end to trigger immediate finalize. Prints per-request status codes.
"""
import os, re, sys, urllib.request

def post(url, body, headers):
    # workers.dev bot protection 403s the default Python-urllib UA
    req = urllib.request.Request(
        url, data=body, headers={"User-Agent": "AlloyLoggerReplay/1.0", **headers}, method="POST")
    try:
        with urllib.request.urlopen(req) as r:
            return r.status
    except urllib.error.HTTPError as e:
        return e.code

def main():
    ingest = sys.argv[1].rstrip("/")
    fixtures = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else os.path.join(
        os.path.dirname(__file__), "fixtures", "robots-sbr-1782961596")
    send_end = "--end" in sys.argv
    key = os.environ["ALLOY_API_KEY"]

    files = sorted(os.listdir(fixtures))
    metas = [f for f in files if f.endswith("_meta.json")]
    chunks = []
    for f in files:
        m = re.match(r"(.+)_([A-Za-z0-9_-]+)_(\d+)\.csv$", f)
        if m:
            chunks.append((m.group(1), m.group(2), int(m.group(3)), f))
    chunks.sort(key=lambda c: c[2])
    device = chunks[0][0]
    session = os.environ.get("REPLAY_SESSION") or re.search(
        r"(\d+)$", os.path.basename(fixtures)).group(1)
    base_headers = {
        "Authorization": f"Bearer {key}",
        "X-Alloy-Device": device,
        "X-Alloy-Session": session,
        "X-Alloy-Mesh-Path": "robots/sbr-replay",
    }

    for f in metas:
        body = open(os.path.join(fixtures, f), "rb").read()
        code = post(f"{ingest}/v1/meta", body, {**base_headers, "Content-Type": "application/json"})
        print(f"meta {f}: {code}")

    ok = 0
    for dev, chan, seq, f in chunks:
        body = open(os.path.join(fixtures, f), "rb").read()
        code = post(f"{ingest}/v1/chunk", body, {
            **base_headers, "Content-Type": "text/csv",
            "X-Alloy-Channel": chan, "X-Alloy-Seq": str(seq),
        })
        if code == 204:
            ok += 1
        else:
            print(f"chunk {f}: {code}")
    print(f"chunks accepted: {ok}/{len(chunks)}")

    if send_end:
        code = post(f"{ingest}/v1/end", b"", base_headers)
        print(f"end: {code}")

if __name__ == "__main__":
    main()
