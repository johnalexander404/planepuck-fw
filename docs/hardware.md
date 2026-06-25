# Hardware & boards

PlanePuck targets ESP32-S3 touchscreen "gadget" boards. The codebase is board-agnostic behind the
`puck::` HAL ([architecture.md](architecture.md)); each board has an `[env:…]` +
`boards/<id>/` implementation.

## M5Stack CoreS3 — supported (default)
The reference board (`pio run -e m5stack-cores3`). 320×240 capacitive IPS, 16 MB flash + PSRAM,
AXP2101 PMIC, BMI270 IMU (magnetometer optional → heading-up radar only when present), BM8563 RTC,
speaker. One **physical button**: the power key (used for nav — [buttons.md](buttons.md)). The
**CoreS3 SE** works from the same binary: it has no LTR-553 light sensor, so the firmware
auto-detects that and falls back to the time-of-day brightness schedule.

## Waveshare ESP32-S3-Touch-LCD-1.85C-BOX — scaffolded (stubs)
`pio run -e waveshare_185c_box` **builds** but the drivers are stubs — it won't drive the panel yet.
A round display board (N16R8). Bringing it up (real ST77916 QSPI display, CST816S touch, ES8311
audio, PCF85063 RTC, battery ADC, TCA9554 expander) is the next milestone; several details (display
IC, touch address, expander mapping, battery pin, button count) are **unconfirmed** and need the
schematic — see `HAL_REFACTOR_PLAN.md` §6. It has no magnetometer/light sensor (radar stays north-up,
brightness is time-based). To finish it, fill in `boards/waveshare_1_85c_box/*.cpp`.

## First flash
OTA can't bootstrap a blank chip, so the **first** flash is over USB:
```bash
pio run -e m5stack-cores3 -t upload
```
Or, with no toolchain, build the merged image + serve the **ESP Web Tools** installer
(`tools/webinstall/`) from your firmware host and flash from desktop Chrome/Edge — see
[`tools/OTA-SETUP.md`](../tools/OTA-SETUP.md) "First burn". After that, updates are
[over-the-air](ota.md).

## Adding another board
Copy `boards/waveshare_1_85c_box/` as a starting skeleton, implement the `puck::` interfaces, add an
`[env:…]`, and give it a unique `PUCK_BOARD_ID` / `PUCK_OTA_SUFFIX` so its OTA stays isolated. Full
steps in [architecture.md](architecture.md#add-a-board).
