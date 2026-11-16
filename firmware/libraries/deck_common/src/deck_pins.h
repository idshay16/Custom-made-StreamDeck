// deck_pins.h — THE wiring spec for the stream deck. Single source of truth.
//
// Assemble the hardware from this file. Change a pin here and the firmware
// follows; static_asserts at the bottom reject illegal choices at compile time.
// Never hard-code a GPIO anywhere else.
//
// Board: ESP32-S3 DevKitC-1 clone, ports labelled "COM" and "USB".
// esptool: ESP32-S3 (QFN56) rev v0.2, embedded 2 MB quad PSRAM (AP_3v3),
// 8 MB GigaDevice quad flash (JEDEC c8 4017). I.e. N8R2, NOT the N16R8 the
// build guide lists. Quad PSRAM shares the flash bus, so GPIO35-37 are free.
// Header silkscreen shows GPIO numbers directly.
//
// ============================================================================
//  POWER
// ============================================================================
//   PC USB-C (panel mount) -> DevKit "USB" port (native USB, GPIO19/20).
//   DevKit 5V pin  -> WS2812 strip +5V, display board VCC only if the board
//                     wants 5V (the LT043C "3.3v" variant takes 3V3 — check
//                     its silkscreen in bring-up 04).
//   DevKit 3V3 pin -> pot end A, RA8875 RD (tied high), display 3V3.
//   DevKit GND     -> breadboard GND rail; everything shares it.
//   USB budget is 500 mA total: LED brightness is hard-capped in deck_config.h.
//
// ============================================================================
//  SIGNALS
// ============================================================================
//   Signal          GPIO   Goes to
//   --------------  -----  ---------------------------------------------------
//   POT_WIPER       1      10k pot middle pin (ends: 3V3 and GND). ADC1_CH0.
//   LED_DIN         2      WS2812 chain DIN (first node). 3.3 V logic into a
//                          5 V strip: usually fine on node 1; if it flickers,
//                          add a 74AHCT125 on the breadboard.
//   LCD_D0..D7      4..11  RA8875 DB0..DB7 (8080 8-bit bus)
//   LCD_RS          12     RA8875 RS (a.k.a. DC / A0)
//   LCD_WR          13     RA8875 WR#
//   LCD_WAIT        14     RA8875 WAIT#  (busy, active low)
//   LCD_RST         21     RA8875 RESET#
//   (RA8875 CS# -> GND, RD# -> 3V3: only device on the bus, never read back.)
//   KEY_1..KEY_12   see KEY_PINS  switch leg 1; leg 2 -> GND. INPUT_PULLUP.
//
//   Key numbering = physical layout seen from the front, row-major:
//     K1  K2  K3  K4
//     K5  K6  K7  K8
//     K9  K10 K11 K12
//
// ============================================================================
//  RESERVED on ESP32-S3 N16R8 — do not assign
// ============================================================================
//   0          BOOT button (strapping). Spare; usable as input only.
//   3          strapping (JTAG source select, only if eFuse says so). Spare.
//   19, 20     native USB D-/D+ (HID + CDC to the PC)
//   26..32     SPI flash + quad PSRAM
//   33, 34     not broken out on the DevKitC header
//   36, 37     free (would be octal PSRAM on an R8 board). Spare.
//   43, 44     UART0 TX/RX — kept free for recovery flashing via the "UART" port
//   45, 46     strapping (VDD_SPI voltage, boot mode)
//   38 or 48   on-board RGB LED (v1.1 / v1.0). We use it as a key input; the
//              LED's DIN just sees a static level. Harmless.
#pragma once
#include <stdint.h>

namespace deck {

// --- analog / LEDs ----------------------------------------------------------
constexpr int POT_WIPER = 1;
constexpr int LED_DIN   = 2;

// --- RA8875 8080 parallel bus ----------------------------------------------
constexpr int LCD_D[8]  = {4, 5, 6, 7, 8, 9, 10, 11};
constexpr int LCD_RS    = 12;
constexpr int LCD_WR    = 13;
constexpr int LCD_WAIT  = 14;
constexpr int LCD_RST   = 21;

// --- keys -------------------------------------------------------------------
constexpr int NUM_KEYS  = 12;
constexpr int KEY_PINS[NUM_KEYS] = {
  35, 15, 16, 17,   // K1..K4   top row
  18, 38, 39, 40,   // K5..K8   middle row
  41, 42, 47, 48,   // K9..K12  bottom row
};

// --- LED chain order --------------------------------------------------------
// LED_INDEX[k] = position in the WS2812 chain of the LED under key K(k+1).
// Default assumes the string runs K1->K12 row by row. If you lay it as a
// snake (K1..K4, then K8..K5, then K9..K12) use {0,1,2,3, 7,6,5,4, 8,9,10,11}.
constexpr int LED_INDEX[NUM_KEYS] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

// ============================================================================
//  compile-time checks
// ============================================================================
constexpr bool is_reserved(int gp) {
  return gp == 0 || gp == 3 || gp == 19 || gp == 20 || (gp >= 22 && gp <= 34) ||
         gp == 43 || gp == 44 || gp == 45 || gp == 46 || gp < 0 || gp > 48;
}
constexpr bool is_adc1(int gp) { return gp >= 1 && gp <= 10; }  // ADC2 is unusable with Wi-Fi on

constexpr int ALL_PINS[] = {
  POT_WIPER, LED_DIN,
  LCD_D[0], LCD_D[1], LCD_D[2], LCD_D[3], LCD_D[4], LCD_D[5], LCD_D[6], LCD_D[7],
  LCD_RS, LCD_WR, LCD_WAIT, LCD_RST,
  KEY_PINS[0], KEY_PINS[1], KEY_PINS[2],  KEY_PINS[3],  KEY_PINS[4],  KEY_PINS[5],
  KEY_PINS[6], KEY_PINS[7], KEY_PINS[8],  KEY_PINS[9],  KEY_PINS[10], KEY_PINS[11],
};
constexpr int NUM_PINS = sizeof(ALL_PINS) / sizeof(ALL_PINS[0]);

constexpr bool none_reserved(int i = 0) {
  return i >= NUM_PINS || (!is_reserved(ALL_PINS[i]) && none_reserved(i + 1));
}
constexpr bool all_unique(int i = 0, int j = 1) {
  return i >= NUM_PINS ? true
       : j >= NUM_PINS ? all_unique(i + 1, i + 2)
       : (ALL_PINS[i] != ALL_PINS[j] && all_unique(i, j + 1));
}

static_assert(none_reserved(), "deck_pins.h: a pin is on flash/PSRAM/USB/UART0/strapping");
static_assert(all_unique(),    "deck_pins.h: a GPIO is assigned twice");
static_assert(is_adc1(POT_WIPER), "deck_pins.h: POT_WIPER must be an ADC1 pin (GPIO1-10)");

}  // namespace deck
