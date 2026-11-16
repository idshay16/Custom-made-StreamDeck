// Bring-up step 4: the 4.3" RA8875 display on the 8080 8-bit bus.
//
// BEFORE WIRING: check the display board.
//   - Interface select: many RA8875 boards choose SPI vs 8080 parallel with
//     solder jumpers or 0-ohm resistors (labels like J1..J8, "PS", "SPI",
//     "8080", "IF"). It must be set to 8080 8-bit. Photo it and check the
//     seller's pinout/datasheet.
//   - Supply: the "3.3v" LT043C takes 3V3 for logic. Check whether the
//     backlight wants 3V3 or 5V on its own pin.
//
// Wiring (female-female dupont, ESP32 header -> display header):
//   DB0..DB7 <- GPIO4..11    RS <- GPIO12    WR# <- GPIO13
//   WAIT# -> GPIO14          RESET# <- GPIO21
//   CS# -> GND               RD# -> 3V3
//   VCC/GND per the board's silkscreen (see above)
//
// What it does: detects the panel (WAIT pin), inits it, shows color bars,
// a grid + text (alignment check), runs a full-screen fill benchmark, then
// sweeps the backlight. Repeats every few seconds. Console prints results.
//
// What to check:
//   - "display detected: yes". "no" = WAIT# not wired, or panel not powered
//   - color bars red/green/blue/white in that order. Swapped colors = data
//     lines out of order. Garbage = wrong interface mode or loose wire
//   - the border is fully visible on all 4 edges: if it's cut off or the
//     picture is shifted, the 480x272 timings in deck_display.h need tuning
//   - fill benchmark: expect roughly 15-40 full frames/s
//   - backlight dims and brightens

#include <Arduino.h>
#include <deck_display.h>

deck::Display lcd;

void setup() {
  Serial.begin(115200);
  delay(1500);
  bool present = deck::Display::present();
  Serial.printf("04_display: display detected: %s\n", present ? "yes" : "NO (check WAIT#, power)");
  if (!lcd.init()) Serial.println("lcd.init() failed");
  lcd.setRotation(0);
  Serial.printf("panel %d x %d\n", lcd.width(), lcd.height());
}

static void color_bars() {
  const uint32_t c[] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE};
  int w = lcd.width() / 4;
  for (int i = 0; i < 4; i++) lcd.fillRect(i * w, 0, w, lcd.height(), c[i]);
  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextColor(TFT_BLACK);
  lcd.setTextDatum(middle_center);
  lcd.drawString("R  G  B  W", lcd.width() / 2, lcd.height() / 2);
}

static void grid() {
  lcd.fillScreen(TFT_BLACK);
  for (int x = 0; x < lcd.width(); x += 40) lcd.drawFastVLine(x, 0, lcd.height(), 0x4208);
  for (int y = 0; y < lcd.height(); y += 40) lcd.drawFastHLine(0, y, lcd.width(), 0x4208);
  lcd.drawRect(0, 0, lcd.width(), lcd.height(), TFT_YELLOW);
  lcd.drawRect(1, 1, lcd.width() - 2, lcd.height() - 2, TFT_YELLOW);
  lcd.setFont(&fonts::FreeSansBold9pt7b);
  lcd.setTextColor(TFT_WHITE);
  lcd.setTextDatum(top_left);
  lcd.drawString("top-left", 4, 4);
  lcd.setTextDatum(bottom_right);
  lcd.drawString("bottom-right", lcd.width() - 4, lcd.height() - 4);
  lcd.setTextDatum(middle_center);
  lcd.drawString("yellow border must be fully visible", lcd.width() / 2, lcd.height() / 2);
}

static void bench() {
  const uint32_t c[] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_BLACK};
  uint32_t t0 = millis();
  const int N = 20;
  for (int i = 0; i < N; i++) lcd.fillScreen(c[i % 4]);
  uint32_t dt = millis() - t0;
  Serial.printf("fill benchmark: %d frames in %lu ms = %.1f fps\n", N, (unsigned long)dt, N * 1000.0f / dt);
}

void loop() {
  color_bars();
  delay(2500);
  grid();
  delay(3000);
  bench();
  grid();
  for (int b = 255; b >= 8; b -= 8) {
    lcd.panel.setBacklight(b);
    delay(20);
  }
  for (int b = 8; b <= 255; b += 8) {
    lcd.panel.setBacklight(b);
    delay(20);
  }
  lcd.panel.setBacklight(200);
  delay(1000);
}
