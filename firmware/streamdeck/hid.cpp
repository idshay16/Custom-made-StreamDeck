// hid.cpp — USB HID keyboard + consumer control (the ESP32 core's TinyUSB).
//
// Only the actions task calls into this module (TinyUSB HID is not
// re-entrant), except mounted(), which just reads a flag.
//
// Typing text: HID sends key POSITIONS, not characters. With a Hebrew (or any
// non-US) layout active, "notepad" typed as positions comes out as Hebrew
// letters. "altcode" mode types every ASCII character as Alt + its decimal
// code on the numeric keypad (e.g. 'n' = Alt+1,1,0). Windows translates that
// to the character regardless of the active layout. ~4 HID reports/char,
// so ~25 ms/char: fine for commands and URLs. "direct" mode is fast but
// assumes the US layout.
#include "app.h"
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <USBHIDConsumerControl.h>
#include <atomic>

static USBHIDKeyboard Keyboard;
static USBHIDConsumerControl Consumer;
static std::atomic<uint8_t> g_leds{0x01};   // assume NumLock on until the host says otherwise
static uint16_t g_media_held = 0;

static const uint8_t U_ENTER = 0x28, U_NUMLOCK = 0x53, U_LALT = 0xE2, U_LGUI = 0xE3;

static void on_kbd_event(void *, esp_event_base_t, int32_t id, void *data) {
  if (id == ARDUINO_USB_HID_KEYBOARD_LED_EVENT)
    g_leds = ((arduino_usb_hid_keyboard_event_data_t *)data)->leds;
}

void hid::begin() {
  USB.productName("DIY StreamDeck");
  USB.manufacturerName("ishay");
  Keyboard.onEvent(ARDUINO_USB_HID_KEYBOARD_LED_EVENT, on_kbd_event);
  Keyboard.begin();
  Consumer.begin();
  USB.begin();
}

bool hid::mounted() { return (bool)USB; }

// ------------------------------------------------------------ key names
struct Named {
  const char *name;
  uint8_t usage;
};
static const Named NAMES[] = {
  {"ctrl", 0xE0},   {"control", 0xE0}, {"shift", 0xE1},  {"alt", 0xE2},   {"win", 0xE3},
  {"gui", 0xE3},    {"meta", 0xE3},    {"cmd", 0xE3},    {"rctrl", 0xE4}, {"rshift", 0xE5},
  {"ralt", 0xE6},   {"altgr", 0xE6},   {"rwin", 0xE7},   {"enter", 0x28}, {"return", 0x28},
  {"esc", 0x29},    {"escape", 0x29},  {"backspace", 0x2A}, {"tab", 0x2B}, {"space", 0x2C},
  {"minus", 0x2D},  {"equal", 0x2E},   {"capslock", 0x39}, {"printscreen", 0x46}, {"prtsc", 0x46},
  {"scrolllock", 0x47}, {"pause", 0x48}, {"insert", 0x49}, {"ins", 0x49},  {"home", 0x4A},
  {"pageup", 0x4B}, {"pgup", 0x4B},    {"delete", 0x4C}, {"del", 0x4C},   {"end", 0x4D},
  {"pagedown", 0x4E}, {"pgdn", 0x4E},  {"right", 0x4F},  {"left", 0x50},  {"down", 0x51},
  {"up", 0x52},     {"numlock", 0x53}, {"menu", 0x65},   {"app", 0x65},   {"plus", 0x2E},
};

// Single printable character -> usage (US positions).
static uint8_t char_usage(char c) {
  c = tolower((unsigned char)c);
  if (c >= 'a' && c <= 'z') return 0x04 + (c - 'a');
  if (c >= '1' && c <= '9') return 0x1E + (c - '1');
  if (c == '0') return 0x27;
  switch (c) {
    case '-': return 0x2D;  case '=': return 0x2E;  case '[': return 0x2F;
    case ']': return 0x30;  case '\\': return 0x31; case ';': return 0x33;
    case '\'': return 0x34; case '`': return 0x35;  case ',': return 0x36;
    case '.': return 0x37;  case '/': return 0x38;
  }
  return 0;
}

static uint8_t name_usage(const String &tok) {
  if (tok.length() == 1) return char_usage(tok[0]);
  if ((tok[0] == 'f') && tok.length() <= 3) {
    int n = tok.substring(1).toInt();
    if (n >= 1 && n <= 12) return 0x3A + (n - 1);
    if (n >= 13 && n <= 24) return 0x68 + (n - 13);
  }
  for (auto &nm : NAMES)
    if (tok == nm.name) return nm.usage;
  return 0;
}

