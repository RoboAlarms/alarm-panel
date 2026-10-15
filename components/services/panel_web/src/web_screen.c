/* The remote keypad over one WebSocket (docs/features/31, "Remote keypad").
 *
 * The page opens wss://<panel>/api/v1/screen with its sign-in cookie; its first message carries
 * the session's CSRF token and the video it can decode. From then on:
 *  - panel -> page, binary: one encoded picture per message, 12-byte header (type: 1 H.264 key,
 *    2 H.264, 3 JPEG; sequence; milliseconds) then the picture. A picture is sent only when the
 *    screen changed, at most two ahead of the page's acknowledgements, so a slow link lowers
 *    the rate instead of piling up. H.264 pictures are a few hundred bytes to a few kilobytes
 *    when little changes; the old JPEG frames were 100-200 KB each, every time.
 *  - page -> panel, text: JSON. {"t":"hello","csrf":..,"codec":"h264"|"jpeg"}, {"t":"ack","n":..},
 *    {"t":"key-frame"}, {"t":"view","on":bool}, {"t":"ping"}, and the input: {"t":"tap","x","y",
 *    "ms"}, {"t":"down"|"move"|"up","x","y"}, {"t":"text","s"}, {"t":"key","k":"backspace"|
 *    "enter"|"escape"}.
 *  - panel -> page, text: {"t":"hello",...}, {"t":"control","state":"ok"|"local"|"busy"|
 *    "paused"|"other"}, {"t":"codec","codec":..}, {"t":"bye","why":..} before the panel closes.
 * One viewer at a time; the same signed-in session opening it again takes it over.
 *
 * Threads. The HTTPS worker runs the WebSocket handler, the jobs queued to it and the server's
 * close callback (web_screen_closed); this file's task makes the pictures, so capture and
 * encoding never hold up the worker; the UI task pauses and resumes; panel_web's maintenance
 * task starts and stops the server. Two rules keep them apart:
 *  - `server` changes only under `lock`, and the other tasks reach the server only through
 *    httpd_queue_work() called under `lock` while `server` is set (it never waits here:
 *    CONFIG_HTTPD_QUEUE_WORK_BLOCKING is off). web_screen_stop() clears it before the server is
 *    stopped, so nothing reaches a stopped server.
 *  - A queued message or picture is for one viewer (V.id) on one socket (V.fd), and the worker
 *    checks both before sending. The close callback clears V.fd on the worker before a socket
 *    number can be used again, so nothing meant for the viewer reaches another connection. */
#include "web_screen.h"
#include "sdkconfig.h"
#if !CONFIG_HTTPD_WS_SUPPORT
/* Without WebSocket support in this build the remote keypad has no live screen. */
esp_err_t web_screen_setup(const dev_remote_adapter_t *adapter)
{
    (void)adapter;
    return ESP_OK;
}
void web_screen_register(httpd_handle_t server)
{
    (void)server;
}
void web_screen_stop(void) {}
void web_screen_stopped(void) {}
void web_screen_closed(int fd)
{
    (void)fd;
}
void web_screen_pause(bool paused)
{
    (void)paused;
}
#else
#include "alarm/alarm_types.h"
#include "alarm_mem.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_tls_errors.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i18n/i18n.h"
#include "lwip/sockets.h"
#include "mbedtls/platform_util.h"
#include "net_wifi.h"
#include "web_mem.h"
#include "web_rules.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "web_screen";

#define HEADER       12
#define WS_ROOM      10       /* a WebSocket frame header, at most */
#define AHEAD        2        /* pictures sent beyond the page's acknowledgements */
#define MIN_GAP_US   50000    /* at most 20 pictures a second */
#define QUIET_US     20000000 /* nothing from the page this long: it is gone */
#define SEND_WAIT_MS 15000    /* one message must leave within this, however slow the link */
#define PICTURE_MAX  (1024 * 608 * 3 / 2 + 64) /* the largest picture the adapter makes */
#define TEXT_MAX     256      /* the panel's own messages are shorter */

enum { PIC_H264_KEY = 1, PIC_H264 = 2, PIC_JPEG = 3 };

static dev_remote_adapter_t A;
static httpd_handle_t server;  /* the running server, NULL while none: under lock */
static SemaphoreHandle_t lock; /* server, V, P and pending[] */
static SemaphoreHandle_t done; /* given when the picture job has let go of the task's buffer */
static TaskHandle_t task;
static atomic_bool paused;   /* by the person at the panel, until they answer (not per session) */
static atomic_bool stopping; /* the server is being stopped: a send in progress gives up */
static uint32_t next_id;

