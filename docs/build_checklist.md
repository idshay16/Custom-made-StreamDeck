# Build checklist: what's left for you

Everything you still need to do, in order. The stream deck has **one board, the
ESP32-S3** (the Picos belong to the KVM project). Detailed test steps live in
[test_plan.md](test_plan.md); the wiring spec of record is
[deck_pins.h](../firmware/libraries/deck_common/src/deck_pins.h).

**Safety rules for every step**
- Unplug the USB cable before changing any wire.
- **5V never touches a 3V3 pin or the 3V3 rail.** There is no spare ESP32.
- Plug back in only after checking the wires you just added against the tables below.

---

## 0. Parts: check what you have, order the rest

Tick what's in hand; anything unticked goes on the next order (we review this together).

| Part | Need | Notes |
|---|---|---|
| ESP32-S3 DevKitC-1 (N8R2) | 1 | ✅ have. Spare (~₪15) recommended, optional |
| 4.3" RA8875 display (LT043C, 3.3 V) | 1 | ✅ ordered in Order 2. Check its connector + interface jumpers (section 4) |
| MX switches (Outemu brown) | 12 | check arrived. Look for a clear window on top (LEDs shine through it) |
| Translucent keycaps | 12 | check arrived |
| 10k potentiometer | 1 | check arrived |
| WS2812B pre-wired nodes | 12 | have 10 → order 2 more (or another 10-string) |
| Breadboard 400-point | 1 | ✅ have (2 bought; one is for the KVM) |
| Dupont female-female | ~24 | ✅ have (10 cm). Fine for keys; display wants 20 cm |
| Dupont **male-female 20 cm** | ~15 | ❌ order |
| Dupont **male-male 20 cm** | ~8 | ❌ order |
| Solid-core wire 24 AWG (roll) | 1 | ❌ order: key GND bus (+ key wiring if option B) |
| Heat-shrink assortment | 1 | ❌ order: insulates key-pin connections |
| 74AHCT125 (DIP) | 1 | optional: only if the LEDs flicker (3.3 V data into 5 V LEDs) |
| USB-C panel-mount extension (round, screw) | 1 | check (guide: "still to buy") |
| M3 × 8 countersunk screws | 4 | check (guide: "still to buy") |
| Enclosure print (base + lid + stand) | 1 | ordered 2026-11-16 |

---

## 1. Software: what to flash, in what order

Only one board. Each step replaces the previous firmware. Commands run from the repo root in PowerShell.

| # | Sketch | When | Cable port | Command | Status |
|---|---|---|---|---|---|
| 1 | `01_chipinfo` | board check | COM | `.\scripts\build.ps1 firmware\bringup\01_chipinfo -Port COMx` | ✅ done |
| 2 | `02_usb_hid` | USB keyboard check | USB | `.\scripts\build.ps1 firmware\bringup\02_usb_hid -Port COMx -Usb` | ✅ done (layout test open) |
| 3 | **`streamdeck`** (main) | now, no wiring needed | USB | `.\scripts\build.ps1 firmware\streamdeck -Port COMx -Usb` | ⏳ test_plan.md §1–8 |
| 4 | `03_keys_leds` | after section 3 wiring | USB | `.\scripts\build.ps1 firmware\bringup\03_keys_leds -Port COMx -Usb` | ⏳ |
| 5 | `04_display` | after section 4 wiring | USB | `.\scripts\build.ps1 firmware\bringup\04_display -Port COMx -Usb` | ⏳ |
| 6 | **`streamdeck`** (main) | everything wired | USB | same as #3 | ⏳ final |
| 7 | updates | enclosure closed | Wi-Fi | web UI → Settings → Firmware update | later |

- Find the port: `.\scripts\build.ps1 -ListPorts`.
- If an upload can't find the board: hold **BOOT**, tap **RST**, then upload again **without** `-Usb`, then tap RST.
- Console (all sketches): `& "C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe" monitor -p COMx`

