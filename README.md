# esp32-robolog

A robot **black-box recorder** for ESP32-class hardware. Logs sensor *inputs* + actuator
*outputs* (IMU, ADC, encoders, motor command setpoints, GPIO/e-stop) on **one timeline**, writes
**JSON-Lines** to SD, and uploads each finalized run **directly to Alloy** — no laptop, no
intermediary. Scope: numeric control-loop data only (no camera, no audio), so a bare ESP32 has
50–100× write-bandwidth headroom and there's no cross-stream clock drift (one control tick = one
`esp_timer` ns timestamp = everything aligned by construction).

## Status
- ✅ **Verified live on real hardware (2026-06-29):** a classic ESP32 flashed with the smoke test
  connected WiFi → SNTP → `upload-session` → SigV4 PUT → landed `smoke.jsonl` in Alloy Mesh Storage
  (Cloudflare R2, path-style, region `auto`, `UNSIGNED-PAYLOAD`). Serial: `UPLOAD OK`.
- ✅ Both sketches compile clean on `esp32:esp32:esp32` (main 84% flash, smoke 83%).
- ⏳ Full recorder (`esp32-robolog.ino`) needs an SPI SD card wired + real sensors in the `// TODO` blocks.

## Files
| File | Role |
|---|---|
| `RoboLog.h` | Logger: channels + field layouts, bounded FreeRTOS queue, core-1 JSONL writer, on-change helper, `end()` |
| `AlloyUploader.h` | Direct Alloy upload: `upload-session` POST + SigV4 R2 PutObject (works on any `fs::FS` — SD or LittleFS) |
| `esp32-robolog.ino` | Main recorder: control loop → record run → finalize → upload from SD |
| `upload_smoketest/` | No-SD smoke test: writes a tiny JSONL to LittleFS and uploads it (proves the MCU→Alloy link) |
| `PLAN.md` | Full design, Option 2 (now) → Option 3 (server shim + MCAP, later), deferred items |

## Setup
```bash
cp secrets.h.example secrets.h     # fill WiFi + Alloy key; secrets.h is gitignored
arduino-cli core install esp32:esp32
arduino-cli lib install ArduinoJson
```

## Build / flash
```bash
# no-SD smoke test first (proves WiFi → SNTP → upload-session → SigV4 → R2)
arduino-cli compile -u -p <PORT> --fqbn esp32:esp32:esp32 ./upload_smoketest

# full recorder (needs an SPI SD card wired; SD_CS in the .ino)
arduino-cli compile -u -p <PORT> --fqbn esp32:esp32:esp32 .
```
Serial @115200. On success: `UPLOAD OK` / `uploaded to Alloy` → file appears under
`uploads/sdk-uploads/<MESH_PATH>/` in Alloy Mesh Storage.

## Alloy direct-upload protocol (confirmed)
1. `POST {ALLOY_DATA_URL}/mesh/storage/upload-session`, `Authorization: Bearer {ALLOY_API_KEY}`,
   body `{"path":"<folder>","ttl_seconds":900}` → `{bucket, endpoint_url, region, prefix,
   credentials{access_key_id, secret_access_key, session_token}}`.
2. **S3 PutObject** `{endpoint_url}/{bucket}/{prefix}{file}` SigV4-signed with the temp creds
   (`x-amz-security-token`), `x-amz-content-sha256: UNSIGNED-PAYLOAD`. Needs an SNTP UTC clock.

Backend is Cloudflare R2. `.jsonl` ingests natively into Alloy's queryable tables (SQL/DuckDB).

## Roadmap → Option 3 (live streaming)
Stream frames to a small server-side shim co-located with Alloy that assembles **MCAP** (rich
Replay/Inspect) and uploads via the Python SDK — also removes SigV4/keys from the MCU. The
channel/field-layout model here carries over unchanged; it's a transport swap, not a rewrite.
