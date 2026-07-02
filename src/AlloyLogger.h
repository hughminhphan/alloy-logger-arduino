// AlloyLogger — stream Arduino sensor/telemetry data straight to Alloy (usealloy.ai).
//
// Dead-simple, drop-in for any ESP32 Arduino project. You log name→value pairs at the call site;
// the library RAM-buffers them and uploads CSV to Alloy in the background (no SD, no flash wear,
// never blocks your loop). Alloy AI reasons over the tag + field names + values, so you usually
// don't describe anything — but describe() lets you hand it units/ranges for richer context.
//
//   AlloyLogger alloy;
//   void setup() {
//     alloy.wifi(SSID, PASS);                 // optional — omit if already connected
//     alloy.begin(ALLOY_API_KEY, "robots/sbr");
//   }
//   void loop() {
//     alloy.log("bno055").set("heading", h).set("pitch", p).set("roll", r);  // commits at ';'
//     alloy.log("battery", volts);            // single value
//   }
//
// Each channel uploads as its own CSV chunk: a one-line header (t_ns + your field names) followed
// by bare value rows. One channel = one consistent schema = one queryable table in Alloy.
//
// See README for the full picture. MIT licensed.

#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "AlloyUploader.h"
#include "AlloyCloudUploader.h"

#ifndef ALLOY_CHAN_MAX
#define ALLOY_CHAN_MAX 24         // max bytes in a channel name
#endif
#ifndef ALLOY_HDR_MAX
#define ALLOY_HDR_MAX 160         // max bytes in one CSV header (t_ns + field names)
#endif
#ifndef ALLOY_ROW_MAX
#define ALLOY_ROW_MAX 200         // max bytes in one CSV value row
#endif
#ifndef ALLOY_MAX_CHANNELS
#define ALLOY_MAX_CHANNELS 8      // max distinct channels logged concurrently
#endif
#ifndef ALLOY_MAX_DESC
#define ALLOY_MAX_DESC 48         // max describe() entries
#endif
#ifndef ALLOY_MAX_WATCH
#define ALLOY_MAX_WATCH 16        // max watch() auto-sampled signals
#endif

#define ALLOY_EPOCH_SANE 1700000000  // time() below this = SNTP hasn't synced yet

class AlloyLogger;

// One log record. Built on the stack, commits to the buffer when the statement ends (its ';').
// Carries the field names (CSV header) and their values (CSV row) side by side.
class AlloyRecord {
public:
  AlloyRecord& set(const char* name, float value);
  AlloyRecord& set(const char* name, int value)    { return set(name, (float)value); }
  AlloyRecord& set(const char* name, double value)  { return set(name, (float)value); }
  AlloyRecord& set(const char* name, bool value)    { return set(name, value ? 1.0f : 0.0f); }
  ~AlloyRecord();
  AlloyRecord(AlloyRecord&& o) noexcept;
  AlloyRecord(const AlloyRecord&) = delete;
  AlloyRecord& operator=(const AlloyRecord&) = delete;
private:
  friend class AlloyLogger;
  AlloyRecord(AlloyLogger* log, const char* chan);
  AlloyLogger* _log;
  char _chan[ALLOY_CHAN_MAX];
  char _hdr[ALLOY_HDR_MAX];        // "t_ns,heading,pitch,roll"
  char _row[ALLOY_ROW_MAX];        // "1782715694000000000,-1.23,0.52,3.14"
  int  _hlen, _rlen;
  bool _done;
};

class AlloyLogger {
public:
  // ---- optional config (call before begin) ----
  AlloyLogger& wifi(const char* ssid, const char* pass) { _ssid = ssid; _pass = pass; return *this; }
  AlloyLogger& device(const char* id, const char* firmware = nullptr) { _dev = id; _fw = firmware; return *this; }
  AlloyLogger& buffers(uint8_t count, size_t bytes) { _nBuf = count; _bufBytes = bytes; return *this; }
  AlloyLogger& flushEvery(uint32_t ms) { _flushMs = ms; return *this; }
  AlloyLogger& core(int c) { _core = c; return *this; }
  // Escape hatch for networks where TLS verification can't work (TLS-intercepting proxies etc.).
  // Default is full verification against the ESP32 core's embedded Mozilla root CA bundle.
  AlloyLogger& insecure(bool on = true) { _insecure = on; return *this; }
  // Default transport is AlloyLogger Cloud (ingest.alloylogger.com): the service assembles each
  // power-on into ONE indexed .mcap in your mesh (Replay/Inspect/SQL/missions all light up).
  // direct() opts back into the legacy device→R2 SigV4 path (per-channel CSVs, tables only).
  AlloyLogger& direct(bool on = true) { _direct = on; return *this; }
  AlloyLogger& ingestUrl(const char* url) { _ingestUrl = url; return *this; }

