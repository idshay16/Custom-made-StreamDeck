// config.cpp — /config.json on LittleFS: load, validate, save, atomic swap.
#include "app.h"
#include "default_config.h"
#include <LittleFS.h>
#include <mutex>
#include <esp_heap_caps.h>

static const char *CONFIG_PATH = "/config.json";
static const char *CONFIG_TMP = "/config.tmp";

// ------------------------------------------------------------ PSRAM allocator
void *PsramAllocator::allocate(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  return p ? p : malloc(n);
}
void PsramAllocator::deallocate(void *p) { heap_caps_free(p); }
void *PsramAllocator::reallocate(void *p, size_t n) {
  void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM);
  return q ? q : realloc(p, n);
}
static PsramAllocator g_alloc;

// ------------------------------------------------------------ Config accessors
Config::Config() : doc(&g_alloc) {}

int Config::pageCount() const { return doc["pages"].as<JsonArrayConst>().size(); }

JsonObjectConst Config::page(int i) const { return doc["pages"][i].as<JsonObjectConst>(); }

JsonObjectConst Config::key(int p, int k) const {
  return doc["pages"][p]["keys"][k].as<JsonObjectConst>();
}

int Config::pageIndex(const char *name) const {
  int i = 0;
  for (JsonObjectConst pg : doc["pages"].as<JsonArrayConst>()) {
    if (strcmp(pg["name"] | "", name) == 0) return i;
    i++;
  }
  return -1;
}

bool Config::altcodeTyping() const { return strcmp(doc["typing"] | "altcode", "direct") != 0; }

// ------------------------------------------------------------ validation
static bool fail(String &err, const String &where, const char *msg) {
  err = where + ": " + msg;
  return false;
}

static bool is_color(JsonVariantConst v) {
  if (v.isNull()) return true;
  const char *s = v.as<const char *>();
  if (!s || s[0] != '#' || strlen(s) != 7) return false;
  for (int i = 1; i < 7; i++)
    if (!isxdigit((unsigned char)s[i])) return false;
  return true;
}

static bool validate_action(JsonVariantConst a, const String &where, String &err, int depth = 0);

static bool need_str(JsonObjectConst a, const char *field, const String &where, String &err) {
  if (!a[field].is<const char *>() || strlen(a[field].as<const char *>()) == 0)
    return fail(err, where, (String("needs \"") + field + "\"").c_str());
  return true;
}

static bool validate_action(JsonVariantConst v, const String &where, String &err, int depth) {
  if (v.isNull()) return true;
  if (depth > 3) return fail(err, where, "actions nested too deep");
  if (!v.is<JsonObjectConst>()) return fail(err, where, "action must be an object");
  JsonObjectConst a = v.as<JsonObjectConst>();
  const char *t = a["type"] | "";
  if (!a["flip"].isNull() && !a["flip"].is<const char *>()) return fail(err, where, "\"flip\" must be a string");

  if (!strcmp(t, "none")) return true;
  if (!strcmp(t, "hotkey")) {
    if (!need_str(a, "keys", where, err)) return false;
    uint8_t u[8], m;
    int n;
    if (!hid::parse_combo(a["keys"], u, m, n)) return fail(err, where, "unknown key name in \"keys\"");
    return true;
  }
  if (!strcmp(t, "media")) {
    static const char *ok[] = {"play_pause", "next", "prev", "stop", "mute", "vol_up", "vol_down"};
    const char *k = a["key"] | "";
    for (auto o : ok)
      if (!strcmp(k, o)) return true;
    return fail(err, where, "media \"key\" must be play_pause|next|prev|stop|mute|vol_up|vol_down");
  }
  if (!strcmp(t, "text")) return need_str(a, "text", where, err);
  if (!strcmp(t, "launch")) return need_str(a, "cmd", where, err);
  if (!strcmp(t, "url")) return need_str(a, "url", where, err);
  if (!strcmp(t, "page")) return need_str(a, "to", where, err);
  if (!strcmp(t, "macro")) {
    if (!a["steps"].is<JsonArrayConst>()) return fail(err, where, "macro needs \"steps\" array");
    int i = 0;
    for (JsonObjectConst s : a["steps"].as<JsonArrayConst>()) {
      String w = where + ".steps[" + i++ + "]";
      if (s["keys"].is<const char *>()) {
        uint8_t u[8], m;
        int n;
        if (!hid::parse_combo(s["keys"], u, m, n)) return fail(err, w, "unknown key name");
      } else if (!s["delay"].is<int>() && !s["text"].is<const char *>() && !s["media"].is<const char *>()) {
        return fail(err, w, "step needs keys|delay|text|media");
      }
    }
    return true;
  }
  if (!strcmp(t, "obs")) {
    const char *op = a["op"] | "";
    if (!strcmp(op, "scene")) return need_str(a, "scene", where, err);
    if (!strcmp(op, "mute")) return need_str(a, "input", where, err);
    if (!strcmp(op, "record") || !strcmp(op, "stream")) return true;
    if (!strcmp(op, "raw")) return need_str(a, "request", where, err);
    return fail(err, where, "obs \"op\" must be scene|record|stream|mute|raw");
  }
  if (!strcmp(t, "govee")) {
    if (!need_str(a, "device", where, err)) return false;
    const char *op = a["op"] | "";
    if (!strcmp(op, "color") && !is_color(a["color"])) return fail(err, where, "govee color needs \"color\": \"#rrggbb\"");
    if (strcmp(op, "toggle") && strcmp(op, "on") && strcmp(op, "off") && strcmp(op, "brightness") && strcmp(op, "color"))
      return fail(err, where, "govee \"op\" must be toggle|on|off|brightness|color");
    return true;
  }
  if (!strcmp(t, "multi")) {
    if (!a["actions"].is<JsonArrayConst>()) return fail(err, where, "multi needs \"actions\" array");
    int i = 0;
    for (JsonVariantConst s : a["actions"].as<JsonArrayConst>())
      if (!validate_action(s, where + ".actions[" + i++ + "]", err, depth + 1)) return false;
    return true;
  }
  return fail(err, where, (String("unknown action type \"") + t + "\"").c_str());
}

