// state.cpp — runtime state shared by all tasks. One mutex, one version counter.
#include "app.h"
#include <map>
#include <mutex>
#include <atomic>
#include <WiFi.h>

static std::mutex g_mu;
static std::atomic<uint32_t> g_version{1};
static int g_page = 0;
static uint16_t g_pressed = 0;
static uint8_t g_bright = 128;
static std::map<String, bool> g_bools;
static std::map<String, String> g_strs;

void state::init() {}

uint32_t state::version() { return g_version.load(); }

void state::changed() {
  g_version++;
  render::wake();
}

int state::page() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_page;
}

void state::set_page(int p) {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_page == p) return;
    g_page = p;
  }
  render::invalidate();
  changed();
}

uint16_t state::pressed() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_pressed;
}

void state::set_pressed(int key, bool down) {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    uint16_t m = down ? (g_pressed | (1u << key)) : (g_pressed & ~(1u << key));
    if (m == g_pressed) return;
    g_pressed = m;
  }
  changed();
}

uint8_t state::brightness() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_bright;
}

void state::set_brightness(uint8_t b) {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_bright == b) return;
    g_bright = b;
  }
  changed();
}

bool state::get(const String &name) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (name.startsWith("obs.scene:")) {
    auto it = g_strs.find("obs.scene");
    return it != g_strs.end() && it->second == name.substring(10);
  }
  auto it = g_bools.find(name);
  return it != g_bools.end() && it->second;
}

void state::set(const String &name, bool v) {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_bools.find(name);
    if (it != g_bools.end() && it->second == v) return;
    g_bools[name] = v;
  }
  changed();
}

bool state::flip(const String &name) {
  bool v;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    v = !g_bools[name];
    g_bools[name] = v;
  }
  changed();
  return v;
}

String state::str(const String &name) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_strs.find(name);
  return it == g_strs.end() ? String() : it->second;
}

void state::set_str(const String &name, const String &v) {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_strs.find(name);
    if (it != g_strs.end() && it->second == v) return;
    g_strs[name] = v;
  }
  changed();
}

void state::fill_status(JsonDocument &d) {
  ConfigPtr cfg = config::get();
  {
    std::lock_guard<std::mutex> lk(g_mu);
    d["page"] = g_page;
    d["pressed"] = g_pressed;
    d["brightness"] = g_bright;
    JsonObject b = d["states"].to<JsonObject>();
    for (auto &kv : g_bools) b[kv.first] = kv.second;
    JsonObject s = d["strings"].to<JsonObject>();
    for (auto &kv : g_strs) s[kv.first] = kv.second;
  }
  d["page_name"] = cfg->page(d["page"].as<int>())["name"];
  d["pages"] = cfg->pageCount();
  JsonObject w = d["wifi"].to<JsonObject>();
  w["connected"] = net::connected();
  w["ap"] = net::ap_mode();
  w["ip"] = net::ip();
  w["ssid"] = WiFi.SSID();
  w["rssi"] = net::connected() ? WiFi.RSSI() : 0;
  d["obs"] = get("obs.connected");
  d["usb"] = hid::mounted();
  d["display"] = render::display_present();
  d["uptime"] = millis() / 1000;
  d["heap"] = ESP.getFreeHeap();
  d["psram"] = ESP.getFreePsram();
  d["fw"] = DECK_FW_VERSION;
}