/* A socket that finished the WebSocket handshake but hasn't said hello yet. */
typedef struct {
    int fd;
    const void *session;
    char token[65];
    uint8_t user;
    char who[ALARM_NAME_LEN], where[48];
} pending_t;
static pending_t pending[4];

/* The viewer. */
static struct {
    int fd;      /* -1: nobody; cleared by the close callback when its socket closes */
    uint32_t id; /* this viewer's number: work queued for an earlier one does nothing */
    const void *session;
    char token[65];
    char who[ALARM_NAME_LEN], where[48];
    dev_video_codec_t codec;
    bool viewing, want_key;
    bool ended; /* over (quiet, signed out, a failed send): its socket is being closed */
    uint32_t sent, acked;
    int64_t last_rx;
    dev_input_t reported; /* the control state the page last heard */
} V = {.fd = -1};

/* The picture on its way: one at a time, the task waits for `done` before making the next. */
static struct {
    bool queued; /* queued, and the worker hasn't run it yet */
    int fd;
    uint32_t id;
    const uint8_t *frame; /* in the task's buffer: WebSocket header, picture header, picture */
    size_t len;
} P;

/* ---- sending (the worker) ---------------------------------------------------------------- */

/* The header of an unfragmented, unmasked WebSocket frame of len bytes, written just before
 * body; returns where the frame starts. */
static uint8_t *ws_frame(uint8_t *body, httpd_ws_type_t type, size_t len)
{
    uint8_t h[WS_ROOM];
    size_t n;
    h[0] = (uint8_t)(0x80u | (unsigned)type); /* FIN: every message is one frame */
    if (len <= 125) {
        h[1] = (uint8_t)len;
        n = 2;
    } else if (len <= 0xFFFFu) {
        h[1] = 126;
        h[2] = (uint8_t)(len >> 8);
        h[3] = (uint8_t)len;
        n = 4;
    } else {
        h[1] = 127;
        for (unsigned i = 0; i < 8; i++)
            h[2 + i] = (uint8_t)((uint64_t)len >> (56 - 8 * i));
        n = 10;
    }
    memcpy(body - n, h, n);
    return body - n;
}
/* The connection web_send_all() writes to. */
typedef struct {
    httpd_handle_t hd;
    int fd;
} send_to_t;
/* One piece: one TLS record (it fits the record buffer) and one TCP segment (MSS 1440), small
 * enough for the room lwIP has when it calls the socket writable (over TCP_SNDLOWAT bytes
 * free; the record adds at most about 100 bytes of its own). */
#define WS_PIECE      1024u
#define WS_READY_WAIT 20000 /* us: how long one try waits for the socket to take a piece */
#if defined(CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN)
_Static_assert(WS_PIECE <= (unsigned)CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN,
               "a piece must be one TLS record");
#endif
#if defined(TCP_SNDLOWAT) && defined(LWIP_MIN) && defined(LWIP_MAX)
_Static_assert(WS_PIECE + 128u <= (unsigned)(TCP_SNDLOWAT),
               "a writable socket must take a whole piece");
#endif
static int ws_write(void *ctx, const uint8_t *p, size_t n)
{
    const send_to_t *t = ctx;
    /* Hand a piece over only once the socket has room for all of it: then the send below copies
     * it into lwIP's buffer without waiting for the reader. Not yet: 0, and the deadline and
     * the stop flag are checked again (web_send_all()). */
    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(t->fd, &ready);
    struct timeval tv = {.tv_sec = 0, .tv_usec = WS_READY_WAIT};
    const int s = select(t->fd + 1, NULL, &ready, NULL, &tv);
    if (s < 0)
        return -1;
    if (s == 0)
        return 0;
    const int w = httpd_socket_send(t->hd, t->fd, (const char *)p, n, 0);
    if (w > 0)
        return w;
    /* would block: try again; anything else (an httpd or TLS error): broken */
    return (w == 0 || w == ESP_TLS_ERR_SSL_WANT_WRITE || w == ESP_TLS_ERR_SSL_WANT_READ) ? 0 : -1;
}
static int64_t ws_now(void *ctx)
{
    (void)ctx;
    return esp_timer_get_time();
}
static bool ws_stopping(void *ctx)
{
    (void)ctx;
    return atomic_load(&stopping);
}
static void ws_pause(void *ctx)
{
    (void)ctx;
    vTaskDelay(1);
}
/* All of it, however many writes that takes: esp-tls returns what it managed when a write would
 * block, and httpd's own WebSocket send takes that for the whole frame, which would garble every
 * message after it. False: this connection is no good any more - the caller closes it, as a
 * frame sent in part can't be followed by anything (code review PERF11).
 *
 * How long the worker can be held: the stop flag and SEND_WAIT_MS are checked before every
 * write, also after each piece that went (web_send_all()), so one message takes at most
 * SEND_WAIT_MS plus one try. A try is handed one WS_PIECE, never the whole message: esp-tls's
 * write (esp_mbedtls_write) goes on record after record, and mbedtls_ssl_flush_output() calls
 * send() again for as long as each call moves some bytes - each able to wait the socket's
 * send timeout (send_wait_timeout, 3 s, panel_web.c) - so a whole picture handed to it against
 * a reader taking a few bytes at a time held the worker without limit. A try waits at most
 * WS_READY_WAIT for room, then hands over one record that fits the room there is, so its send()
 * returns without waiting for the reader: about SEND_WAIT_MS + 20 ms + a tick, 15 s. Only if
 * lwIP ran short of buffers could that send() still wait, for one 1 KB record: a few send
 * timeouts at most against a reader that acknowledges normally. */
