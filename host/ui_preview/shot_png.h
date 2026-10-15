/*
 * shot_png.h - screenshots of the panel's screen as PNG files, for the PC programs (the preview
 * and the simulator). The encoder uses the C library's heap, never the board's LVGL heap
 * (shot_png.c says why).
 */
#pragma once

#include <stddef.h>

/* 24-bit RGB rows (w x h) into a PNG in memory; 0 when it worked. Free *out with shot_png_free. */
unsigned shot_png_encode(unsigned char **out, size_t *size, const unsigned char *rgb, unsigned w,
                         unsigned h);
void shot_png_free(void *png);
