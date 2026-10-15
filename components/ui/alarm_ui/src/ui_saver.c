/*
 * ui_saver.c - the screen saver's picture (docs/features/07 UI-009).
 *
 * The photo is a baseline JPEG from the picture pack (part `photo`, asked of the backend's
 * asset_picture hook; the firmware build makes the pack from images/saver_forest.jpg, the PC
 * builds hand over the embedded copy made with tools/embed_image.py). The first time the screen
 * saver shows, LVGL's bundled TJPGD decodes it once into an RGB565 image in LVGL's heap (1.2 MB,
 * PSRAM on the panel), so redrawing the clock never decodes again. Pictures from the SD card
 * (later) go through the same decoder. Without the memory the saver keeps its plain dark
 * background.
 */
#include <string.h>

#include "src/libs/tjpgd/tjpgd.h"
#include "ui_internal.h"

#define TJPGD_POOL 4096u /* TJPGD's working memory, as LVGL's own decoder uses */

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
    uint16_t *out;
    uint32_t width;
} jpeg_job_t;

static lv_image_dsc_t *s_picture;
static bool s_tried;
static uint32_t s_generation; /* the backend's asset generation the last try saw */

static size_t jpeg_in(JDEC *jd, uint8_t *buf, size_t n)
{
    jpeg_job_t *job = jd->device;
    if (n > job->size - job->pos) {
        n = job->size - job->pos;
    }
    if (buf != NULL) {
        memcpy(buf, job->data + job->pos, n);
    }
    job->pos += n;
    return n;
}

/* TJPGD hands over blocks of B, G, R bytes. They become RGB565 with a 4 x 4 ordered dither, so
 * a dark night sky doesn't show bands at the saver's low backlight (the pattern is fixed: it
 * never moves or flickers). */
static int jpeg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    static const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    jpeg_job_t *job = jd->device;
    const uint8_t *p = bitmap;
    for (uint32_t y = rect->top; y <= rect->bottom; y++) {
        uint16_t *row = job->out + y * job->width;
        for (uint32_t x = rect->left; x <= rect->right; x++) {
            const uint32_t t = BAYER[y & 3u][x & 3u];
            uint32_t b = p[0] + (t >> 1u); /* 5-bit channel: steps of 8 */
            uint32_t g = p[1] + (t >> 2u); /* 6-bit channel: steps of 4 */
            uint32_t r = p[2] + (t >> 1u);
            b = b > 255u ? 255u : b;
            g = g > 255u ? 255u : g;
            r = r > 255u ? 255u : r;
            row[x] = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
            p += 3;
        }
    }
    return 1;
}

/* A baseline JPEG in memory as an RGB565 image, or NULL. */
static lv_image_dsc_t *decode_jpeg(const uint8_t *data, size_t size)
{
    jpeg_job_t job = {.data = data, .size = size};
    JDEC jd;
    void *pool = lv_malloc(TJPGD_POOL);
    if (pool == NULL) {
        return NULL;
    }
    lv_image_dsc_t *dsc = NULL;
    uint16_t *pixels = NULL;
    if (jd_prepare(&jd, jpeg_in, pool, TJPGD_POOL, &job) == JDR_OK) {
        const size_t bytes = (size_t)jd.width * jd.height * 2u;
        pixels = lv_malloc(bytes);
        dsc = lv_malloc_zeroed(sizeof(*dsc));
        job.out = pixels;
        job.width = jd.width;
        if (pixels != NULL && dsc != NULL && jd_decomp(&jd, jpeg_out, 0) == JDR_OK) {
            dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
            dsc->header.cf = LV_COLOR_FORMAT_RGB565;
            dsc->header.w = jd.width;
            dsc->header.h = jd.height;
            dsc->header.stride = (uint32_t)jd.width * 2u;
            dsc->data = (const uint8_t *)pixels;
            dsc->data_size = (uint32_t)bytes;
            pixels = NULL; /* now owned by dsc */
        } else {
            lv_free(dsc);
            dsc = NULL;
        }
    }
    lv_free(pixels);
    lv_free(pool);
    return dsc;
}

void ui_saver_prepare(void)
{
    /* A picture pack installed while the panel runs changes the generation: try once more, and
     * only then (the decode never runs at every showing). */
    const uint32_t generation = S.be.asset_generation != NULL ? S.be.asset_generation(S.be.ctx) : 0u;
    if (s_tried && s_picture == NULL && generation != s_generation) {
        s_tried = false;
    }
    if (!s_tried) {
        s_tried = true;
        s_generation = generation;
        const void *jpeg = NULL;
        size_t jpeg_size = 0;
        const uint32_t start = lv_tick_get();
        LV_UNUSED(start); /* only the log reads it, and the log can be compiled out */
        if (S.be.asset_picture != NULL && S.be.asset_picture(S.be.ctx, "photo", &jpeg, &jpeg_size)) {
            s_picture = decode_jpeg(jpeg, jpeg_size);
        }
        if (s_picture != NULL) {
            LV_LOG_USER("screen saver picture decoded in %u ms", (unsigned)lv_tick_elaps(start));
        } else {
            LV_LOG_WARN("screen saver picture not decoded: plain background");
        }
    }
    lv_obj_t *bg = ui_find(S.saver, "saver_bg");
    if (bg != NULL && s_picture != NULL && lv_image_get_src(bg) != s_picture) {
        lv_image_set_src(bg, s_picture);
    }
}
