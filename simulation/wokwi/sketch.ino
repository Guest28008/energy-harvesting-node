/*
 * SIMULATION: Microclimate Energy-Harvesting Node (for wokwi.com)
 *
 * A simulator cannot charge a capacitor from real sunlight, so the solar panel
 * and the storage capacitor are MODELLED in software with the same formulas
 * used in the real project:
 *   - "LIGHT" knob (GPIO34)  = illumination of the solar panel, 0..100 %
 *   - "WATER" knob (GPIO35)  = water sensor, 0..100 %
 *   - storage capacitor      = E = 1/2 * C * V^2
 *   - ESP32 deep sleep       = a countdown timer (model time is accelerated)
 *
 * The sleep-schedule logic (state machine) is THE SAME as in the real node.
 * The OLED shows the storage voltage, the state and the sleep interval.
 * With USE_THINGSPEAK = true the data is also published to ThingSpeak over MQTT.
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <PubSubClient.h>

// ======================= SIMULATION SETTINGS =======================
const float SIM_SPEED = 20.0;        // time acceleration: 1 real second = 20 model seconds

// Storage. 1 F supercapacitor -> 1.0 ;  20 x 1000 uF electrolytics -> 0.02
const float C_FARAD = 1.0;
const float V_START = 4.3;           // initial storage voltage

// Solar panel: power at 100 % light (indoors near a window ~10 mW)
const float P_PANEL_MAX_W = 0.010;
const float P_SLEEP_W     = 0.0003;  // sleep consumption (board + low-Iq regulator)

// Energy of one "wake + measure + transmit" cycle
//   ESP-NOW ~0.06 J,  direct Wi-Fi ~0.5 J
const float E_CYCLE_J = 0.06;

const float V_ZENER  = 5.1;          // the Zener clamps the storage voltage
const float V_FULL   = 4.9;          // nearly full
const float V_CUTOFF = 3.6;          // below this the regulator cannot hold 3.3 V
const float SAFETY   = 1.3;

const uint32_t MIN_INTERVAL_S = 15;
const uint32_t MAX_INTERVAL_S = 600;         // 10 min in the simulation (= 30 real seconds); 1800 in the real node
const uint32_t FIRST_BOOT_S   = 60;

// ======================= ThingSpeak (optional) =======================
const bool  USE_THINGSPEAK = false;  // true = publish to ThingSpeak
const char* MQTT_HOST      = "mqtt3.thingspeak.com";
const char* MQTT_CLIENT_ID = "YOUR_CLIENT_ID";
const char* MQTT_USER      = "YOUR_USERNAME";
const char* MQTT_PASS      = "YOUR_MQTT_PASSWORD";
const char* CHANNEL_ID     = "YOUR_CHANNEL_ID";

// ======================= PINS =======================
const int PIN_LIGHT = 34;   // "LIGHT" knob
const int PIN_WATER = 35;   // "WATER" knob
const int PIN_LED   = 2;    // blinks on every transmission

// ======================= MODEL STATE =======================
float energyJ;              // energy in the storage, J
float simTimeS = 0;         // model time, s
float sleepLeftS = 5;       // time left until the next wake-up
float vAfterLast = 0;       // voltage when the node fell asleep
uint32_t lastSleepS = 0;
float pHarvestAvg = -1;     // estimated harvested power, W
uint32_t nextS = 0;
uint32_t wakeups = 0, sent = 0, brownouts = 0;
int lightPct = 0, waterPct = 0;

enum State { ST_HARVEST_ONLY, ST_LOW, ST_NORMAL, ST_RICH };
const char* STATE_NAMES[] = { "HARVEST_ONLY", "LOW", "NORMAL", "RICH" };
State st = ST_HARVEST_ONLY;

Adafruit_SSD1306 oled(128, 64, &Wire, -1);
WiFiClient net;
PubSubClient mqtt(net);
uint32_t lastPublishMs = 0;
uint32_t ledOffAt = 0;

// ======================= STORAGE PHYSICS =======================
float vcap()                 { return sqrt(2.0f * energyJ / C_FARAD); }           // V = sqrt(2E/C)
float energyAt(float v)      { return 0.5f * C_FARAD * v * v; }                   // E = 1/2 C V^2
float usableEnergyJ(float v) { return v <= V_CUTOFF ? 0 : energyAt(v) - energyAt(V_CUTOFF); }

void addEnergy(float joules) {
  energyJ += joules;
  if (energyJ < 0) energyJ = 0;
  if (energyJ > energyAt(V_ZENER)) energyJ = energyAt(V_ZENER);   // surplus is burnt in the Zener
}

// ======================= PUBLISH =======================
void publishThingSpeak(float v) {
  if (!USE_THINGSPEAK || WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastPublishMs < 16000) return;                   // ThingSpeak: max one update per 15 s
  if (!mqtt.connected() && !mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)) return;
  char topic[64], payload[200];
  snprintf(topic, sizeof(topic), "channels/%s/publish", CHANNEL_ID);
  snprintf(payload, sizeof(payload),
           "field1=%d&field2=%.3f&field3=%lu&field4=%.2f&field5=%d&field6=%d&status=%s",
           waterPct, v, (unsigned long)nextS, pHarvestAvg * 1000.0f, (int)st, lightPct, STATE_NAMES[st]);
  if (mqtt.publish(topic, payload)) {
    lastPublishMs = millis();
    Serial.printf("   -> ThingSpeak: %s\n", payload);
  }
}

// ============== WAKE-UP (the same steps as setup() in the real node) ==============
void wakeUp() {
  wakeups++;
  float vNow = vcap();

  // 1. Harvested power: how much the storage charged during the last sleep
  if (lastSleepS > 0) {
    float eGain = energyAt(vNow) - energyAt(vAfterLast);
    float pMeas = eGain / lastSleepS;
    if (pMeas < 0) pMeas = 0;
    pHarvestAvg = (pHarvestAvg < 0) ? pMeas : 0.7f * pHarvestAvg + 0.3f * pMeas;
  }

  // 2. State machine
  float eUsable = usableEnergyJ(vNow);
  if (eUsable < 1.5f * E_CYCLE_J)      st = ST_HARVEST_ONLY;
  else if (vNow >= V_FULL)             st = ST_RICH;
  else if (eUsable < 3.0f * E_CYCLE_J) st = ST_LOW;
  else                                 st = ST_NORMAL;

  // 3. Sleep interval
  if (st == ST_HARVEST_ONLY)      nextS = (pHarvestAvg > 0) ? 120 : 600;
  else if (st == ST_RICH)         nextS = MIN_INTERVAL_S;
  else if (pHarvestAvg < 0)       nextS = FIRST_BOOT_S;              // harvest power not known yet
  else {
    // key formula; if the panel delivers (almost) nothing (dark) sleep as long as allowed
    float s = (pHarvestAvg > 0.00001f) ? SAFETY * E_CYCLE_J / pHarvestAvg : (float)MAX_INTERVAL_S;
    if (st == ST_LOW) s *= 2;
    nextS = (uint32_t)constrain(s, (float)MIN_INTERVAL_S, (float)MAX_INTERVAL_S);
  }

  // 4. Work: water measurement + transmission (costs energy)
  if (st != ST_HARVEST_ONLY) {
    if (eUsable < E_CYCLE_J) {                      // not enough -> the real board would brown out
      brownouts++;
      addEnergy(-eUsable);
      nextS = MAX_INTERVAL_S / 4;
    } else {
      waterPct = analogRead(PIN_WATER) * 100 / 4095;
      addEnergy(-E_CYCLE_J);
      sent++;
      digitalWrite(PIN_LED, HIGH);
      ledOffAt = millis() + 150;
      publishThingSpeak(vNow);
    }
  }

  Serial.printf("t=%6.0fs  light=%3d%%  V=%.3f V  harvest=%5.2f mW  %-12s  sleep=%4lu s  water=%3d%%  sent=%lu\n",
                simTimeS, lightPct, vNow, (pHarvestAvg < 0 ? 0 : pHarvestAvg) * 1000.0f,
                STATE_NAMES[st], (unsigned long)nextS, waterPct, (unsigned long)sent);

  // 5. Go to sleep
  vAfterLast = vcap();
  lastSleepS = nextS;
  sleepLeftS = nextS;
}

// ======================= DISPLAY =======================
void drawScreen() {
  float v = vcap();
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  int m = (int)(simTimeS / 60), s = (int)simTimeS % 60;
  oled.printf("ENERGY NODE  %3d:%02d\n", m, s);
  oled.printf("Light %3d%%  P=%.1fmW\n", lightPct, P_PANEL_MAX_W * lightPct / 100.0f * 1000.0f);
  oled.printf("Vcap  %.2f V\n", v);
  // charge bar from V_CUTOFF to V_ZENER
  int w = (int)constrain((v - V_CUTOFF) / (V_ZENER - V_CUTOFF) * 126.0f, 0.0f, 126.0f);
  oled.drawRect(0, 26, 128, 7, SSD1306_WHITE);
  oled.fillRect(1, 27, w, 5, SSD1306_WHITE);
  oled.setCursor(0, 36);
  oled.printf("%s\n", STATE_NAMES[st]);
  oled.printf("Sleep %lus  wake %ds\n", (unsigned long)nextS, (int)sleepLeftS);
  oled.printf("Water %d%%  Sent %lu", waterPct, (unsigned long)sent);
  oled.display();
}

// ======================= SETUP / LOOP =======================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  energyJ = energyAt(V_START);

  Wire.begin(21, 22);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println("OLED not found");

  if (USE_THINGSPEAK) {
    WiFi.begin("Wokwi-GUEST", "", 6);                // the Wi-Fi network provided by the Wokwi simulator
    Serial.print("Wi-Fi");
    while (WiFi.status() != WL_CONNECTED) { delay(200); Serial.print("."); }
    Serial.println(" OK");
    mqtt.setServer(MQTT_HOST, 1883);
  }
  Serial.println("Turn the LIGHT knob and watch the node adapt its sleep interval.");
}

void loop() {
  static uint32_t lastTick = millis(), lastDraw = 0;
  if (millis() - lastTick < 50) return;              // model step every 50 ms
  float dt = (millis() - lastTick) / 1000.0f * SIM_SPEED;
  lastTick = millis();
  simTimeS += dt;

  // the panel charges the storage, sleep current slowly discharges it
  lightPct = analogRead(PIN_LIGHT) * 100 / 4095;
  float pPanel = P_PANEL_MAX_W * lightPct / 100.0f;
  addEnergy((pPanel - P_SLEEP_W) * dt);

  sleepLeftS -= dt;
  if (sleepLeftS <= 0) wakeUp();

  if (millis() > ledOffAt) digitalWrite(PIN_LED, LOW);
  if (millis() - lastDraw > 200) { drawScreen(); lastDraw = millis(); }
  if (USE_THINGSPEAK) mqtt.loop();
}
