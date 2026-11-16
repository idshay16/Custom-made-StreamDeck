// web.cpp — web UI + REST API + live status WebSocket + OTA.
//
// The UI files (webui/) are gzipped into webui_assets.h by
// scripts/embed_webui.ps1, so the UI always matches the firmware and an OTA
// update carries both.
//
// Auth: HTTP digest auth, user "admin", password from settings. With no
// password set, everything except OTA is open (so first-time setup works);
// the UI nags until one is set. OTA always requires the password.
//
// REST:
//   GET  /api/status            runtime state
//   GET  /api/config            config.json        PUT /api/config   validate + apply
//   GET  /api/icons             [{name,size}]      POST /api/icons   multipart upload (.png, <= 64 KB)
//   DELETE /api/icons?name=x.png
//   POST /api/press?key=0..11[&long=1]             simulate a key
//   POST /api/run               body = action JSON, run it once (editor "Test")
//   GET  /api/settings          (no secrets)       POST /api/settings  JSON patch
//   GET  /api/govee             discovered devices POST /api/govee/scan
//   GET  /api/log               recent log lines
//   POST /api/ota               multipart firmware .bin
//   POST /api/reboot
//   WS   /ws                    pushes status JSON on every change
#include "app.h"
#include "webui_assets.h"
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Update.h>
#include <WiFi.h>

static AsyncWebServer g_server(80);
static AsyncWebSocket g_ws("/ws");
static volatile uint32_t g_reboot_at = 0;

// ------------------------------------------------------------ helpers
static bool guard(AsyncWebServerRequest *r, bool strict = false) {
  Settings s = settings::get();
  if (s.web_pass.isEmpty()) {
    if (!strict) return true;
    r->send(403, "text/plain", "Set a web password first (Settings)");
    return false;
  }
  if (r->authenticate("admin", s.web_pass.c_str())) return true;
  r->requestAuthentication("StreamDeck");
  return false;
}

static void send_json(AsyncWebServerRequest *r, int code, const JsonDocument &d) {
  String s;
  serializeJson(d, s);
  r->send(code, "application/json", s);
}

static void send_err(AsyncWebServerRequest *r, int code, const String &msg) {
  JsonDocument d;
  d["error"] = msg;
  send_json(r, code, d);
}

static void send_ok(AsyncWebServerRequest *r) { r->send(200, "application/json", "{\"ok\":true}"); }

// Collect a request body (<= 64 KB) into _tempObject; the library frees it.
static void collect_body(AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total) {
  if (total > 64 * 1024) return;
  if (index == 0) {
    r->_tempObject = ps_malloc(total + 1);
    if (r->_tempObject) ((char *)r->_tempObject)[total] = 0;
  }
  if (r->_tempObject && index + len <= total) memcpy((uint8_t *)r->_tempObject + index, data, len);
}

static const char *body(AsyncWebServerRequest *r) { return (const char *)r->_tempObject; }

static String status_json() {
  JsonDocument d;
  state::fill_status(d);
  String s;
  serializeJson(d, s);
  return s;
}

static bool safe_icon_name(const String &n) {
  if (n.length() < 5 || n.length() > 31 || !n.endsWith(".png")) return false;
  for (char c : n)
    if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-') return false;
  return n.indexOf("..") < 0;
}

