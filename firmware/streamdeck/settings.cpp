// settings.cpp — device settings and secrets in NVS (Preferences).
#include "app.h"
#include <Preferences.h>
#include <mutex>

static std::mutex g_mu;
static Settings g_s;

void settings::load() {
  Preferences p;
  p.begin("deck", true);
  Settings s;
  s.wifi_ssid = p.getString("wifi_ssid", "");
  s.wifi_pass = p.getString("wifi_pass", "");
  s.hostname = p.getString("hostname", s.hostname);
  s.web_pass = p.getString("web_pass", "");
  s.obs_host = p.getString("obs_host", "");
  s.obs_port = p.getUShort("obs_port", s.obs_port);
  s.obs_pass = p.getString("obs_pass", "");
  s.tz = p.getString("tz", s.tz);
  s.pot_enabled = p.getBool("pot", s.pot_enabled);
  s.pot_invert = p.getBool("pot_inv", s.pot_invert);
  s.fixed_brightness = p.getUChar("bright", s.fixed_brightness);
  p.end();
  std::lock_guard<std::mutex> lk(g_mu);
  g_s = s;
}

void settings::save() {
  Settings s = get();
  Preferences p;
  p.begin("deck", false);
  p.putString("wifi_ssid", s.wifi_ssid);
  p.putString("wifi_pass", s.wifi_pass);
  p.putString("hostname", s.hostname);
  p.putString("web_pass", s.web_pass);
  p.putString("obs_host", s.obs_host);
  p.putUShort("obs_port", s.obs_port);
  p.putString("obs_pass", s.obs_pass);
  p.putString("tz", s.tz);
  p.putBool("pot", s.pot_enabled);
  p.putBool("pot_inv", s.pot_invert);
  p.putUChar("bright", s.fixed_brightness);
  p.end();
}

Settings settings::get() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_s;
}

void settings::set(const Settings &s) {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_s = s;
  }
  save();
}
