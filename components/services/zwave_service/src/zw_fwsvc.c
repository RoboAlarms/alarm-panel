/*
 * zw_fwsvc.c - a Z-Wave device's software update on the panel (docs/features/36 ZW-033): the
 * Z-Wave JS update service's HTTPS and the download, in a worker task of their own (the zwave
 * task never waits on the internet), the file and the image in PSRAM, and zwave_zones' zw_fwjob
 * deciding what is offered and whether the file is the one promised. The transfer itself is
 * zwz_panel's (UI_ZW_OP_FIRMWARE).
 *
 * Only when the installer asks (the device page's "Check for an update", "Install it", or the
 * console's `zwave firmware`): nothing is checked on a schedule, nothing installs by itself. The
 * worker's stack is in PSRAM: it never writes flash or NVS (docs/bugs/B001).
 */
#include "zwave_internal.h"

#if CONFIG_ALARM_ZSRC_ZWAVE

#include <stdio.h>
#include <string.h>

#include "alarm_mem.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_wifi.h"
#include "zone_service.h"
#include "zwave_proto/zw_gbl.h"
#include "zwave_zones/zw_fwjob.h"

static const char *TAG = "zwave-fw";

#define WORKER_STACK   12288u          /* TLS: in PSRAM, it writes no flash */
#define ANSWER_MAX     (16u * 1024u)   /* the service's answer for one device */
#define FILE_MAX       (1024u * 1024u) /* a file (Intel HEX text is the longest) */
#define HTTP_TIMEOUT   20000

typedef enum { JOB_NONE = 0, JOB_CHECK, JOB_DOWNLOAD } job_kind_t;

static struct {
    TaskHandle_t worker;
    TaskHandle_t owner;          /* the zwave task: told when the worker is done */
    portMUX_TYPE mux;
    /* the worker's mailbox (the zwave task writes it while `kind` is NONE, the worker while not) */
    volatile uint8_t kind;       /* job_kind_t being worked on */
    volatile bool done;
    char body[ZWFJ_BODY_MAX];
    char url[ZWFC_URL_MAX];
    int status;                  /* HTTP status; 0 not reached */
    uint8_t *data;               /* PSRAM: the answer or the file */
    size_t len;
    /* the zwave task's own */
    bool need_installer;         /* asked from the pages (else the console: disarmed is enough) */
    uint8_t user;
    uint8_t *image;              /* PSRAM: the image zwz_panel sends, kept until it ends */
    size_t image_len;
} F = {.mux = portMUX_INITIALIZER_UNLOCKED};

/* The device page's job, and the radio's own (node 0: network.md §14); read and written under
 * the zone lock. */
static ALARM_SLOW_BSS zwfj_t s_job;
static ALARM_SLOW_BSS zwfj_t s_radio;
static bool s_for_radio;     /* the worker's exchange is the radio's */

static bool network_up(void)
{
    net_wifi_status_t ws;
    net_wifi_get_status(&ws);
    return ws.state == NET_WIFI_CONNECTED;
}

