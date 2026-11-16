// render.cpp — display tiles + status bar + key LEDs, on core 1.
//
// The whole screen lives in a 480x272 RGB565 sprite in PSRAM (~255 KB).
// Each tile and the status bar have a signature (hash of everything that
// affects their look). Only regions whose signature changed get redrawn and
// pushed, so a key press redraws one 120x81 tile, not the whole screen.
//
// No display attached (detected via the WAIT pin) -> LEDs only.
#include "app.h"
#include <deck_display.h>
#include <Adafruit_NeoPixel.h>
#include <LittleFS.h>
#include <atomic>
#include <time.h>

static constexpr int W = 480, H = 272, BAR_H = 29;
static constexpr int COLS = 4, ROWS = 3, TILE_W = W / COLS, TILE_H = (H - BAR_H) / ROWS;   // 120 x 81

static deck::Display *g_lcd = nullptr;
static LGFX_Sprite *g_spr = nullptr;
static bool g_present = false;
static Adafruit_NeoPixel g_strip(DECK_NUM_LEDS, deck::LED_DIN, NEO_GRB + NEO_KHZ800);
static SemaphoreHandle_t g_wake;
static std::atomic<bool> g_invalid{true};

struct Icon {
  String path;
  uint8_t *data = nullptr;
  size_t len = 0;
};
static Icon g_icons[deck::NUM_KEYS];

void render::wake() {
  if (g_wake) xSemaphoreGive(g_wake);
}
void render::invalidate() {
  g_invalid = true;
  wake();
}
bool render::display_present() { return g_present; }

// ------------------------------------------------------------ helpers
struct RGB {
  uint8_t r = 0, g = 0, b = 0;
};