static bool send_all(httpd_handle_t hd, int fd, const uint8_t *p, size_t n)
{
    send_to_t to = {.hd = hd, .fd = fd};
    const web_send_io_t io = {.write = ws_write,
                              .now_us = ws_now,
                              .stopping = ws_stopping,
                              .pause = ws_pause,
                              .ctx = &to,
                              .piece = WS_PIECE};
    return web_send_all(&io, p, n, esp_timer_get_time() + (int64_t)SEND_WAIT_MS * 1000);
}
/* One text message to fd, which the caller knows is still the connection it means. */
static bool send_text(httpd_handle_t hd, int fd, const char *json)
{
    uint8_t buf[WS_ROOM + TEXT_MAX];
    const size_t n = strnlen(json, TEXT_MAX + 1);
    if (n > TEXT_MAX)
        return false;
    memcpy(buf + WS_ROOM, json, n);
    const uint8_t *f = ws_frame(buf + WS_ROOM, HTTPD_WS_TYPE_TEXT, n);
    return send_all(hd, fd, f, (size_t)(buf + WS_ROOM - f) + n);
}
/* To the socket whose message is being handled. Not sent in full: the socket is closed (nothing
 * may follow a frame cut off midway, PERF11) and false. */
static bool reply(httpd_req_t *r, const char *json)
{
    const int fd = httpd_req_to_sockfd(r);
    if (send_text(r->handle, fd, json))
        return true;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (V.fd == fd)
        V.ended = true; /* no picture goes to it meanwhile */
    xSemaphoreGive(lock);
    (void)httpd_sess_trigger_close(r->handle, fd);
    return false;
}
/* {"t":"bye","why":..}, escaped: it may carry a user's name. cJSON_free() it. */
static char *bye_json(const char *why)
{
    cJSON *o = cJSON_CreateObject();
    char *json = NULL;
    if (o && cJSON_AddStringToObject(o, "t", "bye") && cJSON_AddStringToObject(o, "why", why))
        json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return json;
}
/* A goodbye to the socket whose message is being handled; the handler's failure then makes
 * the server close it at once. */
static esp_err_t bye(httpd_req_t *r, const char *why)
{
    char *json = bye_json(why);
    if (json) {
        (void)send_text(r->handle, httpd_req_to_sockfd(r), json); /* closed anyway */
        cJSON_free(json);
    }
    return ESP_FAIL;
}

/* A message for the viewer from another task, sent by the worker if the viewer is still there. */
typedef struct {
    int fd;
    uint32_t id;
    bool close;     /* then close the socket */
    size_t len;     /* of the frame; 0: nothing to send */
    uint8_t *frame; /* inside data */
    uint8_t data[];
} text_job_t;
static void text_work(void *arg)
{
    text_job_t *job = arg;
    xSemaphoreTake(lock, portMAX_DELAY);
    const httpd_handle_t hd = server;
    const bool ours = hd && V.fd == job->fd && V.id == job->id;
    xSemaphoreGive(lock);
    if (ours) {
        /* Not sent in full: closed too, so no picture continues a frame cut off midway. */
        const bool sent = !job->len || send_all(hd, job->fd, job->frame, job->len);
        if (!sent) {
            xSemaphoreTake(lock, portMAX_DELAY);
            if (V.fd == job->fd && V.id == job->id)
                V.ended = true;
            xSemaphoreGive(lock);
        }
        if (job->close || !sent)
            (void)httpd_sess_trigger_close(hd, job->fd);
    }
    free(job);
}
/* Queues json for viewer (fd, id); close: then close its socket. lock held. */
static void post_locked(int fd, uint32_t id, const char *json, bool close)
{
    if (!server || fd < 0)
        return;
    const size_t n = json ? strlen(json) : 0;
    text_job_t *job = web_alloc(sizeof(*job) + WS_ROOM + n);
    if (!job)
        return;
    job->fd = fd;
    job->id = id;
    job->close = close;
    job->len = 0;
    job->frame = NULL;
    if (n) {
        memcpy(job->data + WS_ROOM, json, n);
        job->frame = ws_frame(job->data + WS_ROOM, HTTPD_WS_TYPE_TEXT, n);
        job->len = (size_t)(job->data + WS_ROOM - job->frame) + n;
    }
    if (httpd_queue_work(server, text_work, job) != ESP_OK)
        free(job);
}

