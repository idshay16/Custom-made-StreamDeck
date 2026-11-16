// govee.cpp — Govee LAN API (UDP). Enable "LAN Control" for the device in
// the Govee Home app first (the H6056 supports it; the H6054 doesn't).
//
//   scan:    multicast 239.255.255.250:4001 {"msg":{"cmd":"scan",...}}
//   replies: unicast to us on port 4002 (scan results + devStatus)
//   control: unicast to <device ip>:4003  turn / brightness / colorwc / devStatus
//
// Runs inside the net task. Other tasks queue commands.
#include "app.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <mutex>
#include <vector>

struct GoveeDev {
  String ip, sku, id;
  bool on = false;
};

static WiFiUDP g_udp;
static bool g_listening = false;
static std::mutex g_mu;
static std::vector<GoveeDev> g_devs;
static QueueHandle_t cmd_q() {   // char* "ip|json"; thread-safe lazy init
  static QueueHandle_t q = xQueueCreate(8, sizeof(char *));
  return q;
}
static uint32_t g_last_poll = 0, g_last_scan = 0;
static bool g_scan_req = true;

static void send_to(const String &ip, const String &json) {
  IPAddress a;
  if (!a.fromString(ip)) return;
  g_udp.beginPacket(a, 4003);
  g_udp.print(json);
  g_udp.endPacket();
}

static void do_scan() {
  g_udp.beginPacket(IPAddress(239, 255, 255, 250), 4001);
  g_udp.print("{\"msg\":{\"cmd\":\"scan\",\"data\":{\"account_topic\":\"reserve\"}}}");
  g_udp.endPacket();
  g_last_scan = millis();
}

static void query_status() {
  std::vector<String> ips;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto &d : g_devs) ips.push_back(d.ip);
  }
  for (auto &ip : ips) send_to(ip, "{\"msg\":{\"cmd\":\"devStatus\",\"data\":{}}}");
}

static void on_packet() {
  int n = g_udp.parsePacket();
  if (n <= 0) return;
  char buf[512];
  int len = g_udp.read(buf, sizeof buf - 1);
  if (len <= 0) return;
  buf[len] = 0;
  String from = g_udp.remoteIP().toString();
  JsonDocument d;
  if (deserializeJson(d, buf)) return;
  const char *cmd = d["msg"]["cmd"] | "";
  JsonObjectConst data = d["msg"]["data"];
  if (!strcmp(cmd, "scan")) {
    GoveeDev dev{data["ip"] | from.c_str(), data["sku"] | "", data["device"] | ""};
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto &x : g_devs)
      if (x.ip == dev.ip) return;
    g_devs.push_back(dev);
    LOG("govee: found %s at %s", dev.sku.c_str(), dev.ip.c_str());
  } else if (!strcmp(cmd, "devStatus")) {
    bool on = (data["onOff"] | 0) == 1;
    state::set(String("govee.on:") + from, on);
  }
}

void govee::loop() {
  if (!g_listening) {
    g_listening = g_udp.begin(4002);
    if (!g_listening) return;
  }
  on_packet();
  if (g_scan_req || millis() - g_last_scan > 300000) {   // rescan every 5 min
    g_scan_req = false;
    do_scan();
  }
  if (millis() - g_last_poll > 30000) {
    g_last_poll = millis();
    query_status();
  }
  char *m;
  while (xQueueReceive(cmd_q(), &m, 0) == pdTRUE) {
    char *bar = strchr(m, '|');
    if (bar) {
      *bar = 0;
      send_to(m, bar + 1);
    }
    free(m);
  }
}

void govee::scan() { g_scan_req = true; }

void govee::fill_devices(JsonArray arr) {
  std::lock_guard<std::mutex> lk(g_mu);
  for (auto &d : g_devs) {
    JsonObject o = arr.add<JsonObject>();
    o["ip"] = d.ip;
    o["sku"] = d.sku;
    o["id"] = d.id;
  }
}

static void queue_cmd(const String &ip, const String &json) {
  char *p = strdup((ip + "|" + json).c_str());
  if (p && xQueueSend(cmd_q(), &p, 0) != pdTRUE) free(p);
}

void govee::action(JsonObjectConst a) {
  String ip = a["device"] | "";
  String key = "govee.on:" + ip;
  const char *op = a["op"] | "toggle";
  JsonDocument d;
  if (!strcmp(op, "toggle") || !strcmp(op, "on") || !strcmp(op, "off")) {
    bool on = !strcmp(op, "on") ? true : !strcmp(op, "off") ? false : !state::get(key);
    d["msg"]["cmd"] = "turn";
    d["msg"]["data"]["value"] = on ? 1 : 0;
    state::set(key, on);   // optimistic; corrected by the next devStatus poll
  } else if (!strcmp(op, "brightness")) {
    d["msg"]["cmd"] = "brightness";
    d["msg"]["data"]["value"] = constrain(a["value"] | 100, 1, 100);
  } else if (!strcmp(op, "color")) {
    uint32_t c = strtoul((a["color"] | "#ffffff") + 1, nullptr, 16);
    d["msg"]["cmd"] = "colorwc";
    d["msg"]["data"]["color"]["r"] = (c >> 16) & 0xFF;
    d["msg"]["data"]["color"]["g"] = (c >> 8) & 0xFF;
    d["msg"]["data"]["color"]["b"] = c & 0xFF;
    d["msg"]["data"]["colorTemInKelvin"] = 0;
  } else {
    return;
  }
  String s;
  serializeJson(d, s);
  queue_cmd(ip, s);
}
