/*
 * touch_calib.c - resistive touch calibration and filtering (see touch_calib.h).
 */
#include "touch_calib/touch_calib.h"

#define FIX_ONE 65536 /* 16.16 fixed point */

/* num/den in 16.16, rounded to nearest (halves away from zero). */
static int32_t fix_div(int64_t num, int64_t den)
{
    const int64_t scaled = num * FIX_ONE;
    const bool negative = (scaled < 0) != (den < 0);
    const int64_t n = scaled < 0 ? -scaled : scaled;
    const int64_t d = den < 0 ? -den : den;
    const int64_t q = (n + d / 2) / d;
    return (int32_t)(negative ? -q : q);
}

/* One screen axis from one raw axis: raw lo..hi onto 0..size-1, mirrored if asked. Gives the
 * coefficient on that raw axis and the constant. */
static void axis(uint16_t size, uint16_t lo, uint16_t hi, bool mirror, int32_t *k, int32_t *c)
{
    int64_t span = (int64_t)hi - (int64_t)lo;
    if (span == 0) {
        span = 1;
    }
    const int64_t last = (int64_t)(size > 0u ? size - 1u : 0u);
    if (!mirror) {
        *k = fix_div(last, span);                      /* (raw - lo) * last / span */
        *c = (int32_t)(-(int64_t)lo * (int64_t)(*k));
    } else {
        *k = fix_div(-last, span);                     /* last - (raw - lo) * last / span */
        *c = (int32_t)(last * FIX_ONE - (int64_t)lo * (int64_t)(*k));
    }
}

void touch_calib_linear(touch_calib_t *out, uint16_t width, uint16_t height, uint16_t x_min,
                        uint16_t x_max, uint16_t y_min, uint16_t y_max, bool swap_xy,
                        bool mirror_x, bool mirror_y)
{
    int32_t kx, cx, ky, cy;
    /* x_min/x_max are the raw readings at the screen's left and right edges on the raw axis
     * that runs along the screen's x (the raw y axis when swapped), and likewise for y. */
    axis(width, x_min, x_max, mirror_x, &kx, &cx);
    axis(height, y_min, y_max, mirror_y, &ky, &cy);
    out->width = width;
    out->height = height;
    if (!swap_xy) {
        out->a = kx; out->b = 0;  out->c = cx;
        out->d = 0;  out->e = ky; out->f = cy;
    } else {
        out->a = 0;  out->b = kx; out->c = cx;
        out->d = ky; out->e = 0;  out->f = cy;
    }
}

bool touch_calib_solve(touch_calib_t *out, uint16_t width, uint16_t height,
                       const touch_calib_pair_t pairs[3])
{
    const int64_t x0 = pairs[0].raw_x, y0 = pairs[0].raw_y;
    const int64_t x1 = pairs[1].raw_x, y1 = pairs[1].raw_y;
    const int64_t x2 = pairs[2].raw_x, y2 = pairs[2].raw_y;
    const int64_t det = x0 * (y1 - y2) + x1 * (y2 - y0) + x2 * (y0 - y1);
    /* Twice the raw triangle's area: below about a tenth of a 12-bit screen's, the targets
     * are nearly on a line and the solution would amplify noise. */
    const int64_t min_det = 4096 * 4096 / 50;
    if (det > -min_det && det < min_det) {
        return false;
    }
    for (int axis_i = 0; axis_i < 2; axis_i++) {
        const int64_t s0 = axis_i == 0 ? pairs[0].screen_x : pairs[0].screen_y;
        const int64_t s1 = axis_i == 0 ? pairs[1].screen_x : pairs[1].screen_y;
        const int64_t s2 = axis_i == 0 ? pairs[2].screen_x : pairs[2].screen_y;
        const int32_t k_x = fix_div(s0 * (y1 - y2) + s1 * (y2 - y0) + s2 * (y0 - y1), det);
        const int32_t k_y = fix_div(x0 * (s1 - s2) + x1 * (s2 - s0) + x2 * (s0 - s1), det);
        const int32_t k_c = fix_div(x0 * (y1 * s2 - y2 * s1) + x1 * (y2 * s0 - y0 * s2) +
                                        x2 * (y0 * s1 - y1 * s0),
                                    det);
        if (axis_i == 0) {
            out->a = k_x; out->b = k_y; out->c = k_c;
        } else {
            out->d = k_x; out->e = k_y; out->f = k_c;
        }
    }
    out->width = width;
    out->height = height;
    return true;
}

