#include "AlloyLogger.h"
#include <WiFi.h>
#include <sys/time.h>
#include <math.h>

// ----------------------------- AlloyRecord ---------------------------------
AlloyRecord::AlloyRecord(AlloyLogger* log, const char* chan) : _log(log), _len(0), _done(false) {
  uint64_t ns = log ? log->nowNs() : 0;
  _len = snprintf(_buf, ALLOY_RECORD_MAX, "{\"ch\":\"%s\",\"t_ns\":%llu", chan, (unsigned long long)ns);
  if (_len < 0 || _len >= ALLOY_RECORD_MAX) { _len = 0; _done = true; }
}

AlloyRecord::AlloyRecord(AlloyRecord&& o) noexcept : _log(o._log), _len(o._len), _done(o._done) {
  int n = (o._len > 0 && o._len < ALLOY_RECORD_MAX) ? o._len : 0;
  if (n) memcpy(_buf, o._buf, n);
  o._done = true; o._log = nullptr;
}

AlloyRecord& AlloyRecord::set(const char* name, float v) {
  if (_done || _len > ALLOY_RECORD_MAX - 40) return *this;
  char val[24];
  if (isfinite(v)) snprintf(val, sizeof(val), "%.6g", (double)v);
  else             strcpy(val, "null");
  int n = snprintf(_buf + _len, ALLOY_RECORD_MAX - _len, ",\"%s\":%s", name, val);
  if (n > 0) _len += n;
  return *this;
}

AlloyRecord::~AlloyRecord() {
  if (_done || !_log) return;
  if (_len > 0 && _len < ALLOY_RECORD_MAX - 2) {
    _buf[_len++] = '}'; _buf[_len++] = '\n';
    _log->commitLine(_buf, _len);
  }
}

// ----------------------------- AlloyLogger ---------------------------------
AlloyLogger& AlloyLogger::describe(const char* channel, const char* field,
                                   const char* unit, float lo, float hi, const char* about) {
  if (_nDesc >= ALLOY_MAX_DESC) return *this;
  Desc& d = _desc[_nDesc++];
  strncpy(d.chan, channel, sizeof(d.chan) - 1); d.chan[sizeof(d.chan)-1] = 0;
  strncpy(d.field, field,  sizeof(d.field) - 1); d.field[sizeof(d.field)-1] = 0;
  d.unit[0] = 0;  if (unit)  { strncpy(d.unit, unit, sizeof(d.unit)-1); d.unit[sizeof(d.unit)-1] = 0; }
  d.about[0] = 0; if (about) { strncpy(d.about, about, sizeof(d.about)-1); d.about[sizeof(d.about)-1] = 0; }
  d.lo = lo; d.hi = hi;
  return *this;
}

uint64_t AlloyLogger::nowNs() {
  struct timeval tv; gettimeofday(&tv, nullptr);
  return (uint64_t)tv.tv_sec * 1000000000ULL + (uint64_t)tv.tv_usec * 1000ULL;
}

bool AlloyLogger::begin(const char* apiKey, const char* meshPath, const char* dataUrl) {
  _apiKey = apiKey; _meshPath = meshPath;
  if (_dev) { strncpy(_devId, _dev, sizeof(_devId)-1); _devId[sizeof(_devId)-1] = 0; }
  else snprintf(_devId, sizeof(_devId), "esp32-%06llx", (unsigned long long)(ESP.getEfuseMac() & 0xFFFFFF));

  if (_ssid && WiFi.status() != WL_CONNECTED) {
    WiFi.begin(_ssid, _pass);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(200);
  }
  if (WiFi.status() != WL_CONNECTED) return false;

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");   // UTC — SigV4 needs a real clock
  _up.begin(dataUrl, apiKey);

  _pool = new Buf[_nBuf];
  _freeQ = xQueueCreate(_nBuf, sizeof(Buf*));
  _pendingQ = xQueueCreate(_nBuf, sizeof(Buf*));
  _mtx = xSemaphoreCreateMutex();
  if (!_pool || !_freeQ || !_pendingQ || !_mtx) return false;
  for (uint8_t i = 0; i < _nBuf; i++) {
    _pool[i].data = (char*)malloc(_bufBytes); _pool[i].len = 0;
    if (!_pool[i].data) return false;
    Buf* p = &_pool[i]; xQueueSend(_freeQ, &p, 0);
  }

  _started = true;
  xTaskCreatePinnedToCore(taskTramp, "alloy_up", 8192, this, 4, nullptr, _core);
  return true;
}

