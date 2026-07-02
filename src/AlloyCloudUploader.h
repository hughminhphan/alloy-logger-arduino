// AlloyCloudUploader.h — internal: posts sealed buffers to AlloyLogger Cloud
// (ingest.alloylogger.com), which assembles each power-on into ONE indexed .mcap and uploads it
// into your Alloy mesh. This is the default transport: plain keep-alive HTTPS POSTs, no SigV4,
// no upload-session dance on the device. See AlloyUploader.h for the legacy direct path.
//
// Protocol: POST {ingestUrl}/v1/{chunk|meta|end}
//   Authorization: Bearer {ALLOY_API_KEY}
//   X-Alloy-Device / X-Alloy-Session / X-Alloy-Mesh-Path on every call,
//   X-Alloy-Channel / X-Alloy-Seq on /v1/chunk.
// 2xx = accepted. 409 = session already finalized (terminal — drop the buffer).
// 429/5xx = retryable. The service finalizes ~10 min after the last chunk, or on /v1/end.

#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "esp_arduino_version.h"

// Same embedded-Mozilla-CA-bundle trick as AlloyUploader.h (identical extern redeclaration is fine).
extern "C" {
  extern const uint8_t alloy_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
  extern const uint8_t alloy_crt_bundle_end[]   asm("_binary_x509_crt_bundle_end");
}

class AlloyCloudUploader {
public:
  void begin(const char* ingestUrl, const char* apiKey, bool insecure = false) {
    _url = ingestUrl; _key = apiKey; _insecure = insecure;
  }

  // Pin the run identity once the SNTP clock has landed (session = epoch seconds at boot).
  void session(const char* devId, uint32_t session, const char* meshPath) {
    _dev = devId; _session = session; _mesh = meshPath;
  }

  bool postChunk(const uint8_t* data, size_t len, const char* chan, uint32_t seq) {
    return post("/v1/chunk", data, len, "text/csv", chan, seq);
  }
  bool postMeta(const uint8_t* json, size_t len) {
    return post("/v1/meta", json, len, "application/json", nullptr, 0);
  }
  bool postEnd() {
    return post("/v1/end", nullptr, 0, "application/octet-stream", nullptr, 0);
  }

  int last() const { return _last; }   // HTTP code of the most recent post (409 = finalized)

private:
  String _url, _key, _dev, _mesh;
  uint32_t _session = 0;
  bool _insecure = false;
  int _last = 0;
  // Persistent keep-alive connection + HTTPClient, same discipline as AlloyUploader: a verified
  // TLS handshake costs ~2s on a classic ESP32; per-POST reconnects can't keep up with multiple
  // channels flushing every few seconds. setupTLS re-arms every POST because any stop() memsets
  // the SSL context (dropping the CA-bundle attach callback).
  WiFiClientSecure _cli; HTTPClient _http; bool _reuseInit = false;

  void setupTLS() {
    if (_insecure) { _cli.setInsecure(); return; }
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
    _cli.setCACertBundle(alloy_crt_bundle_start, alloy_crt_bundle_end - alloy_crt_bundle_start);
#else
    _cli.setCACertBundle(alloy_crt_bundle_start);
#endif
  }

  bool post(const char* route, const uint8_t* data, size_t len, const char* contentType,
            const char* chan, uint32_t seq) {
    setupTLS();
    if (!_reuseInit) { _http.setReuse(true); _reuseInit = true; }
    if (!_http.begin(_cli, _url + route)) { _last = -1; return false; }
    _http.addHeader("Authorization", "Bearer " + _key);
    _http.addHeader("X-Alloy-Device", _dev);
    _http.addHeader("X-Alloy-Session", String(_session));
    _http.addHeader("X-Alloy-Mesh-Path", _mesh);
    if (chan) {
      _http.addHeader("X-Alloy-Channel", chan);
      _http.addHeader("X-Alloy-Seq", String(seq));
    }
    _http.addHeader("Content-Type", contentType);
    int code = _http.sendRequest("POST", (uint8_t*)data, len);
    _http.end();
    if (code < 0) _cli.stop();               // dead connection — force a fresh handshake next POST
    _last = code;
    return code >= 200 && code < 300;
  }
};
