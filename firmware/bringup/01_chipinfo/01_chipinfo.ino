// Bring-up step 1: chip info. No wiring needed.
//
// One cable, into the DevKit's "UART" port: flash and monitor on the same COM.
// The report goes out both UART0 and USB CDC, so whichever port is plugged in
// shows it:
//   .\scripts\build.ps1 firmware\bringup\01_chipinfo -Port COMx
//   arduino-cli monitor -p COMx -c baudrate=115200
//
// Prints a full report every 5 s, plus a live line whenever the pot or a key
// pin changes (all deck pins are read as inputs; nothing is driven).
//
// What to check:
//   - Flash 8 MB, PSRAM ~2 MB (N8R2). PSRAM 0 or a boot loop => the
//     FlashSize= / PSRAM= options in build.ps1 don't match the board.
//   - At boot the on-board RGB LED blinks: RED = it's on GPIO48 (DevKitC v1.0),
//     BLUE = GPIO38 (v1.1). Either way it's fine; tell me which you saw.
//   - Keys should all show '.' (internal pull-ups, nothing wired). Touch a
//     jumper from a key pin to GND and it should flip to 'X'.
//   - Pot reading floats until the pot is wired; ignore it for now.

#include <Arduino.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_mac.h>
#include <deck_pins.h>
#include <deck_config.h>

static void out(const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  Serial.print(buf);    // USB CDC
  Serial0.print(buf);   // UART0 (bridge port)
}

static const char *reset_reason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "int watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "other watchdog";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_USB:      return "usb";
    case ESP_RST_EXT:      return "external pin";
    default:               return "other";
  }
}

static void identify_onboard_led() {
  // rgbLedWrite drives a single WS2812 via RMT. Release the pins afterwards.
  rgbLedWrite(48, 40, 0, 0);  delay(700); rgbLedWrite(48, 0, 0, 0);
  rgbLedWrite(38, 0, 0, 40);  delay(700); rgbLedWrite(38, 0, 0, 0);
}

static uint16_t key_bits() {
  uint16_t b = 0;
  for (int i = 0; i < deck::NUM_KEYS; i++)
    if (digitalRead(deck::KEY_PINS[i]) == LOW) b |= 1u << i;   // 1 = pressed
  return b;
}

static void report() {
  esp_chip_info_t ci;
  esp_chip_info(&ci);
  uint32_t jedec = 0;
  esp_flash_read_id(nullptr, &jedec);
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);

  out("\n==== stream deck 01_chipinfo ====\n");
  out("chip      %s rev v%u.%u, %u cores, %u MHz\n", ESP.getChipModel(),
      ci.revision / 100, ci.revision % 100, ci.cores, ESP.getCpuFreqMHz());
  out("features  %s%s%s\n", (ci.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi " : "",
      (ci.features & CHIP_FEATURE_BLE) ? "BLE " : "",
      (ci.features & CHIP_FEATURE_EMB_PSRAM) ? "embedded-PSRAM " : "");
  out("flash     %lu MB, JEDEC %06lX, %lu MHz\n", (unsigned long)(ESP.getFlashChipSize() >> 20),
      (unsigned long)jedec, (unsigned long)(ESP.getFlashChipSpeed() / 1000000));
  out("psram     %lu KB total, %lu KB free\n", (unsigned long)(ESP.getPsramSize() >> 10),
      (unsigned long)(ESP.getFreePsram() >> 10));
  out("heap      %lu KB total, %lu KB free\n", (unsigned long)(ESP.getHeapSize() >> 10),
      (unsigned long)(ESP.getFreeHeap() >> 10));
  out("mac       %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  out("software  arduino-esp32 %s, IDF %s\n", ESP_ARDUINO_VERSION_STR, ESP.getSdkVersion());
  out("reset     %s, uptime %lu s\n", reset_reason(), (unsigned long)(millis() / 1000));
  out("pins      pot GPIO%d, led GPIO%d, lcd D0-7 GPIO%d-%d RS%d WR%d WAIT%d RST%d\n",
      deck::POT_WIPER, deck::LED_DIN, deck::LCD_D[0], deck::LCD_D[7],
      deck::LCD_RS, deck::LCD_WR, deck::LCD_WAIT, deck::LCD_RST);
  out("keys     ");
  for (int i = 0; i < deck::NUM_KEYS; i++) out(" K%d=GPIO%d", i + 1, deck::KEY_PINS[i]);
  out("\n");
}

void setup() {
  Serial.begin(115200);
  Serial0.begin(115200);
  identify_onboard_led();

  for (int i = 0; i < deck::NUM_KEYS; i++) pinMode(deck::KEY_PINS[i], INPUT_PULLUP);
  analogReadResolution(12);
  analogSetPinAttenuation(deck::POT_WIPER, ADC_11db);   // full 0-3.3 V range

  // PSRAM sanity: allocate and walk a 1 MB block.
  if (psramFound()) {
    const size_t n = 1 << 20;
    uint32_t *p = (uint32_t *)ps_malloc(n);
    bool ok = p != nullptr;
    for (size_t i = 0; ok && i < n / 4; i++) p[i] = i * 2654435761u;
    for (size_t i = 0; ok && i < n / 4; i++) ok = p[i] == i * 2654435761u;
    free(p);
    out("psram 1 MB walk: %s\n", ok ? "OK" : "FAIL");
  } else {
    out("psram NOT FOUND\n");
  }
}

void loop() {
  static uint32_t last_report = 0, last_live = 0;
  static int last_mv = -1000;
  static uint16_t last_keys = 0xFFFF;

  if (millis() - last_report >= 5000) {
    last_report = millis();
    report();
  }
  if (millis() - last_live >= 100) {
    last_live = millis();
    int mv = analogReadMilliVolts(deck::POT_WIPER);
    uint16_t k = key_bits();
    if (abs(mv - last_mv) > 100 || k != last_keys) {
      last_mv = mv;
      last_keys = k;
      out("live  pot %4d mV  keys", mv);
      for (int i = 0; i < deck::NUM_KEYS; i++) out(" %c", (k >> i) & 1 ? 'X' : '.');
      out("\n");
    }
  }
}
