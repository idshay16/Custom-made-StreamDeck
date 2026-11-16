// input.cpp — 1 kHz key scan + pot, on core 1.
//
// Keys: integrator debounce. A key must read the same for DECK_DEBOUNCE_MS
// consecutive 1 ms samples before its state flips. The press edge is sent
// the moment it's confirmed, so added latency is ~DECK_DEBOUNCE_MS.
//
// Pot: 8-sample average every 20 ms, EMA, then 3-step hysteresis on the
// 0..255 output so a pot sitting between two values doesn't flicker.
#include "app.h"

QueueHandle_t g_input_q;   // shared with actions.cpp

static void push(EventType t, int key) {
  InputEvent e{t, (uint8_t)key, nullptr};
  xQueueSend(g_input_q, &e, 0);
}

void input::inject(EventType t, int key) {
  if (key < 0 || key >= deck::NUM_KEYS) return;
  push(t, key);
}

static void input_task(void *) {
  bool stable[deck::NUM_KEYS] = {};
  uint8_t count[deck::NUM_KEYS] = {};
  uint32_t down_at[deck::NUM_KEYS] = {};
  bool long_sent[deck::NUM_KEYS] = {};

  float ema = -1;
  int out = -1;
  uint32_t last_pot = 0;

  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    uint32_t now = millis();
    for (int k = 0; k < deck::NUM_KEYS; k++) {
      bool raw = digitalRead(deck::KEY_PINS[k]) == LOW;
      if (raw == stable[k]) {
        count[k] = 0;
      } else if (++count[k] >= DECK_DEBOUNCE_MS) {
        stable[k] = raw;
        count[k] = 0;
        if (raw) {
          down_at[k] = now;
          long_sent[k] = false;
          push(EV_DOWN, k);
        } else {
          push(EV_UP, k);
        }
      }
      if (stable[k] && !long_sent[k] && now - down_at[k] >= DECK_LONGPRESS_MS) {
        long_sent[k] = true;
        push(EV_LONG, k);
      }
    }

    if (now - last_pot >= 20) {
      last_pot = now;
      Settings s = settings::get();
      if (s.pot_enabled) {
        uint32_t mv = 0;
        for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(deck::POT_WIPER);
        mv /= 8;
        float v = constrain(mv / 3100.0f, 0.0f, 1.0f);   // ADC tops out a bit below 3.3 V
        if (s.pot_invert) v = 1.0f - v;
        ema = ema < 0 ? v : ema + 0.25f * (v - ema);
        int b = (int)(ema * 255.0f + 0.5f);
        if (out < 0 || abs(b - out) >= 3 || (b == 0 && out != 0) || (b == 255 && out != 255)) {
          out = b;
          state::set_brightness(out);
        }
      } else {
        out = -1;
        state::set_brightness(s.fixed_brightness);
      }
    }
    vTaskDelayUntil(&wake, 1);
  }
}

void input::start() {
  g_input_q = xQueueCreate(32, sizeof(InputEvent));
  for (int k = 0; k < deck::NUM_KEYS; k++) pinMode(deck::KEY_PINS[k], INPUT_PULLUP);
  analogReadResolution(12);
  analogSetPinAttenuation(deck::POT_WIPER, ADC_11db);
  xTaskCreatePinnedToCore(input_task, "input", 4096, nullptr, 5, nullptr, 1);
}
