// actions.cpp — turns key events into actions, on core 1.
//
// Press semantics:
//   key without "long": "press" runs on the DOWN edge (lowest latency).
//   key with "long":    wait. Release before 500 ms -> "press";
//                       still held at 500 ms -> "long" (and "press" never runs).
//   "hold": true on hotkey/media: key down on DOWN, key up on UP (push-to-talk).
//
// The config + page are captured at DOWN, so a page change or config upload
// while a key is held can't release the wrong thing.
#include "app.h"

extern QueueHandle_t g_input_q;

struct Held {
  ConfigPtr cfg;
  int page = -1;
  bool deferred = false;   // has a "long" action: decide at release / 500 ms
  bool holding = false;    // "hold" action is currently down
};
static Held g_held[deck::NUM_KEYS];

static void run(JsonObjectConst a, bool altcode, int depth = 0);

static void run_macro(JsonArrayConst steps, bool altcode) {
  for (JsonObjectConst s : steps) {
    if (s["keys"].is<const char *>()) hid::combo_tap(s["keys"]);
    else if (s["text"].is<const char *>()) hid::type_text(s["text"], altcode);
    else if (s["media"].is<const char *>()) hid::media_tap(s["media"]);
    if (s["delay"].is<int>()) delay(constrain(s["delay"].as<int>(), 0, 10000));
  }
}

static void goto_page(JsonObjectConst a, const Config &cfg) {
  const char *to = a["to"] | "";
  int n = cfg.pageCount(), p = state::page();
  if (!strcmp(to, "next")) p = (p + 1) % n;
  else if (!strcmp(to, "prev")) p = (p + n - 1) % n;
  else {
    int i = cfg.pageIndex(to);
    if (i < 0) {
      LOG("action: no page \"%s\"", to);
      return;
    }
    p = i;
  }
  state::set_page(p);
}

static void run(JsonObjectConst a, bool altcode, int depth) {
  if (a.isNull() || depth > 3) return;
  const char *t = a["type"] | "";
  ConfigPtr cfg = config::get();

  if (!strcmp(t, "hotkey")) hid::combo_tap(a["keys"]);
  else if (!strcmp(t, "media")) hid::media_tap(a["key"]);
  else if (!strcmp(t, "text")) {
    hid::type_text(a["text"], altcode);
    if (a["enter"] | false) hid::combo_tap("enter");
  } else if (!strcmp(t, "launch")) hid::run_dialog(a["cmd"], altcode);
  else if (!strcmp(t, "url")) hid::run_dialog(a["url"], altcode);
  else if (!strcmp(t, "macro")) run_macro(a["steps"], altcode);
  else if (!strcmp(t, "page")) goto_page(a, *cfg);
  else if (!strcmp(t, "obs")) obs::action(a);
  else if (!strcmp(t, "govee")) govee::action(a);
  else if (!strcmp(t, "multi"))
    for (JsonObjectConst s : a["actions"].as<JsonArrayConst>()) run(s, altcode, depth + 1);

  if (a["flip"].is<const char *>()) state::flip(a["flip"].as<const char *>());
}

static bool is_hold(JsonObjectConst a) {
  const char *t = a["type"] | "";
  return (a["hold"] | false) && (!strcmp(t, "hotkey") || !strcmp(t, "media"));
}

static void hold_down(JsonObjectConst a) {
  if (!strcmp(a["type"] | "", "hotkey")) hid::combo_down(a["keys"]);
  else hid::media_down(a["key"]);
  if (a["flip"].is<const char *>()) state::flip(a["flip"].as<const char *>());
}

static void hold_up(JsonObjectConst a) {
  if (!strcmp(a["type"] | "", "hotkey")) hid::combo_up(a["keys"]);
  else hid::media_up();
  if (a["flip"].is<const char *>()) state::flip(a["flip"].as<const char *>());
}

static void on_event(const InputEvent &e) {
  if (e.type == EV_RUN) {
    JsonDocument d;
    if (!deserializeJson(d, e.json)) {
      LOG("test action -> %s", d["type"] | "none");
      run(d.as<JsonObjectConst>(), config::get()->altcodeTyping());
    }
    free(e.json);
    return;
  }
  int k = e.key;
  Held &h = g_held[k];

  if (e.type == EV_DOWN) {
    uint32_t t0 = micros();
    state::set_pressed(k, true);
    h.cfg = config::get();
    h.page = state::page();
    JsonObjectConst key = h.cfg->key(h.page, k);
    h.deferred = !key["long"].isNull();
    h.holding = false;
    if (h.deferred || key.isNull()) return;
    JsonObjectConst press = key["press"];
    if (is_hold(press)) {
      hold_down(press);
      h.holding = true;
    } else {
      run(press, h.cfg->altcodeTyping());
    }
    LOG("K%d down -> %s (%lu us)", k + 1, press["type"] | "none", (unsigned long)(micros() - t0));
    return;
  }

  if (!h.cfg) return;   // UP/LONG without a DOWN we saw (e.g. after boot)
  JsonObjectConst key = h.cfg->key(h.page, k);

  if (e.type == EV_LONG) {
    if (!h.deferred) return;
    h.deferred = false;
    LOG("K%d long -> %s", k + 1, key["long"]["type"] | "none");
    run(key["long"], h.cfg->altcodeTyping());
    return;
  }

  // EV_UP
  state::set_pressed(k, false);
  if (h.deferred) {
    h.deferred = false;
    LOG("K%d tap -> %s", k + 1, key["press"]["type"] | "none");
    run(key["press"], h.cfg->altcodeTyping());
  } else if (h.holding) {
    hold_up(key["press"]);
    h.holding = false;
  }
  h.cfg.reset();
}

static void actions_task(void *) {
  InputEvent e;
  for (;;)
    if (xQueueReceive(g_input_q, &e, portMAX_DELAY) == pdTRUE) on_event(e);
}

bool actions::execute(const String &action_json) {
  InputEvent e{EV_RUN, 0, strdup(action_json.c_str())};
  if (!e.json) return false;
  if (xQueueSend(g_input_q, &e, 0) != pdTRUE) {
    free(e.json);
    return false;
  }
  return true;
}

void actions::start() {
  xTaskCreatePinnedToCore(actions_task, "actions", 8192, nullptr, 4, nullptr, 1);
}
