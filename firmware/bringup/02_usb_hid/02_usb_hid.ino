// Bring-up step 2: USB HID keyboard + media keys + CDC console.
//
// Wiring: one female-female dupont wire on a GND pin. Slide its other end
// onto pin 35 (K1) = key press; pull it off = release. Nothing else.
// Cable: the board's "USB" port (native USB), NOT "COM".
//
//   .\scripts\build.ps1 firmware\bringup\02_usb_hid -Port COMx -Usb
//   (first time, while 01_chipinfo still runs, -Usb works via 1200-baud touch;
//    if the board doesn't show up: hold BOOT, tap RST, upload WITHOUT -Usb)
//
// The PC should now see a composite device "DIY StreamDeck": keyboard +
// consumer control (media keys) + a COM port (console).
//
// K1 press -> Play/Pause (media key; Windows shows the media overlay).
//
// Console commands (arduino-cli monitor -p COMx, type + Enter). Each one waits
// 3 s first so you can click into Notepad:
//   type <text>   types text via HID           e.g. type Hello abc 123
//   run <cmd>     Win+R, types cmd, Enter      e.g. run notepad
//   f13 .. f24    taps that function key
//   mute / vol+ / vol- / play
//   help
//
// What to check:
//   - K1 toggles media playback.
//   - "type abc" in Notepad with your English layout, then switch Windows to
//     Hebrew (Alt+Shift) and repeat. HID sends key POSITIONS, not letters, so
//     under Hebrew you'll likely get Hebrew letters. That decides how the
//     "launch app" action works later.
//   - console prints the key edge -> HID report time in microseconds.

#include <Arduino.h>
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <USBHIDConsumerControl.h>
#include <deck_pins.h>
#include <deck_config.h>

USBHIDKeyboard Keyboard;
USBHIDConsumerControl Consumer;

static void tap_consumer(uint16_t code) {
  Consumer.press(code);
  delay(10);
  Consumer.release();
}

static void tap_key(uint8_t k) {
  Keyboard.press(k);
  delay(10);
  Keyboard.release(k);
}

static void countdown() {
  for (int i = 3; i > 0; i--) {
    Serial.printf("  %d...\n", i);
    delay(1000);
  }
}

static void run_command(String line) {
  line.trim();
  if (line.isEmpty()) return;
  String cmd = line, arg;
  int sp = line.indexOf(' ');
  if (sp > 0) { cmd = line.substring(0, sp); arg = line.substring(sp + 1); }
  cmd.toLowerCase();

  if (cmd == "help") {
    Serial.println("type <text> | run <cmd> | f13..f24 | mute | vol+ | vol- | play");
    return;
  }
  Serial.printf("> %s\n", line.c_str());
  countdown();

  if (cmd == "type") {
    Keyboard.print(arg);
  } else if (cmd == "run") {
    Keyboard.press(KEY_LEFT_GUI);
    Keyboard.press('r');
    delay(20);
    Keyboard.releaseAll();
    delay(500);                      // Run dialog needs a moment to open
    Keyboard.print(arg);
    delay(50);
    tap_key(KEY_RETURN);
  } else if (cmd.length() >= 3 && cmd[0] == 'f') {
    int n = cmd.substring(1).toInt();
    if (n >= 13 && n <= 24) tap_key(KEY_F13 + (n - 13));
    else { Serial.println("  only f13..f24"); return; }
  } else if (cmd == "mute") tap_consumer(CONSUMER_CONTROL_MUTE);
  else if (cmd == "vol+")   tap_consumer(CONSUMER_CONTROL_VOLUME_INCREMENT);
  else if (cmd == "vol-")   tap_consumer(CONSUMER_CONTROL_VOLUME_DECREMENT);
  else if (cmd == "play")   tap_consumer(CONSUMER_CONTROL_PLAY_PAUSE);
  else { Serial.println("  unknown, try: help"); return; }
  Serial.println("  sent");
}

void setup() {
  pinMode(deck::KEY_PINS[0], INPUT_PULLUP);

  USB.productName("DIY StreamDeck");
  USB.manufacturerName("ishay");
  Keyboard.begin();
  Consumer.begin();
  USB.begin();
  Serial.begin(115200);
}

void loop() {
  // --- K1: integrator debounce, act on the press edge ----------------------
  static bool pressed = false;
  static uint32_t stable_since = 0;
  static bool raw_last = false;

  bool raw = digitalRead(deck::KEY_PINS[0]) == LOW;
  uint32_t now = millis();
  if (raw != raw_last) { raw_last = raw; stable_since = now; }
  if (raw != pressed && now - stable_since >= DECK_DEBOUNCE_MS) {
    pressed = raw;
    if (pressed) {
      uint32_t t0 = micros();
      Consumer.press(CONSUMER_CONTROL_PLAY_PAUSE);
      uint32_t dt = micros() - t0;
      Serial.printf("K1 down -> Play/Pause (report queued in %lu us)\n", (unsigned long)dt);
    } else {
      Consumer.release();
      Serial.println("K1 up");
    }
  }

  // --- console --------------------------------------------------------------
  static String buf;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { run_command(buf); buf = ""; }
    else if (buf.length() < 200) buf += c;
  }
  static uint32_t hello = 0;
  if (now - hello > 10000) { hello = now; Serial.println("02_usb_hid alive. type 'help'"); }
}
