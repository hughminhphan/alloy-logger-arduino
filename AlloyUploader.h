// AlloyUploader.h — upload a finalized session file directly to the Alloy data API from an ESP32.
//
// Protocol (confirmed against alloy-sdk 0.1.1 source + a live call — backend is Cloudflare R2,
// path-style, region "auto"):
//   1. POST {ALLOY_DATA_URL}/mesh/storage/upload-session
//        header: Authorization: Bearer {ALLOY_API_KEY}
//        body  : {"path":"<mesh-folder>","ttl_seconds":<n>}
//      -> {"bucket","endpoint_url","region","prefix","expires_at",
//          "credentials":{"access_key_id","secret_access_key","session_token"}}
//      File lands at mesh key  uploads/sdk-uploads/<path>/<filename>  (prefix carries that).
//   2. S3 PutObject  {endpoint_url}/{bucket}/{prefix}{filename}  signed with the TEMP STS creds
//      via AWS SigV4. Over TLS we use x-amz-content-sha256: UNSIGNED-PAYLOAD (no whole-file hashing).
//      VERIFIED: this exact signing lands a file in Alloy Mesh (R2 PUT 200).
//
// Requires: WiFi connected, clock set via configTime()/SNTP (SigV4 needs real UTC), ArduinoJson v7.
//
// SECURITY: this puts an org ALLOY_API_KEY on the device. Fine for a single dev rig; for a fleet,
// move to per-device keys or the Option-3 server shim (which keeps creds off the robot entirely).

#pragma once
#include <FS.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

class AlloyUploader {
public:
  void begin(const char* dataUrl, const char* apiKey) { _dataUrl = dataUrl; _apiKey = apiKey; }

  // Upload localPath (on `fs`, e.g. SD or LittleFS) into mesh folder `meshPath`. Blocking.
  bool uploadFile(fs::FS& fs, const char* localPath, const char* meshPath) {
    String bucket, endpoint, region, prefix, ak, sk, tok;
    if (!createSession(meshPath, bucket, endpoint, region, prefix, ak, sk, tok)) return false;

    File f = fs.open(localPath, FILE_READ);
    if (!f) return false;
    size_t size = f.size();
    const char* base = strrchr(localPath, '/'); base = base ? base + 1 : localPath;
    String key = prefix + String(base);                 // prefix already ends with '/'

    bool ok = putObject(endpoint, bucket, region, key, ak, sk, tok, f, size);
    f.close();
    return ok;
  }

private:
  String _dataUrl, _apiKey;

  bool createSession(const char* meshPath, String& bucket, String& endpoint, String& region,
                     String& prefix, String& ak, String& sk, String& tok) {
    WiFiClientSecure cli; cli.setInsecure();           // TODO: pin Alloy/R2 CA for production
    HTTPClient http;
    if (!http.begin(cli, _dataUrl + "/mesh/storage/upload-session")) return false;
    http.addHeader("Authorization", "Bearer " + _apiKey);
    http.addHeader("Content-Type", "application/json");
    String body = String("{\"path\":\"") + meshPath + "\",\"ttl_seconds\":900}";
    int code = http.POST(body);
    if (code != 200 && code != 201) { http.end(); return false; }
    String resp = http.getString(); http.end();

    JsonDocument doc;
    if (deserializeJson(doc, resp)) return false;
    bucket   = doc["bucket"].as<String>();
    endpoint = doc["endpoint_url"].as<String>();
    region   = doc["region"].as<String>();
    prefix   = doc["prefix"].as<String>();
    ak = doc["credentials"]["access_key_id"].as<String>();
    sk = doc["credentials"]["secret_access_key"].as<String>();
    tok = doc["credentials"]["session_token"].as<String>();
    if (prefix.length() && prefix[prefix.length()-1] != '/') prefix += '/';
    return bucket.length() && endpoint.length() && ak.length();
  }