static const char *state_name(dev_input_t r)
{
    switch (r) {
    case DEV_INPUT_OK: return "ok";
    case DEV_INPUT_LOCAL: return "local";
    case DEV_INPUT_PAUSED: return "paused";
    case DEV_INPUT_OTHER: return "other";
    default: return "busy";
    }
}
/* Tells the page when the panel's answer to its input changes. */
static void report(httpd_req_t *r, dev_input_t state)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    const bool changed = state != V.reported;
    V.reported = state;
    xSemaphoreGive(lock);
    if (!changed)
        return;
    char json[64];
    snprintf(json, sizeof(json), "{\"t\":\"control\",\"state\":\"%s\"}", state_name(state));
    (void)reply(r, json);
}

/* The picture P: sent if its viewer is still there, else dropped; a connection that can't take
 * it is closed. */
static void picture_work(void *arg)
{
    (void)arg;
    xSemaphoreTake(lock, portMAX_DELAY);
    const httpd_handle_t hd = server;
    const int fd = P.fd;
    const uint32_t id = P.id;
    const bool ours = hd && V.fd == fd && V.id == id && !V.ended;
    xSemaphoreGive(lock);
    if (ours && !send_all(hd, fd, P.frame, P.len)) {
        ESP_LOGW(TAG, "remote keypad picture not sent: closing");
        xSemaphoreTake(lock, portMAX_DELAY);
        if (V.fd == fd && V.id == id)
            V.ended = true;
        xSemaphoreGive(lock);
        (void)httpd_sess_trigger_close(hd, fd);
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    P.queued = false;
    xSemaphoreGive(lock);
    xSemaphoreGive(done);
    xTaskNotifyGive(task);
}

/* ---- the picture task ------------------------------------------------------------------------ */

static void stream_task(void *arg)
{
    (void)arg;
    uint8_t *buf = NULL; /* WS_ROOM + HEADER + a picture */
    int64_t last_pic = 0;
    bool streaming = false; /* a session this task announced to the panel */
    uint32_t announced = 0; /* the viewer the panel's banner names */
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(40));
        const int64_t now = esp_timer_get_time();
        xSemaphoreTake(lock, portMAX_DELAY);
        if (V.fd >= 0 && !V.ended && now - V.last_rx > QUIET_US) {
            V.ended = true; /* the page stopped answering: goodbye, then its socket closes */
            post_locked(V.fd, V.id, "{\"t\":\"bye\",\"why\":\"quiet\"}", true);
        }
        const bool active = V.fd >= 0 && !V.ended;
        const int fd = V.fd;
        const uint32_t id = V.id;
        char who[ALARM_NAME_LEN], where[48];
        i18n_utf8_copy(who, sizeof(who), V.who);
        snprintf(where, sizeof(where), "%s", V.where);
        const bool ready = active && V.viewing && !atomic_load(&paused) &&
                           V.sent - V.acked < AHEAD && now - last_pic >= MIN_GAP_US;
        const dev_video_codec_t codec = V.codec;
        const bool key = V.want_key;
        xSemaphoreGive(lock);
        /* The session begins and ends here, the one task that touches the encoder. A viewer
         * that took over from another is announced too: the banner names who is there now. */
        if (active && (!streaming || id != announced)) {
            if (!buf)
                buf = web_alloc(WS_ROOM + HEADER + PICTURE_MAX);
            streaming = true;
            announced = id;
            if (A.session)
                A.session(true, who, where);
        } else if (!active && streaming) {
            streaming = false;
            if (A.session)
                A.session(false, "", "");
            if (A.video_stop)
                A.video_stop();
            free(buf);
            buf = NULL;
            ESP_LOGI(TAG, "remote keypad closed");
        }
        if (!ready || !buf || !A.video)
            continue;
        net_wifi_note_interaction(); /* no Wi-Fi power saving while someone watches */
        const uint8_t *data = NULL;
        size_t len = 0;
        bool is_key = false;
        const esp_err_t e = A.video(codec, key, &data, &len, &is_key);
        if (e == ESP_ERR_NO_MEM && codec == DEV_VIDEO_H264) {
            /* No encoder (internal RAM short, or none): JPEG still works. */
            xSemaphoreTake(lock, portMAX_DELAY);
            if (V.id == id) {
                V.codec = DEV_VIDEO_JPEG;
                V.want_key = true;
                post_locked(fd, id, "{\"t\":\"codec\",\"codec\":\"jpeg\"}", false);
            }
            xSemaphoreGive(lock);
            continue;
        }
        if (e != ESP_OK || len > PICTURE_MAX)
            continue; /* unchanged (ESP_ERR_NOT_FOUND), or busy: next time */
        last_pic = now;
        uint8_t *pic = buf + WS_ROOM;
        const uint32_t ms = (uint32_t)(now / 1000);
        pic[0] = codec == DEV_VIDEO_JPEG ? PIC_JPEG : is_key ? PIC_H264_KEY : PIC_H264;
        pic[1] = pic[2] = pic[3] = 0;
        for (unsigned i = 0; i < 4; i++)
            pic[8 + i] = (uint8_t)(ms >> (8 * i));
        memcpy(pic + HEADER, data, len);
        const uint8_t *frame = ws_frame(pic, HTTPD_WS_TYPE_BINARY, HEADER + len);
        bool queued = false;
        xSemaphoreTake(lock, portMAX_DELAY);
        if (server && V.fd == fd && V.id == id && !V.ended) {
            const uint32_t n = V.sent + 1;
            for (unsigned i = 0; i < 4; i++)
                pic[4 + i] = (uint8_t)(n >> (8 * i));
            P.fd = fd;
            P.id = id;
            P.frame = frame;
            P.len = (size_t)(pic - frame) + HEADER + len;
            P.queued = true;
            xSemaphoreTake(done, 0); /* nothing stale */
            queued = httpd_queue_work(server, picture_work, NULL) == ESP_OK;
            if (queued) {
                V.sent = n;
                if (is_key)
                    V.want_key = false;
            } else {
                P.queued = false;
                V.want_key = true; /* a picture the page never got: start again from a whole one */
            }
        }
        xSemaphoreGive(lock);
        if (queued && xSemaphoreTake(done, pdMS_TO_TICKS(3 * SEND_WAIT_MS)) != pdTRUE) {
            /* Not expected: the worker gives it, or web_screen_stopped() when the server stopped
             * before running the job. Leave buf to whatever may still read it. */
            ESP_LOGE(TAG, "remote keypad picture never finished: its buffer is left alone");
            buf = NULL;
            xSemaphoreTake(lock, portMAX_DELAY);
            if (V.id == id)
                V.ended = true;
            xSemaphoreGive(lock);
        }
    }
}

