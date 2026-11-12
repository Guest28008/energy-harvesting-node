# Wokwi simulation

A software model of the node for [wokwi.com](https://wokwi.com). The panel and the storage capacitor are simulated in code; the sleep-schedule logic is the same as in `firmware/node_espnow_water`.

## Files

| File | Purpose |
|---|---|
| `sketch.ino` | model + state machine + OLED display |
| `diagram.json` | parts and wiring: ESP32, two potentiometers, SSD1306 OLED, LED |
| `libraries.txt` | Adafruit SSD1306, Adafruit GFX Library, PubSubClient |

## How to run

1. Open <https://wokwi.com/projects/new/esp32>.
2. Replace the contents of the `sketch.ino` tab with `sketch.ino` from this folder.
3. Replace the contents of the `diagram.json` tab with `diagram.json` from this folder.
4. Click the small arrow next to the tabs → *New file…* → name it `libraries.txt` and paste the three library names (one per line).
5. Press the green **Play** button. Free builds are queued, so the first start can take a minute or two.

## What to try

| LIGHT knob | Expected behaviour |
|---|---|
| ~50 % | NORMAL, sleep ≈ 16 s |
| 100 % | storage charges to 4.9 V → RICH, sleep 15 s |
| 0 % | the sleep interval grows step by step (20, 29, 42, 60, 85 … s) while the voltage barely falls |

Experiments: set `C_FARAD` to `0.02` (20 × 1000 µF electrolytics) or `E_CYCLE_J` to `0.5` (direct Wi-Fi instead of ESP-NOW) and compare.

The node only "sees" the light when it wakes up, exactly like the real hardware, so after turning the knob wait for the next wake-up or restart the simulation.

To publish to ThingSpeak from the simulation set `USE_THINGSPEAK = true` and fill in the MQTT values (fields: 1 water %, 2 storage voltage, 3 sleep interval, 4 harvested power mW, 5 state, 6 light %).
