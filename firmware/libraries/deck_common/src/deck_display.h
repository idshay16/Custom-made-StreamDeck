// deck_display.h — LovyanGFX driver for the 4.3" 480x272 RA8875 on the
// ESP32-S3 LCD_CAM 8080 8-bit bus.
//
// LovyanGFX ships Panel_RA8875 with an 800x480 timing table. This subclass
// swaps in the 480x272 timings (values from Adafruit_RA8875's 480x272
// profile, which matches the common 4.3" panels) and fixes HDWR, which the
// stock init writes as width/8 instead of width/8 - 1.
//
// Only one task may touch the display (the UI task owns it).
#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "deck_pins.h"
#include "deck_config.h"

namespace deck {

class Panel_RA8875_480x272 : public lgfx::Panel_RA8875 {
 public:
  Panel_RA8875_480x272() {
    _cfg.memory_width = _cfg.panel_width = 480;
    _cfg.memory_height = _cfg.panel_height = 272;
  }

  bool init(bool use_reset) override {
    if (!lgfx::Panel_RA8875::init(use_reset)) return false;
    beginTransaction();
    _write_reg(0x14, (_cfg.panel_width / 8) - 1);   // HDWR: width = (HDWR+1)*8
    endTransaction();
    return true;
  }

  // RA8875 PWM1 duty = backlight (0..255).
  void setBacklight(uint8_t v) {
    beginTransaction();
    _write_reg(0x8B, v);
    endTransaction();
  }

 protected:
  const uint8_t *getInitCommands(uint8_t listno) const override {
    static constexpr uint8_t list0[] = {
      0x88, 1 + CMD_INIT_DELAY, 0x0A, 1,   // PLLC1: PLLDIV1, x11
      0x89, 1 + CMD_INIT_DELAY, 0x02, 1,   // PLLC2: /4
      0x04, 1 + CMD_INIT_DELAY, 0x82, 1,   // PCSR: falling-edge fetch, PCLK = SYS/4
      0x15, 1, 0x00,                       // HNDFTR (DE high, finetune 0)
      0x16, 1, 0x01,                       // HNDR  = (10 - 0 - 2) / 8
      0x17, 1, 0x00,                       // HSTR  = 8/8 - 1
      0x18, 1, 0x05,                       // HPWR  = low, 48/8 - 1
      0x1B, 1, 0x02,                       // VNDR0 = 3 - 1
      0x1C, 1, 0x00,                       // VNDR1
      0x1D, 1, 0x07,                       // VSTR0 = 8 - 1
      0x1E, 1, 0x00,                       // VSTR1
      0x1F, 1, 0x09,                       // VPWR  = low, 10 - 1
      0xC7, 1, 0x01,                       // GPIOX high: display enable on most boards
      0x8A, 1, 0x80,                       // PWM1 on
      0x8A, 1, 0x81,                       // PWM1 on, clk /2
      0x8B, 1, 0x80,                       // backlight 50 %
      0x01, 1, 0x80,                       // display on
      0xFF, 0xFF,
    };
    return listno == 0 ? list0 : nullptr;
  }
};

class Display : public lgfx::LGFX_Device {
 public:
  Panel_RA8875_480x272 panel;
  lgfx::Bus_Parallel8 bus;

  Display() {
    auto b = bus.config();
    b.freq_write = DECK_LCD_WRITE_HZ;
    b.pin_wr = LCD_WR;
    b.pin_rd = -1;          // RD# tied to 3V3: never read back
    b.pin_rs = LCD_RS;
    for (int i = 0; i < 8; i++) b.pin_data[i] = LCD_D[i];
    bus.config(b);
    panel.setBus(&bus);

    auto p = panel.config();
    p.pin_cs = -1;          // CS# tied to GND
    p.pin_rst = LCD_RST;
    p.pin_busy = LCD_WAIT;
    p.panel_width = p.memory_width = 480;
    p.panel_height = p.memory_height = 272;
    p.readable = false;
    p.bus_shared = false;
    panel.config(p);
    setPanel(&panel);
  }

  // RA8875 drives WAIT# high when idle. With no display attached the pin
  // floats and follows the internal pull resistor. Call BEFORE init().
  static bool present() {
    pinMode(LCD_WAIT, INPUT_PULLDOWN);
    delayMicroseconds(200);
    bool lo = digitalRead(LCD_WAIT);
    pinMode(LCD_WAIT, INPUT_PULLUP);
    delayMicroseconds(200);
    bool hi = digitalRead(LCD_WAIT);
    pinMode(LCD_WAIT, INPUT);
    return lo && hi;   // driven high both times => something is there
  }
};

}  // namespace deck
