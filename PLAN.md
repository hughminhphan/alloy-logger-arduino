# ESP32 robot black-box → Alloy: plan

A multimodal control-loop recorder on ESP32-class hardware that uploads **directly to Alloy**.
Nothing runs on a laptop. Scope settled over design: **no camera, no audio** — only numeric
control-loop signals (sensor inputs + actuator outputs). That removes every hard problem
(SRAM/SD saturation, cross-DMA clock drift); a bare ESP32 has 50–100× headroom.

## What we log
Sensor inputs + actuator outputs on ONE timeline, sampled in the control loop:
- IMU (accel/gyro/mag), analog/ADC (current, temp, pressure, GPS, …)
- Encoders (via **PCNT** hardware counter — snapshot in loop, never ISR-per-edge)
- Motor **commands/setpoints** (the duty value, NOT the PWM carrier waveform)
- GPIO / e-stop / mode as **on-change** events (sample-and-hold downstream)

One control tick = one `esp_timer` ns timestamp = everything aligned by construction.
No free-running stream (audio/camera gone) ⇒ no drift to reconcile.

## Path chosen — Option 2 (now): ESP32 → JSONL → direct upload
- Record each run as **JSON-Lines** (`.jsonl`) on SD. One object per record:
  `{"ch":"imu","t_ns":..., "ax":...}`. Alloy ingests `.jsonl` natively → queryable tables (SQL/DuckDB).
- Hot path stays cheap: control loop only `memcpy`s packed bytes into a bounded FreeRTOS queue;
  JSON formatting runs in the SD-writer task on **core 1**. Queue-full ⇒ drop oldest, counted.
- On run end: `logger.end()` finalizes the file, then upload **directly** to the Alloy data API.

### Confirmed upload protocol (from `alloy-sdk` 0.1.1 source, `alloy/storage.py`)
1. `POST {ALLOY_DATA_URL}/mesh/storage/upload-session`, `Authorization: Bearer {ALLOY_API_KEY}`,
   body `{"path":"<mesh-folder>","ttl_seconds":900}`.
2. Response → `bucket, endpoint_url, region, prefix, expires_at, credentials{access_key_id,
   secret_access_key, session_token}`. File lands at `uploads/sdk-uploads/<path>/<filename>`.
3. **S3 PutObject** `{endpoint_url}/{bucket}/{prefix}{filename}` signed with the temp STS creds via
   **AWS SigV4**. Over TLS use `x-amz-content-sha256: UNSIGNED-PAYLOAD` ⇒ no whole-file hashing on-device.
   Requires the device clock set via SNTP (SigV4 needs real UTC).

> The `alloy-edge` Linux/ROS2 device client uses simpler per-file **presigned URLs**, but it can't
> run on an MCU and the data-API path doesn't expose presigned PUTs — hence on-device SigV4.

## Option 3 (later): ESP32 → server shim → MCAP, live streaming
When we want **live** telemetry + richest replay:
- ESP32 streams compact frames (MQTT/WebSocket/ESP-NOW) to a ~50-line service **co-located with Alloy**.
- The shim assembles **MCAP** (rich multi-channel Replay/Inspect/ROS-graph) and uploads via the
  supported **Python SDK** (or drops files into an `alloy-edge` track-folder).
- Benefits: live view + store-and-forward, and it **removes SigV4/TLS/keys from the MCU** entirely.
- **Carry-over:** the channel + field-layout model (`channel("imu","ax:f32,...")`) is identical, so
  moving from JSONL-direct to frames→shim→MCAP is a transport swap, not a rewrite.

## Files
- `RoboLog.h` — the logger (channels, bounded queue, core-1 JSONL writer, on-change helper, `end()`).
- `AlloyUploader.h` — confirmed two-step session + SigV4 PutObject (UNSIGNED-PAYLOAD).
- `esp32-robolog.ino` — control-loop skeleton: WiFi+SNTP, declare channels, record a run, finalize, upload.

## Decision framework (recap)
| Situation | Path |
|---|---|
| Numeric control-loop logging, batch upload per run | **Option 2** (here) — JSONL direct |
| Need live telemetry / rich MCAP replay / fleet keys off-device | **Option 3** — server shim |
| Re-add audio | back to S3+PSRAM + ADPCM, reconsider live vs batch |
| Re-add camera | step up to Linux SBC (Pi/Jetson) running `alloy-edge` natively |

## Honest limits / deferred
- **Org API key on device** — fine for one dev rig; for a fleet move to per-device keys or Option 3.
- **CA pinning** — uploader uses `setInsecure()` TODO; pin Alloy's CA for production.
- **No on-device compression** — not needed at this data rate; DoD-timestamp / Gorilla-XOR is a later option.
- **Verify against a live session** — exact S3 addressing (path-style vs vhost) + 200/204 codes need one
  real `upload-session` round-trip with Hugh's API key to confirm; everything else is pinned to SDK source.
- **JSONL vs CSV** — JSONL chosen for heterogeneous channels + Option-3 friendliness; CSV is a trivial
  swap if minimum bytes ever matter.

## Open / next
- [ ] One real upload-session call (needs API key) to confirm S3 addressing + response shape.
- [ ] Wire real sensors/motor driver/encoders into the `// TODO` blocks.
- [ ] `arduino-cli compile` against the target board.
- [ ] (Option 3) spec the server shim + framing when live streaming is wanted.
