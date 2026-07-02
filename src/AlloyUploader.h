// AlloyUploader.h — internal: uploads bytes directly to the Alloy data API from an ESP32.
//
// Protocol (confirmed against alloy-sdk + a live call — backend is Cloudflare R2, path-style,
// region "auto"):
//   1. POST {ALLOY_DATA_URL}/mesh/storage/upload-session, Authorization: Bearer {ALLOY_API_KEY},
//      body {"path":"<folder>","ttl_seconds":900}
//      -> {bucket, endpoint_url, region, prefix, credentials{access_key_id, secret_access_key, session_token}}
//   2. S3 PutObject {endpoint_url}/{bucket}/{prefix}{filename} SigV4-signed with the temp creds,
//      x-amz-content-sha256: UNSIGNED-PAYLOAD.
//
// The upload-session is cached and reused across files (creds live ~15 min). Uploads a raw byte
// buffer (RAM-buffered logging — no filesystem, no flash wear). Needs WiFi + an SNTP UTC clock.

#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "esp_arduino_version.h"

// The ESP32 Arduino core's prebuilt SDK embeds the full Mozilla root CA bundle
// (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL) inside libmbedtls; reference its
// linker symbols so TLS verifies against it without shipping any certs ourselves.
extern "C" {
  extern const uint8_t alloy_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
  extern const uint8_t alloy_crt_bundle_end[]   asm("_binary_x509_crt_bundle_end");
}

class AlloyUploader {
public:
  void begin(const char* dataUrl, const char* apiKey, bool insecure = false) {
    _dataUrl = dataUrl; _apiKey = apiKey; _insecure = insecure;
  }

  // Upload `len` bytes as <meshPath>/<filename> in Alloy. Returns true on 2xx.
  bool uploadBuffer(const uint8_t* data, size_t len, const char* filename, const char* meshPath,
                    const char* contentType = "text/csv") {
    if (!ensureSession(meshPath)) return false;
    String key = _prefix + String(filename);
    bool ok = putObject(key, data, len, contentType);
    if (!ok) _haveSession = false;          // force a fresh session next time
    return ok;
  }

private:
  String _dataUrl, _apiKey;
  String _bucket, _endpoint, _region, _prefix, _ak, _sk, _tok, _meshCached;
  bool _insecure = false;
  bool _haveSession = false; time_t _sessAt = 0;
  // Persistent keep-alive connection + HTTPClient for PUTs: a verified TLS handshake costs ~2s
  // on a classic ESP32, so per-file reconnects can't keep up with multiple channels flushing
  // every few seconds. Both must persist — a destructed HTTPClient stops the client, and
  // stop_ssl_socket() memsets the SSL context (dropping the CA-bundle attach callback).
  WiFiClientSecure _cli; HTTPClient _http; bool _reuseInit = false;

  void setupTLS(WiFiClientSecure& cli) {
    if (_insecure) { cli.setInsecure(); return; }
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
    cli.setCACertBundle(alloy_crt_bundle_start, alloy_crt_bundle_end - alloy_crt_bundle_start);
#else
    cli.setCACertBundle(alloy_crt_bundle_start);
#endif
  }

  bool ensureSession(const char* meshPath) {
    time_t now = time(nullptr);
    if (_haveSession && _meshCached == meshPath && (now - _sessAt) < 720) return true;
    if (!createSession(meshPath)) return false;
    _meshCached = meshPath; _sessAt = now; _haveSession = true;
    return true;
  }

  bool createSession(const char* meshPath) {
    WiFiClientSecure cli; setupTLS(cli);
    HTTPClient http;
    if (!http.begin(cli, _dataUrl + "/mesh/storage/upload-session")) return false;
    http.addHeader("Authorization", "Bearer " + _apiKey);
    http.addHeader("Content-Type", "application/json");
    JsonDocument body; body["path"] = meshPath; body["ttl_seconds"] = 900;
    String payload; serializeJson(body, payload);
    int code = http.POST(payload);
    if (code != 200 && code != 201) { http.end(); return false; }
    String resp = http.getString(); http.end();
    JsonDocument doc;
    if (deserializeJson(doc, resp)) return false;
    _bucket = doc["bucket"].as<String>();   _endpoint = doc["endpoint_url"].as<String>();
    _region = doc["region"].as<String>();   _prefix   = doc["prefix"].as<String>();
    _ak = doc["credentials"]["access_key_id"].as<String>();
    _sk = doc["credentials"]["secret_access_key"].as<String>();
    _tok = doc["credentials"]["session_token"].as<String>();
    if (_prefix.length() && _prefix[_prefix.length()-1] != '/') _prefix += '/';
    return _bucket.length() && _endpoint.length() && _ak.length();
  }

