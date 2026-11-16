// DIY stream deck — main firmware. ESP32-S3 N8R2.
//
// Build + flash (native USB port):
//   .\scripts\embed_webui.ps1                       (only after editing webui/)
//   .\scripts\build.ps1 firmware\streamdeck -Port COMx -Usb
//
// First boot: no Wi-Fi credentials -> setup AP "StreamDeck-Setup" (password
// "streamdeck"). Or on the console: wifi "<ssid>" "<password>".
// Then open http://streamdeck.local
//
// Module map: app.h. Wiring: deck_pins.h. Config schema: default_config.h.
#include "app.h"
#include <esp_ota_ops.h>

// Arduino calls this at boot: returning true means "don't mark this app
// valid yet". If a freshly OTA'd image crashes before we mark it valid in
// loop(), the bootloader rolls back to the previous one.
extern "C" bool verifyRollbackLater() { return true; }

void setup() {
  Serial.setTxTimeoutMs(0);   // never block on logging when no console is open
  Serial.begin(115200);
  hid::begin();               // USB first: the PC sees the keyboard ASAP
  LOG("DIY StreamDeck %s, %s rev %u, PSRAM %u KB", DECK_FW_VERSION, ESP.getChipModel(), ESP.getChipRevision(),
      (unsigned)(ESP.getPsramSize() / 1024));

  settings::load();
  state::init();
  config::init();             // mounts LittleFS, loads /config.json
  render::start();            // detects display, starts the render task
  input::start();
  actions::start();
  net::start();
  web::start();
  render::invalidate();
}

void loop() {
  console::loop();
  web::loop();

  static bool marked = false;
  if (!marked && millis() > 30000) {
    marked = true;
    const esp_partition_t *p = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(p, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
      esp_ota_mark_app_valid_cancel_rollback();
      LOG("ota: new firmware marked valid");
    }
  }
  delay(5);
}
