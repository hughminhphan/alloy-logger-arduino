# AlloyLogger

Stream Arduino sensor & telemetry data **straight to [Alloy](https://usealloy.ai)** from an ESP32 —
in about ten lines. You log `name → value` pairs at the call site; the library RAM-buffers them and
uploads CSV to Alloy in the background. **No SD card, no flash wear, never blocks your loop.**

You usually don't declare anything: Alloy's AI reasons over your tag + field names + values (a
`heading` ranging 0–360 under a `bno055` tag → it's a magnetic heading). Optionally `describe()` a
field to hand Alloy units/ranges for even sharper context.

> Verified end-to-end on real hardware: an ESP32 streaming `env`/`battery` telemetry → a `meta.json`
> semantics sidecar + CSV chunks land in Alloy Mesh Storage, `uploaded` climbing, `dropped=0`.

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
- **Alloy-native.** Streams compact per-channel CSV (ingested into queryable tables) plus a one-time semantics sidecar.

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
| `alloy.buffers(count, bytes)` | RAM buffer pool. Keep count above your channel count, and the total well under half the free heap (a verified TLS handshake needs ~60 KB headroom). | `4 × 12 KB` |
| `alloy.flushEvery(ms)` | Max time before a partial buffer is sent. | `4000` |
| `alloy.describe(channel, field, unit, min, max, about)` | Richer semantics for Alloy AI. | — |
| `alloy.insecure()` | Skip TLS verification (TLS-intercepting proxies etc.). | verify via Mozilla roots |

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

**Auto-capture (set-and-forget)** — register a signal *once* and the library samples it for you on
a background timer, with **no code in `loop()`**. When something unexpected happens, the data is
already there — no reflash to add a probe, no serial monitor:
| Call | Streams | Channel |
|---|---|---|
| `alloy.watch(pin, "name")` | a digital pin (`digitalRead`) | `io` |
| `alloy.watchAnalog(pin, "name")` | an analog pin (`analogRead`) | `adc` |
| `alloy.watch("chan", "field", fn)` | any variable/expr via a captureless `float(*)()` | `chan` |
| `alloy.watchSystem()` | free heap, WiFi RSSI, uptime (`heap` / `rssi` / `uptime_s`) | `sys` |
| `alloy.sampleEvery(ms)` | sampler period (default `100` = 10 Hz) | — |

```cpp
float g_pitch;  float readPitch() { return g_pitch; }   // expose a variable

alloy.watch(0, "boot_btn");                 // GPIO0 state
alloy.watchAnalog(34, "batt_raw");          // ADC
alloy.watch("imu", "pitch", readPitch);     // your own variable
alloy.watchSystem();                        // heap/rssi/uptime
alloy.begin(ALLOY_KEY, "demos/auto");       // register before begin(); sampler runs on your core()
```
Watched fields sharing a channel are written as one aligned row per tick — same CSV tables, same
`describe()` semantics. Mix freely with explicit `log()` calls.

**Stats:** `alloy.uploaded()`, `alloy.failed()`, `alloy.dropped()` (buffers shed under backpressure).

---

## What Alloy receives

A one-time **`meta.json`** (from your `describe()` calls + device info):
```json
{ "device":"sbr-01", "firmware":"fw16", "session":"2026-06-29T06:48:14Z",
  "fields":[ {"channel":"env","name":"temp_c","unit":"degC","min":-40,"max":125,"about":"ambient temperature"} ] }
```
Then compact **CSV** data, one file per channel: a one-line header (`t_ns` + your field names),
then bare value rows, wall-clock-timestamped so channels align with no extra math:
```csv
t_ns,temp_c,humidity
1782715694000000000,22.4,51.2
1782715695000000000,22.5,51.1
```
One channel = one consistent schema = one table. Dropping the per-row JSON keys roughly halves the
bytes on the wire and storage, and takes the per-field text formatting off your hot loop.

Each power-on uploads into its **own subfolder** `<meshPath>/<session>/` (files named
`<device>_<channel>_<seq>.csv`), so every run is a distinct mission in Alloy. `.csv` ingests
natively into queryable tables — replay it, or query with SQL / DuckDB.

---

## Examples

- **[BasicSensor](examples/BasicSensor)** — stream a sensor in ~10 lines.
- **[AutoCapture](examples/AutoCapture)** — set-and-forget: `watch()` pins/variables/system, nothing in `loop()`.
- **[SelfBalancingRobot](examples/SelfBalancingRobot)** — add streaming to a 100 Hz control loop
  without disturbing real-time stepping (the pattern for a robot that already manages WiFi).

---

## Limits & notes

- **Sustained rate is bounded by upload throughput** (~one R2 PUT per buffer over WiFi). A few hundred
  records/sec is comfortable; far higher sheds oldest buffers (counted in `dropped()`). Tune with
  `buffers()` / `flushEvery()`, or use an ESP32-S3 / better WiFi.
- **A real UTC clock is required** (SigV4) — the library runs SNTP automatically. Records logged
  before the first sync are stamped with a boot-relative clock and rebased to wall-clock time
  in-buffer once SNTP lands, so nothing is lost or mis-timed.
- **TLS is verified by default** against the ESP32 core's embedded Mozilla root CA bundle (no extra
  flash shipped by this library). `alloy.insecure()` opts out for networks that intercept TLS.
- **One API key on-device** is fine for a single rig; for a fleet, use per-device keys.
- ESP32 / ESP32-S3 only (uses WiFi + mbedTLS).

## License

MIT — see [LICENSE](LICENSE).
