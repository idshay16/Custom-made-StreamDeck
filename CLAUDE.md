# DIY stream deck (ESP32-S3) — firmware + web UI

Hobby embedded project. I build the hardware and write the firmware myself as a learning project, so don't suggest off-the-shelf stream deck firmware. Hardware is solder-free: breadboard, dupont wires, pre-wired parts. Parts and enclosure are in `build_guide.html` (Project 1; gitignored copy of `Downloads\build_guide.html`). The KVM in `..\Solenoid-based-video-switching-KVM` is a separate project; they don't interact.

Status (2026-11-16): parts mostly in hand, 3D-printed enclosure ordered → all bring-up happens on the open breadboard. MX switches can't sit in a breadboard; use the 6×6 tact switches as stand-ins until the lid exists.

## Hardware
- **ESP32-S3 DevKitC-1 clone, N8R2** — NOT the N16R8 the guide lists. esptool: ESP32-S3 (QFN56) rev v0.2, embedded 2 MB quad PSRAM, 8 MB GigaDevice flash (`c8 4017`), MAC `b0:81:84:80:5f:94`. Ports labelled "COM" (CH343 USB-serial → UART0, flashing) and "USB" (native → PC through the USB-C panel mount: HID + CDC console). Quad PSRAM leaves GPIO35–37 free. **No spare board**: the guide's "build + spare" pair was not bought, so double-check power wiring before plugging in.
- **4.3" TFT 480×272, RA8875**, 8080 8-bit parallel (LT043C, 3.3 V, no touch).
- **12 keys**, one GPIO each, INPUT_PULLUP, no matrix. Layout 3 rows × 4, K1 top-left, row-major.
- **12 WS2812B** under translucent keycaps (10 in hand). USB-powered → hard current cap.
- **10k pot** = deck brightness (screen backlight + key LEDs). Nothing else.

## Decisions
- **No PC-side app.** Everything runs on the ESP32:
  - hotkeys / media / text / macros: USB HID keyboard + consumer control
  - launch app / URL: HID Win+R macro (risk: breaks under a non-US input language — fallback is a chord bound to a `.lnk` shortcut key)
  - Discord: global keybinds on F13–F24 combos, optimistic toggle state
  - OBS: obs-websocket v5 over Wi-Fi, real state feedback
  - Govee H6056: LAN UDP API
- **Config via a web UI served by the ESP32** at `http://streamdeck.local` (LittleFS). Basic-auth protected; OTA with app rollback.
- Secrets (Wi-Fi, OBS password, web password) in NVS, never in `config.json`.

## Pins and config
- **All pin assignments live in `firmware/libraries/deck_common/src/deck_pins.h`.** It is the wiring spec; `static_assert`s reject flash/PSRAM/USB/UART0/strapping pins, duplicates, and a non-ADC1 pot pin. Never hard-code a GPIO elsewhere.
- Tunables in `deck_config.h`, overridable with `-D`.

## Firmware architecture (planned)
FreeRTOS tasks. Core 1: input (1 kHz keys + pot), actions (key → HID < 5 ms), UI (PSRAM sprite → dirty-rect push), LEDs. Core 0: Wi-Fi, async web server, OBS ws client, Govee UDP. One model owner; others talk via queues.
Display: LovyanGFX `Bus_Parallel8` + `Panel_RA8875` subclass with 480×272 init timings (stock table is 800×480). LovyanGFX's RA8875 busy-wait uses only the WAIT pin, so CS# → GND and RD# → 3V3.
Full plan: `C:\Users\test\.claude\plans\this-is-the-folder-wise-crown.md`.

## Toolchain
**Primary: Arduino IDE 2.** `scripts/build.ps1` wraps arduino-cli (IDE-bundled copy, not on PATH). Same cores and libraries.
- Core **esp32:esp32 3.3.12**.
- FQBN (built by build.ps1): `esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=8M,PSRAM=enabled,PartitionScheme=<custom|default_8MB>,...`
  Partition is `custom` when the sketch folder has `partitions.csv`. Main firmware plan for 8 MB: nvs + otadata + app0 3 MB + app1 3 MB + littlefs ~1.9 MB.