  // ---- v2: automatic capture (set-and-forget) ----
  // Register a signal ONCE (must be before begin(); later calls are ignored) and the library
  // samples it for you on a timer — no code in loop(). Each tick becomes one aligned CSV row per
  // channel, streamed like everything else. So when something unexpected happens, the data is
  // just already there.
  AlloyLogger& watch(uint8_t pin, const char* field = nullptr);         // digital pin  -> "io"
  AlloyLogger& watchAnalog(uint8_t pin, const char* field = nullptr);   // analog pin   -> "adc"
  AlloyLogger& watch(const char* channel, const char* field, float (*fn)());  // any variable/expr
  AlloyLogger& watchSystem();                                           // heap/rssi/uptime -> "sys"
  AlloyLogger& sampleEvery(uint32_t ms) { _sampleMs = ms; return *this; }     // sampler period (100)

  // Optional richer semantics for Alloy AI (units/range/description). Call in setup() before begin().
  AlloyLogger& describe(const char* channel, const char* field,
                        const char* unit = nullptr, float lo = NAN, float hi = NAN,
                        const char* about = nullptr);

  // Connect + start the background uploader. meshPath e.g. "robots/sbr".
  bool begin(const char* apiKey, const char* meshPath,
             const char* dataUrl = "https://data.usealloy.ai");

  // ---- logging ----
  AlloyRecord log(const char* channel) { return AlloyRecord(this, channel); }
  void log(const char* channel, float value) { AlloyRecord(this, channel).set("value", value); }

  // Graceful end-of-run (cloud mode): seals open buffers, drains uploads (bounded), then tells
  // the service to finalize the .mcap now instead of after the ~10 min inactivity window.
  // Optional — power loss is handled server-side. No-op in direct mode.
  void end(uint32_t drainMs = 8000);

  // ---- stats ----
  uint32_t uploaded() const { return _uploaded; }
  uint32_t failed() const { return _failed; }
  uint32_t dropped() const { return _droppedBufs; }   // buffers shed under backpressure
  uint32_t stale() const { return _stale; }           // chunks refused post-finalize (cloud 409)

  uint64_t nowNs();   // wall-clock ns (gettimeofday); used to stamp records

private:
  friend class AlloyRecord;
  struct Buf { char* data; size_t len; char chan[ALLOY_CHAN_MAX]; };

  // One open CSV stream per channel: which buffer it's filling, since when, and the field-set hash.
  struct Slot { char chan[ALLOY_CHAN_MAX]; Buf* active; uint32_t since; uint32_t sig; };

  void commitRow(const char* chan, const char* hdr, int hlen, const char* row, int rlen);
  Buf* getFree(Buf* avoid = nullptr);
  void seal(Buf* b);
  void rebaseRows(Buf* b);
  void teardown();
  Slot* slotFor(const char* chan);
  static uint32_t hashStr(const char* s, int n);
  static void taskTramp(void* self);
  void taskLoop();
  String buildMetaJson();

  // v2 auto-capture
  struct Watch { char chan[ALLOY_CHAN_MAX]; char field[24]; uint8_t kind; uint8_t pin; float (*fn)(); };
  AlloyLogger& addWatch(const char* chan, const char* field, uint8_t kind, uint8_t pin, float (*fn)());
  static float readWatch(const Watch& w);
  static void samplerTramp(void* self);
  void samplerLoop();
  void sampleTick();

  // config
  const char *_ssid = nullptr, *_pass = nullptr, *_dev = nullptr, *_fw = nullptr;
  const char *_apiKey = nullptr, *_meshPath = nullptr;
  // 4x12KB. Two constraints: keep the pool well under half the free heap (a VERIFIED TLS
  // handshake needs ~60KB headroom — 4x24KB starved it on a classic ESP32), and keep count
  // above the number of concurrent channels or flushes shed buffers waiting for a free one.
  uint8_t  _nBuf = 4;
  size_t   _bufBytes = 12 * 1024;
  uint32_t _flushMs = 4000;
  int      _core = 0;
  uint32_t _sampleMs = 100;
  bool     _insecure = false;
  bool     _direct = false;
  const char* _ingestUrl = "https://ingest.alloylogger.com";

  // describe() store
  struct Desc { char chan[24], field[20], unit[12], about[48]; float lo, hi; };
  Desc    _desc[ALLOY_MAX_DESC]; uint8_t _nDesc = 0;

  // watch() store
  Watch   _watch[ALLOY_MAX_WATCH]; uint8_t _nWatch = 0;

  // runtime
  AlloyUploader      _up;     // direct mode
  AlloyCloudUploader _cloud;  // cloud mode (default)
  Buf*          _pool = nullptr;
  QueueHandle_t _freeQ = nullptr, _pendingQ = nullptr;
  SemaphoreHandle_t _mtx = nullptr;
  Slot          _slots[ALLOY_MAX_CHANNELS]; uint8_t _nSlots = 0;
  uint64_t      _bootOffsetNs = 0;   // wall-clock minus boot-clock ns, captured once SNTP lands
  uint32_t      _session = 0, _seq = 0;
  char          _devId[24] = {0};
  volatile uint32_t _uploaded = 0, _failed = 0, _droppedBufs = 0, _stale = 0;
  bool          _started = false;
};
