// SelfBalancingRobot — add Alloy streaming to a real-time control loop.
//
// Shows the pattern for a robot that ALREADY runs a tight control loop (and may already manage
// WiFi): the background uploader is pinned to core 0, so the balance loop + step generation on
// core 1 are never disturbed. log() is a non-blocking memcpy. This sketch simulates the loop so it
// runs standalone — drop the same calls into your firmware (e.g. the SBR right after computePID()).

#include <AlloyLogger.h>
#include <math.h>

const char* WIFI_SSID = "your-2.4GHz-ssid";
const char* WIFI_PASS = "your-wifi-password";
const char* ALLOY_KEY = "your-alloy-api-key";

AlloyLogger alloy;

void setup() {
  Serial.begin(115200);

  // optional semantics so Alloy AI labels the streams precisely
  alloy.describe("balance", "pitch",  "deg", -90, 90, "fused tilt angle");
  alloy.describe("balance", "output", "pwm", -255, 255, "PID motor command");
  alloy.describe("balance", "rate",   "deg/s", -500, 500, "gyro angular rate");

  alloy.wifi(WIFI_SSID, WIFI_PASS);          // omit on the real robot if comms already connected
  alloy.device("sbr-01", "fw16");
  alloy.core(0);                             // keep uploads off the control core (core 1)
  alloy.begin(ALLOY_KEY, "robots/sbr");

  xTaskCreatePinnedToCore(controlLoop, "control", 8192, nullptr, configMAX_PRIORITIES - 2, nullptr, 1);

  // On a kill switch / end of a scripted run, alloy.end() finalizes the mission .mcap immediately.
  // Without it the run finalizes ~10 min after the last data (power loss is handled server-side).
}

void loop() { delay(1000); }

// 100 Hz balance loop (simulated). On the real robot this is your existing PID cycle.
void controlLoop(void*) {
  const TickType_t period = pdMS_TO_TICKS(10);
  TickType_t next = xTaskGetTickCount();
  float integral = 0;
  for (;;) {
    float t = millis() / 1000.0;
    float pitch = 6.0 * sin(t * 1.7) + 1.5 * sin(t * 9.0);   // pretend IMU
    float rate  = 60.0 * cos(t * 1.7);
    float setpoint = 0.5;
    float error = setpoint - pitch;
    integral += error * 0.01;
    float p = 20.0 * error, i = 200.0 * integral, d = 0.0 * rate;
    float output = constrain(p + i + d, -255, 255);
    bool motorsOn = fabs(output) > 3.0;

    // ---- one record per control cycle, all fields aligned to this instant ----
    alloy.log("balance")
         .set("pitch", pitch)
         .set("setpoint", setpoint)
         .set("output", output)
         .set("p", p).set("i", i).set("d", d)
         .set("rate", rate)
         .set("motors", motorsOn);

    vTaskDelayUntil(&next, period);
  }
}