- Memory budget (2 MB PSRAM): full-screen sprite ~261 KB + icon cache for the current page only (~230 KB); decode a page's PNGs on page switch.
- IDE Tools menu must match: USB Mode = USB-OTG (TinyUSB), USB CDC On Boot = Enabled, Flash Size = 8MB, PSRAM = QSPI PSRAM, Partition = 8M with spiffs (3MB APP/1.5MB SPIFFS) or Custom.
- `deck_common` reaches the IDE through a junction: `Documents\Arduino\libraries\deck_common` → `firmware\libraries\deck_common`.
  Recreate: `New-Item -ItemType Junction -Path "$env:USERPROFILE\Documents\Arduino\libraries\deck_common" -Target "<repo>\firmware\libraries\deck_common"`
- Libraries: LovyanGFX 1.3.0, ArduinoJson 7.4.3, Adafruit_NeoPixel 1.15.5, **ESP Async WebServer 3.12.1** + **Async TCP 3.5.0** (ESP32Async), **WebSockets 2.7.2** (links2004). Installed pinned with `--no-deps`; don't let the library manager bump Async TCP past 3.5.0 without rebuilding and testing.
- **Never include Adafruit_TinyUSB in these sketches**: the ESP32 core ships its own TinyUSB (`USB.h`, `USBHIDKeyboard`, ...). The user library is for the KVM.

```powershell
.\scripts\build.ps1 firmware\bringup\01_chipinfo               # compile
.\scripts\build.ps1 firmware\bringup\01_chipinfo -Port COM5    # upload via UART port
.\scripts\build.ps1 firmware\streamdeck -Port COM9 -Usb        # upload via native USB port
.\scripts\build.ps1 -ListPorts
```
**One USB cable only.** `01_chipinfo` uses the UART port. From `02_usb_hid` on, the cable lives in the native "USB" port (same as the final build): upload with `-Usb`. If the sketch is crashed or not enumerating: hold BOOT, tap RST → ROM bootloader appears as a COM port → upload without `-Usb` → tap RST.

## Layout
```
firmware/libraries/deck_common/   deck_pins.h, deck_config.h, deck_display.h (RA8875 480x272 LovyanGFX driver)
firmware/bringup/NN_name/         one small test sketch per step (01-04)
firmware/streamdeck/              main firmware: app.h (module map), one .cpp per module, partitions.csv,
                                  default_config.h (config schema + default), webui_assets.h (GENERATED)
webui/                            web UI source (vanilla HTML/JS/CSS), embedded gzipped into the firmware
scripts/build.ps1                 arduino-cli wrapper (runs embed_webui.ps1 for firmware\streamdeck)
scripts/embed_webui.ps1           webui/ → firmware/streamdeck/webui_assets.h
tools/mock_deck.js                fake deck API for UI work without hardware: node tools/mock_deck.js → localhost:8080
tools/ui_test.js                  end-to-end UI test in headless Edge: node tools/ui_test.js
docs/test_plan.md                 hands-on test checklist for the hardware
```
- Web UI is embedded in the firmware (not a LittleFS image), so OTA updates firmware + UI together. LittleFS holds only `/config.json` and `/icons/*.png`.
- After editing `webui/`: run `node tools/ui_test.js`, then rebuild (build.ps1 regenerates the header). Commit the regenerated `webui_assets.h`.

## Git
- Commit directly to `main`, one commit per completed step (only after it compiles / tests pass). Remote: `origin` (GitHub).

## Build order (status 2026-11-16)
0. Repo + toolchain, `01_chipinfo` — done, flashed (found N8R2).
1. `02_usb_hid` — done, flashed, enumerates as keyboard + consumer + CDC. Keyboard-layout test still open.
2. `03_keys_leds` — written, compiles; needs wiring.
3. `04_display` — written, compiles; needs wiring + interface-jumper check.
4–9. Main firmware v0.1.0 (config, tiles, actions, Wi-Fi, web UI, OTA, OBS, Govee, Discord binds) — written, compiles, web UI passes `tools/ui_test.js` against the mock. **Not yet flashed** (board was unplugged). See docs/test_plan.md.
10. Soak, power measurement, final assembly — after enclosure arrives.

## Working style
- Build incrementally, one subsystem per step, each with a small test sketch I can flash and check.
- Explain embedded-specific choices briefly. I'm a software engineer with ESP32/Arduino experience.
- Keep build and flash commands runnable with arduino-cli.