/* One HTTPS exchange: POST `body` (JSON) or GET, following redirects, into a PSRAM buffer. */
static int http(const char *url, const char *body, size_t cap, uint8_t **out, size_t *len)
{
    *out = NULL;
    *len = 0u;
    if (strncmp(url, "https://", 8u) != 0 || !network_up()) {
        return 0;
    }
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .user_agent = ZWFJ_USER_AGENT, /* the service requires one naming the client */
        .method = body != NULL ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        return 0;
    }
    int status = 0;
    uint8_t *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        esp_http_client_cleanup(c);
        return 0;
    }
    if (body != NULL) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
    }
    const int body_len = body != NULL ? (int)strlen(body) : 0;
    for (int hop = 0; hop < 5; hop++) {
        if (esp_http_client_open(c, body_len) != ESP_OK) {
            status = 0;
            break;
        }
        if (body_len > 0 && esp_http_client_write(c, body, body_len) != body_len) {
            esp_http_client_close(c);
            status = 0;
            break;
        }
        (void)esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        if (body == NULL && (status == 301 || status == 302 || status == 303 || status == 307 ||
                             status == 308)) {
            esp_http_client_flush_response(c, NULL);
            esp_http_client_set_redirection(c);
            esp_http_client_close(c);
            continue; /* a file host sends its storage's address */
        }
        break;
    }
    size_t n = 0u;
    if (status == 200) {
        for (;;) {
            if (n >= cap) {
                status = 413; /* bigger than anything the panel takes */
                break;
            }
            const int r = esp_http_client_read(c, (char *)buf + n, (int)(cap - n));
            if (r <= 0) {
                if (r < 0 || !esp_http_client_is_complete_data_received(c)) {
                    status = 0; /* cut off */
                }
                break;
            }
            n += (size_t)r;
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (status != 200) {
        heap_caps_free(buf);
        return status;
    }
    *out = buf;
    *len = n;
    return status;
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const uint8_t kind = F.kind;
        if (kind == JOB_NONE || F.done) {
            continue;
        }
        uint8_t *data = NULL;
        size_t len = 0u;
        const int status = kind == JOB_CHECK
                               ? http(ZWFJ_SERVICE_URL, F.body, ANSWER_MAX, &data, &len)
                               : http(F.url, NULL, FILE_MAX, &data, &len);
        ESP_LOGI(TAG, "%s: HTTP %d, %u bytes", kind == JOB_CHECK ? "check" : "download", status,
                 (unsigned)len);
        taskENTER_CRITICAL(&F.mux);
        F.status = status;
        F.data = data;
        F.len = len;
        F.done = true;
        taskEXIT_CRITICAL(&F.mux);
        if (F.owner != NULL) {
            xTaskNotifyGive(F.owner);
        }
    }
}

static bool worker_start(void)
{
    if (F.worker != NULL) {
        return true;
    }
    F.owner = xTaskGetCurrentTaskHandle();
    return xTaskCreatePinnedToCoreWithCaps(worker, "zwave-fw", WORKER_STACK, NULL, 2, &F.worker,
                                           tskNO_AFFINITY, MALLOC_CAP_SPIRAM) == pdPASS;
}

static const char *region_word(const zwz_panel_t *p)
{
    return p->setup.cfg.region_set ? zwfc_region(p->setup.cfg.region) : NULL;
}

ui_zw_req_result_t zwsvc_fw_request(const zwsvc_req_t *req, bool need_installer)
{
    if (!req->allowed) {
        return UI_ZW_REQ_NOT_ALLOWED;
    }
    if (F.kind != JOB_NONE || F.image != NULL) {
        return UI_ZW_REQ_BUSY; /* one exchange, one update at a time */
    }
    if (!worker_start()) {
        return UI_ZW_REQ_BUSY;
    }
    zwz_panel_t *p = zwsvc_panel();
    ui_zw_req_result_t r = UI_ZW_REQ_INVALID;
    uint8_t kind = JOB_NONE;
    zone_service_lock();
    const zw_node_record_t *rec = zw_node_find_const(&p->net.nodes, req->ui.node);
    const bool radio = req->ui.node == 0u; /* the radio itself: asked about as Z-Wave JS does */
    if (radio) {
        if (req->ui.kind == UI_ZW_REQ_FW_CHECK) {
            if (zwfj_check_radio(&s_radio, zw_host_facts(&p->net.host), region_word(p), F.body,
                                 sizeof F.body) > 0u) {
                kind = JOB_CHECK;
            }
        } else {
            const char *url = zwfj_download_begin(&s_radio, 0u, 0u);
            if (url != NULL) {
                snprintf(F.url, sizeof F.url, "%s", url);
                kind = JOB_DOWNLOAD;
            }
        }
    } else if (rec != NULL && rec->generation == req->ui.generation &&
        zwz_panel_fw_updatable(p, req->ui.node, req->ui.generation)) {
        if (req->ui.kind == UI_ZW_REQ_FW_CHECK) {
            if (zwfj_check_begin(&s_job, rec, region_word(p), F.body, sizeof F.body) > 0u) {
                kind = JOB_CHECK;
            }
        } else {
            const char *url = zwfj_download_begin(&s_job, req->ui.node, req->ui.generation);
            if (url != NULL) {
                snprintf(F.url, sizeof F.url, "%s", url);
                kind = JOB_DOWNLOAD;
            }
        }
    }
    zone_service_unlock();
    if (kind != JOB_NONE) {
        s_for_radio = radio;
        F.need_installer = need_installer;
        F.user = req->ui.user;
        F.done = false;
        F.kind = kind;
        xTaskNotifyGive(F.worker);
        r = UI_ZW_REQ_OK;
    }
    return r;
}