static RGB parse_color(const char *s, RGB def) {
  if (!s || s[0] != '#' || strlen(s) != 7) return def;
  uint32_t v = strtoul(s + 1, nullptr, 16);
  return {uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
}

static RGB scale(RGB c, float f) {
  return {uint8_t(c.r * f), uint8_t(c.g * f), uint8_t(c.b * f)};
}

static uint32_t fnv(uint32_t h, const void *p, size_t n) {
  const uint8_t *b = (const uint8_t *)p;
  for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
  return h;
}
static uint32_t fnv(uint32_t h, const char *s) { return fnv(h, s, strlen(s)); }

struct Look {
  bool empty = true, pressed = false, on = false;
  RGB color;
  const char *label = "";
  const char *icon = "";
  uint32_t sig() const {
    uint32_t h = 2166136261u;
    uint8_t f = empty | (pressed << 1) | (on << 2);
    h = fnv(h, &f, 1);
    h = fnv(h, &color, sizeof color);
    h = fnv(h, label);
    return fnv(h, icon);
  }
};

static Look look_of(const Config &cfg, int page, int k, uint16_t pressed) {
  Look l;
  JsonObjectConst key = cfg.key(page, k);
  l.pressed = pressed & (1u << k);
  if (key.isNull() || key.size() == 0) return l;
  l.empty = false;
  const char *st = key["state"] | "";
  l.on = st[0] && state::get(st);
  l.color = parse_color(key["color"], {90, 90, 90});
  l.label = key["label"] | "";
  l.icon = key["icon"] | "";
  if (l.on) {
    l.color = parse_color(key["on_color"], l.color);
    if (key["on_label"].is<const char *>()) l.label = key["on_label"];
  }
  return l;
}

// ------------------------------------------------------------ icons
static void load_icons(const Config &cfg, int page) {
  for (int k = 0; k < deck::NUM_KEYS; k++) {
    const char *name = cfg.key(page, k)["icon"] | "";
    String path = name[0] ? String("/icons/") + name : String();
    if (path == g_icons[k].path) continue;
    free(g_icons[k].data);
    g_icons[k] = Icon();
    g_icons[k].path = path;
    if (path.isEmpty()) continue;
    File f = LittleFS.open(path, "r");
    if (!f || f.size() == 0 || f.size() > 64 * 1024) {
      LOG("render: icon %s missing or too big", path.c_str());
      continue;
    }
    g_icons[k].len = f.size();
    g_icons[k].data = (uint8_t *)ps_malloc(g_icons[k].len);
    if (g_icons[k].data) f.readBytes((char *)g_icons[k].data, g_icons[k].len);
    f.close();
  }
}

// ------------------------------------------------------------ drawing
static void draw_label(const char *text, int cx, int cy, int maxw, uint16_t fg, bool big) {
  const lgfx::IFont *fonts[] = {&fonts::FreeSansBold12pt7b, &fonts::FreeSansBold9pt7b, &fonts::Font2};
  g_spr->setTextColor(fg);
  g_spr->setTextDatum(middle_center);
  for (int i = big ? 0 : 1; i < 3; i++) {
    g_spr->setFont(fonts[i]);
    if (g_spr->textWidth(text) <= maxw) {
      g_spr->drawString(text, cx, cy);
      return;
    }
  }
  // Two lines at 9pt, split at the space closest to the middle.
  String s(text);
  int best = -1, mid = s.length() / 2;
  for (int i = 0; i < (int)s.length(); i++)
    if (s[i] == ' ' && (best < 0 || abs(i - mid) < abs(best - mid))) best = i;
  g_spr->setFont(&fonts::FreeSansBold9pt7b);
  if (best > 0) {
    String a = s.substring(0, best), b = s.substring(best + 1);
    if (g_spr->textWidth(a) <= maxw && g_spr->textWidth(b) <= maxw) {
      g_spr->drawString(a, cx, cy - 10);
      g_spr->drawString(b, cx, cy + 10);
      return;
    }
  }
  g_spr->setFont(&fonts::Font2);
  while (s.length() > 1 && g_spr->textWidth(s + "..") > maxw) s.remove(s.length() - 1);
  g_spr->drawString(s + "..", cx, cy);
}

static void draw_tile(int k, const Look &l) {
  int x = (k % COLS) * TILE_W, y = BAR_H + (k / COLS) * TILE_H;
  g_spr->fillRect(x, y, TILE_W, TILE_H, TFT_BLACK);
  if (l.empty) {
    g_spr->drawRoundRect(x + 4, y + 4, TILE_W - 8, TILE_H - 8, 10, g_spr->color888(40, 40, 40));
    return;
  }
  RGB bg = l.on ? l.color : scale(l.color, 0.45f);
  if (l.pressed) bg = {uint8_t(min(255, bg.r + 70)), uint8_t(min(255, bg.g + 70)), uint8_t(min(255, bg.b + 70))};
  g_spr->fillRoundRect(x + 3, y + 3, TILE_W - 6, TILE_H - 6, 10, g_spr->color888(bg.r, bg.g, bg.b));
  if (l.pressed) {
    g_spr->drawRoundRect(x + 3, y + 3, TILE_W - 6, TILE_H - 6, 10, TFT_WHITE);
    g_spr->drawRoundRect(x + 4, y + 4, TILE_W - 8, TILE_H - 8, 9, TFT_WHITE);
  }
  int luma = (bg.r * 299 + bg.g * 587 + bg.b * 114) / 1000;
  uint16_t fg = luma > 150 ? TFT_BLACK : TFT_WHITE;
  const Icon &ic = g_icons[k];
  if (ic.data) {
    g_spr->drawPng(ic.data, ic.len, x + (TILE_W - 48) / 2, y + 7, 48, 48);
    draw_label(l.label, x + TILE_W / 2, y + TILE_H - 14, TILE_W - 12, fg, false);
  } else {
    draw_label(l.label, x + TILE_W / 2, y + TILE_H / 2, TILE_W - 14, fg, true);
  }
}

static uint32_t bar_sig(const Config &cfg, int page, char *clock, size_t n) {
  time_t t = time(nullptr);
  clock[0] = 0;
  if (t > 1700000000) {
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(clock, n, "%H:%M", &tm);
  }
  uint32_t h = 2166136261u;
  h = fnv(h, cfg.page(page)["name"] | "");
  uint8_t f[4] = {(uint8_t)page, (uint8_t)cfg.pageCount(), (uint8_t)(net::connected() | (net::ap_mode() << 1)),
                  (uint8_t)state::get("obs.connected")};
  h = fnv(h, f, 4);
  return fnv(h, clock);
}

static void draw_bar(const Config &cfg, int page, const char *clock) {
  g_spr->fillRect(0, 0, W, BAR_H, g_spr->color888(24, 24, 28));
  g_spr->setFont(&fonts::FreeSansBold9pt7b);
  g_spr->setTextColor(TFT_WHITE);
  g_spr->setTextDatum(middle_left);
  String name = String(cfg.page(page)["name"] | "?");
  if (cfg.pageCount() > 1) name += String("  ") + (page + 1) + "/" + cfg.pageCount();
  g_spr->drawString(name, 8, BAR_H / 2);

  g_spr->setTextDatum(middle_center);
  if (net::ap_mode() && !net::connected()) {
    g_spr->setFont(&fonts::Font2);
    g_spr->setTextColor(TFT_YELLOW);
    g_spr->drawString("Wi-Fi setup: join StreamDeck-Setup / streamdeck", W / 2 + 20, BAR_H / 2);
  } else if (clock[0]) {
    g_spr->drawString(clock, W / 2, BAR_H / 2);
  }

  g_spr->setFont(&fonts::Font2);
  g_spr->setTextDatum(middle_right);
  bool obs = state::get("obs.connected");
  g_spr->setTextColor(obs ? TFT_GREEN : g_spr->color888(90, 90, 90));
  g_spr->drawString("OBS", W - 52, BAR_H / 2);
  g_spr->setTextColor(net::connected() ? TFT_GREEN : g_spr->color888(90, 90, 90));
  g_spr->drawString("WiFi", W - 8, BAR_H / 2);
}

static void push_region(int x, int y, int w, int h) {
  g_lcd->setClipRect(x, y, w, h);
  g_spr->pushSprite(g_lcd, 0, 0);
  g_lcd->clearClipRect();
}

// ------------------------------------------------------------ LEDs
static void update_leds(const Config &cfg, int page, uint16_t pressed, uint8_t bright) {
  static uint8_t last[DECK_NUM_LEDS][3];
  static bool first = true;
  uint8_t px[DECK_NUM_LEDS][3] = {};
  float b = (bright * bright) / 65025.0f;   // perceptual-ish curve
  uint32_t total = 0;
  for (int k = 0; k < deck::NUM_KEYS; k++) {
    int idx = deck::LED_INDEX[k];
    if (idx < 0 || idx >= DECK_NUM_LEDS) continue;
    Look l = look_of(cfg, page, k, pressed);
    RGB c = l.empty ? RGB{} : (l.pressed ? RGB{255, 255, 255} : (l.on ? l.color : scale(l.color, 0.5f)));
    c = scale(c, b);
    px[idx][0] = c.r;
    px[idx][1] = c.g;
    px[idx][2] = c.b;
    total += c.r + c.g + c.b;
  }
  // ~20 mA per channel at 255, plus ~1 mA idle per LED.
  float ma = total * 20.0f / 255.0f + DECK_NUM_LEDS;
  float cap = ma > DECK_LED_MAX_MA ? (DECK_LED_MAX_MA - DECK_NUM_LEDS) / (ma - DECK_NUM_LEDS) : 1.0f;
  bool diff = first;
  for (int i = 0; i < DECK_NUM_LEDS; i++)
    for (int c = 0; c < 3; c++) {
      px[i][c] = uint8_t(px[i][c] * cap);
      if (px[i][c] != last[i][c]) diff = true;
    }
  if (!diff) return;
  first = false;
  memcpy(last, px, sizeof px);
  for (int i = 0; i < DECK_NUM_LEDS; i++) g_strip.setPixelColor(i, px[i][0], px[i][1], px[i][2]);
  g_strip.show();
}

// ------------------------------------------------------------ task
static void render_task(void *) {
  uint32_t tile_sig[deck::NUM_KEYS] = {};
  uint32_t last_bar = 0;
  int last_bl = -1;
  for (;;) {
    xSemaphoreTake(g_wake, pdMS_TO_TICKS(1000));   // also wakes every second for the clock
    ConfigPtr cfg = config::get();
    int page = state::page();
    if (page >= cfg->pageCount()) page = 0;
    uint16_t pressed = state::pressed();
    uint8_t bright = state::brightness();
    bool full = g_invalid.exchange(false);
    if (full) load_icons(*cfg, page);

    update_leds(*cfg, page, pressed, bright);
    if (!g_present) continue;

    int bl = max<int>(DECK_BACKLIGHT_MIN, bright);
    if (bl != last_bl) {
      g_lcd->panel.setBacklight(bl);
      last_bl = bl;
    }
    char clock[8];
    uint32_t bs = bar_sig(*cfg, page, clock, sizeof clock);
    if (full || bs != last_bar) {
      last_bar = bs;
      draw_bar(*cfg, page, clock);
      push_region(0, 0, W, BAR_H);
    }
    for (int k = 0; k < deck::NUM_KEYS; k++) {
      Look l = look_of(*cfg, page, k, pressed);
      uint32_t s = l.sig();
      if (!full && s == tile_sig[k]) continue;
      tile_sig[k] = s;
      draw_tile(k, l);
      push_region((k % COLS) * TILE_W, BAR_H + (k / COLS) * TILE_H, TILE_W, TILE_H);
    }
  }
}

void render::start() {
  g_wake = xSemaphoreCreateBinary();
  g_strip.begin();
  g_strip.clear();
  g_strip.show();

  g_present = deck::Display::present();
  if (g_present) {
    g_lcd = new deck::Display();
    g_lcd->init();
    g_spr = new LGFX_Sprite(g_lcd);
    g_spr->setPsram(true);
    g_spr->setColorDepth(16);
    if (!g_spr->createSprite(W, H)) {
      LOG("render: no PSRAM for the frame buffer; display disabled");
      g_present = false;
    } else {
      g_spr->fillScreen(TFT_BLACK);
      g_spr->setFont(&fonts::FreeSansBold12pt7b);
      g_spr->setTextColor(TFT_WHITE);
      g_spr->setTextDatum(middle_center);
      g_spr->drawString("DIY StreamDeck " DECK_FW_VERSION, W / 2, H / 2);
      g_spr->pushSprite(0, 0);
      LOG("render: display OK (480x272, %u Hz bus)", (unsigned)DECK_LCD_WRITE_HZ);
    }
  } else {
    LOG("render: no display detected on WAIT pin; running headless (LEDs only)");
  }
  xTaskCreatePinnedToCore(render_task, "render", 8192, nullptr, 2, nullptr, 1);
}
