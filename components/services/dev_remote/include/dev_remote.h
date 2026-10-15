#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The remote keypad's screen as a video stream (docs/features/31). */
typedef enum {
    DEV_VIDEO_JPEG = 1, /* every picture a whole JPEG */
    DEV_VIDEO_H264 = 2, /* H.264 constrained baseline, Annex B, key frames on request */
} dev_video_codec_t;

/* What became of a remote press, key or text. */
typedef enum {
    DEV_INPUT_OK = 0,
    DEV_INPUT_LOCAL,  /* someone is touching the panel's own screen: it wins for a moment */
    DEV_INPUT_BUSY,   /* someone at the panel has a code typed or a code-protected page open */
    DEV_INPUT_PAUSED, /* the person at the panel paused the remote keypad (its red banner) */
    DEV_INPUT_OTHER,  /* another remote (the enrolled workstation) has the screen */
} dev_input_t;

/* Remote keys (ui_remote_key_t's order). */
enum { DEV_KEY_BACKSPACE = 0, DEV_KEY_ENTER, DEV_KEY_ESCAPE };

/* The Z-Wave routes' bounds (docs/features/30 "Z-Wave"): the buffers the adapter's callbacks
 * write into (with the NUL), the request body the link accepts. */
#define DEV_ZWAVE_STATE_MAX  36864u /* GET /dev/v1/zwave */
#define DEV_ZWAVE_TRACE_MAX  12288u /* GET /dev/v1/zwave/trace: about 12 KB, 64 entries at most */
#define DEV_ZWAVE_BODY_MAX   256u   /* POST /dev/v1/zwave's body */
#define DEV_ZWAVE_ANSWER_MAX 64u    /* and its answer */

/* Task-level adapter. All buffers are caller-owned; no network work under a UI lock. */
typedef struct {
    const char *board; /* stable artifact family, no user-provided text */
    uint16_t width, height;
    /* ESP_OK, ESP_ERR_TIMEOUT (busy), ESP_ERR_INVALID_STATE (private view). */
    esp_err_t (*frame)(uint16_t *pixels, size_t count);
    bool (*control)(bool acquire);
    bool (*renew)(void); /* renew only an existing lease, never reacquire after local touch */
    bool (*input)(uint32_t sequence, uint16_t x, uint16_t y, bool pressed);
    void (*metrics)(char *json, size_t cap);
    esp_err_t (*jpeg)(const uint8_t **data, size_t *length);
    uint16_t frame_interval_ms; /* board-tested capture budget; zero uses conservative default */
    void (*preferences_changed)(void);
    bool (*browser_grant)(char one_time_token[65]);

    /* ---- the Panel Manager's remote keypad session (docs/features/31) ----
     * Being on the page is control: presses take the screen when the panel is free, a touch at
     * the panel's own screen takes it back for a few seconds, and the person at the panel can
     * pause the session with its red banner. */
    uint8_t video_codecs; /* DEV_VIDEO_* bits this board streams; 0: none */
    /* The screen, when it changed since the last call or key is asked for: ESP_OK with the
     * encoded picture in *data / *length (valid until the next call or video_stop()) and
     * whether it is a key picture; ESP_ERR_NOT_FOUND when nothing changed; anything else:
     * try again later. One caller (the stream task). */
    esp_err_t (*video)(dev_video_codec_t codec, bool key, const uint8_t **data, size_t *length,
                       bool *is_key);
    void (*video_stop)(void); /* the viewer left: the encoder and its buffers go */
    /* A tap: pressed and released by the panel itself after hold_ms, so a slow network can't
     * hold a key down long enough to repeat it. */
    dev_input_t (*tap)(uint16_t x, uint16_t y, uint16_t hold_ms);
    /* A press held (drags, a hold-to-confirm button): down, moves, then up. A press not kept up
     * by moves (or repeats) for two seconds is let go. */
    dev_input_t (*press)(uint16_t x, uint16_t y, bool down);
    /* A keyboard at the remote end: text into the sheet or code pad on screen, and keys
     * (DEV_KEY_*). */
    dev_input_t (*text)(const char *utf8);
    dev_input_t (*key)(uint8_t key);
    /* The session as the person at the panel sees it: the red banner while active. */
    void (*session)(bool active, const char *who, const char *where);
    /* Paused by the person at the panel: presses, keys and text are refused until it isn't. */
    void (*pause)(bool paused);

    /* ---- Z-Wave on the enrolled link (docs/features/30 "Z-Wave", /36) ----
     * All NULL: the firmware has no Z-Wave, its routes aren't served and "zwave" isn't among
     * info's capabilities. Called on the HTTPS worker, never under a lock of the link's; each
     * writes one JSON document into json (cap is the DEV_ZWAVE_*_MAX above, with the NUL). */
    /* GET /dev/v1/zwave: the radio, the devices and the operation running. Its length; 0 when
     * it could not be answered now. */
    size_t (*zwave_state)(char *json, size_t cap);
    /* GET /dev/v1/zwave/trace?since=N: the link's frames from sequence number N (development
     * builds; NULL: no trace, the route isn't served). Its length; 0 when it could not. */
    size_t (*zwave_trace)(uint64_t since, char *json, size_t cap);
    /* POST /dev/v1/zwave: a request (body: length bytes, NUL-terminated) from the adapter's
     * allowlist, allowed or refused by the panel's own rules; true with the answer in json,
     * false when the body is malformed or names no operation it knows (400). */
    bool (*zwave_request)(const char *body, size_t length, char *json, size_t cap);
} dev_remote_adapter_t;
void dev_remote_set_allowed(bool allowed);
esp_err_t dev_remote_enroll_public(const uint8_t *certificate, size_t length);
void dev_remote_public_identity(char *der_hex, size_t capacity);
size_t dev_remote_copy_log(char *out, size_t capacity);
esp_err_t dev_remote_start(const dev_remote_adapter_t *adapter);
