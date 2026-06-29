# AlloyLogger

Stream Arduino sensor & telemetry data **straight to [Alloy](https://usealloy.ai)** from an ESP32 —
in about ten lines. You log `name → value` pairs at the call site; the library RAM-buffers them and
uploads JSONL to Alloy in the background. **No SD card, no flash wear, never blocks your loop.**

You usually don't declare anything: Alloy's AI reasons over your tag + field names + values (a
`heading` ranging 0–360 under a `bno055` tag → it's a magnetic heading). Optionally `describe()` a
field to hand Alloy units/ranges for even sharper context.

> Verified end-to-end on real hardware: an ESP32 streaming `env`/`battery` telemetry → a `meta.json`
> semantics sidecar + JSONL chunks land in Alloy Mesh Storage, `uploaded` climbing, `dropped=0`.

```cpp
#include <AlloyLogger.h>
AlloyLogger alloy;

void setup() {
  alloy.wifi("ssid", "pass");                 // optional — omit if already connected
  alloy.begin("ALLOY_API_KEY", "robots/sbr"); // starts background upload
}

void loop() {
  alloy.log("bno055")
       .set("heading", heading)
       .set("pitch",   pitch)
       .set("roll",    roll);                  // commits at the ';'
  alloy.log("battery", volts);                 // single value
}
```

---

## Why it's nice

- **Self-documenting calls.** The field name sits next to its value — nothing to declare, nothing to
  keep in sync, no positional args to get wrong.
- **Non-blocking & real-time-safe.** `log()` is a `memcpy` into a RAM buffer (microseconds). A
  background task on **core 0** does the TLS upload, so a control loop on core 1 is never disturbed.
- **Reliable by default.** RAM-buffered with **store-and-forward** — if WiFi drops, buffers queue and
  flush on reconnect; if the uplink can't keep up, the oldest buffer is shed (counted), never a crash.
- **Zero flash wear.** Nothing touches the filesystem, so it coexists with OTA / `min_spiffs` layouts.
- **Alloy-native.** Streams JSONL (ingested into queryable tables) plus a one-time semantics sidecar.

---

## Install

**Arduino IDE:** Sketch → Include Library → Add .ZIP Library (or clone into `~/Arduino/libraries/`).
Depends on **ArduinoJson** (Library Manager).

**arduino-cli / PlatformIO:**
```bash
arduino-cli lib install ArduinoJson
# clone this repo into your libraries folder, or for a one-off build:
arduino-cli compile --fqbn esp32:esp32:esp32 --library /path/to/AlloyLogger your_sketch
```

You need an Alloy account + a data-API key (Dashboard → Mesh Storage → API key).

---

## API

```cpp
AlloyLogger alloy;
```

**Config (all optional, before `begin`)** — each returns `*this` so you can chain:
| Call | Purpose | Default |
|---|---|---|
| `alloy.wifi(ssid, pass)` | Connect WiFi. Omit if your sketch already connected. | — |
| `alloy.device(id, firmware)` | Device id + firmware tag (into `meta.json`). | id = chip MAC |
| `alloy.core(c)` | Core the uploader runs on. | `0` |
| `alloy.buffers(count, bytes)` | RAM buffer pool. | `4 × 24 KB` |
| `alloy.flushEvery(ms)` | Max time before a partial buffer is sent. | `4000` |
| `alloy.describe(channel, field, unit, min, max, about)` | Richer semantics for Alloy AI. | — |

**Start:**
```cpp
alloy.begin(apiKey, meshPath);   // meshPath e.g. "robots/sbr"; optional 3rd arg = data URL
```

**Log:**
```cpp
alloy.log("channel").set("a", x).set("b", y);   // multi-field, commits at end of statement
alloy.log("channel", value);                     // single value (field name "value")
```
`set()` takes `float` / `int` / `double` / `bool`. Values are stored as numbers.

**Stats:** `alloy.uploaded()`, `alloy.failed()`, `alloy.dropped()` (buffers shed under backpressure).

---

## What Alloy receives

A one-time **`meta.json`** (from your `describe()` calls + device info):
```json
{ "device":"sbr-01", "firmware":"fw16", "session":"2026-06-29T06:48:14Z",
  "fields":[ {"channel":"env","name":"temp_c","unit":"degC","min":-40,"max":125,"about":"ambient temperature"} ] }
```
Then compact **JSONL** data, wall-clock-timestamped so channels align with no extra math:
```json
{"ch":"env","t_ns":1782715694000000000,"temp_c":22.4,"humidity":51.2}
```
Each power-on uploads into its **own subfolder** `<meshPath>/<session>/`, so every run is a distinct
mission in Alloy. `.jsonl` ingests natively into queryable tables — replay it, or query with SQL / DuckDB.

---

## Examples

- **[BasicSensor](examples/BasicSensor)** — stream a sensor in ~10 lines.
- **[SelfBalancingRobot](examples/SelfBalancingRobot)** — add streaming to a 100 Hz control loop
  without disturbing real-time stepping (the pattern for a robot that already manages WiFi).

---

## Limits & notes

- **Sustained rate is bounded by upload throughput** (~one R2 PUT per buffer over WiFi). A few hundred
  records/sec is comfortable; far higher sheds oldest buffers (counted in `dropped()`). Tune with
  `buffers()` / `flushEvery()`, or use an ESP32-S3 / better WiFi.
- **A real UTC clock is required** (SigV4) — the library runs SNTP automatically and self-heals; the
  first second or two before sync may be skipped.
- **One API key on-device** is fine for a single rig; for a fleet, use per-device keys.
- ESP32 / ESP32-S3 only (uses WiFi + mbedTLS).

## License

MIT — see [LICENSE](LICENSE).
