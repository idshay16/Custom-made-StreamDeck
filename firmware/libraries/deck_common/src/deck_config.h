// deck_config.h — tunables. Each can be overridden with -D (build.ps1 -Define).
// The Arduino IDE can't pass -D: edit the defaults here instead.
#pragma once

// --- keys -------------------------------------------------------------------
#ifndef DECK_DEBOUNCE_MS
#define DECK_DEBOUNCE_MS 5          // integrator debounce window
#endif
#ifndef DECK_LONGPRESS_MS
#define DECK_LONGPRESS_MS 500
#endif

// --- LEDs -------------------------------------------------------------------
#ifndef DECK_NUM_LEDS
#define DECK_NUM_LEDS 12            // 10 in hand; chain the 2 extra when they arrive
#endif
// Hard cap on the LED chain's estimated current, in mA. Whole deck runs off
// one USB port (500 mA); ESP32 + Wi-Fi + display backlight take the rest.
#ifndef DECK_LED_MAX_MA
#define DECK_LED_MAX_MA 250
#endif

// --- display ----------------------------------------------------------------
#ifndef DECK_LCD_WRITE_HZ
#define DECK_LCD_WRITE_HZ 16000000  // 8080 WR clock; RA8875 init runs at 5 MHz regardless
#endif
#ifndef DECK_BACKLIGHT_MIN
#define DECK_BACKLIGHT_MIN 8        // knob fully down never blanks the screen completely
#endif
