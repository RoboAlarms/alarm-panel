#pragma once
/*
 * touch_calib.h - resistive touch: from a controller's raw readings to a steady point on the
 * screen (docs/features/19 CYD-008, CYD-009).
 *
 * A resistive film (the XPT2046 on the 4" boards) gives a raw position per axis and a pressure,
 * all noisy: the first reading after touch-down is not settled, a finger's pressure dips while
 * it holds, and the raw axes run in whatever direction the panel was glued. This component turns
 * a poll of raw samples into what LVGL should see:
 *
 *   - calibration: raw -> screen through an affine map (16.16 fixed point), made from a
 *     plain raw range per axis (touch_calib_linear) or from three touched targets
 *     (touch_calib_solve, for the calibration screen);
 *   - filtering: a pressure threshold, the median of each poll's samples, the first poll after
 *     touch-down dropped, a release only after several quiet polls (a 2 s hold - panic, factory
 *     reset - survives short pressure dips), and a dead band that holds the point still while a
 *     finger rests.
 *
 * Portable C11: no heap, no globals, no ESP-IDF. The board's driver reads the controller and
 * owns the state.
 */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TOUCH_SAMPLES 5u /* raw samples per axis in one poll (the median is taken) */

/* One poll of the controller. */
typedef struct {
    uint16_t x[TOUCH_SAMPLES]; /* 12-bit raw positions */
    uint16_t y[TOUCH_SAMPLES];
    uint16_t z1, z2;           /* the two pressure measurements */
} touch_raw_t;

/* raw -> screen: sx = (a*rx + b*ry + c) >> 16, sy = (d*rx + e*ry + f) >> 16, clamped. */
typedef struct {
    int32_t a, b, c, d, e, f;
    uint16_t width, height; /* the screen */
} touch_calib_t;

/* A touched target: where the controller said and where the target was drawn. */
typedef struct {
    uint16_t raw_x, raw_y;
    uint16_t screen_x, screen_y;
} touch_calib_pair_t;

typedef struct {
    uint16_t press_min;    /* touch_pressure() at or above this: pressed */
    uint8_t release_polls; /* polls in a row below it before the finger counts as lifted */
    uint8_t dead_band;     /* px a resting point must move before the reported point moves */
} touch_filter_cfg_t;

typedef struct {
    touch_filter_cfg_t cfg;
    bool pressed;  /* what the filter reports */
    bool settling; /* pressure seen, the first (unsettled) poll dropped */
    uint8_t quiet; /* polls in a row without pressure while pressed */
    int16_t x, y;  /* the reported point (kept after a release: LVGL wants the last one) */
} touch_filter_t;

/* A map from a raw range per axis: raw x_min..x_max onto 0..width-1 (and y likewise), after
 * swapping the raw axes if the panel's run the other way, and mirroring either screen axis.
 * The ranges are the raw readings at the screen's edges; x_min may be larger than x_max. */
void touch_calib_linear(touch_calib_t *out, uint16_t width, uint16_t height, uint16_t x_min,
                        uint16_t x_max, uint16_t y_min, uint16_t y_max, bool swap_xy,
                        bool mirror_x, bool mirror_y);

/* The affine map through three touched targets. false (and *out untouched) when the targets
 * are too close to a line to tell the axes apart. */
bool touch_calib_solve(touch_calib_t *out, uint16_t width, uint16_t height,
                       const touch_calib_pair_t pairs[3]);

/* A raw position on the screen, clamped to it. */
void touch_calib_map(const touch_calib_t *cal, uint16_t raw_x, uint16_t raw_y, int16_t *x,
                     int16_t *y);

/* The pressure of a poll, 0 (nothing) .. 4095 (hard press). */
uint16_t touch_pressure(const touch_raw_t *raw);

/* The median of n (1..TOUCH_SAMPLES) samples. */
uint16_t touch_median(const uint16_t *samples, uint32_t n);

void touch_filter_init(touch_filter_t *f, const touch_filter_cfg_t *cfg);

/* One poll: true while the finger counts as down; *x, *y the point to report (the last one
 * during a pressure dip and after the release). */
bool touch_filter_step(touch_filter_t *f, const touch_calib_t *cal, const touch_raw_t *raw,
                       int16_t *x, int16_t *y);

#ifdef __cplusplus
}
#endif