/* ---- the WebSocket (the worker) ------------------------------------------------------------ */

static pending_t *pending_for(int fd, bool take)
{
    pending_t *free_slot = NULL;
    for (unsigned i = 0; i < sizeof(pending) / sizeof(pending[0]); i++) {
        if (pending[i].session && pending[i].fd == fd)
            return &pending[i];
        if (!pending[i].session && !free_slot)
            free_slot = &pending[i];
    }
    return take ? free_slot : NULL;
}
/* Before the upgrade: only a signed-in session gets a WebSocket. */
static esp_err_t screen_pre(httpd_req_t *r)
{
    uint8_t user = 0;
    char who[ALARM_NAME_LEN] = "", token[65] = "";
    const void *s = panel_web_ws_session(r, &user, who, sizeof(who), token);
    if (!s) {
        httpd_resp_set_status(r, "401 Unauthorized");
        httpd_resp_send(r, NULL, 0);
        return ESP_FAIL;
    }
    const int fd = httpd_req_to_sockfd(r);
    xSemaphoreTake(lock, portMAX_DELAY);
    pending_t *p = pending_for(fd, true);
    if (!p) {
        p = &pending[0]; /* four sockets at most: the oldest waiter goes */
    }
    memset(p, 0, sizeof(*p));
    p->fd = fd;
    p->session = s;
    memcpy(p->token, token, sizeof(p->token));
    p->user = user;
    i18n_utf8_copy(p->who, sizeof(p->who), who);
    struct sockaddr_storage peer;
    socklen_t plen = sizeof(peer);
    if (getpeername(fd, (struct sockaddr *)&peer, &plen) == 0) {
        if (peer.ss_family == AF_INET)
            inet_ntop(AF_INET, &((struct sockaddr_in *)&peer)->sin_addr, p->where, sizeof(p->where));
#if LWIP_IPV6
        else if (peer.ss_family == AF_INET6)
            inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&peer)->sin6_addr, p->where, sizeof(p->where));
