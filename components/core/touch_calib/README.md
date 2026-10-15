# touch_calib

**Tier:** core (portable C11) · **Features:** [19 4" ESP32 panel](../../../docs/features/19-CYD-ESP32-Port/README.md)
(CYD-008 calibration, CYD-009 filtering) · **Tests:** [`test/`](test/) (1 suite:
`touch_calib`)

Resistive touch, from a controller's raw readings to a steady point on the screen. A
resistive film gives a raw position per axis and a pressure, all noisy: the first reading after
touch-down isn't settled, a finger's pressure dips while it holds, and the raw axes run however
the panel was glued. This turns one poll of raw samples into what LVGL should see:

- **calibration**: raw -> screen through an affine map in 16.16 fixed point, from a plain raw
  range per axis with swap and mirrors (`touch_calib_linear`) or from three touched targets
  (`touch_calib_solve`, which refuses targets too close to a line);
- **filtering**: a pressure threshold (`touch_pressure`), the median of each poll's samples, the
  first poll after touch-down dropped, a release only after several quiet polls - so a 2 s hold
  (panic, factory reset) survives short dips - and a dead band that keeps a resting point
  still.

No heap, no globals, no ESP-IDF. The board's driver ([`touch_xpt2046`](../../drivers/touch_xpt2046))
reads the controller; the board owns the filter state and stores the calibration.

## Public API
| Header | What it offers |
|---|---|
| `include/touch_calib/touch_calib.h` | `touch_raw_t`, `touch_calib_t`, `touch_calib_pair_t`, `touch_filter_t`/`_cfg_t`; `touch_calib_linear`, `touch_calib_solve`, `touch_calib_map`, `touch_pressure`, `touch_median`, `touch_filter_init`, `touch_filter_step` |

## Depends on
- Project components: none
- ESP-IDF: none (portable)

## Options
None.

## Rules
- Tests are tagged with the docs/features/19 requirement IDs (CYD-008, CYD-009).
