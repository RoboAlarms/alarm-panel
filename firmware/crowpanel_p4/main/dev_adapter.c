#include "sdkconfig.h"
#if CONFIG_ALARM_DEV_REMOTE || CONFIG_ALARM_PANEL_WEB
#include "board.h"
#include "dev_remote.h"
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_h264_alloc.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "h264_sps/h264_sps.h"
#include "panel_web.h"
#if CONFIG_ALARM_FEATURE_UI
#include "ui_app.h"
#include "ui_port_esp.h"
#endif
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#if CONFIG_ALARM_ZSRC_ZWAVE
/* Z-Wave on the enrolled link (docs/features/30 "Z-Wave"): the panel runs the radio, the
 * workstation drives and records. The trace is development builds' only. */
#include "zwave_service.h"
_Static_assert(ZWAVE_SERVICE_STATE_JSON_MAX <= DEV_ZWAVE_STATE_MAX, "the state fits its buffer");
_Static_assert(ZWAVE_SERVICE_TRACE_JSON_MAX <= DEV_ZWAVE_TRACE_MAX, "a trace fits its buffer");
_Static_assert(ZWAVE_SERVICE_ANSWER_JSON_MAX <= DEV_ZWAVE_ANSWER_MAX, "an answer fits its buffer");
#endif

/* A headless build (CONFIG_ALARM_FEATURE_UI off, S09) has no screen to show or press: the
 * links start without the remote screen, its input and its video, and keep the rest. */
#if CONFIG_ALARM_FEATURE_UI
static const char *TAG = "dev_adapter";

static lv_indev_t *input;
static bool held, pressed;
static uint8_t owner; /* 1 enrolled developer transport, 2 browser transport */
static uint16_t px, py;
static uint32_t seq, touches;
static int64_t expires;
/* A tap's release, timed here rather than by the network (0: none). A press held from one
 * request to the next lasted as long as the network took, and on a VPN that was long enough for
 * the on-screen keyboard to repeat the key (doubled letters seen 2026-09-23). */
static int64_t release_at;
/* The last touch at the panel's own screen: remote presses wait LOCAL_GRACE_US after it. */
static int64_t local_at;
static uint32_t local_presses;
/* The person at the panel paused the remote (the banner). It lasts until they answer, or
 * remote access goes off: a remote that disconnects and comes back is still paused. */
static atomic_bool paused;
static bool web_session;           /* a Panel Manager remote keypad is open */
static char web_who[32], web_where[48];

#define LOCAL_GRACE_US  3000000 /* a touch at the panel keeps the remote out this long */
#define HOLD_LEASE_US   2000000 /* a held remote press not kept up is let go */
#define IDLE_LEASE_US   15000000
#define TAP_MIN_MS      30
#define TAP_MAX_MS      250     /* well under LVGL's 400 ms long press */

/* Every screen can be viewed and operated remotely (decided 2026-09-22): the viewer
 * is signed in, the panel's own pages are protected by the codes typed on them, and since
 * 2026-09-23 a red banner tells whoever is at the panel (ui_app_remote_session). */

/* The banner: the Panel Manager's session, else the workstation while it holds the screen. */
static void banner(void)
{
    ui_remote_session_t s;
    memset(&s, 0, sizeof(s));
    s.paused = atomic_load(&paused);
    if (web_session) {
        s.active = true;
        snprintf(s.who, sizeof(s.who), "%s", web_who);
        snprintf(s.where, sizeof(s.where), "%s", web_where);
    } else if (held && owner == 1) {
        s.active = true;
        snprintf(s.who, sizeof(s.who), "Development workstation");
    }
    ui_app_remote_session(&s);
}

