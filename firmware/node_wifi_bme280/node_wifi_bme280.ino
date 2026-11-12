/*
 * ALTERNATIVE NODE — single ESP32, direct Wi-Fi + MQTT, BME280 air sensor
 * ESP32 + BME280 + small 5 V solar panel + SUPERCAPACITOR -> MQTT (ThingSpeak)
 *
 * This variant needs a real supercapacitor (e.g. 2 x 2.7 V 5 F in series):
 * one Wi-Fi connection costs ~0.3-1 J, far more than ordinary electrolytic
 * capacitors can store. The adaptive sleep logic is the same idea as in the
 * ESP-NOW node: measure the storage voltage, estimate the harvested power,
 * choose the next sleep interval.
 *
 * Libraries (Arduino IDE -> Library Manager):
 *   - Adafruit BME280 Library (+ Adafruit Unified Sensor)
 *   - PubSubClient (Nick O'Leary)
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_BME280.h>
#include "esp_sleep.h"
#include "esp_system.h"

// ============================ SETTINGS ============================
const char* WIFI_SSID = "YOUR_WIFI";
const char* WIFI_PASS = "YOUR_PASSWORD";

// A static IP skips DHCP -> faster connection -> less energy per cycle.
// Use addresses from your own network, or set USE_STATIC_IP = false.
const bool USE_STATIC_IP = true;
IPAddress LOCAL_IP(192, 168, 1, 77);
IPAddress GATEWAY(192, 168, 1, 1);
IPAddress SUBNET(255, 255, 255, 0);
IPAddress DNS1(8, 8, 8, 8);

// ThingSpeak: Devices -> MQTT -> Add a new device
const char* MQTT_HOST      = "mqtt3.thingspeak.com";
const int   MQTT_PORT      = 1883;
const char* MQTT_CLIENT_ID = "YOUR_CLIENT_ID";
const char* MQTT_USER      = "YOUR_USERNAME";
const char* MQTT_PASS      = "YOUR_MQTT_PASSWORD";
const char* CHANNEL_ID     = "YOUR_CHANNEL_ID";

// ============================ HARDWARE ============================
const int   PIN_VCAP   = 34;     // ADC1 (ADC2 does not work together with Wi-Fi!)
const float DIVIDER    = 2.0;    // 1 MOhm / 1 MOhm divider
const float C_FARAD    = 2.5;    // 2 x 5 F (2.7 V) in series = 2.5 F, 5.4 V

// ============================ ENERGY BUDGET ============================
// Measure your own values (INA219 or multimeter) and put them here.
const float E_CYCLE_J  = 1.0;     // energy of one "wake + Wi-Fi + publish" cycle, J
const float P_SLEEP_W  = 0.0003;  // sleep consumption (board + regulator + divider), W

const float V_CUTOFF   = 3.6;     // below this the 3.3 V regulator drops out
const float V_FULL     = 4.9;     // nearly full (above this the Zener burns the surplus)
const float SAFETY     = 1.3;     // spend only ~75 % of what is harvested

const uint32_t MIN_INTERVAL_S = 20;    // ThingSpeak accepts at most one update per 15 s
const uint32_t MAX_INTERVAL_S = 3600;  // never sleep longer than one hour
const uint32_t FIRST_BOOT_S   = 300;

// ================= MEMORY THAT SURVIVES DEEP SLEEP =================
RTC_DATA_ATTR uint32_t bootCount   = 0;
RTC_DATA_ATTR float    vAfterLast  = 0;    // voltage at the end of the previous cycle
RTC_DATA_ATTR uint32_t lastSleepS  = 0;    // how long the previous sleep was
RTC_DATA_ATTR float    pHarvestAvg = -1;   // smoothed harvested power, W (-1 = unknown yet)
RTC_DATA_ATTR int32_t  wifiChannel = 0;    // cached router channel + BSSID -> fast reconnect
RTC_DATA_ATTR uint8_t  wifiBssid[6];

Adafruit_BME280 bme;
WiFiClient net;
PubSubClient mqtt(net);

enum State { ST_HARVEST_ONLY, ST_LOW, ST_NORMAL, ST_RICH };
const char* STATE_NAMES[] = { "HARVEST_ONLY", "LOW", "NORMAL", "RICH" };

// --------------------------------------------------------------------------------
float readVcap() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(PIN_VCAP);
  return (sum / 16.0f) / 1000.0f * DIVIDER;
}

// Energy that can be drawn before the supercapacitor falls to V_CUTOFF
float usableEnergyJ(float v) {
  if (v <= V_CUTOFF) return 0;
  return 0.5f * C_FARAD * (v * v - V_CUTOFF * V_CUTOFF);
}

void goToSleep(uint32_t seconds, float vNow) {
  vAfterLast = vNow;
  lastSleepS = seconds;
  Serial.printf("Sleeping %lu s\n", (unsigned long)seconds);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_deep_sleep_start();
}

bool connectWifiFast() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  if (USE_STATIC_IP) WiFi.config(LOCAL_IP, GATEWAY, SUBNET, DNS1);

  // Known channel + BSSID skips the scan: ~0.3-0.5 s instead of 2-3 s
  if (wifiChannel > 0) WiFi.begin(WIFI_SSID, WIFI_PASS, wifiChannel, wifiBssid);
  else                 WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > 8000) {      // do not burn energy forever
      wifiChannel = 0;               // next time do a full scan
      return false;
    }
    delay(20);
  }
  wifiChannel = WiFi.channel();
  memcpy(wifiBssid, WiFi.BSSID(), 6);
  return true;
}

bool publish(float t, float h, float p, float v, uint32_t nextS, float pHarvest_mW, State st) {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  if (!mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)) return false;

  char topic[64];
  snprintf(topic, sizeof(topic), "channels/%s/publish", CHANNEL_ID);

  // field1 temperature, field2 humidity, field3 pressure, field4 storage voltage,
  // field5 sleep interval, field6 harvested power mW
  char payload[200];
  snprintf(payload, sizeof(payload),
           "field1=%.2f&field2=%.1f&field3=%.1f&field4=%.3f&field5=%lu&field6=%.2f&status=%s",
           t, h, p, v, (unsigned long)nextS, pHarvest_mW, STATE_NAMES[st]);

  bool ok = mqtt.publish(topic, payload);
  mqtt.loop();
  delay(50);              // let the packet leave
  mqtt.disconnect();
  return ok;
}

// --------------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  bootCount++;

  // 1. Brown-out reset = the last cycle ran out of energy -> just harvest for a while
  if (esp_reset_reason() == ESP_RST_BROWNOUT) {
    Serial.println("Brown-out! Harvesting.");
    goToSleep(MAX_INTERVAL_S / 2, readVcap());
  }

  // 2. Storage voltage BEFORE Wi-Fi (no load)
  float vNow = readVcap();

  // 3. Harvested power: how much the supercapacitor charged during the last sleep
  if (lastSleepS > 0 && vAfterLast > 0) {
    float eGain = 0.5f * C_FARAD * (vNow * vNow - vAfterLast * vAfterLast);
    float pMeas = eGain / lastSleepS + P_SLEEP_W;  // add back what sleep itself consumed
    if (pMeas < 0) pMeas = 0;
    pHarvestAvg = (pHarvestAvg < 0) ? pMeas : 0.7f * pHarvestAvg + 0.3f * pMeas;
  }

  // 4. State machine
  State st;
  float eUsable = usableEnergyJ(vNow);
  if (eUsable < 2.0f * E_CYCLE_J)      st = ST_HARVEST_ONLY;   // not even two cycles left
  else if (vNow >= V_FULL)             st = ST_RICH;           // full: surplus would be wasted anyway
  else if (eUsable < 5.0f * E_CYCLE_J) st = ST_LOW;
  else                                 st = ST_NORMAL;

  // 5. Next sleep interval
  uint32_t nextS;
  if (st == ST_HARVEST_ONLY) {
    nextS = MAX_INTERVAL_S;
  } else if (st == ST_RICH) {
    nextS = MIN_INTERVAL_S;
  } else if (pHarvestAvg < 0) {
    nextS = FIRST_BOOT_S;
  } else {
    float pNet = pHarvestAvg - P_SLEEP_W;          // what is really left for work
    float s = (pNet > 0) ? SAFETY * E_CYCLE_J / pNet : MAX_INTERVAL_S;
    if (st == ST_LOW) s *= 2;                      // small reserve -> save up longer
    nextS = (uint32_t)constrain(s, (float)MIN_INTERVAL_S, (float)MAX_INTERVAL_S);
  }

  float pmW = (pHarvestAvg < 0 ? 0 : pHarvestAvg) * 1000.0f;
  Serial.printf("#%lu V=%.3f V, E=%.1f J, harvest=%.2f mW, state=%s, next=%lu s\n",
                (unsigned long)bootCount, vNow, eUsable, pmW, STATE_NAMES[st], (unsigned long)nextS);

  if (st == ST_HARVEST_ONLY) goToSleep(nextS, vNow);

  // 6. Air measurement (forced mode: one sample, then the sensor sleeps by itself)
  float t = NAN, h = NAN, p = NAN;
  Wire.begin();  // SDA=21, SCL=22 on most ESP32 boards
  if (bme.begin(0x76) || bme.begin(0x77)) {
    bme.setSampling(Adafruit_BME280::MODE_FORCED,
                    Adafruit_BME280::SAMPLING_X1, Adafruit_BME280::SAMPLING_X1,
                    Adafruit_BME280::SAMPLING_X1, Adafruit_BME280::FILTER_OFF);
    bme.takeForcedMeasurement();
    t = bme.readTemperature();
    h = bme.readHumidity();
    p = bme.readPressure() / 100.0f;
  } else {
    Serial.println("BME280 not found! Check the wiring.");
  }

  // 7. Publish
  if (connectWifiFast()) {
    bool ok = publish(t, h, p, vNow, nextS, pmW, st);
    Serial.println(ok ? "Published" : "MQTT error");
  } else {
    Serial.println("Wi-Fi did not connect");
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  // 8. Voltage after the work (let the capacitor recover for 50 ms), then sleep
  delay(50);
  goToSleep(nextS, readVcap());
}

void loop() {}  // never reached: after deep sleep the ESP32 starts again from setup()
