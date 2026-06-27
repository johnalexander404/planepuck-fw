#include <Arduino.h>
#include <Wire.h>
#include <LovyanGFX.hpp>
#include "Panel_ST77916.hpp"
#include "hal/Display.h"

// Waveshare ESP32-S3-Touch-LCD-1.85C — 360x360 round ST77916 over QSPI.
// Pin map + reset path confirmed against the shipping xiaozhi board support (two independent sources):
//   QSPI:  SCLK 40, CS 21, D0 46, D1 45, D2 42, D3 41   (SPI2_HOST)
//   RST:   none direct — pulsed via TCA9554 EXIO0 (see resetPanelViaExpander)
//   BL:    GPIO 5 (PWM, non-inverted)
//   TCA9554 @ 0x20 on I2C0 (SDA 11 / SCL 10): EXIO0 = LCD reset, EXIO1 = touch reset.
// Touch (CST816 on the same I2C0) is added in Touch.cpp / Phase 2 — display first.

namespace {

class LGFX_Waveshare185C : public lgfx::LGFX_Device {
  lgfx::Bus_SPI         _bus;
  lgfx::Panel_ST77916   _panel;
  lgfx::Light_PWM       _light;
  lgfx::Touch_CST816S   _touch;
public:
  LGFX_Waveshare185C() {
    {                                   // ---- QSPI bus ----
      auto c = _bus.config();
      c.spi_host   = SPI2_HOST;
      c.spi_mode   = 0;
      c.freq_write = 40000000;          // 40 MHz to start; bump once stable
      c.freq_read  = 16000000;
      c.spi_3wire  = false;
      c.use_lock   = true;
      c.pin_sclk   = 40;
      c.pin_io0    = 46;                // D0 (MOSI lane)
      c.pin_io1    = 45;                // D1
      c.pin_io2    = 42;                // D2
      c.pin_io3    = 41;                // D3
      c.pin_mosi   = -1;
      c.pin_miso   = -1;
      c.pin_dc     = -1;                // QSPI: command/data framed in-band, no DC pin
      _bus.config(c);
      _panel.setBus(&_bus);
    }
    {                                   // ---- panel ----
      auto c = _panel.config();
      c.pin_cs        = 21;
      c.pin_rst       = -1;             // reset is on the TCA9554, pulsed in begin()
      c.pin_busy      = -1;
      c.panel_width   = 360;
      c.panel_height  = 360;
      c.memory_width  = 360;
      c.memory_height = 360;
      c.offset_x      = 0;
      c.offset_y      = 0;
      c.offset_rotation = 0;
      c.readable      = true;           // Panel_AMOLED is RAM-framebuffer-backed, so it IS readable;
                                        // anti-aliased fills/fonts drawn directly to the panel
                                        // (launcher chips, titles) need read-back to blend edges.
      c.invert        = false;          // INVON is issued inside the init list
      c.rgb_order     = false;          // RGB (panel default)
      c.dlen_16bit    = false;
      c.bus_shared    = false;
      _panel.config(c);
    }
    {                                   // ---- backlight (PWM on GPIO 5) ----
      auto c = _light.config();
      c.pin_bl      = 5;
      c.invert      = false;
      c.freq        = 12000;
      c.pwm_channel = 7;
      _light.config(c);
      _panel.setLight(&_light);
    }
    {                                   // ---- touch (CST816 on I2C0, shared SDA11/SCL10) ----
      auto c = _touch.config();
      c.i2c_port  = 0;                  // Wire is released (Wire.end) before init -> LGFX owns I2C0
      c.pin_sda   = 11;
      c.pin_scl   = 10;
      c.i2c_addr  = 0x15;
      c.freq      = 400000;
      c.pin_int   = -1;                 // poll every frame (continuous coords for radar drag)
      c.pin_rst   = -1;                 // touch reset is on TCA9554 EXIO1, pulsed in begin()
      c.bus_shared = false;
      c.offset_rotation = 0;            // orientation already matches the display (no rotate/flip)
      // The CST816 reports 8-bit raw coords (0..255); map that range onto the 360px panel. (Setting
      // these to 359 compressed every tap to ~70% — verified with an on-screen touch tester.)
      c.x_min = 0; c.x_max = 255;
      c.y_min = 0; c.y_max = 255;
      _touch.config(c);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }

  // Route all drawing through the Panel_AMOLED RAM framebuffer. The bare Panel_AMOLED writes straight
  // to QSPI and its readRect() is empty {} -> anti-aliased fills/fonts (launcher chips, titles) drawn
  // directly garble because edge-blending can't read the background. The framebuffer wrapper draws into
  // readable PSRAM (real readRect) and flushes dirty regions to QSPI, so AA works. (Official LGFX
  // pattern, see lgfx_user/Lilygo_T_Display_S3_AMOLED.) auto_display=true -> each draw self-flushes.
  bool enableFrameBuffer() {
    if (!_panel.initPanelFb()) return false;
    auto fb = _panel.getPanelFb();
    if (!fb) return false;
    fb->setBus(&_bus);
    fb->setAutoDisplay(true);
    setPanel(fb);
    return true;
  }
};

LGFX_Waveshare185C gPanel;

// --- TCA9554 reset pulse (EXIO0 = LCD, EXIO1 = touch), matching the vendor reset sequence ---
inline void tcaReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(0x20);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}
void resetPanelViaExpander() {
  Wire.begin(11 /*SDA*/, 10 /*SCL*/, 400000);
  tcaReg(0x01, 0x03);          // output latch: EXIO0,1 high
  tcaReg(0x03, 0xFC);          // config: EXIO0,1 = output (low bits 0), rest input
  delay(20);
  tcaReg(0x01, 0x00);          // assert reset (both low)
  delay(20);
  tcaReg(0x01, 0x03);          // release reset (both high)
  delay(150);
  Wire.end();                  // hand I2C0 (SDA11/SCL10) back for the touch driver
}

} // namespace

namespace puck {
lgfx::LGFX_Device& display() { return gPanel; }
namespace Display {
  void begin() {
    resetPanelViaExpander();   // must precede init(): LGFX can't drive the expander-routed RST
    gPanel.init();
    if (!gPanel.enableFrameBuffer()) log_e("[disp] framebuffer alloc failed -> AA will garble");
    gPanel.setRotation(0);     // tune on-device if upside-down / mirrored
    // Inversion MUST be set here, post-init: the init-list 0x20/0x21 is swallowed by the Panel_AMOLED
    // framebuffer path (verified on-device — toggling it did nothing), but invertDisplay() after init
    // sticks. This panel needs INVON for correct (non-one's-complement) colours.
    gPanel.invertDisplay(true);
    gPanel.setBrightness(255);
  }
  bool isRound() { return true; }
}
} // namespace puck
