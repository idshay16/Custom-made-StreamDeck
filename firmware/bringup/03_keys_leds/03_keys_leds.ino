// Bring-up step 3: all 12 keys, the WS2812 chain, and the pot.
//
// Wiring (deck_pins.h is the spec):
//   keys:  each key between its GPIO and GND (INPUT_PULLUP, no resistors)
//   LEDs:  strip DIN <- GPIO2, +5V <- DevKit 5V, GND <- GND
//   pot:   ends to 3V3 and GND, wiper -> GPIO1
// Cable on the "USB" port. Console: arduino-cli monitor -p COMx
//
// What it does:
//   - boot: each LED lights in turn (white, low) so you can check the chain
//     order against LED_INDEX in deck_pins.h
//   - each key gets its own color; pressing it turns that LED white and
//     prints "K5 down/up" with the measured debounce time
//   - the pot sets global brightness; the console prints pot mV and the
//     brightness value (0-255) when it moves
//   - the estimated LED current is printed and capped at DECK_LED_MAX_MA
//
// What to check:
//   - every key prints exactly one down and one up per press (no chatter)
//   - the lit LED sits under the key you pressed (else fix LED_INDEX)
//   - pot sweeps 0 -> ~3100 mV and brightness 0 -> 255 smoothly
//   - first LED flickers or shows wrong colors -> 3.3 V data is too weak
//     for the 5 V strip: add a 74AHCT125 level shifter (see deck_pins.h)

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <deck_pins.h>
#include <deck_config.h>

Adafruit_NeoPixel strip(DECK_NUM_LEDS, deck::LED_DIN, NEO_GRB + NEO_KHZ800);

static const uint32_t KEY_COLORS[deck::NUM_KEYS] = {
  0xFF0000, 0xFF8000, 0xFFFF00, 0x80FF00, 0x00FF00, 0x00FF80,
  0x00FFFF, 0x0080FF, 0x0000FF, 0x8000FF, 0xFF00FF, 0xFF0080,
};

static bool pressed[deck::NUM_KEYS];
static uint8_t brightness = 64;

static void show() {
  float b = (brightness * brightness) / 65025.0f;
  uint32_t total = 0;
  uint8_t px[DECK_NUM_LEDS][3] = {};
  for (int k = 0; k < deck::NUM_KEYS; k++) {
    int i = deck::LED_INDEX[k];
    if (i >= DECK_NUM_LEDS) continue;
    uint32_t c = pressed[k] ? 0xFFFFFF : KEY_COLORS[k];
    px[i][0] = ((c >> 16) & 0xFF) * b;
    px[i][1] = ((c >> 8) & 0xFF) * b;
    px[i][2] = (c & 0xFF) * b;
    total += px[i][0] + px[i][1] + px[i][2];
  }
  float ma = total * 20.0f / 255.0f + DECK_NUM_LEDS;
  float cap = ma > DECK_LED_MAX_MA ? (DECK_LED_MAX_MA - DECK_NUM_LEDS) / (ma - DECK_NUM_LEDS) : 1.0f;
  for (int i = 0; i < DECK_NUM_LEDS; i++)
    strip.setPixelColor(i, px[i][0] * cap, px[i][1] * cap, px[i][2] * cap);
  strip.show();
  static uint32_t last_print = 0;
  if (millis() - last_print > 500) {
    last_print = millis();
    Serial.printf("LEDs: est %.0f mA%s\n", min(ma, (float)DECK_LED_MAX_MA), ma > DECK_LED_MAX_MA ? " (capped)" : "");
  }
}

void setup() {
  Serial.begin(115200);
  for (int k = 0; k < deck::NUM_KEYS; k++) pinMode(deck::KEY_PINS[k], INPUT_PULLUP);
  analogReadResolution(12);
  analogSetPinAttenuation(deck::POT_WIPER, ADC_11db);
  strip.begin();
  strip.clear();
  strip.show();
  delay(1500);   // give the console time to attach
  Serial.println("03_keys_leds: chain walk");
  for (int i = 0; i < DECK_NUM_LEDS; i++) {
    strip.clear();
    strip.setPixelColor(i, 40, 40, 40);
    strip.show();
    Serial.printf("  LED %d on\n", i);
    delay(300);
  }
  show();
}

void loop() {
  static uint8_t count[deck::NUM_KEYS];
  static uint32_t first_change[deck::NUM_KEYS];
  bool dirty = false;
  uint32_t now = millis();

  for (int k = 0; k < deck::NUM_KEYS; k++) {
    bool raw = digitalRead(deck::KEY_PINS[k]) == LOW;
    if (raw == pressed[k]) {
      count[k] = 0;
      continue;
    }
    if (count[k] == 0) first_change[k] = now;
    if (++count[k] >= DECK_DEBOUNCE_MS) {
      pressed[k] = raw;
      count[k] = 0;
      dirty = true;
      Serial.printf("K%d %s (GPIO%d, settled after %lu ms)\n", k + 1, raw ? "down" : "up",
                    deck::KEY_PINS[k], (unsigned long)(now - first_change[k]));
    }
  }

  static uint32_t last_pot = 0;
  static float ema = -1;
  if (now - last_pot >= 20) {
    last_pot = now;
    uint32_t mv = 0;
    for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(deck::POT_WIPER);
    mv /= 8;
    float v = constrain(mv / 3100.0f, 0.0f, 1.0f);
    ema = ema < 0 ? v : ema + 0.25f * (v - ema);
    int b = ema * 255 + 0.5f;
    if (abs(b - brightness) >= 3 || (b == 0 && brightness) || (b == 255 && brightness != 255)) {
      brightness = b;
      dirty = true;
      Serial.printf("pot %4lu mV -> brightness %d\n", (unsigned long)mv, brightness);
    }
  }

  if (dirty) show();
  delay(1);
}