static void release(void)
{
    if (held) {
        const bool workstation = owner == 1;
        held = false;
        owner = 0;
        pressed = false;
        release_at = 0;
        lv_indev_reset(input, NULL);
        ui_dev_control_end();
        if (workstation) {
            banner();
        }
    }
}
static void read_input(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    board_touch_stats_t s;
    board_touch_stats(&s, false);
    const int64_t now = esp_timer_get_time();
    if (s.pressed || s.presses != local_presses) {
        local_at = now;
        local_presses = s.presses;
    }
    /* The Panel Manager's open session is its lease: someone reading a page doesn't lose it by
     * not pressing. A press held down still ends two seconds after its last word (a lost "up"). */
    const bool lease_over = now > expires && !(owner == 2 && web_session && !pressed);
    if (held && (lease_over || s.pressed || s.presses != touches || atomic_load(&paused)))
        release();
    if (held && pressed && release_at != 0 && now >= release_at) {
        pressed = false; /* the tap ends here, however late the network is */
        release_at = 0;
    }
    data->point.x = px;
    data->point.y = py;
    data->state = held && pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
/* The screen for requester, if the panel lets it have it. UI lock held. */
static dev_input_t take(uint8_t requester)
{
    if (atomic_load(&paused))
        return DEV_INPUT_PAUSED;
    if (held)
        return owner == requester ? DEV_INPUT_OK : DEV_INPUT_OTHER;
    board_touch_stats_t s;
    board_touch_stats(&s, false);
    if (s.pressed || esp_timer_get_time() - local_at < LOCAL_GRACE_US)
        return DEV_INPUT_LOCAL;
    if (!ui_dev_control_begin())
        return DEV_INPUT_BUSY;
    held = true;
    owner = requester;
    pressed = false;
    release_at = 0;
    seq = 0;
    touches = s.presses;
    expires = esp_timer_get_time() + IDLE_LEASE_US;
    if (requester == 1)
        banner();
    return DEV_INPUT_OK;
}
static bool control_for(uint8_t requester, bool acquire)
{
    if (!board_ui_lock(100))
        return false;
    bool ok = true;
    if (!acquire) {
        if (owner == requester)
            release();
    } else
        ok = take(requester) == DEV_INPUT_OK;
    if (ok && acquire)
        expires = esp_timer_get_time() + IDLE_LEASE_US;
    board_ui_unlock();
    return ok;
}
static bool pointer_for(uint8_t requester, uint32_t next, uint16_t x, uint16_t y, bool down)
{
    if (!board_ui_lock(100))
        return false;
    bool ok = held && owner == requester && esp_timer_get_time() < expires && next > seq &&
              !atomic_load(&paused);
    if (ok) {
        seq = next;
        px = x;
        py = y;
        pressed = down;
        release_at = 0;
        expires = esp_timer_get_time() + HOLD_LEASE_US;
        /* Consume every down/up synchronously under the same lock as LVGL. */
        lv_indev_read(input);
    }
    board_ui_unlock();
    return ok;
}
static bool control(bool acquire)
{
    return control_for(1, acquire);
}
static bool web_control(bool acquire)
{
    return control_for(2, acquire);
}
static bool web_renew(void)
{
    if (!board_ui_lock(100))
        return false;
    board_touch_stats_t s;
    board_touch_stats(&s, false);
    bool ok = held && owner == 2 && esp_timer_get_time() < expires && !s.pressed &&
              s.presses == touches && !atomic_load(&paused);
    if (ok && !pressed)
        expires = esp_timer_get_time() + IDLE_LEASE_US;
    else if (!ok && owner == 2)
        release();
    board_ui_unlock();
    return ok;
}
static bool pointer(uint32_t seq, uint16_t x, uint16_t y, bool pressed)
{
    return pointer_for(1, seq, x, y, pressed);
}
static bool web_pointer(uint32_t seq, uint16_t x, uint16_t y, bool pressed)
{
    return pointer_for(2, seq, x, y, pressed);
}

/* ---- the Panel Manager's remote keypad session (WebSocket) ---------------------------------- */

static dev_input_t web_tap(uint16_t x, uint16_t y, uint16_t hold_ms)
{
    if (x >= BOARD_LCD_H_RES || y >= BOARD_LCD_V_RES)
        return DEV_INPUT_BUSY;
    if (!board_ui_lock(100))
        return DEV_INPUT_BUSY;
    const dev_input_t r = take(2);
    if (r == DEV_INPUT_OK) {
        const int64_t now = esp_timer_get_time();
        if (pressed) { /* the last tap still down: let it go first */
            pressed = false;
            release_at = 0;
            lv_indev_read(input);
        }
        const uint16_t ms = hold_ms < TAP_MIN_MS ? TAP_MIN_MS : hold_ms > TAP_MAX_MS ? TAP_MAX_MS : hold_ms;
        px = x;
        py = y;
        pressed = true;
        release_at = now + (int64_t)ms * 1000;
        expires = now + IDLE_LEASE_US;
        lv_indev_read(input); /* LVGL sees the press now; read_input lets it go at release_at */
    }
    board_ui_unlock();
    return r;
}
static dev_input_t web_press(uint16_t x, uint16_t y, bool down)
{
    if (x >= BOARD_LCD_H_RES || y >= BOARD_LCD_V_RES)
        return DEV_INPUT_BUSY;
    if (!board_ui_lock(100))
        return DEV_INPUT_BUSY;
    dev_input_t r;
    if (!down) /* letting go is always allowed, and never takes the screen */
        r = held && owner == 2 ? DEV_INPUT_OK : DEV_INPUT_BUSY;
    else
        r = take(2);
    if (r == DEV_INPUT_OK) {
        px = x;
        py = y;
        pressed = down;
        release_at = 0;
        expires = esp_timer_get_time() + (down ? HOLD_LEASE_US : IDLE_LEASE_US);
        lv_indev_read(input);
    }
    board_ui_unlock();
    return r;
}
static dev_input_t web_text(const char *utf8)
{
    if (!board_ui_lock(100))
        return DEV_INPUT_BUSY;
    dev_input_t r = take(2);
    if (r == DEV_INPUT_OK) {
        expires = esp_timer_get_time() + IDLE_LEASE_US;
        (void)ui_app_remote_text(utf8); /* nothing on screen takes text: nothing happens */
    }
    board_ui_unlock();
    return r;
}
static dev_input_t web_key(uint8_t key)
{
    if (key > DEV_KEY_ESCAPE)
        return DEV_INPUT_BUSY;
    if (!board_ui_lock(100))
        return DEV_INPUT_BUSY;
    dev_input_t r = take(2);
    if (r == DEV_INPUT_OK) {
        expires = esp_timer_get_time() + IDLE_LEASE_US;
        (void)ui_app_remote_key((ui_remote_key_t)key);
    }
    board_ui_unlock();
    return r;
}
static void web_session_changed(bool active, const char *who, const char *where)
{
    /* The banner must follow the session: wait for the screen rather than skip it. */
    while (!board_ui_lock(1000))
        ESP_LOGW(TAG, "remote keypad banner waits for the screen");
    web_session = active;
    snprintf(web_who, sizeof(web_who), "%s", active && who ? who : "");
    snprintf(web_where, sizeof(web_where), "%s", active && where ? where : "");
    if (!active && owner == 2)
        release();
    banner();
    board_ui_unlock();
}
static void web_pause(bool p)
{
    /* From the UI (the banner's question, on the LVGL task): the next read lets go. */
    atomic_store(&paused, p);
}

/* ---- capture -------------------------------------------------------------------------------- */

static esp_err_t frame(uint16_t *out, size_t count)
{
    if (!board_ui_lock(100))
        return ESP_ERR_TIMEOUT;
    esp_err_t result = board_dev_frame(out, count) ? ESP_OK : ESP_ERR_TIMEOUT;
    board_ui_unlock();
    return result;
}
/* The whole screen into out when LVGL drew something since *seen (or always, when forced).
 * ESP_ERR_NOT_FOUND: unchanged. The copy is taken under the UI lock, so it is one frame. */
static esp_err_t capture_changed(uint16_t *out, uint32_t *seen, bool *have, bool force)
{
    board_display_stats_t st;
    board_display_stats(&st, false);
    if (*have && !force && st.refreshes == *seen)
        return ESP_ERR_NOT_FOUND;
    if (!board_ui_lock(100))
        return ESP_ERR_TIMEOUT;
    board_display_stats(&st, false); /* LVGL is held: nothing is drawing now */
    const bool ok = board_dev_frame(out, BOARD_LCD_H_RES * BOARD_LCD_V_RES);
    board_ui_unlock();
    if (!ok)
        return ESP_ERR_TIMEOUT; /* a flush in flight: the next try gets the finished frame */
    *seen = st.refreshes;
    *have = true;
    return ESP_OK;
}
#endif /* CONFIG_ALARM_FEATURE_UI */

static void metrics(char *out, size_t cap)
{
    board_display_stats_t s;
    board_display_stats(&s, false);
    /* backlight: the level the screen is at now, in percent (docs/features/07 UI-043) */
    snprintf(out, cap,
             "{\"frames\":%lu,\"late\":%lu,\"guard_trips\":%lu,\"forced_off\":%s,"
             "\"backlight\":%u}",
             (unsigned long)s.frames, (unsigned long)s.late, (unsigned long)s.guard_trips,
             s.forced_off ? "true" : "false", (unsigned)s.backlight);
}
#if CONFIG_ALARM_FEATURE_UI
static void preferences_changed(void)
{
    ui_options_t opt;
    ui_port_esp_options(&opt);
    if (board_ui_lock(100)) {
        ui_app_apply_options(&opt);
        board_ui_unlock();
    }
}

/* ---- JPEG ------------------------------------------------------------------------------------ */

/* One encoder engine, two sets of buffers: GET /frame's (the HTTPS worker, which sends from
 * them after the call) and the remote keypad stream's (its task copies the picture out before
 * the next call). jpeg_lock covers the engine, its setup and each encode. */
typedef struct {
    uint8_t *raw, *jpg;
    size_t raw_size, jpg_size;
} jpeg_bufs_t;
static SemaphoreHandle_t jpeg_lock;
static jpeg_encoder_handle_t jpeg_engine;
static jpeg_bufs_t J, JS;
static esp_err_t jpeg_ready(jpeg_bufs_t *b)
{
    if (!jpeg_engine) {
        jpeg_encode_engine_cfg_t cfg = {.timeout_ms = 500};
        esp_err_t e = jpeg_new_encoder_engine(&cfg, &jpeg_engine);
        if (e != ESP_OK)
            return e;
    }
    if (!b->raw) {
        jpeg_encode_memory_alloc_cfg_t cfg = {.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER};
        b->raw = jpeg_alloc_encoder_mem(BOARD_LCD_H_RES * BOARD_LCD_V_RES * 2, &cfg, &b->raw_size);
    }
    if (!b->jpg) {
        jpeg_encode_memory_alloc_cfg_t cfg = {.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
        b->jpg = jpeg_alloc_encoder_mem(768 * 1024, &cfg, &b->jpg_size);
    }
    return b->raw && b->jpg ? ESP_OK : ESP_ERR_NO_MEM;
}
static esp_err_t jpeg_encode(jpeg_bufs_t *b, const uint8_t **data, size_t *length)
{
    jpeg_encode_cfg_t cfg = {.width = BOARD_LCD_H_RES,
                             .height = BOARD_LCD_V_RES,
                             .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
                             .sub_sample = JPEG_DOWN_SAMPLING_YUV444,
                             .image_quality = 85,
                             .pixel_reverse = false};
    uint32_t bytes = 0;
    esp_err_t e = jpeg_encoder_process(jpeg_engine, &cfg, b->raw, (uint32_t)b->raw_size, b->jpg,
                                       (uint32_t)b->jpg_size, &bytes);
    if (e == ESP_OK) {
        *data = b->jpg;
        *length = bytes;
    }
    return e;
}
static void jpeg_free(jpeg_bufs_t *b)
{
    heap_caps_free(b->raw);
    heap_caps_free(b->jpg);
    memset(b, 0, sizeof(*b));
}
static esp_err_t jpeg_frame(const uint8_t **data, size_t *length)
{
    /* Copy at the safe render boundary, release LVGL, then compress; the caller transmits from
     * J outside any lock (only GET /frame, on the one HTTPS worker, uses J). */
    xSemaphoreTake(jpeg_lock, portMAX_DELAY);
    esp_err_t e = jpeg_ready(&J);
    if (e == ESP_OK)
        e = frame((uint16_t *)J.raw, BOARD_LCD_H_RES * BOARD_LCD_V_RES);
    if (e == ESP_OK)
        e = jpeg_encode(&J, data, length);
    xSemaphoreGive(jpeg_lock);
    return e;
}

/* ---- H.264 ----------------------------------------------------------------------------------
 * The P4's hardware encoder takes packed YUV 4:2:0 (O_UYY_E_VYY) on v1 silicon; the PPA turns
 * the RGB565 screen into exactly that. The encoder needs about 82 KB of internal RAM, which
 * the alarm keeps first (docs/bugs/B001): it exists only while someone watches, and not at all when
 * internal RAM is short (the viewer gets JPEG instead). */
#define ENC_W        BOARD_LCD_H_RES
#define ENC_H        BOARD_LCD_V_RES
#define ENC_H_MB     ((ENC_H + 15) & ~15) /* 608: whole macroblocks, the SPS crops the rest */
#define YUV_BYTES    (ENC_W * ENC_H_MB * 3 / 2)
#define RGB_BYTES    (ENC_W * ENC_H * 2)
#define H264_QP      26  /* fixed: text stays sharp, a still screen costs next to nothing */
#define H264_RESERVE (160 * 1024) /* internal RAM left for the alarm with the encoder running */

static struct {
    esp_h264_enc_handle_t enc;
    esp_h264_enc_param_hw_handle_t param;
    ppa_client_handle_t ppa;
    uint16_t *rgb;
    uint8_t *yuv, *bits, *out;
    uint32_t rgb_len, yuv_len, bits_len;
    size_t out_cap;
    uint32_t pts;
} H;
static uint32_t video_seen;
static bool video_have;

static void h264_close(void)
{
    if (H.enc) {
        esp_h264_enc_close(H.enc);
        esp_h264_enc_del(H.enc);
    }
    if (H.ppa)
        ppa_unregister_client(H.ppa);
    esp_h264_free(H.rgb);
    esp_h264_free(H.yuv);
    esp_h264_free(H.bits);
    heap_caps_free(H.out);
    memset(&H, 0, sizeof(H));
}
static esp_err_t h264_open(void)
{
    if (H.enc)
        return ESP_OK;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < H264_RESERVE)
        return ESP_ERR_NO_MEM;
    H.rgb = esp_h264_aligned_calloc(128, 1, RGB_BYTES, &H.rgb_len, ESP_H264_MEM_SPIRAM);
    H.yuv = esp_h264_aligned_calloc(128, 1, YUV_BYTES, &H.yuv_len, ESP_H264_MEM_SPIRAM);
    H.bits = esp_h264_aligned_calloc(128, 1, YUV_BYTES, &H.bits_len, ESP_H264_MEM_SPIRAM);
    H.out_cap = YUV_BYTES + H264_SPS_GROWTH;
    H.out = heap_caps_malloc(H.out_cap, MALLOC_CAP_SPIRAM);
    if (!H.rgb || !H.yuv || !H.bits || !H.out)
        goto fail;
    /* The eight lines below the screen, black once: the PPA writes only the screen's 600. The
     * CPU wrote them, so they go out to PSRAM now and the cache holds nothing dirty over the
     * buffer the PPA and the encoder share by DMA. */
    for (size_t row = ENC_H; row < ENC_H_MB; row++) {
        uint8_t *line = H.yuv + row * (ENC_W * 3 / 2);
        for (size_t i = 0; i < ENC_W / 2; i++) {
            line[i * 3] = 128;    /* U or V */
            line[i * 3 + 1] = 16; /* Y, Y: limited-range black */
            line[i * 3 + 2] = 16;
        }
    }
    esp_cache_msync(H.yuv, H.yuv_len, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    const ppa_client_config_t pc = {.oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1};
    if (ppa_register_client(&pc, &H.ppa) != ESP_OK)
        goto fail;
    const esp_h264_enc_cfg_hw_t cfg = {.pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
                                       .gop = 255,
                                       .fps = 15,
                                       .res = {.width = ENC_W, .height = ENC_H},
                                       .rc = {.bitrate = 2000000, .qp_min = H264_QP, .qp_max = H264_QP}};
    if (esp_h264_enc_hw_new(&cfg, &H.enc) != ESP_H264_ERR_OK || !H.enc ||
        esp_h264_enc_hw_get_param_hd(H.enc, &H.param) != ESP_H264_ERR_OK ||
        esp_h264_enc_open(H.enc) != ESP_H264_ERR_OK)
        goto fail;
    ESP_LOGI(TAG, "H.264 encoder ready: %ux%u QP %u, internal RAM free %u KB", ENC_W, ENC_H,
             H264_QP, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    return ESP_OK;
fail:
    ESP_LOGW(TAG, "H.264 encoder unavailable: the remote keypad uses JPEG");
    h264_close();
    return ESP_ERR_NO_MEM;
}
static esp_err_t h264_encode(bool key, const uint8_t **data, size_t *length, bool *is_key)
{
    esp_cache_msync(H.rgb, H.rgb_len, ESP_CACHE_MSYNC_FLAG_DIR_C2M); /* the CPU's copy, out */
    const ppa_srm_oper_config_t op = {
        .in = {.buffer = H.rgb, .pic_w = ENC_W, .pic_h = ENC_H, .block_w = ENC_W, .block_h = ENC_H,
               .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .out = {.buffer = H.yuv, .buffer_size = H.yuv_len, .pic_w = ENC_W, .pic_h = ENC_H_MB,
                .srm_cm = PPA_SRM_COLOR_MODE_YUV420, .yuv_range = PPA_COLOR_RANGE_LIMIT,
                .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601},
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    esp_err_t e = ppa_do_scale_rotate_mirror(H.ppa, &op);
    if (e != ESP_OK)
        return e;
    if (key)
        esp_h264_enc_force_idr(&H.param->base);
    esp_h264_enc_in_frame_t in = {.raw_data = {.buffer = H.yuv, .len = YUV_BYTES}, .pts = H.pts};
    esp_h264_enc_out_frame_t out = {.raw_data = {.buffer = H.bits, .len = H.bits_len}};
    H.pts += 66;
    if (esp_h264_enc_process(H.enc, &in, &out) != ESP_H264_ERR_OK || out.length == 0) {
        esp_h264_enc_force_idr(&H.param->base); /* whatever went wrong, start clean */
        return ESP_FAIL;
    }
    *is_key = out.frame_type == ESP_H264_FRAME_TYPE_IDR;
    *data = H.bits;
    *length = out.length;
    if (*is_key) { /* no reordering, one picture buffered, BT.601 (components/core/h264_sps) */
        const size_t n = h264_sps_low_delay(H.bits, out.length, H.out, H.out_cap);
        if (n != 0) {
            *data = H.out;
            *length = n;
        }
    }
    return ESP_OK;
}

static esp_err_t video(dev_video_codec_t codec, bool key, const uint8_t **data, size_t *length,
                       bool *is_key)
{
    if (codec == DEV_VIDEO_H264) {
        esp_err_t e = h264_open();
        if (e != ESP_OK)
            return e;
        e = capture_changed(H.rgb, &video_seen, &video_have, key);
        return e == ESP_OK ? h264_encode(key, data, length, is_key) : e;
    }
    xSemaphoreTake(jpeg_lock, portMAX_DELAY);
    esp_err_t e = jpeg_ready(&JS);
    if (e == ESP_OK)
        e = capture_changed((uint16_t *)JS.raw, &video_seen, &video_have, key);
    if (e == ESP_OK)
        e = jpeg_encode(&JS, data, length);
    xSemaphoreGive(jpeg_lock);
    *is_key = true;
    return e;
}
static void video_stop(void)
{
    h264_close();
    xSemaphoreTake(jpeg_lock, portMAX_DELAY);
    jpeg_free(&JS); /* 2 MB of PSRAM, only while someone watches */
    xSemaphoreGive(jpeg_lock);
    video_have = false;
}
#endif /* CONFIG_ALARM_FEATURE_UI */

esp_err_t dev_adapter_start(void)
{
#if CONFIG_ALARM_FEATURE_UI
    jpeg_lock = xSemaphoreCreateMutex();
    if (!jpeg_lock)
        return ESP_ERR_NO_MEM;
    if (!board_ui_lock(1000))
        return ESP_ERR_TIMEOUT;
    input = lv_indev_create();
    if (input) {
        lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(input, read_input);
        lv_indev_set_scroll_limit(input, 20);
        ui_app_remote_indev(input);
    }
    board_ui_unlock();
    if (!input)
        return ESP_ERR_NO_MEM;
#endif
    /* The board's own id ("crowpanel_p4", "crowpanel_10"): the Flasher trusts it to pick the image. */
    const dev_remote_adapter_t a = {.board = board_info()->id,
                                    .width = BOARD_LCD_H_RES,
                                    .height = BOARD_LCD_V_RES,
#if CONFIG_ALARM_FEATURE_UI
                                    .frame = frame,
                                    .control = control,
                                    .input = pointer,
                                    .jpeg = jpeg_frame,
                                    .frame_interval_ms = 100,
                                    .preferences_changed = preferences_changed,
#endif
                                    .metrics = metrics,
                                    .browser_grant = panel_web_issue_grant,
#if CONFIG_ALARM_ZSRC_ZWAVE
                                    .zwave_state = zwave_service_state_json,
                                    .zwave_request = zwave_service_request_json,
#if CONFIG_ALARM_BUILD_DEV
                                    .zwave_trace = zwave_service_trace_json,
#endif
#endif
    };
    esp_err_t result = dev_remote_start(&a);
    if (result != ESP_OK)
        return result;
    dev_remote_adapter_t web = a;
    /* The Panel Manager's Z-Wave page is served by panel_web itself (web_zwave.c, straight from
     * zwave_service), not through this adapter: nothing of the developer link's routes there. */
    web.zwave_state = NULL;
    web.zwave_trace = NULL;
    web.zwave_request = NULL;
#if CONFIG_ALARM_FEATURE_UI
    web.control = web_control;
    web.renew = web_renew;
    web.input = web_pointer;
    web.video_codecs = DEV_VIDEO_JPEG | DEV_VIDEO_H264;
    web.video = video;
    web.video_stop = video_stop;
    web.tap = web_tap;
    web.press = web_press;
    web.text = web_text;
    web.key = web_key;
    web.session = web_session_changed;
    web.pause = web_pause;
#endif
    return panel_web_start(&web);
}
#endif