bool config::validate(const JsonDocument &doc, String &err) {
  if ((doc["version"] | 0) != 1) return fail(err, "config", "\"version\" must be 1");
  JsonArrayConst pages = doc["pages"].as<JsonArrayConst>();
  if (pages.isNull() || pages.size() == 0) return fail(err, "config", "needs at least one page");
  if (pages.size() > 20) return fail(err, "config", "at most 20 pages");
  int p = 0;
  for (JsonObjectConst pg : pages) {
    String wp = String("pages[") + p++ + "]";
    if (!pg["name"].is<const char *>() || strlen(pg["name"]) == 0) return fail(err, wp, "needs a \"name\"");
    for (int q = 0; q < p - 1; q++)
      if (!strcmp(pages[q]["name"] | "", pg["name"])) return fail(err, wp, "duplicate page name");
    JsonArrayConst keys = pg["keys"].as<JsonArrayConst>();
    if (keys.isNull()) return fail(err, wp, "needs \"keys\" array");
    if (keys.size() > deck::NUM_KEYS) return fail(err, wp, "more than 12 keys");
    int k = 0;
    for (JsonVariantConst kv : keys) {
      String wk = wp + ".keys[" + k++ + "]";
      if (kv.isNull()) continue;
      if (!kv.is<JsonObjectConst>()) return fail(err, wk, "key must be an object or null");
      JsonObjectConst key = kv.as<JsonObjectConst>();
      if (!is_color(key["color"])) return fail(err, wk, "\"color\" must be #rrggbb");
      if (!is_color(key["on_color"])) return fail(err, wk, "\"on_color\" must be #rrggbb");
      if (!validate_action(key["press"], wk + ".press", err)) return false;
      if (!validate_action(key["long"], wk + ".long", err)) return false;
    }
  }
  return true;
}

// ------------------------------------------------------------ storage + swap
static std::mutex g_mu;
static ConfigPtr g_cfg;

static bool parse(const char *json, size_t len, std::shared_ptr<Config> &out, String &err) {
  auto c = std::make_shared<Config>();
  DeserializationError e = deserializeJson(c->doc, json, len);
  if (e) {
    err = String("JSON: ") + e.c_str();
    return false;
  }
  if (!config::validate(c->doc, err)) return false;
  out = c;
  return true;
}

static bool write_file(const char *json, size_t len) {
  // Write to a temp file, then rename: a power cut never leaves half a config.
  File f = LittleFS.open(CONFIG_TMP, "w");
  if (!f) return false;
  size_t w = f.write((const uint8_t *)json, len);
  f.close();
  if (w != len) return false;
  LittleFS.remove(CONFIG_PATH);
  return LittleFS.rename(CONFIG_TMP, CONFIG_PATH);
}

static void swap_in(std::shared_ptr<Config> c) {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_cfg = c;
  }
  int pc = c->pageCount();
  if (state::page() >= pc) state::set_page(0);
  render::invalidate();
  state::changed();
}

void config::init() {
  if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
    LOG("config: LittleFS mount failed even after format; using defaults in RAM");
  } else {
    LittleFS.mkdir("/icons");
    LOG("config: LittleFS %u KB used of %u KB", (unsigned)(LittleFS.usedBytes() / 1024),
        (unsigned)(LittleFS.totalBytes() / 1024));
  }

  std::shared_ptr<Config> c;
  String err;
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (f) {
    size_t n = f.size();
    char *buf = (char *)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
    if (buf) {
      f.readBytes(buf, n);
      buf[n] = 0;
      if (!parse(buf, n, c, err)) LOG("config: /config.json invalid (%s); using default", err.c_str());
      heap_caps_free(buf);
    }
    f.close();
  } else {
    LOG("config: no /config.json; writing default");
  }
  if (!c) {
    if (!parse(DEFAULT_CONFIG, strlen(DEFAULT_CONFIG), c, err)) {
      LOG("config: BUG: default config invalid: %s", err.c_str());
      c = std::make_shared<Config>();
    } else if (!LittleFS.exists(CONFIG_PATH)) {
      write_file(DEFAULT_CONFIG, strlen(DEFAULT_CONFIG));
    }
  }
  std::lock_guard<std::mutex> lk(g_mu);
  g_cfg = c;
  LOG("config: %d pages", c->pageCount());
}

ConfigPtr config::get() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_cfg;
}

bool config::apply(const char *json, size_t len, String &err) {
  std::shared_ptr<Config> c;
  if (!parse(json, len, c, err)) return false;
  // Store compact JSON (what the web UI sends may be pretty-printed).
  String out;
  serializeJson(c->doc, out);
  if (!write_file(out.c_str(), out.length())) {
    err = "failed to write /config.json";
    return false;
  }
  swap_in(c);
  LOG("config: applied (%u bytes, %d pages)", (unsigned)out.length(), c->pageCount());
  return true;
}

String config::json() {
  String out;
  serializeJson(get()->doc, out);
  return out;
}

void config::reset_default() {
  String err;
  if (!apply(DEFAULT_CONFIG, strlen(DEFAULT_CONFIG), err)) LOG("config: reset failed: %s", err.c_str());
}