static void radio_file_arrived(zwz_panel_t *p, const uint8_t *data, size_t len);

/* The worker's answer, in the zwave task: the check's verdict, or the file checked, made the image
 * and handed to the transfer. And the image let go once its transfer ended. */
void zwsvc_fw_poll(void)
{
    zwz_panel_t *p = zwsvc_panel();
    if (F.image != NULL) {
        ui_zw_progress_t g;
        zone_service_lock();
        zwz_panel_progress(p, zwsvc_now_ms(), &g);
        const bool running = zw_fwupd_running(&p->fw) ||
                             (g.op == UI_ZW_OP_RADIO_UPDATE && g.step != UI_ZW_STEP_DONE);
        zone_service_unlock();
        if (!running) {
            heap_caps_free(F.image);
            F.image = NULL;
            F.image_len = 0u;
        }
    }
    if (F.kind == JOB_NONE || !F.done) {
        return;
    }
    taskENTER_CRITICAL(&F.mux);
    const uint8_t kind = F.kind;
    const int status = F.status;
    uint8_t *data = F.data;
    const size_t len = F.len;
    F.data = NULL;
    taskEXIT_CRITICAL(&F.mux);
    zwfj_t *job_of = s_for_radio ? &s_radio : &s_job;
    if (kind == JOB_CHECK) {
        zone_service_lock();
        zwfj_check_answer(job_of, status, (const char *)data, len, region_word(p));
        zone_service_unlock();
    } else if (status != 200 || data == NULL) {
        zone_service_lock();
        zwfj_download_failed(job_of, status);
        zone_service_unlock();
    } else if (s_for_radio) {
        radio_file_arrived(p, data, len);
    } else {
        /* the image: the file as it is, or converted - never longer than the file. Its digest
         * and conversion take a while: on a copy of the job, outside the zone lock, the result
         * put back only if the job is still this one */
        uint8_t *image = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        static ALARM_SLOW_BSS zwfj_t job;
        zone_service_lock();
        job = s_job;
        zone_service_unlock();
        size_t n = 0u;
        if (image == NULL) {
            zwfj_download_failed(&job, 507);
        } else {
            n = zwfj_file(&job, data, len, image, len);
        }
        zone_service_lock();
        if (s_job.state == UI_ZW_FW_DOWNLOADING && s_job.node == job.node &&
            s_job.generation == job.generation) {
            s_job = job;
        } else {
            n = 0u; /* asked about something else meanwhile */
        }
        zone_service_unlock();
        if (n > 0u) {
            const bool allowed = F.need_installer ? zwsvc_installer_and_disarmed()
                                                  : zone_service_all_disarmed();
            zone_service_lock();
            const ui_zw_req_result_t r =
                zwz_panel_fw_start(p, zwsvc_now_ms(), s_job.node, s_job.generation, 0u, image, n,
                                   allowed, F.user);
            if (r == UI_ZW_REQ_OK) {
                zwfj_started(&s_job);
                F.image = image;
                F.image_len = n;
                image = NULL;
            } else {
                zwfj_not_started(&s_job, r);
            }
            zone_service_unlock();
            ESP_LOGI(TAG, "image %u bytes: %s", (unsigned)n,
                     r == UI_ZW_REQ_OK ? "sending" : "not started");
        }
        heap_caps_free(image);
    }
    heap_caps_free(data);
    taskENTER_CRITICAL(&F.mux);
    F.kind = JOB_NONE;
    F.done = false;
    taskEXIT_CRITICAL(&F.mux);
}

/* ---- the radio's own firmware (network.md §14) ------------------------------------------ */

/* The radio's image goes: the update service's file (its SHA-256 checked, then its .gbl) or the
 * installer's own (its .gbl checked). `image` is PSRAM, kept in F.image while the update runs. */
static ui_zw_req_result_t radio_start(zwz_panel_t *p, uint8_t *image, size_t n, bool allowed,
                                      uint8_t user)
{
    zone_service_lock();
    const ui_zw_req_result_t r = zwz_panel_radio_update(p, zwsvc_now_ms(), image, n, allowed, user);
    zone_service_unlock();
    if (r == UI_ZW_REQ_OK) {
        F.image = image;
        F.image_len = n;
    } else {
        heap_caps_free(image);
    }
    ESP_LOGW(TAG, "radio image %u bytes: %s", (unsigned)n, r == UI_ZW_REQ_OK ? "sending" : "not started");
    return r;
}

