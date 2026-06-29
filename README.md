# esp32-robolog

A **robot black-box recorder** for ESP32-class microcontrollers that logs your whole control loop —
every sensor *input* and actuator *output* on one timeline — and streams it **directly to
[Alloy](https://usealloy.ai)** for replay and SQL query. **No SD card. No laptop. No intermediary
server.** Runtime is unlimited.

It records numeric control-loop data only (IMU, ADC, encoders, motor command setpoints, GPIO/e-stop —
no camera, no audio), which keeps everything tractable on a bare ESP32: one control tick = one
nanosecond timestamp = every channel aligned by construction, with 50–100× write-bandwidth headroom.

> **Verified end-to-end on real hardware (2026-06-29):** a classic ESP32 (no SD card) records →
> rolls JSONL chunks on on-chip flash → uploads each chunk to Alloy via SigV4 → deletes it. A 40 s
> run produced 5 chunks, all in Alloy Mesh Storage, `dropped=0 chunks_dropped=0`.

---

## How it works

```
   control loop (core 0, fixed rate)              writer task (core 1)
   ┌───────────────────────────────┐              ┌────────────────────────────┐
   │ t = esp_timer ns  (one stamp)  │   record     │ format JSONL, append to the │
   │ read sensors  ───────────────► │   queue      │ current chunk file          │
   │ compute / actuate             │ ───────────► │ roll a new chunk every       │
   │ log(channel, t, payload)      │              │ CHUNK_MS / CHUNK_BYTES       │
   └───────────────────────────────┘              └─────────────┬──────────────┘
                                                    closed chunk │ (bounded queue)
                                                                 ▼
                                              ┌────────────────────────────────┐
                                              │ ChunkUploader task              │
                                              │  upload-session (cached) ──┐    │
                                              │  SigV4 PutObject ──► R2 ────┘    │
                                              │  delete local chunk on success  │
                                              └────────────────────────────────┘
                                                                 ▼
                                       Alloy Mesh Storage  →  Replay · SQL · DuckDB
```

- **One master clock.** `esp_timer` (µs → ns). Everything written in a control tick shares one
  timestamp, so heterogeneous channels need no drift correction.
- **Rolling chunks.** The writer rolls a new chunk file every `CHUNK_MS` or `CHUNK_BYTES`. A separate
  uploader task ships each closed chunk to Alloy and deletes it — so on-chip flash is just a recycling
  buffer and **runtime is unbounded**.
- **Never blocks, never overruns.** Recording runs on its own core; the TLS upload runs on another.
  The pending-chunk buffer is bounded and the writer sheds the oldest chunk if flash is full, so a
  slow or dropped uplink degrades to bounded data loss (counted), never a crash.
- **Store-and-forward.** If WiFi drops, chunks queue on flash and flush on reconnect.

---

## Hardware

- Any **ESP32** or **ESP32-S3** dev board with WiFi. Nothing else is required — no SD card, no extra
  parts to run the demo (it logs synthetic data).
- For real logging: your sensors/actuators wired to the board (see *Adding your sensors* below).
- 2.4 GHz WiFi (ESP32 radios are 2.4 GHz only).

---

## Prerequisites

```bash
# Arduino toolchain
arduino-cli core install esp32:esp32
arduino-cli lib install ArduinoJson

# An Alloy account + a data-API key (Dashboard → Mesh Storage → API key)
```

---

## Quick start

1. **Clone & configure secrets** (never committed — `secrets.h` is gitignored):
   ```bash
   git clone https://github.com/hughminhphan/esp32-robolog
   cd esp32-robolog
   cp secrets.h.example secrets.h
   ```
   Edit `secrets.h`:
   ```c
   #define WIFI_SSID      "your-2.4GHz-ssid"
   #define WIFI_PASS      "your-wifi-password"
   #define ALLOY_DATA_URL "https://data.usealloy.ai"
   #define ALLOY_API_KEY  "your-alloy-api-key"
   #define MESH_PATH      "robots/sbr"   // where files land in Alloy
   ```

2. **Find your board's port:**
   ```bash
   arduino-cli board list
   ```

3. **Flash & run** (the main recorder logs synthetic data out of the box):
   ```bash
   arduino-cli compile -u -p /dev/cu.usbserial-XXXX --fqbn esp32:esp32:esp32 .
   ```
   Open the serial monitor at **115200**. You'll see WiFi connect, the clock sync, and on each chunk
   roll an upload. Files appear in Alloy under
   `uploads/sdk-uploads/<MESH_PATH>/c_<session>_<seq>.jsonl`.

4. **First-time smoke test** (optional, proves only the upload path, no recording):
   ```bash
   arduino-cli compile -u -p /dev/cu.usbserial-XXXX --fqbn esp32:esp32:esp32 ./upload_smoketest
   ```

For ESP32-S3 boards use `--fqbn esp32:esp32:esp32s3`.

---

## Configuration

All knobs are at the top of [`esp32-robolog.ino`](esp32-robolog.ino):

| Setting | Default | Meaning |
|---|---|---|
| `CONTROL_HZ` | 1000 | Control-loop rate (your controller runs here). |
| `LOG_HZ` | 50 | Recording rate (decimated from the control loop). |
| `CHUNK_MS` | 8000 | Roll a new chunk after this many ms… |
| `CHUNK_BYTES` | 300000 | …or this many bytes, whichever first. |
| `CHUNK_QDEPTH` | 5 | Max pending chunks buffered before the oldest is shed. `QDEPTH × CHUNK_BYTES` must stay under the LittleFS partition (~1.4 MB on a 4 MB board). |
| `RUN_SECONDS` | 40 | Demo length. **Set `0` to record forever.** |

---

## Adding your sensors

Open [`esp32-robolog.ino`](esp32-robolog.ino) and fill the four `// TODO` blocks in `controlTask`.
Each signal is a **channel** declared once with a field layout, then written each tick. Worked example
for an MPU6050 IMU over I²C:

```cpp
#include <Adafruit_MPU6050.h>
Adafruit_MPU6050 mpu;

// in setup(), after logger.beginRolling(...):
mpu.begin();
CH_IMU = logger.channel("imu", "ax:f32,ay:f32,az:f32,gx:f32,gy:f32,gz:f32");

// in controlTask(), block 1 (READ):
sensors_event_t a, g, tmp;
mpu.getEvent(&a, &g, &tmp);
ImuRec imu = { a.acceleration.x, a.acceleration.y, a.acceleration.z,
               g.gyro.x, g.gyro.y, g.gyro.z };

// block 4 (LOG):
logger.write(CH_IMU, t, &imu, sizeof(imu));
```

Rules of thumb:
- The packed `struct` field order/types **must match** the channel layout string.
- **Encoders:** use the ESP32 **PCNT** hardware counter and snapshot it each tick — don't interrupt
  per edge (it starves the loop at high RPM).
- **Motors/GPIO:** log the *command/setpoint* and discrete state, never the PWM carrier waveform. Use
  `logger.writeOnChange(...)` for discrete signals (e-stop, mode) so they're logged only when they change.
- Layout types: `f32 f64 i8 u8 i16 u16 i32 u32 i64 u64`.

---

## The data in Alloy

Each line is a self-describing JSON record:

```json
{"ch":"imu","t_ns":1782713626000000,"ax":0.12,"ay":-0.03,"az":9.79,"gx":0.01,"gy":0.0,"gz":-0.02}
{"ch":"motor0","t_ns":1782713626000000,"cmd":0.42,"current":1.1,"enc":10342}
```

Alloy ingests `.jsonl` natively into queryable tables. Once the chunks are **Ready**, query them with
SQL / DuckDB or scrub them in Replay. `t_ns` is a monotonic device timestamp shared across channels in
the same tick — join on it to reconstruct full robot state at any instant.

---

## Direct-upload protocol (no SDK on the MCU)

The firmware replicates Alloy's Python-SDK upload path in C:

1. `POST {ALLOY_DATA_URL}/mesh/storage/upload-session` with `Authorization: Bearer {ALLOY_API_KEY}`
   and body `{"path":"<MESH_PATH>","ttl_seconds":900}` → returns the bucket, endpoint, region, prefix,
   and **temporary S3 credentials**.
2. **S3 PutObject** to `{endpoint}/{bucket}/{prefix}{file}`, **AWS SigV4**-signed with those temp
   creds (`x-amz-security-token`), using `x-amz-content-sha256: UNSIGNED-PAYLOAD` so the whole file is
   never hashed on-device. Backend is **Cloudflare R2** (path-style, region `auto`).

The session is cached and reused across chunks (one R2 PUT per chunk). A real UTC clock (SNTP) is
required for SigV4.

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| Stuck at `WiFi....` | Wrong SSID/pass, or a 5 GHz network — ESP32 is 2.4 GHz only. |
| `upload FAILED` / chunks not appearing | Check `ALLOY_API_KEY` and `ALLOY_DATA_URL`; confirm the clock synced (SigV4 needs correct UTC). |
| `No more free space` (LittleFS) | `CHUNK_QDEPTH × CHUNK_BYTES` exceeds the flash partition — lower one, or raise `LOG_HZ` headroom by shrinking chunks. |
| `chunks_dropped` climbing | The uplink can't keep up with `LOG_HZ`. Lower the rate, enlarge chunks, or use a faster board/WiFi. |
| Garbled serial | Set the monitor baud to **115200**. |

---

## Limitations & roadmap

- **Sustained rate is bounded by upload throughput** (~one R2 PUT per chunk over WiFi). ~50 Hz keeps up
  cleanly on a classic ESP32; higher rates shed oldest chunks. ESP32-S3 / better WiFi / bigger chunks lift it.
- **No resume across reboot yet** — undelivered chunks are cleared at boot (store-and-forward only
  survives WiFi drops, not power cycles).
- **Org API key on-device** — fine for one rig; for a fleet, use per-device keys or the planned
  **server-shim** path (Option 3): stream frames to a small service co-located with Alloy that
  assembles rich **MCAP** and uploads via the SDK, moving SigV4/keys off the MCU. The channel model
  here carries over unchanged — it's a transport swap, not a rewrite. See [`PLAN.md`](PLAN.md).

---

## License

MIT — see [LICENSE](LICENSE).
