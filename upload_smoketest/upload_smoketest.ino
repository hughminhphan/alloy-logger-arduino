// upload_smoketest.ino — prove the ESP32 -> Alloy upload path with NO SD card.
// Writes a tiny JSONL to on-chip LittleFS and uploads it directly to Alloy. Watch serial @115200.
// Copy ../secrets.h next to this sketch (or symlink) before building.

#include <WiFi.h>
#include <LittleFS.h>
#include "secrets.h"
#include "AlloyUploader.h"

AlloyUploader alloy;

void setup() {
  Serial.begin(115200);
  delay(500);
  if (!LittleFS.begin(true)) { Serial.println("LittleFS fail"); return; }

  File f = LittleFS.open("/smoke.jsonl", FILE_WRITE);
  f.print("{\"ch\":\"imu\",\"t_ns\":1000,\"ax\":0.1,\"ay\":0.2,\"az\":0.3}\n");
  f.print("{\"ch\":\"motor0\",\"t_ns\":1000,\"cmd\":0.5,\"current\":1.25,\"enc\":-42}\n");
  f.close();

  Serial.print("WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(250); Serial.print("."); }
  Serial.println(" connected");

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  while (time(nullptr) < 1700000000) delay(100);
  Serial.println("clock set (UTC)");

  alloy.begin(ALLOY_DATA_URL, ALLOY_API_KEY);
  bool ok = alloy.uploadFile(LittleFS, "/smoke.jsonl", MESH_PATH);
  Serial.println(ok ? "UPLOAD OK -> check Alloy Mesh Storage" : "UPLOAD FAILED");
}

void loop() {}