  // ---- SigV4 PutObject (path-style, UNSIGNED-PAYLOAD) ----
  bool putObject(const String& endpoint, const String& bucket, const String& region,
                 const String& key, const String& ak, const String& sk, const String& tok,
                 File& body, size_t size) {
    String host = endpoint; host.replace("https://", ""); host.replace("http://", "");
    int slash = host.indexOf('/'); if (slash >= 0) host = host.substring(0, slash);

    time_t now = time(nullptr); struct tm tm; gmtime_r(&now, &tm);
    char amzdate[20], datestamp[12];
    strftime(amzdate, sizeof(amzdate), "%Y%m%dT%H%M%SZ", &tm);
    strftime(datestamp, sizeof(datestamp), "%Y%m%d", &tm);

    String canonUri = "/" + bucket + "/" + uriEncodePath(key);
    const char* payloadHash = "UNSIGNED-PAYLOAD";
    String signedHeaders = "host;x-amz-content-sha256;x-amz-date;x-amz-security-token";
    String canonHeaders = "host:" + host + "\n"
                        + "x-amz-content-sha256:" + payloadHash + "\n"
                        + "x-amz-date:" + amzdate + "\n"
                        + "x-amz-security-token:" + tok + "\n";
    String canonReq = "PUT\n" + canonUri + "\n\n" + canonHeaders + "\n" + signedHeaders + "\n" + payloadHash;

    uint8_t h[32]; char hhex[65];
    sha256((const uint8_t*)canonReq.c_str(), canonReq.length(), h); hex(h, 32, hhex);
    String scope = String(datestamp) + "/" + region + "/s3/aws4_request";
    String toSign = "AWS4-HMAC-SHA256\n" + String(amzdate) + "\n" + scope + "\n" + String(hhex);

    uint8_t kDate[32], kReg[32], kSvc[32], kSig[32], sig[32]; char sighex[65];
    String k0 = "AWS4" + sk;
    hmac((const uint8_t*)k0.c_str(), k0.length(), (const uint8_t*)datestamp, strlen(datestamp), kDate);
    hmac(kDate, 32, (const uint8_t*)region.c_str(), region.length(), kReg);
    hmac(kReg, 32, (const uint8_t*)"s3", 2, kSvc);
    hmac(kSvc, 32, (const uint8_t*)"aws4_request", 12, kSig);
    hmac(kSig, 32, (const uint8_t*)toSign.c_str(), toSign.length(), sig); hex(sig, 32, sighex);

    String auth = "AWS4-HMAC-SHA256 Credential=" + ak + "/" + scope
                + ", SignedHeaders=" + signedHeaders + ", Signature=" + sighex;

    WiFiClientSecure cli; cli.setInsecure();
    HTTPClient http;
    if (!http.begin(cli, endpoint + canonUri)) return false;
    http.addHeader("Authorization", auth);
    http.addHeader("x-amz-content-sha256", payloadHash);
    http.addHeader("x-amz-date", amzdate);
    http.addHeader("x-amz-security-token", tok);
    http.addHeader("Content-Type", "application/octet-stream");
    int code = http.sendRequest("PUT", &body, size);     // streams the file as the body
    http.end();
    return code == 200 || code == 204;
  }

  static void sha256(const uint8_t* d, size_t n, uint8_t out[32]) { mbedtls_sha256(d, n, out, 0); }
  static void hmac(const uint8_t* key, size_t klen, const uint8_t* d, size_t dlen, uint8_t out[32]) {
    const mbedtls_md_info_t* mi = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_hmac(mi, key, klen, d, dlen, out);
  }
  static void hex(const uint8_t* b, size_t n, char* out) {
    static const char* H = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2*i] = H[b[i] >> 4]; out[2*i+1] = H[b[i] & 0xF]; }
    out[2*n] = 0;
  }
  static String uriEncodePath(const String& s) {
    String o; for (size_t i = 0; i < s.length(); i++) {
      char c = s[i];
      if (isalnum(c) || c=='/' || c=='-' || c=='_' || c=='.' || c=='~') o += c;
      else { char b[4]; sprintf(b, "%%%02X", (uint8_t)c); o += b; }
    }
    return o;
  }
};
