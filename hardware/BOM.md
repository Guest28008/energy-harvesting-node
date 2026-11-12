# Bill of materials

| # | Part | Qty | Notes |
|---|---|---|---|
| 1 | ESP32 dev board | 2 | One node, one gateway. For the node a board with low deep-sleep current is best; many DevKit clones draw several mA in sleep because of the power LED and the USB chip. |
| 2 | Solar panel 5–6 V, 1–2 W | 1–2 | Indoors it delivers only a few mW. Two in parallel help. Leads must be soldered. |
| 3 | Storage capacitors | see notes | **As built:** electrolytic 1000 µF 16 V / 25 V in parallel (the more, the better; 20+ recommended). **Better:** 2 × 2.7 V 5 F supercapacitors in series (2.5 F, 5.4 V). |
| 4 | Schottky diode 1N5819 | 1 | Between panel + and the storage, stripe towards the storage. |
| 5 | Zener diode 1N4733A, 5.1 V 1 W | 1 | Across the storage, stripe to +. Clamps the open-circuit panel voltage (> 6 V). |
| 6 | 3.3 V LDO regulator | 1 | Low quiescent current and ≥ 250 mA: HT7333, ME6211, XC6220. An AMS1117 works electrically but draws ~5 mA itself, more than the panel gives indoors. |
| 7 | Electrolytic capacitor 1000 µF ≥ 10 V | 1 | On the 3.3 V rail, supplies the radio current peaks. |
| 8 | Resistor 1 MΩ | 2 | Voltage divider VCAP → GPIO34. |
| 9 | Ceramic capacitor 100 nF (marked 104) | 1 | From the divider midpoint to GND, stabilises the ADC reading. |
| 10 | Water level sensor module (S, +, −) | 1 | `+` is powered from GPIO25. |
| 11 | Breadboard, jumper wires, USB data cable | — | |
| 12 | Multimeter | 1 | Needed for checking the rails before connecting the ESP32. |
| 13 | INA219 current sensor module | 1 (optional) | For measuring the energy of one cycle and the sleep current. |

## Notes on the regulator

| Regulator | Quiescent current | Dropout | Usable storage window | Suitable for harvesting |
|---|---|---|---|---|
| AMS1117-3.3 | ~5 mA | ~1.1 V | 5.1 V → 4.5 V | no (bench testing only) |
| HT7333 | ~4 µA | ~0.1–0.3 V | 5.1 V → 3.6 V | yes (add a large capacitor on the output, 250 mA max) |
| ME6211 / XC6220 | ~40 µA / ~8 µA | ~0.1–0.2 V | 5.1 V → 3.6 V | yes |

AMS1117-3.3 pinout (SOT-223, text facing you, pins down): 1 = GND, 2 = VOUT (also the tab), 3 = VIN.
HT7333 pinout (SOT-89): 1 = GND, 2 = VIN (also the tab), 3 = VOUT.
Always check the datasheet of the exact part before wiring.
