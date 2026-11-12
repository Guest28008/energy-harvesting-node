/*
 * NODE — Microclimate Energy-Harvesting Node
 * ESP32 + water sensor + small 5 V solar panel + capacitor storage
 *
 * The node never connects to Wi-Fi (too expensive in energy). It sends one
 * short ESP-NOW packet to a second ESP32 (the gateway), and the gateway
 * forwards the data to ThingSpeak over MQTT.
 *
 * Adaptive state machine:
 *   on every wake-up the node measures the storage voltage, estimates how
 *   much energy the panel delivered while it was asleep, and chooses its own
 *   next sleep interval so that it never drains the storage.
 *
 * Board: "ESP32 Dev Module" (or your ESP32 board). No extra libraries needed.
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "esp_sleep.h"
#include "esp_system.h"

// ============================ SETTINGS ============================

// Wi-Fi channel. The GATEWAY prints it on start-up ("Wi-Fi channel: N").
// Node and gateway MUST use the same channel.
const uint8_t WIFI_CHANNEL = 1;

// Bench test on USB power: true = ignore the energy logic, send every 20 s.
// Use it to check the sensor and the radio link first. Set false for solar operation.
const bool TEST_MODE = false;

// ---- Pins (see hardware/schematics) ----
const int PIN_VCAP      = 34;   // storage voltage through a 1M/1M divider (ADC1)
const int PIN_WATER_PWR = 25;   // water sensor "+" (powered only while measuring)
const int PIN_WATER_SIG = 35;   // water sensor "S" (ADC1)

// ---- Energy storage ----
const float DIVIDER  = 2.0;                 // 1 MOhm / 1 MOhm divider
const int   N_CAPS   = 20;                  // number of 1000 uF capacitors in parallel
const float C_FARAD  = N_CAPS * 0.001f;     // total capacitance, F

// ---- Regulator ----
// Below this input voltage the regulator can no longer deliver 3.3 V:
//   AMS1117 -> 4.5     HT7333 / ME6211 -> 3.6
const float V_CUTOFF = 4.5;
const float V_FULL   = 4.9;                 // nearly full (above this the Zener burns the surplus)

// ---- Energy budget ----
// Energy taken from the storage by one cycle "wake + measure + ESP-NOW".
// Initial estimate; measure it with an INA219 and put your own value here.
const float E_CYCLE_J = 0.06;
const float SAFETY    = 1.3;                // spend only ~75 % of what is harvested

const uint32_t MIN_INTERVAL_S = 15;
const uint32_t MAX_INTERVAL_S = 1800;       // never sleep longer than 30 minutes
const uint32_t FIRST_BOOT_S   = 60;

// ============================ PACKET ============================
// The same struct must be used in the gateway sketch!
typedef struct __attribute__((packed)) {
  uint32_t magic;        // marks the packet as ours
  uint32_t boot;         // wake-up counter
  uint16_t water;        // raw water sensor value 0..4095
  float    vcap;         // storage voltage, V
  float    harvest_mW;   // estimated harvested power, mW
  uint32_t nextS;        // seconds until the next transmission
  uint8_t  state;        // state machine state
} Packet;

const uint32_t MAGIC = 0xEC0DA7A1;
uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};   // broadcast: no gateway MAC needed

// ================= MEMORY THAT SURVIVES DEEP SLEEP =================
RTC_DATA_ATTR uint32_t bootCount   = 0;
RTC_DATA_ATTR float    vAfterLast  = 0;     // voltage at the end of the previous cycle
RTC_DATA_ATTR uint32_t lastSleepS  = 0;     // how long the previous sleep was
RTC_DATA_ATTR float    pHarvestAvg = -1;    // smoothed harvested power, W (-1 = unknown yet)

enum State : uint8_t { ST_HARVEST_ONLY = 0, ST_LOW = 1, ST_NORMAL = 2, ST_RICH = 3 };
const char* STATE_NAMES[] = { "HARVEST_ONLY", "LOW", "NORMAL", "RICH" };

// ============================ HELPERS ============================
float readVcap() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(PIN_VCAP);
  return (sum / 16.0f) / 1000.0f * DIVIDER;
}

// Energy that can be drawn before the storage falls to V_CUTOFF:  E = 1/2 C (V^2 - Vcut^2)
float usableEnergyJ(float v) {
  if (v <= V_CUTOFF) return 0;
  return 0.5f * C_FARAD * (v * v - V_CUTOFF * V_CUTOFF);
}

uint16_t readWater() {
  pinMode(PIN_WATER_PWR, OUTPUT);
  digitalWrite(PIN_WATER_PWR, HIGH);        // power the sensor
  delay(10);                                // let it settle
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogRead(PIN_WATER_SIG);
  digitalWrite(PIN_WATER_PWR, LOW);         // power off: saves energy, avoids electrode corrosion
  return sum / 8;
}

void goToSleep(uint32_t seconds, float vNow) {
  vAfterLast = vNow;
  lastSleepS = seconds;
  Serial.printf("Sleeping %lu s\n\n", (unsigned long)seconds);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_deep_sleep_start();
}

bool sendEspNow(const Packet& pkt) {
  WiFi.mode(WIFI_STA);
  esp_wifi_set_promiscuous(true);                       // required to change the channel
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() != ESP_OK) return false;

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST, 6);
  peer.channel = 0;          // 0 = current channel
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) return false;

  esp_err_t r = esp_now_send(BROADCAST, (const uint8_t*)&pkt, sizeof(pkt));
  delay(15);                 // give the frame time to leave the radio
  esp_now_deinit();
  WiFi.mode(WIFI_OFF);
  return r == ESP_OK;
}

// ============================ MAIN LOGIC ============================
// After deep sleep the ESP32 restarts from setup(), so the whole duty cycle lives here.
void setup() {
  Serial.begin(115200);
  bootCount++;

  // 1. Brown-out reset = the last cycle ran out of energy -> just harvest for a while.
  if (!TEST_MODE && esp_reset_reason() == ESP_RST_BROWNOUT) {
    Serial.println("Brown-out! Not enough energy, harvesting.");
    goToSleep(MAX_INTERVAL_S / 4, readVcap());
  }

  // 2. Storage voltage BEFORE the radio is switched on (no load)
  float vNow = readVcap();

  // 3. Harvested power: how much the storage charged during the last sleep
  if (lastSleepS > 0 && vAfterLast > 0) {
    float eGain = 0.5f * C_FARAD * (vNow * vNow - vAfterLast * vAfterLast);
    float pMeas = eGain / lastSleepS;                   // W, sleep losses already included
    if (pMeas < 0) pMeas = 0;
    pHarvestAvg = (pHarvestAvg < 0) ? pMeas : 0.7f * pHarvestAvg + 0.3f * pMeas;   // smoothing
  }

  // 4. State machine
  float eUsable = usableEnergyJ(vNow);
  State st;
  if (TEST_MODE)                            st = ST_RICH;
  else if (eUsable < 1.5f * E_CYCLE_J)      st = ST_HARVEST_ONLY;  // not enough for one cycle with margin
  else if (vNow >= V_FULL)                  st = ST_RICH;          // full: surplus would be wasted anyway
  else if (eUsable < 3.0f * E_CYCLE_J)      st = ST_LOW;
  else                                      st = ST_NORMAL;

  // 5. Next sleep interval
  uint32_t nextS;
  if (TEST_MODE)                  nextS = 20;
  else if (st == ST_HARVEST_ONLY) nextS = (pHarvestAvg > 0) ? 120 : 600;
  else if (st == ST_RICH)         nextS = MIN_INTERVAL_S;
  else if (pHarvestAvg < 0)       nextS = FIRST_BOOT_S;              // harvest power not known yet
  else {
    // Key formula: sleep long enough for the panel to pay back one cycle.
    // If the panel delivers (almost) nothing (dark), sleep as long as allowed.
    float s = (pHarvestAvg > 0.00001f) ? SAFETY * E_CYCLE_J / pHarvestAvg : (float)MAX_INTERVAL_S;
    if (st == ST_LOW) s *= 2;     // small reserve -> save up longer
    nextS = (uint32_t)constrain(s, (float)MIN_INTERVAL_S, (float)MAX_INTERVAL_S);
  }

  float pmW = (pHarvestAvg < 0 ? 0 : pHarvestAvg) * 1000.0f;
  Serial.printf("#%lu  V=%.3f V  E=%.3f J  harvest=%.2f mW  state=%s  next=%lu s\n",
                (unsigned long)bootCount, vNow, eUsable, pmW, STATE_NAMES[st], (unsigned long)nextS);

  if (st == ST_HARVEST_ONLY) goToSleep(nextS, vNow);

  // 6. Water measurement
  uint16_t water = readWater();
  Serial.printf("Water sensor: %u\n", water);

  // 7. Send to the gateway
  Packet pkt;
  pkt.magic      = MAGIC;
  pkt.boot       = bootCount;
  pkt.water      = water;
  pkt.vcap       = vNow;
  pkt.harvest_mW = pmW;
  pkt.nextS      = nextS;
  pkt.state      = st;
  Serial.println(sendEspNow(pkt) ? "Sent via ESP-NOW" : "ESP-NOW error");

  // 8. Voltage after the work, then sleep
  delay(20);
  goToSleep(nextS, readVcap());
}

void loop() {}   // never reached: after deep sleep the ESP32 starts again from setup()