// ------------------------------------------------------------ routes
static void routes() {
  for (size_t i = 0; i < WEB_ASSET_COUNT; i++) {
    const WebAsset &a = WEB_ASSETS[i];
    auto handler = [&a](AsyncWebServerRequest *r) {
      if (!guard(r)) return;
      AsyncWebServerResponse *res = r->beginResponse(200, a.type, a.data, a.len);
      res->addHeader("Content-Encoding", "gzip");
      res->addHeader("Cache-Control", "no-cache");
      r->send(res);
    };
    g_server.on(a.path, HTTP_GET, handler);
    if (!strcmp(a.path, "/index.html")) g_server.on("/", HTTP_GET, handler);
  }

  g_server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!guard(r)) return;
    r->send(200, "application/json", status_json());
  });

  g_server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!guard(r)) return;
    r->send(200, "application/json", config::json());
  });

  g_server.on(
    "/api/config", HTTP_PUT,
    [](AsyncWebServerRequest *r) {
      if (!guard(r)) return;
      if (!body(r)) return send_err(r, 400, "empty or too large body");
      String err;
      if (!config::apply(body(r), strlen(body(r)), err)) return send_err(r, 400, err);
      send_ok(r);
    },
    nullptr, collect_body);

  g_server.on("/api/icons", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!guard(r)) return;
    JsonDocument d;
    JsonArray arr = d.to<JsonArray>();
    File dir = LittleFS.open("/icons");
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      JsonObject o = arr.add<JsonObject>();
      o["name"] = String(f.name());
      o["size"] = f.size();
    }
    send_json(r, 200, d);
  });

  g_server.on(
    "/api/icons", HTTP_POST,
    [](AsyncWebServerRequest *r) {
      if (!guard(r)) return;
      if (r->_tempObject) return send_err(r, 400, (const char *)r->_tempObject);
      send_ok(r);
      render::invalidate();
    },
    [](AsyncWebServerRequest *r, const String &filename, size_t index, uint8_t *data, size_t len, bool final) {
      static const char *bad_name = "icon name must be letters/digits/._- and end in .png";
      static const char *too_big = "icon larger than 64 KB";
      if (index == 0) {
        if (!safe_icon_name(filename)) {
          r->_tempObject = strdup(bad_name);
          return;
        }
        r->_tempFile = LittleFS.open("/icons/" + filename, "w");
      }
      if (r->_tempObject || !r->_tempFile) return;
      if (index + len > 64 * 1024) {
        r->_tempFile.close();
        LittleFS.remove("/icons/" + filename);
        r->_tempObject = strdup(too_big);
        return;
      }
      r->_tempFile.write(data, len);
      if (final) r->_tempFile.close();
    });

  g_server.on("/api/icons", HTTP_DELETE, [](AsyncWebServerRequest *r) {
    if (!guard(r)) return;
    String n = r->hasParam("name") ? r->getParam("name")->value() : "";
    if (!safe_icon_name(n)) return send_err(r, 400, "bad name");
    LittleFS.remove("/icons/" + n);
    render::invalidate();
    send_ok(r);
  });

  g_server.serveStatic("/icons/", LittleFS, "/icons/", "max-age=60");

  g_server.on("/api/press", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!guard(r)) return;
    int k = r->hasParam("key") ? r->getParam("key")->value().toInt() : -1;
    if (k < 0 || k >= deck::NUM_KEYS) return send_err(r, 400, "key must be 0..11");
    bool lng = r->hasParam("long") && r->getParam("long")->value() == "1";
    input::inject(EV_DOWN, k);
    if (lng) input::inject(EV_LONG, k);
    input::inject(EV_UP, k);
    send_ok(r);
  });

  g_server.on(
    "/api/run", HTTP_POST,
    [](AsyncWebServerRequest *r) {
      if (!guard(r)) return;
      if (!body(r)) return send_err(r, 400, "empty body");
      JsonDocument d;
      if (deserializeJson(d, body(r))) return send_err(r, 400, "bad JSON");
      JsonDocument wrap;   // validate as a one-key config
      wrap["version"] = 1;
      wrap["pages"][0]["name"] = "t";
      wrap["pages"][0]["keys"][0]["press"] = d;
      String err;
      if (!config::validate(wrap, err)) return send_err(r, 400, err);
      if (!actions::execute(String(body(r)))) return send_err(r, 503, "busy");
      send_ok(r);
    },
    nullptr, collect_body);

  g_server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!guard(r)) return;
    Settings s = settings::get();
    JsonDocument d;
    d["wifi_ssid"] = s.wifi_ssid;
    d["has_wifi_pass"] = s.wifi_pass.length() > 0;
    d["hostname"] = s.hostname;
    d["has_web_pass"] = s.web_pass.length() > 0;
    d["obs_host"] = s.obs_host;
    d["obs_port"] = s.obs_port;
    d["has_obs_pass"] = s.obs_pass.length() > 0;
    d["tz"] = s.tz;
    d["pot_enabled"] = s.pot_enabled;
    d["pot_invert"] = s.pot_invert;
    d["fixed_brightness"] = s.fixed_brightness;
    send_json(r, 200, d);
  });

  g_server.on(
    "/api/settings", HTTP_POST,
    [](AsyncWebServerRequest *r) {
      if (!guard(r)) return;
      JsonDocument d;
      if (!body(r) || deserializeJson(d, body(r))) return send_err(r, 400, "bad JSON");
      Settings s = settings::get();
      bool wifi = false, obs_changed = false;
      if (d["wifi_ssid"].is<const char *>() && s.wifi_ssid != d["wifi_ssid"].as<const char *>()) {
        s.wifi_ssid = d["wifi_ssid"].as<const char *>();
        wifi = true;
      }
      if (d["wifi_pass"].is<const char *>() && strlen(d["wifi_pass"])) {
        s.wifi_pass = d["wifi_pass"].as<const char *>();
        wifi = true;
      }
      if (d["hostname"].is<const char *>() && strlen(d["hostname"])) s.hostname = d["hostname"].as<const char *>();
      if (d["web_pass"].is<const char *>() && strlen(d["web_pass"])) s.web_pass = d["web_pass"].as<const char *>();
      if (d["obs_host"].is<const char *>()) {
        s.obs_host = d["obs_host"].as<const char *>();
        obs_changed = true;
      }
      if (d["obs_port"].is<int>()) {
        s.obs_port = d["obs_port"];
        obs_changed = true;
      }
      if (d["obs_pass"].is<const char *>() && strlen(d["obs_pass"])) {
        s.obs_pass = d["obs_pass"].as<const char *>();
        obs_changed = true;
      }
      if (d["tz"].is<const char *>()) s.tz = d["tz"].as<const char *>();
      if (d["pot_enabled"].is<bool>()) s.pot_enabled = d["pot_enabled"];
      if (d["pot_invert"].is<bool>()) s.pot_invert = d["pot_invert"];
      if (d["fixed_brightness"].is<int>()) s.fixed_brightness = constrain(d["fixed_brightness"].as<int>(), 0, 255);
      settings::set(s);
      if (obs_changed) obs::reconfigure();
      JsonDocument out;
      out["ok"] = true;
      out["reboot"] = wifi;
      send_json(r, 200, out);
      if (wifi) g_reboot_at = millis() + 1500;
    },
    nullptr, collect_body);

  g_server.on("/api/govee", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!guard(r)) return;
    JsonDocument d;
    govee::fill_devices(d.to<JsonArray>());
    send_json(r, 200, d);
  });

  g_server.on("/api/govee/scan", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!guard(r)) return;
    govee::scan();
    send_ok(r);
  });

  g_server.on("/api/log", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!guard(r)) return;
    r->send(200, "text/plain", log_tail());
  });

  g_server.on(
    "/api/ota", HTTP_POST,
    [](AsyncWebServerRequest *r) {
      if (!guard(r, true)) return;
      if (Update.hasError() || !Update.isFinished()) return send_err(r, 500, String("update failed: ") + Update.errorString());
      send_ok(r);
      LOG("ota: success, rebooting");
      g_reboot_at = millis() + 1000;
    },
    [](AsyncWebServerRequest *r, const String &filename, size_t index, uint8_t *data, size_t len, bool final) {
      if (settings::get().web_pass.isEmpty() || !r->authenticate("admin", settings::get().web_pass.c_str())) return;
      if (index == 0) {
        LOG("ota: receiving %s", filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
          LOG("ota: begin failed: %s", Update.errorString());
          return;
        }
      }
      if (Update.isRunning() && Update.write(data, len) != len) LOG("ota: write failed: %s", Update.errorString());
      if (final && Update.isRunning()) {
        if (Update.end(true)) LOG("ota: %u bytes written", (unsigned)(index + len));
        else LOG("ota: end failed: %s", Update.errorString());
      }
    });

  g_server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!guard(r)) return;
    send_ok(r);
    g_reboot_at = millis() + 500;
  });

  g_ws.onEvent([](AsyncWebSocket *, AsyncWebSocketClient *c, AwsEventType t, void *, uint8_t *, size_t) {
    if (t == WS_EVT_CONNECT) c->text(status_json());
  });
  g_server.addHandler(&g_ws);

  // Captive portal: in setup-AP mode, send every unknown URL to the UI.
  g_server.onNotFound([](AsyncWebServerRequest *r) {
    if (net::ap_mode() && !net::connected()) {
      r->redirect(String("http://") + WiFi.softAPIP().toString() + "/#settings");
      return;
    }
    r->send(404, "text/plain", "not found");
  });
}

void web::start() {
  routes();
  g_server.begin();
  LOG("web: server on port 80");
}

void web::loop() {
  static uint32_t last_ver = 0, last_push = 0, last_clean = 0;
  uint32_t now = millis();
  if (g_reboot_at && (int32_t)(now - g_reboot_at) >= 0) {
    LOG("rebooting");
    delay(100);
    ESP.restart();
  }
  if (now - last_clean > 1000) {
    last_clean = now;
    g_ws.cleanupClients(4);
  }
  uint32_t v = state::version();
  if (v != last_ver && now - last_push >= 50 && g_ws.count() > 0) {
    last_ver = v;
    last_push = now;
    g_ws.textAll(status_json());
  }
}
