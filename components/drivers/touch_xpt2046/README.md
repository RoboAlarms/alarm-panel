# touch_xpt2046

**Tier:** drivers (ESP-IDF) · **Features:** [19 4" ESP32 panel](../../../docs/features/19-CYD-ESP32-Port/README.md)
(CYD-009) · **Tests:** the filtering and calibration it feeds are host-tested in
[`touch_calib`](../../core/touch_calib/test/); the driver itself on the board

The XPT2046 resistive touch controller on an SPI bus, as the 4" boards carry it (sharing the
display's bus). One call reads a poll: `TOUCH_SAMPLES` raw readings of each axis and the two
pressure readings (`touch_raw_t`), with the controller powered down afterwards so its pen
interrupt is armed. The poll holds the bus for its dozen conversions, so the display's
transfers wait rather than interleave. Turning the raw poll into a steady screen point
(pressure threshold, median, settling, release debounce, calibration) is
[`touch_calib`](../../core/touch_calib)'s job; the board glues the two into LVGL.

## Public API
| Header | What it offers |
|---|---|
| `include/touch_xpt2046/touch_xpt2046.h` | `touch_xpt2046_open` (add to a bus), `touch_xpt2046_read` (one poll), `touch_xpt2046_close`; `TOUCH_XPT2046_CLOCK_HZ` |

## Depends on
- Project components: `touch_calib` (the raw poll's type)
- ESP-IDF: `esp_driver_spi`

## Options
None.

## Rules
- Poll it from one task (the LVGL task on the 4" board); it knows nothing about the screen.
- The pen interrupt line is not used: pressure decides (`touch_pressure`), which is steadier.