#endif
    }
    xSemaphoreGive(lock);
    mbedtls_platform_zeroize(token, sizeof(token));
    return ESP_OK;
}
static int number_of(const cJSON *o, const char *key, int lo, int hi, bool *ok)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < lo || v->valuedouble > hi) {
        *ok = false;
        return 0;
    }
    return (int)v->valuedouble;
}
static esp_err_t hello(httpd_req_t *r, int fd, const cJSON *o)
{
    const cJSON *csrf = cJSON_GetObjectItemCaseSensitive(o, "csrf");
    const cJSON *want = cJSON_GetObjectItemCaseSensitive(o, "codec");
    xSemaphoreTake(lock, portMAX_DELAY);
    pending_t *p = pending_for(fd, false);
    if (!p || !cJSON_IsString(csrf) || !panel_web_ws_alive(p->session, p->token, csrf->valuestring)) {
        if (p)
            memset(p, 0, sizeof(*p));
        xSemaphoreGive(lock);
        return bye(r, "signed_out");
    }
    if (V.fd >= 0 && V.fd != fd && !V.ended && strcmp(V.token, p->token) != 0) {
        char why[ALARM_NAME_LEN + 8];
        snprintf(why, sizeof(why), "in_use:%s", V.who);
        memset(p, 0, sizeof(*p));
        xSemaphoreGive(lock);
        return bye(r, why);
    }
    const int old = V.fd != fd ? V.fd : -1; /* still open: its close would have cleared V.fd */
    const bool old_ended = V.ended;
    const bool h264 = cJSON_IsString(want) && !strcmp(want->valuestring, "h264") &&
                      (A.video_codecs & DEV_VIDEO_H264);
    const bool is_paused = atomic_load(&paused);
    V.fd = fd;
    V.id = ++next_id;
    V.session = p->session;
    memcpy(V.token, p->token, sizeof(V.token));
    i18n_utf8_copy(V.who, sizeof(V.who), p->who);
    snprintf(V.where, sizeof(V.where), "%s", p->where);
    V.codec = h264 ? DEV_VIDEO_H264 : DEV_VIDEO_JPEG;
    V.viewing = true;
    V.want_key = true;
    V.ended = false;
    V.sent = V.acked = 0;
    V.last_rx = esp_timer_get_time();
    V.reported = is_paused ? DEV_INPUT_PAUSED : DEV_INPUT_OK;
    memset(p, 0, sizeof(*p));
    xSemaphoreGive(lock);
    if (old >= 0) { /* the same person in another tab, or a viewer on its way out */
        char *json = bye_json(old_ended ? "ended" : "replaced");
        if (json) {
            (void)send_text(r->handle, old, json);
            cJSON_free(json);
        }
        (void)httpd_sess_trigger_close(r->handle, old);
    }
    char json[160];
    snprintf(json, sizeof(json),
             "{\"t\":\"hello\",\"codec\":\"%s\",\"width\":%u,\"height\":%u,\"paused\":%s}",
             h264 ? "h264" : "jpeg", A.width, A.height, is_paused ? "true" : "false");
    if (reply(r, json) && is_paused) /* not sent: the socket is closing, nothing more to it */
        (void)reply(r, "{\"t\":\"control\",\"state\":\"paused\"}");
    ESP_LOGI(TAG, "remote keypad opened (%s)", h264 ? "H.264" : "JPEG");
    xTaskNotifyGive(task);
    return ESP_OK;
}
static void input(httpd_req_t *r, const char *t, const cJSON *o)
{
    bool ok = true;
    dev_input_t state = DEV_INPUT_BUSY;
    if (!strcmp(t, "tap")) {
        const int x = number_of(o, "x", 0, A.width - 1, &ok), y = number_of(o, "y", 0, A.height - 1, &ok);
        const int ms = number_of(o, "ms", 0, 10000, &ok);
        if (!ok || !A.tap)
            return;
        state = A.tap((uint16_t)x, (uint16_t)y, (uint16_t)ms);
    } else if (!strcmp(t, "down") || !strcmp(t, "move") || !strcmp(t, "up")) {
        const int x = number_of(o, "x", 0, A.width - 1, &ok), y = number_of(o, "y", 0, A.height - 1, &ok);
        if (!ok || !A.press)
            return;
        state = A.press((uint16_t)x, (uint16_t)y, strcmp(t, "up") != 0);
    } else if (!strcmp(t, "text")) {
        const cJSON *s = cJSON_GetObjectItemCaseSensitive(o, "s");
        if (!cJSON_IsString(s) || strlen(s->valuestring) > 16 || !A.text)
            return;
        for (const unsigned char *c = (const unsigned char *)s->valuestring; *c; c++)
            if (*c < 0x20 || *c == 0x7f)
                return; /* printable text only: keys have their own message */
        state = A.text(s->valuestring);
    } else if (!strcmp(t, "key")) {
        const cJSON *k = cJSON_GetObjectItemCaseSensitive(o, "k");
        if (!cJSON_IsString(k) || !A.key)
            return;
        const uint8_t key = !strcmp(k->valuestring, "backspace") ? DEV_KEY_BACKSPACE
                            : !strcmp(k->valuestring, "enter")   ? DEV_KEY_ENTER
                            : !strcmp(k->valuestring, "escape")  ? DEV_KEY_ESCAPE
                                                                 : 0xFF;
        if (key == 0xFF)
            return;
        state = A.key(key);
    } else {
        return;
    }
    if (atomic_load(&paused))
        state = DEV_INPUT_PAUSED;
    report(r, state);
    xTaskNotifyGive(task); /* the screen probably changed */
}
static esp_err_t handle(httpd_req_t *r, int fd, const cJSON *o)
{
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(o, "t");
    if (!cJSON_IsObject(o) || !cJSON_IsString(t))
        return ESP_OK;
    const char *type = t->valuestring;
    if (!strcmp(type, "hello"))
        return hello(r, fd, o);
    /* Everything else only from the viewer, and only while its sign-in lasts. */
    xSemaphoreTake(lock, portMAX_DELAY);
    const bool viewer = fd == V.fd && !V.ended;
    const bool waiting = !viewer && pending_for(fd, false) != NULL; /* its hello is on the way */
    const bool alive = viewer && panel_web_ws_alive(V.session, V.token, NULL);
    if (viewer && !alive)
        V.ended = true;
    if (alive)
        V.last_rx = esp_timer_get_time();
    xSemaphoreGive(lock);
    if (!alive)
        return waiting ? ESP_OK : bye(r, viewer ? "signed_out" : "not_viewer");
    bool ok = true;
    if (!strcmp(type, "ack")) {
        const int n = number_of(o, "n", 0, INT32_MAX, &ok);
        xSemaphoreTake(lock, portMAX_DELAY);
        if (ok && (uint32_t)n <= V.sent && (uint32_t)n > V.acked)
            V.acked = (uint32_t)n;
        xSemaphoreGive(lock);
        xTaskNotifyGive(task);
    } else if (!strcmp(type, "key-frame")) {
        xSemaphoreTake(lock, portMAX_DELAY);
        V.want_key = true;
        V.acked = V.sent; /* the page dropped what it had: start clean */
        xSemaphoreGive(lock);
        xTaskNotifyGive(task);
    } else if (!strcmp(type, "view")) {
        const cJSON *on = cJSON_GetObjectItemCaseSensitive(o, "on");
        xSemaphoreTake(lock, portMAX_DELAY);
        V.viewing = cJSON_IsTrue(on);
        if (V.viewing) {
            V.want_key = true;
            V.acked = V.sent;
        }
        xSemaphoreGive(lock);
        xTaskNotifyGive(task);
    } else if (strcmp(type, "ping")) {
        input(r, type, o);
    }
    return ESP_OK;
}
/* The strings a message carried (the CSRF token, typed text) don't stay in freed memory. */
static void wipe(cJSON *o)
{
    for (cJSON *p = o ? o->child : NULL; p; p = p->next) {
        if (p->valuestring)
            mbedtls_platform_zeroize(p->valuestring, strlen(p->valuestring));
        wipe(p);
    }
}
static esp_err_t screen_handler(httpd_req_t *r)
{
    httpd_ws_frame_t pkt = {0};
    if (httpd_ws_recv_frame(r, &pkt, 0) != ESP_OK)
        return ESP_FAIL;
    if (pkt.type != HTTPD_WS_TYPE_TEXT || pkt.len == 0 || pkt.len > 512)
        return pkt.type == HTTPD_WS_TYPE_TEXT ? ESP_FAIL : ESP_OK; /* oversized text: close */
    char text[513];
    pkt.payload = (uint8_t *)text;
    if (httpd_ws_recv_frame(r, &pkt, 512) != ESP_OK)
        return ESP_FAIL;
    text[pkt.len] = 0;
    cJSON *o = cJSON_ParseWithLength(text, pkt.len);
    mbedtls_platform_zeroize(text, sizeof(text)); /* the hello carries the CSRF token */
    const esp_err_t ret = handle(r, httpd_req_to_sockfd(r), o);
    wipe(o);
    cJSON_Delete(o);
    return ret;
}