---

## 2. Pinout

### ESP32 header (components up, USB ports toward you)
Standard DevKitC-1 order. **Check against the labels printed on your board**; if the left side starts GND/TX/RX, the sides are swapped.

| # | Left pin | Connects to | Right pin | Connects to |
|---|---|---|---|---|
| 1 | 3V3 | breadboard **3V3 rail** | GND | breadboard **GND rail** |
| 2 | 3V3 | display VCC (if 3.3 V) | TX (43) | — keep free |
| 3 | RST | — | RX (44) | — keep free |
| 4 | 4 | display **DB0** | 1 | pot **wiper** (middle leg) |
| 5 | 5 | display **DB1** | 2 | LED string **DIN** |
| 6 | 6 | display **DB2** | 42 | key **K10** |
| 7 | 7 | display **DB3** | 41 | key **K9** |
| 8 | 15 | key **K2** | 40 | key **K8** |
| 9 | 16 | key **K3** | 39 | key **K7** |
| 10 | 17 | key **K4** | 38 | key **K6** |
| 11 | 18 | key **K5** | 37 | spare |
| 12 | 8 | display **DB4** | 36 | spare |
| 13 | 3 | spare (boot pin) | 35 | key **K1** |
| 14 | 46 | — keep free | 0 | — BOOT button |
| 15 | 9 | display **DB5** | 45 | — keep free |
| 16 | 10 | display **DB6** | 48 | key **K12** |
| 17 | 11 | display **DB7** | 47 | key **K11** |
| 18 | 12 | display **RS** | 21 | display **RESET#** |
| 19 | 13 | display **WR#** | 20 | — USB |
| 20 | 14 | display **WAIT#** | 19 | — USB |
| 21 | 5V | breadboard **5V rail** | GND | key **GND bus** |
| 22 | GND | breadboard **GND rail** | GND | display **GND** |

### Keys (front view)
```
K1  K2  K3  K4
K5  K6  K7  K8
K9  K10 K11 K12
```
Each switch: pin 1 → its GPIO (table above), pin 2 → GND bus. No resistors, no diodes.

### Display (match names to the display board's silkscreen / seller pinout)
| Display signal | Goes to |
|---|---|
| DB0–DB7 | ESP32 GPIO 4, 5, 6, 7, 8, 9, 10, 11 |
| RS (also called DC / A0) | GPIO 12 |
| WR# | GPIO 13 |
| WAIT# | GPIO 14 |
| RESET# | GPIO 21 |
| CS# | **GND** rail |
| RD# | **3V3** rail |
| VCC / VDD | 3V3 (confirm on the board first) |
| Backlight supply (if a separate pin) | 3V3 or 5V per the board's spec |
| GND | GND |

### LED string
| Wire (usual colors) | Goes to |
|---|---|
| +5V (red) | **5V** rail |
| GND (white/black) | GND rail |
| DIN (green) | ESP32 GPIO 2 |
Use the **input** end (arrow on the node points away from it). Daisy-chain order should follow K1→K12 row by row; if it doesn't, change `LED_INDEX` in deck_pins.h.

