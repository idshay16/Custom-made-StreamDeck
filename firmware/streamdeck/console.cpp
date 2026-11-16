// console.cpp — logging + serial console (USB CDC).
//
// Commands (type "help"):
//   status | config | config reset | ls | log
//   wifi <ssid> <password>    (use quotes for spaces: wifi "My Net" "pass word")
//   wifi clear
//   webpass <password>
//   obs <host> [port] [password]  |  obs off
//   press <1-12> | long <1-12> | page <n|name>
//   pot on|off|invert          |  bright <0-255> (fixed brightness when pot off)
//   govee scan
//   reboot
#include "app.h"
#include <LittleFS.h>
#include <mutex>
#include <vector>

static std::mutex g_log_mu;
static char g_ring[4096];
static size_t g_ring_pos = 0;
static bool g_ring_wrapped = false;

void log_line(const char *fmt, ...) {
  char buf[256];
  int n = snprintf(buf, sizeof buf, "[%7.3f] ", millis() / 1000.0f);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf + n, sizeof buf - n - 2, fmt, ap);
  va_end(ap);
  strcat(buf, "\n");
  std::lock_guard<std::mutex> lk(g_log_mu);
  Serial.print(buf);
  for (const char *p = buf; *p; p++) {
    g_ring[g_ring_pos++] = *p;
    if (g_ring_pos == sizeof g_ring) {
      g_ring_pos = 0;
      g_ring_wrapped = true;
    }
  }
}

String log_tail() {
  std::lock_guard<std::mutex> lk(g_log_mu);
  String s;
  if (g_ring_wrapped) {
    s.reserve(sizeof g_ring);
    for (size_t i = g_ring_pos; i < sizeof g_ring; i++) s += g_ring[i];
    int nl = s.indexOf('\n');   // drop the partial first line
    if (nl >= 0) s.remove(0, nl + 1);
  }
  for (size_t i = 0; i < g_ring_pos; i++) s += g_ring[i];
  return s;
}

// Split a line into words; "double quotes" group words.
static std::vector<String> split(const String &line) {
  std::vector<String> out;
  String cur;
  bool q = false, any = false;
  for (char c : line) {
    if (c == '"') {
      q = !q;
      any = true;
    } else if (c == ' ' && !q) {
      if (any) out.push_back(cur);
      cur = "";
      any = false;
    } else {
      cur += c;
      any = true;
    }
  }
  if (any) out.push_back(cur);
  return out;
}

static void print_status() {
  JsonDocument d;
  state::fill_status(d);
  serializeJsonPretty(d, Serial);
  Serial.println();
  Settings s = settings::get();
  Serial.printf("settings: ssid=\"%s\" host=%s obs=%s:%u web_pass=%s pot=%s%s\n", s.wifi_ssid.c_str(),
                s.hostname.c_str(), s.obs_host.length() ? s.obs_host.c_str() : "(off)", s.obs_port,
                s.web_pass.length() ? "set" : "NOT SET", s.pot_enabled ? "on" : "off", s.pot_invert ? " inverted" : "");
}

static int key_arg(const std::vector<String> &a) {
  if (a.size() < 2) return -1;
  int k = a[1].toInt();
  return (k >= 1 && k <= deck::NUM_KEYS) ? k - 1 : -1;
}

static void run_line(const String &line) {
  auto a = split(line);
  if (a.empty()) return;
  String c = a[0];
  c.toLowerCase();
  Settings s = settings::get();

  if (c == "help") {
    Serial.println("status | config [reset] | ls | log | wifi <ssid> <pass> | wifi clear | webpass <pw>\n"
                   "obs <host> [port] [pass] | obs off | press <1-12> | long <1-12> | page <n|name>\n"
                   "pot on|off|invert | bright <0-255> | govee scan | reboot");
  } else if (c == "status") {
    print_status();
  } else if (c == "config") {
    if (a.size() > 1 && a[1] == "reset") {
      config::reset_default();
      Serial.println("config reset to default");
    } else {
      serializeJsonPretty(config::get()->doc, Serial);
      Serial.println();
    }
  } else if (c == "ls") {
    for (const char *dirname : {"/", "/icons"}) {
      File dir = LittleFS.open(dirname);
      for (File f = dir.openNextFile(); f; f = dir.openNextFile())
        if (!f.isDirectory()) Serial.printf("  %s/%s  %u\n", dirname[1] ? dirname : "", f.name(), (unsigned)f.size());
    }
    Serial.printf("  used %u / %u KB\n", (unsigned)(LittleFS.usedBytes() / 1024), (unsigned)(LittleFS.totalBytes() / 1024));
  } else if (c == "log") {
    Serial.print(log_tail());
  } else if (c == "wifi") {
    if (a.size() == 2 && a[1] == "clear") {
      s.wifi_ssid = s.wifi_pass = "";
    } else if (a.size() >= 2) {
      s.wifi_ssid = a[1];
      s.wifi_pass = a.size() >= 3 ? a[2] : "";
    } else {
      Serial.println("usage: wifi <ssid> <password> | wifi clear");
      return;
    }
    settings::set(s);
    Serial.println("saved; rebooting to apply");
    delay(200);
    ESP.restart();
  } else if (c == "webpass" && a.size() >= 2) {
    s.web_pass = a[1];
    settings::set(s);
    Serial.println("web password set (user: admin)");
  } else if (c == "obs") {
    if (a.size() >= 2 && a[1] == "off") s.obs_host = "";
    else if (a.size() >= 2) {
      s.obs_host = a[1];
      if (a.size() >= 3) s.obs_port = a[2].toInt();
      if (a.size() >= 4) s.obs_pass = a[3];
    } else {
      Serial.println("usage: obs <host> [port] [password] | obs off");
      return;
    }
    settings::set(s);
    obs::reconfigure();
    Serial.println("obs settings saved");
  } else if (c == "press" || c == "long") {
    int k = key_arg(a);
    if (k < 0) return (void)Serial.println("usage: press|long <1-12>");
    input::inject(EV_DOWN, k);
    if (c == "long") input::inject(EV_LONG, k);
    input::inject(EV_UP, k);
  } else if (c == "page" && a.size() >= 2) {
    ConfigPtr cfg = config::get();
    int p = cfg->pageIndex(a[1].c_str());
    if (p < 0 && isdigit((unsigned char)a[1][0])) p = a[1].toInt() - 1;
    if (p < 0 || p >= cfg->pageCount()) return (void)Serial.println("no such page");
    state::set_page(p);
  } else if (c == "pot" && a.size() >= 2) {
    if (a[1] == "on") s.pot_enabled = true;
    else if (a[1] == "off") s.pot_enabled = false;
    else if (a[1] == "invert") s.pot_invert = !s.pot_invert;
    settings::set(s);
  } else if (c == "bright" && a.size() >= 2) {
    s.fixed_brightness = constrain(a[1].toInt(), 0, 255);
    settings::set(s);
  } else if (c == "govee" && a.size() >= 2 && a[1] == "scan") {
    govee::scan();
    Serial.println("scanning; results in log and /api/govee");
  } else if (c == "reboot") {
    ESP.restart();
  } else {
    Serial.println("unknown command; type help");
  }
}

void console::loop() {
  static String buf;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (buf.length()) {
        Serial.printf("> %s\n", buf.c_str());
        run_line(buf);
      }
      buf = "";
    } else if (buf.length() < 200) {
      buf += ch;
    }
  }
}
