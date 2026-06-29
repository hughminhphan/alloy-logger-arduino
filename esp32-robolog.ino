// esp32-robolog.ino — robot black-box recorder (no camera, no audio).
// Records sensor INPUTS + actuator OUTPUTS on one timeline -> JSONL on SD -> uploads the
// finalized file DIRECTLY to the Alloy data API. No laptop, no intermediary.
//
// Session model: record a run -> end() -> upload. Triggered here by a fixed duration; swap for a
// button / e-stop / "mission end" signal as needed. Small data (no AV) = a run is KB..few MB.
//
// Board: ESP32 / ESP32-S3 + SPI SD. Libs: ArduinoJson. WiFi for upload; SNTP for SigV4 UTC clock.
// Config (WiFi + Alloy key) lives in secrets.h (gitignored) — copy secrets.h.example.

#include <WiFi.h>
#include "secrets.h"

// Storage backend: 0 = on-chip LittleFS (NO SD card needed), 1 = SPI SD card.
#define USE_SD 0
#if USE_SD
  #include <SD.h>
  #include <SPI.h>
  #define LOGFS SD
  #define SD_CS 5
#else
  #include <LittleFS.h>
  #define LOGFS LittleFS
#endif

#include "RoboLog.h"
#include "AlloyUploader.h"

RoboLog logger;
AlloyUploader alloy;

uint16_t CH_IMU, CH_M0, CH_M1, CH_ENC, CH_ANALOG, CH_ESTOP, CH_MODE;

static const uint32_t CONTROL_HZ = 1000;               // controller rate
static const uint32_t LOG_HZ     = 100;                // recorder rate (decimated; keeps JSONL light)
static const uint32_t LOG_EVERY  = CONTROL_HZ / LOG_HZ;
// LittleFS holds ~1.4MB on a 4MB board, so a no-SD run is capped (~10s here ≈ 0.4MB).
// Set USE_SD 1 (or stream in chunks) to lift the cap.
static const uint32_t RUN_SECONDS = 10;

#pragma pack(push,1)
struct ImuRec   { float ax,ay,az,gx,gy,gz; };
struct MotorRec { float cmd, current; int32_t enc; };
struct AnalogRec{ uint16_t ch[8]; };
#pragma pack(pop)

const char* RUN_FILE = "/run.jsonl";

void setup() {
  Serial.begin(115200);
#if USE_SD
  SPI.begin();
  if (!SD.begin(SD_CS)) { Serial.println("SD fail"); while(1) delay(1000); }
#else
  if (!LittleFS.begin(true)) { Serial.println("LittleFS fail"); while(1) delay(1000); }
#endif

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(250); Serial.print("."); }
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");   // UTC — required for SigV4
  while (time(nullptr) < 1700000000) delay(100);
  alloy.begin(ALLOY_DATA_URL, ALLOY_API_KEY);

  if (!logger.begin(LOGFS, RUN_FILE, /*writerCore=*/1)) { Serial.println("log fail"); while(1) delay(1000); }
  CH_IMU    = logger.channel("imu",     "ax:f32,ay:f32,az:f32,gx:f32,gy:f32,gz:f32");
  CH_M0     = logger.channel("motor0",  "cmd:f32,current:f32,enc:i32");
  CH_M1     = logger.channel("motor1",  "cmd:f32,current:f32,enc:i32");
  CH_ENC    = logger.channel("encoders","left:i32,right:i32");
  CH_ANALOG = logger.channel("analog",  "a0:u16,a1:u16,a2:u16,a3:u16,a4:u16,a5:u16,a6:u16,a7:u16");
  CH_ESTOP  = logger.channel("estop",   "state:u32");
  CH_MODE   = logger.channel("mode",    "state:u32");

  xTaskCreatePinnedToCore(controlTask, "control", 8192, nullptr, configMAX_PRIORITIES-2, nullptr, 0);
}

void loop() { delay(1000); }   // work happens in controlTask

void controlTask(void*) {
  TickType_t next = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(1000 / CONTROL_HZ);
  uint32_t tick = 0;
  uint64_t endAt = logger.nowNs() + (uint64_t)RUN_SECONDS * 1000000000ULL;

  for (;;) {
    uint64_t t = logger.nowNs();          // ONE timestamp this tick -> everything aligned

    // 1. READ (TODO: real sensors; use PCNT for encoders, not ISR-per-edge)
    ImuRec imu = {};
    AnalogRec an; for (int i=0;i<8;i++) an.ch[i] = analogRead(i);
    int32_t encL=0, encR=0;

    // 2. COMPUTE (TODO: your controller)
    float m0=0, m1=0;

    // 3. ACTUATE (TODO: ledcWrite/MCPWM/GPIO)

    // 4. LOG (decimated to LOG_HZ; discrete signals on-change at full rate)
    if (tick % LOG_EVERY == 0) {
      logger.write(CH_IMU, t, &imu, sizeof(imu));
      MotorRec a={m0,0,encL}, b={m1,0,encR};
      logger.write(CH_M0, t, &a, sizeof(a));
      logger.write(CH_M1, t, &b, sizeof(b));
      struct {int32_t l,r;} e={encL,encR}; logger.write(CH_ENC, t, &e, sizeof(e));
      logger.write(CH_ANALOG, t, &an, sizeof(an));
    }
    logger.writeOnChange(CH_ESTOP, 0, digitalRead(0), t);
    logger.writeOnChange(CH_MODE,  1, 0, t);

    if (t >= endAt) break;
    tick++;
    vTaskDelayUntil(&next, period);
  }

  // ---- finalize + upload directly to Alloy ----
  Serial.printf("run done, dropped=%u — finalizing\n", logger.dropped());
  logger.end();
  Serial.println(alloy.uploadFile(LOGFS, RUN_FILE, MESH_PATH) ? "uploaded to Alloy" : "upload FAILED");
  vTaskDelete(nullptr);
}