static int16_t clamp(int64_t v, uint16_t size)
{
    const int64_t last = size > 0u ? (int64_t)size - 1 : 0;
    return (int16_t)(v < 0 ? 0 : v > last ? last : v);
}

void touch_calib_map(const touch_calib_t *cal, uint16_t raw_x, uint16_t raw_y, int16_t *x,
                     int16_t *y)
{
    const int64_t rx = raw_x, ry = raw_y;
    const int64_t half = FIX_ONE / 2; /* round to nearest */
    const int64_t sx = ((int64_t)cal->a * rx + (int64_t)cal->b * ry + cal->c + half) >> 16;
    const int64_t sy = ((int64_t)cal->d * rx + (int64_t)cal->e * ry + cal->f + half) >> 16;
    *x = clamp(sx, cal->width);
    *y = clamp(sy, cal->height);
}

uint16_t touch_pressure(const touch_raw_t *raw)
{
    /* The XPT2046's two pressure readings: z1 rises and z2 falls as the film is pressed
     * harder. 4095 + z1 - z2 is 0 with nothing on the film and grows with the press. */
    if (raw->z1 == 0u) {
        return 0u;
    }
    const int32_t z = 4095 + (int32_t)raw->z1 - (int32_t)raw->z2;
    return (uint16_t)(z < 0 ? 0 : z > 4095 ? 4095 : z);
}

uint16_t touch_median(const uint16_t *samples, uint32_t n)
{
    uint16_t s[TOUCH_SAMPLES];
    if (n == 0u) {
        return 0u;
    }
    if (n > TOUCH_SAMPLES) {
        n = TOUCH_SAMPLES;
    }
    for (uint32_t i = 0; i < n; i++) {
        s[i] = samples[i];
    }
    for (uint32_t i = 1; i < n; i++) { /* insertion sort: five values */
        const uint16_t v = s[i];
        uint32_t j = i;
        while (j > 0u && s[j - 1u] > v) {
            s[j] = s[j - 1u];
            j--;
        }
        s[j] = v;
    }
    return s[n / 2u];
}

void touch_filter_init(touch_filter_t *f, const touch_filter_cfg_t *cfg)
{
    f->cfg = *cfg;
    f->pressed = false;
    f->settling = false;
    f->quiet = 0;
    f->x = 0;
    f->y = 0;
}

static int16_t distance(int16_t a, int16_t b)
{
    return (int16_t)(a > b ? a - b : b - a);
}

bool touch_filter_step(touch_filter_t *f, const touch_calib_t *cal, const touch_raw_t *raw,
                       int16_t *x, int16_t *y)
{
    const bool pressure = touch_pressure(raw) >= f->cfg.press_min;
    if (!pressure) {
        f->settling = false;
        if (f->pressed) {
            f->quiet++;
            if (f->quiet >= f->cfg.release_polls) {
                f->pressed = false;
                f->quiet = 0;
            }
        }
        *x = f->x;
        *y = f->y;
        return f->pressed;
    }
    f->quiet = 0;
    if (!f->pressed && !f->settling) {
        f->settling = true; /* the first poll after touch-down isn't settled: drop it */
        *x = f->x;
        *y = f->y;
        return false;
    }
    int16_t nx, ny;
    touch_calib_map(cal, touch_median(raw->x, TOUCH_SAMPLES), touch_median(raw->y, TOUCH_SAMPLES),
                    &nx, &ny);
    if (!f->pressed) {
        f->pressed = true; /* a new press lands where it is, no dead band */
        f->settling = false;
        f->x = nx;
        f->y = ny;
    } else if (distance(nx, f->x) > f->cfg.dead_band || distance(ny, f->y) > f->cfg.dead_band) {
        f->x = nx;
        f->y = ny;
    }
    *x = f->x;
    *y = f->y;
    return true;
}
