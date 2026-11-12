/*
 * GATEWAY — second ESP32, powered from USB
 * Receives ESP-NOW packets from the node and publishes them to ThingSpeak over MQTT.
 *
 * Library (Arduino IDE -> Library Manager): PubSubClient (Nick O'Leary)
 *
 * After start-up open the Serial Monitor (115200) and read the line
 * "Wi-Fi channel: N" — put that number into WIFI_CHANNEL in the NODE sketch.
 */

#include <WiFi.h>
#include <esp_now.h>
#include <PubSubClient.h>
#include "esp_arduino_version.h"

// ============================ SETTINGS ============================
const char* WIFI_SSID = "YOUR_WIFI";
const char* WIFI_PASS = "YOUR_PASSWORD";

// ThingSpeak: Devices -> MQTT -> Add a new device (gives these three values)
const char* MQTT_HOST      = "mqtt3.thingspeak.com";
const int   MQTT_PORT      = 1883;
const char* MQTT_CLIENT_ID = "YOUR_CLIENT_ID";
const char* MQTT_USER      = "YOUR_USERNAME";
const char* MQTT_PASS      = "YOUR_MQTT_PASSWORD";
const char* CHANNEL_ID     = "YOUR_CHANNEL_ID";

// Water sensor calibration: raw value when the sensor is fully submerged
const int WATER_FULL = 2500;

const uint32_t PUBLISH_MIN_MS = 16000;   // ThingSpeak accepts at most one update per 15 s

// ====================== PACKET (same as in the node!) ======================
typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint32_t boot;
  uint16_t water;
  float    vcap;
  float    harvest_mW;
  uint32_t nextS;
  uint8_t  state;
} Packet;

const uint32_t MAGIC = 0xEC0DA7A1;
const char* STATE_NAMES[] = { "HARVEST_ONLY", "LOW", "NORMAL", "RICH" };

// ============================ RECEIVE ============================
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
Packet lastPkt;
volatile bool hasNew = false;
volatile uint32_t packetsReceived = 0;

// The receive callback has a different signature in ESP32 Arduino core 2.x and 3.x
#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onReceive(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
#else
void onReceive(const uint8_t* mac, const uint8_t* data, int len) {
#endif
  if (len != sizeof(Packet)) return;
  Packet p;
  memcpy(&p, data, sizeof(p));
  if (p.magic != MAGIC) return;          // not our packet
  portENTER_CRITICAL(&mux);              // protect the shared packet from loop()
  lastPkt = p;
  hasNew = true;
  packetsReceived++;
  portEXIT_CRITICAL(&mux);
}

// ============================ MQTT ============================
WiFiClient net;
PubSubClient mqtt(net);
uint32_t lastPublish = 0;

void ensureMqtt() {
  if (mqtt.connected()) return;
  Serial.print("Connecting to ThingSpeak MQTT... ");
  if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)) Serial.println("OK");
  else { Serial.printf("error %d\n", mqtt.state()); delay(2000); }
}

void publish(const Packet& p, uint32_t count) {
  int waterPct = constrain(map(p.water, 0, WATER_FULL, 0, 100), 0, 100);
  uint8_t st = p.state < 4 ? p.state : 0;

  char topic[64];
  snprintf(topic, sizeof(topic), "channels/%s/publish", CHANNEL_ID);

  // field1 water raw, field2 water %, field3 storage voltage, field4 sleep interval,
  // field5 harvested power mW, field6 state, field7 node wake-up counter, field8 packets received
  char payload[256];
  snprintf(payload, sizeof(payload),
           "field1=%u&field2=%d&field3=%.3f&field4=%lu&field5=%.2f&field6=%u&field7=%lu&field8=%lu&status=%s",
           p.water, waterPct, p.vcap, (unsigned long)p.nextS, p.harvest_mW,
           st, (unsigned long)p.boot, (unsigned long)count, STATE_NAMES[st]);

  bool ok = mqtt.publish(topic, payload);
  Serial.printf("-> ThingSpeak %s: %s\n", ok ? "OK" : "FAILED", payload);
}

// ============================ SETUP / LOOP ============================
void setup() {
  Serial.begin(115200);
  delay(500);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.println(" OK");
  Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf(">>> Wi-Fi channel: %d  (put this number into WIFI_CHANNEL of the node)\n", WiFi.channel());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed!");
    return;
  }
  esp_now_register_recv_cb(onReceive);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  Serial.println("Waiting for packets from the node...");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {      // Wi-Fi lost -> reconnect
    WiFi.reconnect();
    delay(1000);
    return;
  }
  ensureMqtt();
  mqtt.loop();

  // Publish the newest packet, respecting the ThingSpeak rate limit
  if (hasNew && millis() - lastPublish >= PUBLISH_MIN_MS) {
    Packet p;
    uint32_t count;
    portENTER_CRITICAL(&mux);
    p = lastPkt;
    count = packetsReceived;
    hasNew = false;
    portEXIT_CRITICAL(&mux);

    Serial.printf("Packet #%lu: water=%u  V=%.3f  harvest=%.2f mW  %s  next in %lu s\n",
                  (unsigned long)p.boot, p.water, p.vcap, p.harvest_mW,
                  STATE_NAMES[p.state < 4 ? p.state : 0], (unsigned long)p.nextS);
    publish(p, count);
    lastPublish = millis();
  }
}