esp_err_t web_screen_setup(const dev_remote_adapter_t *adapter)
{
    A = *adapter;
    if (!A.video || !A.video_codecs)
        return ESP_OK; /* no live screen on this board */
    lock = xSemaphoreCreateMutex();
    done = xSemaphoreCreateBinary();
    if (!lock || !done)
        return ESP_ERR_NO_MEM;
    /* The picture task runs the encoders and the PPA (drivers, DMA): an internal stack. It
     * writes no flash. */
    return xTaskCreate(stream_task, "web_screen", 6144, NULL, 2, &task) == pdPASS ? ESP_OK
                                                                                 : ESP_ERR_NO_MEM;
}
void web_screen_register(httpd_handle_t hd)
{
    if (!task)
        return;
    const httpd_uri_t u = {.uri = "/api/v1/screen",
                           .method = HTTP_GET,
                           .handler = screen_handler,
                           .is_websocket = true,
                           .ws_pre_handshake_cb = screen_pre};
    const esp_err_t e = httpd_register_uri_handler(hd, &u);
    if (e != ESP_OK) { /* the remote keypad is missing, the panel runs on (docs/bugs/B017) */
        ESP_LOGE(TAG, "not registered: %s", esp_err_to_name(e));
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    server = hd;
    xSemaphoreGive(lock);
}
void web_screen_stop(void)
{
    if (!task)
        return;
    atomic_store(&stopping, true); /* a send in progress gives up: the server waits for it */
    xSemaphoreTake(lock, portMAX_DELAY);
    server = NULL; /* from here on nothing reaches this server */
    V.fd = -1;     /* its sockets close with it */
    memset(pending, 0, sizeof(pending));
    xSemaphoreGive(lock);
    /* Remote access off (or a mode change): the next session, once it is back, starts unpaused. */
    atomic_store(&paused, false);
    if (A.pause)
        A.pause(false);
    xTaskNotifyGive(task); /* it ends the session: the banner goes, the encoder is freed */
}
void web_screen_stopped(void)
{
    if (!task)
        return;
    xSemaphoreTake(lock, portMAX_DELAY);
    const bool orphan = P.queued; /* queued, and the server stopped before running it */
    P.queued = false;
    xSemaphoreGive(lock);
    atomic_store(&stopping, false);
    if (orphan)
        xSemaphoreGive(done); /* its worker is gone: the task may use its buffer again */
}
void web_screen_closed(int fd)
{
    if (!task)
        return;
    xSemaphoreTake(lock, portMAX_DELAY);
    const bool viewer = V.fd == fd;
    if (viewer)
        V.fd = -1;
    for (unsigned i = 0; i < sizeof(pending) / sizeof(pending[0]); i++)
        if (pending[i].session && pending[i].fd == fd)
            memset(&pending[i], 0, sizeof(pending[i]));
    xSemaphoreGive(lock);
    if (viewer)
        xTaskNotifyGive(task); /* the session ends now, not at the next poll */
}
void web_screen_pause(bool p)
{
    atomic_store(&paused, p);
    if (A.pause)
        A.pause(p);
    if (!task)
        return;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (V.fd >= 0 && !V.ended) {
        if (!p) {
            V.want_key = true; /* the page shows the screen again from a whole picture */
            V.acked = V.sent;
        }
        V.reported = p ? DEV_INPUT_PAUSED : DEV_INPUT_OK;
        post_locked(V.fd, V.id,
                    p ? "{\"t\":\"control\",\"state\":\"paused\"}" : "{\"t\":\"control\",\"state\":\"ok\"}",
                    false);
    }
    xSemaphoreGive(lock);
    xTaskNotifyGive(task);
}
#endif /* CONFIG_HTTPD_WS_SUPPORT */