### Pot
Outer leg → 3V3 rail, other outer leg → GND rail, middle leg → GPIO 1. (Swapping the outer legs only reverses the direction; there's a "Reverse knob" setting.)

### What the breadboard is for
The ESP32 does **not** sit on the breadboard (it would cover every hole). The breadboard only holds the pot and distributes power:
```
left rails : +  = 5V   (from ESP32 5V)     → LED string +5V
             −  = GND
right rails: +  = 3V3  (from ESP32 3V3)    → pot, display RD#, display VCC
             −  = GND  (from ESP32 GND)    → pot, display CS#, LED GND
add one male-male wire left − ↔ right − so both GND rails are joined
```

---

## 3. Assembly on the bench (before the enclosure)

Do these in order; each ends with a test.

### 3.1 Main firmware on the bare board (no wiring)
- [ ] Flash `streamdeck` (section 1, #3).
- [ ] Run [test_plan.md](test_plan.md) §1–8: console key presses, **Hebrew-layout launch test**, Wi-Fi + web UI, OBS, Discord, Govee, OTA.

### 3.2 Power rails + pot (needs male-female + male-male wires)
- [ ] ESP32 3V3 → right + rail. ESP32 GND → right − rail. ESP32 5V → left + rail. ESP32 GND (2nd) → left − rail. Male-male: left − ↔ right −.
- [ ] Pot into the breadboard (legs in three different rows), wired per section 2.
- [ ] **Before plugging in:** confirm nothing connects the left + (5V) rail to the right + (3V3) rail.

### 3.3 Keys
1. Pick the key-wiring method after testing one switch:
   - **A:** slide the plastic sleeve off one end of a female-female wire, push the bare metal clip onto the switch pin, squeeze lightly if loose, cover with heat-shrink or tape.
   - **B:** wrap stripped solid-core wire 3–4 turns around the pin, cover with heat-shrink.
2. For each key: signal wire from switch pin 1 to its ESP32 GPIO.
3. GND bus: one bare solid-core wire wrapped once around every switch's pin 2, ending in an ESP32 GND pin (or the GND rail).
4. Before the lid exists you can test with switches loose on the desk.

### 3.4 LEDs
- [ ] Connect the 10-node string (section 2). Order the 2 extra; they chain on the end.

### 3.5 Test keys + LEDs + pot
- [ ] Flash `03_keys_leds` (section 1, #4). Checks are in the sketch header: one down/up per press, the lit LED is under the pressed key, the pot sweeps 0→255, no LED flicker (flicker → 74AHCT125).

### 3.6 Display
1. **Check before wiring:** interface set to **8080 8-bit** (jumpers/resistors labeled SPI/8080/PS/J1–J8), and whether VCC/backlight want 3.3 V or 5 V. Send me a photo if unsure.
2. Wire per section 2 (female-female straight from ESP32 to the display header where possible; 20 cm wires recommended).
3. Flash `04_display` (section 1, #5). Expect: "display detected: yes", color bars R-G-B-W, full yellow border, ~15–40 fps, backlight sweep.
4. Report results; I tune timings if the image is shifted or cropped.

### 3.7 Everything together
- [ ] Flash `streamdeck` again. Tiles + status bar show, pressed tile highlights, LEDs follow tiles, knob dims screen + LEDs.

---

## 4. Enclosure assembly (when the print arrives)

Order matters: wire switches before the lid is closed, and test on the bench first.

1. Check the print: switch snaps into a key hole and holds (MX clips want a 1.5 mm plate); brass inserts present in the base bosses.
2. Snap the 12 switches into the lid from the top, all facing the same way.
3. Wire the switches (3.3 method) and the GND bus, labeling each signal wire K1–K12.
4. LEDs: tape each node under its switch (light window side), keeping the chain order.
5. Screen: board behind the window, held with double-sided foam tape; check it's straight before pressing down.
6. Pot through its 7 mm hole, nut on top; knob on.
7. USB-C panel mount through the 13 mm floor hole, nut tight; inner end into the ESP32 **USB** port (not COM).
8. ESP32 + breadboard on the floor with double-sided tape (ESP32 under the keys or the screen; see the height budget in the guide).
9. Power on with the lid open. Re-run 3.7.
10. Close the lid, 4× M3 × 8 countersunk screws. Keycaps on.
11. From now on, update firmware through the web UI (Settings → Firmware update).

---

## 5. Open items (decided together)

- [ ] Key-wiring method A vs B (test one switch).
- [ ] Do the switches have a light window? If not: LEDs become a glow strip instead of per-key backlight.
- [ ] Display connector type + interface jumpers + supply voltage.
- [ ] Hebrew-layout launch test result (test_plan.md §3).
- [ ] Order list from section 0.