static void radio_file_arrived(zwz_panel_t *p, const uint8_t *data, size_t len)
{
    uint8_t *image = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    static ALARM_SLOW_BSS zwfj_t job;
    zone_service_lock();
    job = s_radio;
    zone_service_unlock();
    size_t n = 0u;
    if (image == NULL) {
        zwfj_download_failed(&job, 507);
    } else {
        n = zwfj_file(&job, data, len, image, len); /* its SHA-256, then the file as it is */
    }
    zone_service_lock();
    if (s_radio.state == UI_ZW_FW_DOWNLOADING) {
        s_radio = job;
    } else {
        n = 0u;
    }
    zone_service_unlock();
    if (n == 0u) {
        heap_caps_free(image);
        return;
    }
    const bool allowed = F.need_installer ? zwsvc_installer_and_disarmed()
                                          : zone_service_all_disarmed();
    const ui_zw_req_result_t r = radio_start(p, image, n, allowed, F.user);
    zone_service_lock();
    if (r == UI_ZW_REQ_OK) {
        zwfj_started(&s_radio);
    } else {
        zwfj_not_started(&s_radio, r == UI_ZW_REQ_INVALID ? UI_ZW_REQ_UNSUPPORTED : r);
    }
    zone_service_unlock();
}

/* `image` (PSRAM) is this call's: kept while its update runs, else freed. */
ui_zw_req_result_t zwsvc_fw_radio_file(uint8_t *image, size_t len, bool allowed, uint8_t user)
{
    ui_zw_req_result_t r = UI_ZW_REQ_OK;
    if (!allowed) {
        r = UI_ZW_REQ_NOT_ALLOWED;
    } else if (F.kind != JOB_NONE || F.image != NULL) {
        r = UI_ZW_REQ_BUSY; /* one exchange, one update at a time */
    } else if (image == NULL || zw_gbl_check(image, len, NULL) != ZW_GBL_OK) {
        r = UI_ZW_REQ_INVALID;
    }
    if (r != UI_ZW_REQ_OK) {
        heap_caps_free(image);
        return r;
    }
    return radio_start(zwsvc_panel(), image, len, true, user);
}

void zwsvc_fw_radio_status(ui_zw_status_t *st)
{
    zone_service_lock();
    st->radio_fw_check = s_radio.state;
    snprintf(st->radio_fw_version, sizeof st->radio_fw_version, "%s", s_radio.update.version);
    snprintf(st->radio_fw_words, sizeof st->radio_fw_words, "%s", s_radio.words);
    zone_service_unlock();
}

bool zwsvc_fw_cancel(void)
{
    zone_service_lock();
    const bool stopped = zwfj_cancel(&s_job);
    zone_service_unlock();
    return stopped;
}

void zwsvc_fw_page(uint16_t node, uint16_t generation, ui_zw_device_page_t *out)
{
    zwfj_page(&s_job, node, generation, out);
}

void zwsvc_fw_describe(char *out, size_t len)
{
    zone_service_lock();
    const zwfj_t j = s_job;
    zone_service_unlock();
    static const char *const STATE[] = {"none",      "checking",     "up to date",
                                        "unknown",   "available",    "check failed",
                                        "downloading", "file failed"};
    snprintf(out, len, "job: node %u generation %u: %s%s%s%s%s", (unsigned)j.node,
             (unsigned)j.generation, j.state < 8u ? STATE[j.state] : "?",
             j.update.version[0] != '\0' ? ", version " : "", j.update.version,
             j.words[0] != '\0' ? " - " : "", j.words);
}

void zw_fwsvc_stack(uint32_t *free_bytes, uint32_t *size)
{
    const TaskHandle_t t = F.worker;
    *free_bytes = t != NULL ? (uint32_t)uxTaskGetStackHighWaterMark(t) : 0u;
    *size = t != NULL ? (uint32_t)WORKER_STACK : 0u;
}

#endif /* CONFIG_ALARM_ZSRC_ZWAVE */
