// net.cpp — Wi-Fi manager + network task, on core 0.
//
// Station mode with the saved credentials. If there are none, or the router
// doesn't answer within 20 s, a setup access point comes up alongside
// ("StreamDeck-Setup", password "streamdeck") with a captive DNS, so a phone
// joining it lands on the web UI's settings page. Station keeps retrying in
// the background; once it connects, the setup AP shuts down.
#include "app.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <atomic>

static const char *AP_SSID = "StreamDeck-Setup";
static const char *AP_PASS = "streamdeck";

static std::atomic<bool> g_connected{false}, g_ap{false};
static DNSServer g_dns;
static String g_ip;
static bool g_mdns = false;

bool net::connected() { return g_connected; }
bool net::ap_mode() { return g_ap; }
String net::ip() { return g_ip; }

static void start_ap() {
  if (g_ap) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  g_dns.start(53, "*", WiFi.softAPIP());
  g_ap = true;
  LOG("net: setup AP up: join \"%s\" (password \"%s\"), open http://%s", AP_SSID, AP_PASS,
      WiFi.softAPIP().toString().c_str());
  state::changed();
}

static void stop_ap() {
  if (!g_ap) return;
  g_dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  g_ap = false;
  LOG("net: setup AP down");
  state::changed();
}

static void on_connected() {
  Settings s = settings::get();
  g_ip = WiFi.localIP().toString();
  g_connected = true;
  LOG("net: connected to \"%s\", ip %s, rssi %d", WiFi.SSID().c_str(), g_ip.c_str(), WiFi.RSSI());
  if (!g_mdns && MDNS.begin(s.hostname.c_str())) {
    MDNS.addService("http", "tcp", 80);
    g_mdns = true;
    LOG("net: http://%s.local", s.hostname.c_str());
  }
  configTzTime(s.tz.c_str(), "pool.ntp.org", "time.google.com");
  stop_ap();
  obs::reconfigure();
  state::changed();
}

static void net_task(void *) {
  Settings s = settings::get();
  WiFi.persistent(false);
  WiFi.setHostname(s.hostname.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // lower latency for OBS/Govee; costs ~20 mA
  uint32_t started = millis();
  if (s.wifi_ssid.length()) {
    LOG("net: connecting to \"%s\"", s.wifi_ssid.c_str());
    WiFi.begin(s.wifi_ssid.c_str(), s.wifi_pass.c_str());
  } else {
    LOG("net: no Wi-Fi credentials (console: wifi <ssid> <password>)");
    start_ap();
  }

  uint32_t last_retry = millis();
  for (;;) {
    bool up = WiFi.status() == WL_CONNECTED;
    if (up && !g_connected) on_connected();
    if (!up && g_connected) {
      g_connected = false;
      g_ip = "";
      LOG("net: Wi-Fi lost");
      state::changed();
    }
    if (!up && !g_ap && millis() - started > 20000) start_ap();
    if (!up && s.wifi_ssid.length() && millis() - last_retry > 15000) {
      last_retry = millis();
      WiFi.disconnect();
      WiFi.begin(s.wifi_ssid.c_str(), s.wifi_pass.c_str());
    }
    if (g_ap) {
      g_dns.processNextRequest();
      if (g_ap && !g_connected) g_ip = WiFi.softAPIP().toString();
    }
    if (g_connected) {
      obs::loop();
      govee::loop();
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void net::start() {
  xTaskCreatePinnedToCore(net_task, "net", 8192, nullptr, 2, nullptr, 0);
}
