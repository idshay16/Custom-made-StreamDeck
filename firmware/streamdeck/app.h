// app.h — shared declarations for the stream deck firmware.
//
// Task layout (see CLAUDE.md):
//   core 1: input (1 kHz keys + pot) -> actions (HID, page nav, net requests)
//           render (display + LEDs), Arduino loop (console, web push)
//   core 0: Wi-Fi stack, net task (Wi-Fi manager, captive DNS, OBS, Govee),
//           async web server
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <memory>
#include <deck_pins.h>
#include <deck_config.h>

#define DECK_FW_VERSION "0.1.0"

// ---------------------------------------------------------------- logging
void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) log_line(__VA_ARGS__)
String log_tail();   // recent log lines (for /api/log)

// ---------------------------------------------------------------- config
// The config is a JSON document (see default_config.h for the schema). It is
// parsed once, validated, and then shared read-only between tasks through a
// shared_ptr: a new config swaps the pointer, readers keep the old one alive
// until they're done.
struct PsramAllocator : ArduinoJson::Allocator {
  void *allocate(size_t n) override;
  void deallocate(void *p) override;
  void *reallocate(void *p, size_t n) override;
};

class Config {
 public:
  JsonDocument doc;
  Config();
  int pageCount() const;
  JsonObjectConst page(int i) const;
  JsonObjectConst key(int page, int k) const;   // null object if empty
  int pageIndex(const char *name) const;        // -1 if not found
  bool altcodeTyping() const;                   // "typing": "altcode" (default) | "direct"
};
using ConfigPtr = std::shared_ptr<const Config>;

namespace config {
void init();                                     // mount LittleFS, load or create /config.json
ConfigPtr get();
bool apply(const char *json, size_t len, String &err);   // validate + save + swap
bool validate(const JsonDocument &doc, String &err);
String json();                                   // current config, serialized
void reset_default();
}  // namespace config

// ---------------------------------------------------------------- settings (NVS)
// Secrets and device settings. Never part of config.json.
struct Settings {
  String wifi_ssid, wifi_pass;
  String hostname = "streamdeck";
  String web_pass;
  String obs_host;
  uint16_t obs_port = 4455;
  String obs_pass;
  String tz = "IST-2IDT,M3.4.4/26,M10.5.0";   // POSIX TZ; default Israel
  bool pot_enabled = true;
  bool pot_invert = false;
  uint8_t fixed_brightness = 160;            // used when the pot is disabled
};
namespace settings {
void load();
void save();
Settings get();                 // copy
void set(const Settings &s);    // replace + save
}  // namespace settings

// ---------------------------------------------------------------- runtime state
namespace state {
void init();
uint32_t version();             // bumps on every change
void changed();                 // bump + wake render task

int page();
void set_page(int p);

uint16_t pressed();
void set_pressed(int key, bool down);

uint8_t brightness();
void set_brightness(uint8_t b);

// Named boolean states that drive key "on" colors:
//   obs.record, obs.stream, obs.mute:<input>, obs.scene:<name> (derived),
//   govee.on:<ip>, discord.mute, discord.deafen, any custom name used by "flip".
bool get(const String &name);
void set(const String &name, bool v);
bool flip(const String &name);

String str(const String &name);                     // e.g. "obs.scene"
void set_str(const String &name, const String &v);

void fill_status(JsonDocument &d);                  // everything, for web/console
}  // namespace state

// ---------------------------------------------------------------- input
enum EventType : uint8_t { EV_DOWN, EV_UP, EV_LONG, EV_RUN };
struct InputEvent {
  EventType type;
  uint8_t key;     // 0..11
  char *json;      // EV_RUN only: malloc'd action JSON, freed by the actions task
};
namespace input {
void start();
void inject(EventType t, int key);    // simulated press from console / web
}  // namespace input

// ---------------------------------------------------------------- HID
namespace hid {
void begin();
bool mounted();
bool parse_combo(const char *s, uint8_t *usages, uint8_t &mods, int &n);   // "ctrl+shift+m"
void combo_down(const char *s);
void combo_up(const char *s);
void combo_tap(const char *s);
bool media_tap(const char *name);
void media_down(const char *name);
void media_up();
void type_text(const char *s, bool altcode);
void run_dialog(const char *cmd, bool altcode);   // Win+R, type, Enter
void release_all();
}  // namespace hid

// ---------------------------------------------------------------- actions
namespace actions {
void start();
bool execute(const String &action_json);   // queue one action (web "test" button); runs on the actions task
}  // namespace actions

// ---------------------------------------------------------------- render (display + LEDs)
namespace render {
void start();
bool display_present();
void invalidate();    // config/page changed: redraw everything, reload icons
void wake();          // something changed: re-evaluate tiles + LEDs now
}  // namespace render

// ---------------------------------------------------------------- network
namespace net {
void start();
bool connected();
bool ap_mode();
String ip();
}  // namespace net

namespace web {
void start();
void loop();          // push status over websocket when state changes
}  // namespace web

namespace obs {
void loop();
void reconfigure();
bool request(const char *requestType, JsonObjectConst data);   // queue a request
void action(JsonObjectConst a);                                // "obs" key action
}  // namespace obs

namespace govee {
void loop();
void scan();
void action(JsonObjectConst a);
void fill_devices(JsonArray arr);
}  // namespace govee

namespace console {
void loop();
}  // namespace console