void AlloyLogger::commitLine(const char* line, int len) {
  if (!_started) return;
  xSemaphoreTake(_mtx, portMAX_DELAY);
  if (!_active) { _active = getFree(); if (!_active) { xSemaphoreGive(_mtx); return; } _activeSince = millis(); }
  if (_active->len + (size_t)len > _bufBytes) {
    seal(_active); _active = getFree();
    if (!_active) { xSemaphoreGive(_mtx); return; }
    _activeSince = millis();
  }
  memcpy(_active->data + _active->len, line, len); _active->len += len;
  if (_active->len >= (_bufBytes * 9) / 10 || (millis() - _activeSince) >= _flushMs) {
    seal(_active); _active = nullptr;
  }
  xSemaphoreGive(_mtx);
}

// caller holds _mtx
AlloyLogger::Buf* AlloyLogger::getFree() {
  Buf* b = nullptr;
  if (xQueueReceive(_freeQ, &b, 0) == pdTRUE) return b;
  if (xQueueReceive(_pendingQ, &b, 0) == pdTRUE) { _droppedBufs++; b->len = 0; return b; }  // shed oldest
  return nullptr;
}

// caller holds _mtx
void AlloyLogger::seal(Buf* b) {
  if (xQueueSend(_pendingQ, &b, 0) != pdTRUE) {
    Buf* old;
    if (xQueueReceive(_pendingQ, &old, 0) == pdTRUE) { old->len = 0; _droppedBufs++; xQueueSend(_freeQ, &old, 0); }
    xQueueSend(_pendingQ, &b, 0);
  }
}

String AlloyLogger::buildMetaJson() {
  String s = "{\"device\":\""; s += _devId; s += "\"";
  if (_fw) { s += ",\"firmware\":\""; s += _fw; s += "\""; }
  time_t t = _session; struct tm tm; gmtime_r(&t, &tm);
  char iso[24]; strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm);
  s += ",\"session\":\""; s += iso; s += "\",\"fields\":[";
  for (uint8_t i = 0; i < _nDesc; i++) {
    Desc& d = _desc[i];
    if (i) s += ",";
    s += "{\"channel\":\""; s += d.chan; s += "\",\"name\":\""; s += d.field; s += "\"";
    if (d.unit[0])     { s += ",\"unit\":\""; s += d.unit; s += "\""; }
    if (isfinite(d.lo)) { s += ",\"min\":"; s += String(d.lo, 4); }
    if (isfinite(d.hi)) { s += ",\"max\":"; s += String(d.hi, 4); }
    if (d.about[0])    { s += ",\"about\":\""; s += d.about; s += "\""; }
    s += "}";
  }
  s += "]}";
  return s;
}

void AlloyLogger::taskTramp(void* self) { static_cast<AlloyLogger*>(self)->taskLoop(); }

void AlloyLogger::taskLoop() {
  while (time(nullptr) < 1700000000) vTaskDelay(pdMS_TO_TICKS(200));  // wait for SNTP (SigV4 needs UTC)
  _session = (uint32_t)time(nullptr);

  // Each power-on gets its OWN folder under meshPath (a separate mission in Alloy).
  String sessionMesh = String(_meshPath) + "/" + String(_session);

  // upload the semantics sidecar first (Alloy ingests metadata before data)
  String meta = buildMetaJson();
  char fn[48]; snprintf(fn, sizeof(fn), "%s_meta.json", _devId);
  for (int a = 0; a <= 3; a++) {
    if (_up.uploadBuffer((const uint8_t*)meta.c_str(), meta.length(), fn, sessionMesh.c_str())) break;
    vTaskDelay(pdMS_TO_TICKS(1000UL << a));
  }

  Buf* b;
  for (;;) {
    if (xQueueReceive(_pendingQ, &b, pdMS_TO_TICKS(250)) == pdTRUE) {
      snprintf(fn, sizeof(fn), "%s_%lu.jsonl", _devId, (unsigned long)_seq++);
      bool ok = false;
      for (int a = 0; a <= 4 && !ok; a++) {
        if (a) vTaskDelay(pdMS_TO_TICKS(1000UL << (a - 1)));
        ok = _up.uploadBuffer((const uint8_t*)b->data, b->len, fn, sessionMesh.c_str());
      }
      if (ok) _uploaded++; else _failed++;
      b->len = 0; xQueueSend(_freeQ, &b, 0);
    } else {
      xSemaphoreTake(_mtx, portMAX_DELAY);
      if (_active && _active->len > 0 && (millis() - _activeSince) >= _flushMs) { seal(_active); _active = nullptr; }
      xSemaphoreGive(_mtx);
    }
  }
}