  bool putObject(const String& key, const uint8_t* data, size_t len, const char* contentType) {
    // _endpoint may carry a path; both signing and the request URL want scheme://host only
    String scheme = _endpoint.startsWith("http://") ? "http://" : "https://";
    String host = _endpoint; host.replace("https://", ""); host.replace("http://", "");
    int sl = host.indexOf('/'); if (sl >= 0) host = host.substring(0, sl);
    String base = scheme + host;

    time_t now = time(nullptr); struct tm tm; gmtime_r(&now, &tm);
    char amzdate[20], datestamp[12];
    strftime(amzdate, sizeof(amzdate), "%Y%m%dT%H%M%SZ", &tm);
    strftime(datestamp, sizeof(datestamp), "%Y%m%d", &tm);

    String canonUri = "/" + _bucket + "/" + uriEncode(key);
    const char* ph = "UNSIGNED-PAYLOAD";
    String sh = "host;x-amz-content-sha256;x-amz-date;x-amz-security-token";
    String ch = "host:" + host + "\nx-amz-content-sha256:" + ph + "\nx-amz-date:" + amzdate
              + "\nx-amz-security-token:" + _tok + "\n";
    String creq = "PUT\n" + canonUri + "\n\n" + ch + "\n" + sh + "\n" + ph;

    uint8_t h[32]; char hx[65];
    mbedtls_sha256((const uint8_t*)creq.c_str(), creq.length(), h, 0); hex(h, 32, hx);
    String scope = String(datestamp) + "/" + _region + "/s3/aws4_request";
    String sts = "AWS4-HMAC-SHA256\n" + String(amzdate) + "\n" + scope + "\n" + String(hx);

    uint8_t kD[32], kR[32], kS[32], kSig[32], sig[32]; char sigx[65];
    String k0 = "AWS4" + _sk;
    hmac((const uint8_t*)k0.c_str(), k0.length(), (const uint8_t*)datestamp, strlen(datestamp), kD);
    hmac(kD, 32, (const uint8_t*)_region.c_str(), _region.length(), kR);
    hmac(kR, 32, (const uint8_t*)"s3", 2, kS);
    hmac(kS, 32, (const uint8_t*)"aws4_request", 12, kSig);
    hmac(kSig, 32, (const uint8_t*)sts.c_str(), sts.length(), sig); hex(sig, 32, sigx);

    String auth = "AWS4-HMAC-SHA256 Credential=" + _ak + "/" + scope
                + ", SignedHeaders=" + sh + ", Signature=" + sigx;

    setupTLS(_cli);                          // re-arm every PUT: any stop() memsets the attach cb
    if (!_reuseInit) { _http.setReuse(true); _reuseInit = true; }
    if (!_http.begin(_cli, base + canonUri)) return false;
    _http.addHeader("Authorization", auth);
    _http.addHeader("x-amz-content-sha256", ph);
    _http.addHeader("x-amz-date", amzdate);
    _http.addHeader("x-amz-security-token", _tok);
    _http.addHeader("Content-Type", contentType);
    int code = _http.sendRequest("PUT", (uint8_t*)data, len);
    _http.end();
    if (code < 0) _cli.stop();               // dead connection — force a fresh handshake next PUT
    return code == 200 || code == 204;
  }

  static void hmac(const uint8_t* k, size_t kl, const uint8_t* d, size_t dl, uint8_t out[32]) {
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), k, kl, d, dl, out);
  }
  static void hex(const uint8_t* b, size_t n, char* out) {
    static const char* H = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2*i] = H[b[i] >> 4]; out[2*i+1] = H[b[i] & 0xF]; }
    out[2*n] = 0;
  }
  static String uriEncode(const String& s) {
    String o; for (size_t i = 0; i < s.length(); i++) { char c = s[i];
      if (isalnum((uint8_t)c) || c=='/'||c=='-'||c=='_'||c=='.'||c=='~') o += c;
      else { char b[4]; sprintf(b, "%%%02X", (uint8_t)c); o += b; } }
    return o;
  }
};
