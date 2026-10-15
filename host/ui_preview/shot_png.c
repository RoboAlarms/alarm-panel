/*
 * shot_png.c - the PC programs' screenshot encoder: LVGL's copy of LodePNG, compiled here on the
 * C library's heap.
 *
 * LVGL's own copy allocates from LVGL's heap, which the PC builds cap at the board's size (its
 * manifest's CONFIG_LV_MEM_SIZE_KILOBYTES) so that a UI too big for the board runs out on the
 * PC first. An encoder that only the PC programs run must not spend that memory: it would run
 * the UI out of room the board never lacks, and spoil the measurement of what the UI needs.
 * lv_conf.h leaves LVGL's copy out (LV_USE_LODEPNG 0); this is the same code with the same
 * settings, so the PNG files are the same.
 */
#include "shot_png.h"

#include <stdlib.h>

#include "lvgl.h"

#undef LV_USE_LODEPNG
#define LV_USE_LODEPNG 1
#define LODEPNG_NO_COMPILE_ALLOCATORS
#include "src/libs/lodepng/lodepng.c"

void *lodepng_malloc(size_t size)
{
    return malloc(size);
}

void *lodepng_realloc(void *ptr, size_t new_size)
{
    return realloc(ptr, new_size);
}

void lodepng_free(void *ptr)
{
    free(ptr);
}

unsigned shot_png_encode(unsigned char **out, size_t *size, const unsigned char *rgb, unsigned w,
                         unsigned h)
{
    return lodepng_encode24(out, size, rgb, w, h);
}

void shot_png_free(void *png)
{
    free(png);
}