// "ctrl+shift+m" -> usages[] (modifiers first). mods = modifier bitmask.
bool hid::parse_combo(const char *s, uint8_t *usages, uint8_t &mods, int &n) {
  n = 0;
  mods = 0;
  if (!s) return false;
  String str(s);
  str.toLowerCase();
  str.replace(" ", "");
  int start = 0;
  while (start <= (int)str.length()) {
    int plus = str.indexOf('+', start);
    // allow a literal '+' key as the last token ("ctrl++")
    if (plus == start && plus == (int)str.length() - 1) plus = -1;
    String tok = plus < 0 ? str.substring(start) : str.substring(start, plus);
    if (tok.isEmpty()) return false;
    uint8_t u = name_usage(tok);
    if (!u || n >= 8) return false;
    if (u >= 0xE0) mods |= 1 << (u - 0xE0);
    usages[n++] = u;
    if (plus < 0) break;
    start = plus + 1;
  }
  return n > 0;
}

void hid::combo_down(const char *s) {
  uint8_t u[8], m;
  int n;
  if (!parse_combo(s, u, m, n)) return;
  for (int i = 0; i < n; i++) Keyboard.pressRaw(u[i]);
}

void hid::combo_up(const char *s) {
  uint8_t u[8], m;
  int n;
  if (!parse_combo(s, u, m, n)) return;
  for (int i = n - 1; i >= 0; i--) Keyboard.releaseRaw(u[i]);
}

void hid::combo_tap(const char *s) {
  combo_down(s);
  delay(15);
  combo_up(s);
}

void hid::release_all() {
  Keyboard.releaseAll();
  if (g_media_held) media_up();
}

// ------------------------------------------------------------ media
static uint16_t media_code(const char *name) {
  if (!strcmp(name, "play_pause")) return CONSUMER_CONTROL_PLAY_PAUSE;
  if (!strcmp(name, "next")) return CONSUMER_CONTROL_SCAN_NEXT;
  if (!strcmp(name, "prev")) return CONSUMER_CONTROL_SCAN_PREVIOUS;
  if (!strcmp(name, "stop")) return CONSUMER_CONTROL_STOP;
  if (!strcmp(name, "mute")) return CONSUMER_CONTROL_MUTE;
  if (!strcmp(name, "vol_up")) return CONSUMER_CONTROL_VOLUME_INCREMENT;
  if (!strcmp(name, "vol_down")) return CONSUMER_CONTROL_VOLUME_DECREMENT;
  return 0;
}

bool hid::media_tap(const char *name) {
  uint16_t c = media_code(name);
  if (!c) return false;
  Consumer.press(c);
  delay(15);
  Consumer.release();
  return true;
}

void hid::media_down(const char *name) {
  uint16_t c = media_code(name);
  if (!c) return;
  Consumer.press(c);
  g_media_held = c;
}

void hid::media_up() {
  Consumer.release();
  g_media_held = 0;
}

// ------------------------------------------------------------ text
static void tap_raw(uint8_t u) {
  Keyboard.pressRaw(u);
  delay(4);
  Keyboard.releaseRaw(u);
  delay(4);
}

static void ensure_numlock() {
  if (g_leds & 0x01) return;
  tap_raw(U_NUMLOCK);
  delay(30);
}

static void altcode_char(uint8_t c) {
  // Keypad usages: 1..9 = 0x59..0x61, 0 = 0x62
  char digits[4];
  snprintf(digits, sizeof digits, "%u", c);
  Keyboard.pressRaw(U_LALT);
  delay(4);
  for (char *d = digits; *d; d++) tap_raw(*d == '0' ? 0x62 : 0x59 + (*d - '1'));
  Keyboard.releaseRaw(U_LALT);
  delay(4);
}

void hid::type_text(const char *s, bool altcode) {
  if (!s) return;
  if (!altcode) {
    Keyboard.print(s);
    return;
  }
  ensure_numlock();
  for (const uint8_t *p = (const uint8_t *)s; *p; p++) {
    if (*p == '\n') tap_raw(U_ENTER);
    else if (*p == '\t') tap_raw(0x2B);
    else if (*p >= 32 && *p < 127) altcode_char(*p);
    // non-ASCII (e.g. UTF-8 Hebrew) is skipped: Alt codes can't reach it reliably
  }
}

void hid::run_dialog(const char *cmd, bool altcode) {
  Keyboard.pressRaw(U_LGUI);
  Keyboard.pressRaw(char_usage('r'));
  delay(30);
  Keyboard.releaseAll();
  delay(450);   // let the Run dialog open and take focus
  type_text(cmd, altcode);
  delay(40);
  tap_raw(U_ENTER);
}
