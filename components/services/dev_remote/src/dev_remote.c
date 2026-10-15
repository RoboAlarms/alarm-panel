/* Feature 30. In every P4 profile since Feature 31, off until the installer turns Developer on.
 * One enrolled client certificate, no bearer defaults.
 * HTTP server owns uploads; maintenance task owns listener restart and discovery. */
#include "dev_remote.h"
#include "zigbee_service.h"
#include "alarm_mem.h"
#include "esp_attr.h"
#include "alarm/alarm_strings.h"
#include "alarm_service.h"
#include "asset_packs.h"
#include "code_hash.h"
#include "dev_cert.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_https_server.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mdns.h"
#include "net_wifi.h"
#include "nvs.h"
#include "pack_assets.h" /* PA_PICTURE_MAX_FILE */
#include "panel_log.h"
#include "panel_ota.h"
#include "panel_prefs.h"
#include "panel_voice.h"
#include "psa/crypto.h"
#include "sounder.h"
#include "zone_service.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PORT      6055
#define CERT_MAX  1024
#define LOG_COUNT 32
static dev_remote_adapter_t A;
/* Certificates and the log ring are task-only (mbedTLS parses the PEM, the HTTP worker reads the
 * log): PSRAM. The key stays internal with the other small secrets. */
static uint8_t cert[CERT_MAX] ALARM_SLOW_BSS, peer[CERT_MAX] ALARM_SLOW_BSS,
    pending[CERT_MAX] ALARM_SLOW_BSS;
static uint8_t key[256], peer_fp[32];
static size_t cert_len, key_len, peer_len, pending_len;
static char identity[13], boot[33], uploaded_op[33], uploaded_elf[65];
/* Only the HTTP worker accesses the lease. Adapter independently releases on local
 * touch/timeout, so a stalled network worker cannot leave a pointer held down. */
static char controller[33];
static int64_t controller_until;
static atomic_bool enabled, receiving, revoke_pending;
static atomic_bool allowed;
static atomic_uint generation;
void dev_remote_set_allowed(bool on)
{
    atomic_store(&allowed, on);
    atomic_fetch_add(&generation, 1);
}
static SemaphoreHandle_t trust_lock, log_lock; /* published together by dev_remote_start */
static atomic_bool started; /* the maintenance task exists: a start is never repeated */
static char logs[LOG_COUNT][192] ALARM_SLOW_BSS;
static uint32_t log_seq;
static httpd_handle_t server;
static uint16_t *pixels;

static void hex(const uint8_t *in, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = digits[in[i] >> 4];
        out[i * 2 + 1] = digits[in[i] & 15];
    }
    out[n * 2] = 0;
}
static int nibble(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
static bool unhex(const char *s, uint8_t *out, size_t n)
{
    if (strlen(s) != n * 2)
        return false;
    for (size_t i = 0; i < n; i++) {
        int a = nibble(s[i * 2]), b = nibble(s[i * 2 + 1]);
        if (a < 0 || b < 0)
            return false;
        out[i] = (uint8_t)(a * 16 + b);
    }
    return true;
}
static bool digest(const void *data, size_t n, uint8_t out[32])
{
    size_t got = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, data, n, out, 32, &got) == PSA_SUCCESS && got == 32;
}
void dev_remote_public_identity(char *out, size_t capacity)
{
    if (capacity < cert_len * 2 + 1) {
        if (capacity)
            out[0] = 0;
        return;
    }
    hex(cert, cert_len, out);
}
esp_err_t dev_remote_enroll_public(const uint8_t *data, size_t length)
{
    if (!atomic_load(&allowed) || !trust_lock || !data || !length || length > 2048)
        return ESP_ERR_INVALID_STATE;
    mbedtls_x509_crt parsed;
    mbedtls_x509_crt_init(&parsed);
    int result = mbedtls_x509_crt_parse(&parsed, data, length);
    uint8_t fp[32];
    if (result != 0 || parsed.next || !parsed.raw.len || parsed.raw.len > CERT_MAX ||
        !digest(parsed.raw.p, parsed.raw.len, fp)) {
        mbedtls_x509_crt_free(&parsed);
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t e = nvs_open("devrem", NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_blob(h, "peer", parsed.raw.p, parsed.raw.len);
        if (e == ESP_OK)
            e = nvs_commit(h);
        nvs_close(h);
    }
    if (e == ESP_OK) {
        xSemaphoreTake(trust_lock, portMAX_DELAY);
        memcpy(peer, parsed.raw.p, parsed.raw.len);
        peer_len = parsed.raw.len;
        memcpy(peer_fp, fp, 32);
        atomic_store(&revoke_pending, false);
        atomic_store(&enabled, true);
        xSemaphoreGive(trust_lock);
        atomic_fetch_add(&generation, 1);
    }
    mbedtls_x509_crt_free(&parsed);
    return e;
}
static void record(const char *message)
{
    xSemaphoreTake(log_lock, portMAX_DELAY);
    uint32_t seq = ++log_seq;
    snprintf(logs[seq % LOG_COUNT], 192, "%" PRIu32 " %" PRIi64 " %s", seq,
             esp_timer_get_time() / 1000, message);
    xSemaphoreGive(log_lock);
}
static esp_err_t fail(httpd_req_t *r, const char *status, const char *why)
{
    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, "text/plain");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, why, HTTPD_RESP_USE_STRLEN);
}
static void tls_session(esp_https_server_user_cb_arg_t *arg)
{
    if (arg->user_cb_state != HTTPD_SSL_USER_CB_SESS_CREATE || !server)
        return;
    int fd;
    if (esp_tls_get_conn_sockfd(arg->tls, &fd) != ESP_OK)
        return;
    const mbedtls_x509_crt *p = mbedtls_ssl_get_peer_cert(esp_tls_get_ssl_context(arg->tls));
    uint8_t *fp = malloc(32);
    if (!p || !fp || !digest(p->raw.p, p->raw.len, fp)) {
        free(fp);
        return;
    }
    httpd_sess_set_ctx(server, fd, fp, free);
}
static bool authorized(httpd_req_t *r)
{
    if (!atomic_load(&enabled) || !atomic_load(&allowed))
        return false;
    /* HTTPS transport_ctx is private to IDF. Its supported TLS callback gives us the
     * peer certificate; retain only the public digest in the ordinary session context. */
    const uint8_t *fp = httpd_sess_get_ctx(r->handle, httpd_req_to_sockfd(r));
    if (!fp)
        return false;
    xSemaphoreTake(trust_lock, portMAX_DELAY);
    bool ok = atomic_load(&enabled) && memcmp(fp, peer_fp, 32) == 0;
    xSemaphoreGive(trust_lock);
    return ok;
}
static bool body(httpd_req_t *r, char *buf, size_t cap)
{
    if (r->content_len == 0 || r->content_len >= cap)
        return false;
    size_t got = 0;
    while (got < r->content_len) {
        int n = httpd_req_recv(r, buf + got, r->content_len - got);
        if (n <= 0)
            return false;
        got += (size_t)n;
    }
    buf[got] = 0;
    return strlen(buf) == got;
}
/* The adapter serves Z-Wave: its state and requests (the trace is development builds' only). */
static bool zwave_present(void)
{
    return A.zwave_state != NULL && A.zwave_request != NULL;
}
/* Zigbee (docs/features/20 devices.md §12, BL-143): straight from zigbee_service, whose answers
 * say whether this panel has it - false without CONFIG_ALARM_ZSRC_ZIGBEE (its stubs). */
static bool zigbee_present(void)
{
    ui_zb_status_t st;
    return zigbee_service_ui_status(&st);
}
static void describe(char *result, size_t capacity)
{
    panel_ota_status_t st;
    panel_ota_get_status(&st);
    char elf[65], op[33], metrics[512] = "{}";
    bool wait;
    hex(esp_app_get_description()->app_elf_sha256, 32, elf);
    panel_ota_remote_operation(op, &wait);
    if (A.metrics)
        A.metrics(metrics, sizeof(metrics));
    const char *why = NULL;
    bool idle = panel_ota_system_idle(&why);
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    esp_chip_info_t chip;
    esp_chip_info(&chip);
#if CONFIG_ALARM_BUILD_RELEASE
    const char *profile = "release";
#elif CONFIG_ALARM_BUILD_DEV
    const char *profile = "dev";
#else
    const char *profile = "legacy";
#endif
    snprintf(result, capacity,
             "{\"api\":1,\"id\":\"%s\",\"board\":\"%s\",\"profile\":\"%s\","
             "\"chip_revision\":%u,\"version\":\"%s\",\"elf\":\"%s\",\"boot\":\"%s\","
             "\"uptime_ms\":%" PRIi64 ",\"slot_size\":%lu,\"idle\":%s,\"probation\":%s,"
             "\"confirm_required\":%s,\"operation\":\"%s\",\"staged_operation\":\"%s\","
             "\"staged_elf\":\"%s\",\"health_stale\":%lu,\"heap_internal\":%u,"
             "\"heap_largest\":%u,\"crypto_faults\":%lu,\"width\":%u,\"height\":%u,"
             "\"capture_width\":%u,\"capture_height\":%u,\"display\":%s,\"speaker\":\"%s\","
             "\"pixel_format\":\"rgb565le\",\"orientation\":0,\"capabilities\":[\"ota\",\"logs\","
             "\"diagnostics\",\"frame\",\"input\"%s%s]}",
             identity, A.board, profile, (unsigned)chip.revision, st.running, elf, boot,
             esp_timer_get_time() / 1000, (unsigned long)(slot ? slot->size : 0),
             idle ? "true" : "false", st.probation ? "true" : "false", wait ? "true" : "false", op,
             uploaded_op, uploaded_elf, (unsigned long)panel_ota_health_stale(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned long)code_hash_faults(), A.width, A.height, A.width / 2, A.height / 2,
             metrics, sounder_silenced() ? "off" : "on", zwave_present() ? ",\"zwave\"" : "",
             zigbee_present() ? ",\"zigbee\"" : "");
}
static esp_err_t info(httpd_req_t *r)
{
    if (!authorized(r))
        return fail(r, "403 Forbidden", "Developer certificate required");
    char result[2048];
    describe(result, sizeof(result));
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, result, HTTPD_RESP_USE_STRLEN);
}
#if CONFIG_ALARM_BUILD_DEV
/* GET or POST "on"/"off" /dev/v1/speaker: the bench silence (sounder.h, docs/features/08
 * AUD-025); the answer is the state after it, "on" or "off". */
static esp_err_t speaker(httpd_req_t *r)
{
    if (!authorized(r))
        return fail(r, "403 Forbidden", "Developer certificate required");
    if (r->method == HTTP_POST) {
        char b[8];
        if (!body(r, b, sizeof(b)))
            return fail(r, "400 Bad Request", "Send on or off");
        b[strcspn(b, " \r\n")] = 0;
        const bool off = strcmp(b, "off") == 0;
        if (!off && strcmp(b, "on") != 0)
            return fail(r, "400 Bad Request", "Send on or off");
        if (sounder_set_silenced(off) != ESP_OK)
            return fail(r, "500 Internal Server Error",
                        off ? "off now, not kept across a restart" : "Still off: could not be changed");
    }
    return fail(r, "200 OK", sounder_silenced() ? "off" : "on");
}
#endif
static esp_err_t browser_grant(httpd_req_t *r)
{
    if (!authorized(r))
        return fail(r, "403 Forbidden", "Developer certificate required");
    char token[65], response[96];
    if (!A.browser_grant || !A.browser_grant(token))
        return fail(r, "403 Forbidden", "Browser development access is disabled");
    snprintf(response, sizeof(response), "{\"grant\":\"%s\"}", token);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, response, HTTPD_RESP_USE_STRLEN);
}
size_t dev_remote_copy_log(char *out, size_t capacity)
{
    if (!out || !capacity || !log_lock)
        return 0;
    size_t at = 0;
    out[0] = 0;
    xSemaphoreTake(log_lock, portMAX_DELAY);
    uint32_t first = log_seq >= LOG_COUNT ? log_seq - LOG_COUNT + 1 : 1;
    for (uint32_t i = first; i <= log_seq; i++) {
        size_t length = strlen(logs[i % LOG_COUNT]);
        if (length + 2 > capacity - at)
            break;
        memcpy(out + at, logs[i % LOG_COUNT], length);
        at += length;
        out[at++] = '\n';
    }
    out[at] = 0;
    xSemaphoreGive(log_lock);
    return at;
}
static esp_err_t log_read(httpd_req_t *r)
{
    if (!authorized(r))
        return fail(r, "403 Forbidden", "Unauthorized");
    char *out = malloc(LOG_COUNT * 192 + 1);
    if (!out)
        return fail(r, "503 Service Unavailable", "Memory busy");
    size_t at = 0;
    xSemaphoreTake(log_lock, portMAX_DELAY);
    uint32_t first = log_seq >= LOG_COUNT ? log_seq - LOG_COUNT + 1 : 1;
    for (uint32_t i = first; i <= log_seq; i++)
        at += (size_t)snprintf(out + at, LOG_COUNT * 192 + 1 - at, "%s\n", logs[i % LOG_COUNT]);
    xSemaphoreGive(log_lock);
    httpd_resp_set_type(r, "text/plain");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(r, out, (ssize_t)at);
    free(out);
    return err;
}
static esp_err_t frame(httpd_req_t *r)
{
    if (!authorized(r))
        return fail(r, "403 Forbidden", "Unauthorized");
    if (atomic_load(&receiving) || !A.frame || !pixels)
        return fail(r, "503 Service Unavailable", "Capture unavailable");
    size_t count = (size_t)(A.width / 2) * (A.height / 2);
    esp_err_t captured = A.frame(pixels, count);
    if (captured != ESP_OK)
        return fail(r, "503 Service Unavailable", "Display busy; retry later");
    httpd_resp_set_type(r, "application/octet-stream");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    /* Native little-endian RGB565, half resolution; client expands without device encoding. */
    return httpd_resp_send(r, (const char *)pixels, (ssize_t)(count * 2));
}
/* Typed read-only diagnostics. Never export configuration blobs, credentials,
 * raw console commands, or unbounded printf output from other components. */
static bool public_event(void *ctx, const elog_entry_t *entry)
{
    (void)ctx;
    return entry->kind == ELOG_KIND_EVENT && !(entry->ev.flags & ALARM_EVF_SILENT);
}
static esp_err_t diagnostics(httpd_req_t *r)
{
    if (!authorized(r))
        return fail(r, "403 Forbidden", "Unauthorized");
    alarm_snapshot_t *snap = malloc(sizeof(*snap));
    if (!snap)
        return fail(r, "503 Service Unavailable", "Memory busy");
    alarm_service_get_snapshot(snap);
    httpd_resp_set_type(r, "text/plain");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    char line[256];
    esp_err_t err = ESP_OK;
#define SEND_LINE(...)                                                                             \
    do {                                                                                           \
        snprintf(line, sizeof(line), __VA_ARGS__);                                                 \
        err = httpd_resp_send_chunk(r, line, HTTPD_RESP_USE_STRLEN);                               \
        if (err != ESP_OK)                                                                         \
            goto done;                                                                             \
    } while (0)
    SEND_LINE("snapshot=%lu uptime_ms=%llu system_troubles=%lu restored=%lu device_troubles=%lu "
              "installer=%d\n",
              (unsigned long)snap->seq, (unsigned long long)snap->taken_ms,
              (unsigned long)snap->sys_troubles, (unsigned long)snap->sys_troubles_restored,
              (unsigned long)snap->dev_troubles, snap->program);
    for (unsigned i = 0; i < ALARM_MAX_PARTITIONS; i++)
        if (snap->partitions & (1u << i)) {
            const alarm_partition_status_t *p = &snap->part[i];
            SEND_LINE("partition=%u state=%u ready=%d faults=%u alarms=%u troubles=%u\n", i + 1,
                      (unsigned)p->state, p->ready, p->faults, p->alarms, p->troubles);
        }
    zone_hub_source_info_t sources[8];
    size_t count = zone_service_sources(sources, 8);
    for (size_t i = 0; i < count && i < 8; i++)
        SEND_LINE("source=%u caps=%lu usable=%d link_down=%d trouble=%d\n", sources[i].kind,
                  (unsigned long)sources[i].caps, sources[i].usable, sources[i].link_down,
                  sources[i].trouble);
    for (uint16_t i = 1; i <= snap->zones_used && i <= ALARM_MAX_ZONES; i++) {
        zone_hub_info_t z;
        bool bound = zone_service_info(i, &z);
        SEND_LINE("zone=%u flags=%u trouble=%u bound=%d known=%d active=%d unreachable=%d check=%d "
                  "heard_ms=%llu value_x100=%ld\n",
                  i, snap->zone[i - 1].flags, snap->zone[i - 1].trouble, bound, bound && z.known,
                  bound && z.active, bound && z.unreachable, bound && z.check,
                  (unsigned long long)(bound ? z.heard_at : 0),
                  (long)(bound && z.has_value ? z.value_x100 : 0));
    }
    {
        elog_entry_t events[16];
        uint32_t next;
        count = panel_log_read(UINT32_MAX, public_event, NULL, events, 16, 64, &next);
        for (size_t i = 0; i < count; i++) {
            const alarm_event_t *e = &events[i].ev;
            SEND_LINE(
                "event=%lu boot=%lu uptime_s=%lu type=%s partition=%u zone=%u arg=%u arg2=%u\n",
                (unsigned long)events[i].index, (unsigned long)events[i].boot,
                (unsigned long)events[i].uptime_s, alarm_event_name(e->type), e->partition, e->zone,
                e->arg, e->arg2);
        }
    }
    err = httpd_resp_send_chunk(r, NULL, 0);
done:
    free(snap);
    return err;
#undef SEND_LINE
}
static esp_err_t control(httpd_req_t *r)
{
    if (!authorized(r))
        return fail(r, "403 Forbidden", "Unauthorized");
    char b[160], bboot[33], lease[33], action[12], extra;
    unsigned seq, x, y, pressed;
    if (!body(r, b, sizeof(b)))
        return fail(r, "400 Bad Request", "Invalid body");
    if (sscanf(b, "%32s %11s %c", bboot, action, &extra) == 2 && strcmp(bboot, boot) == 0) {
        if (strcmp(action, "acquire") == 0 && A.control &&
            esp_timer_get_time() >= controller_until) {
            A.control(false);
            if (A.control(true)) {
                uint8_t nonce[16];
                esp_fill_random(nonce, sizeof(nonce));
                hex(nonce, 16, controller);
                controller_until = esp_timer_get_time() + 2000000;
                return fail(r, "200 OK", controller);
            }
        }
    }
    if (sscanf(b, "%32s %32s %11s %c", bboot, lease, action, &extra) == 3 &&
        strcmp(bboot, boot) == 0 && controller[0] && strcmp(lease, controller) == 0 &&
        strcmp(action, "release") == 0) {
        if (A.control)
            A.control(false);
        controller[0] = 0;
        controller_until = 0;
        return fail(r, "200 OK", "released");
    }
    if (sscanf(b, "%32s %32s %11s %u %u %u %u %c", bboot, lease, action, &seq, &x, &y, &pressed,
               &extra) == 7 &&
        strcmp(bboot, boot) == 0 && controller[0] && strcmp(lease, controller) == 0 &&
        esp_timer_get_time() < controller_until && strcmp(action, "input") == 0 && x < A.width &&
        y < A.height && pressed <= 1 && A.input &&
        A.input(seq, (uint16_t)x, (uint16_t)y, pressed != 0)) {
        controller_until = esp_timer_get_time() + 2000000;
        return fail(r, "200 OK", "accepted");
    }
    return fail(r, "409 Conflict", "Control unavailable, expired or stale input");
}
static esp_err_t upload(httpd_req_t *r)
{
    if (!authorized(r))
        return fail(r, "403 Forbidden", "Unauthorized");
    const char *why = NULL;
    if (!panel_ota_system_idle(&why))
        return fail(r, "409 Conflict", "Disarm and leave installer mode first");
    char shahex[65], op[33], older[4] = "", reason[96] = "Upload failed", version[32];
    uint8_t sha[32], opbytes[16];
    if (httpd_req_get_hdr_value_str(r, "X-Image-SHA256", shahex, sizeof(shahex)) != ESP_OK ||
        !unhex(shahex, sha, 32) ||
        httpd_req_get_hdr_value_str(r, "X-Operation", op, sizeof(op)) != ESP_OK ||
        !unhex(op, opbytes, 16))
        return fail(r, "400 Bad Request", "Image hash and operation required");
    httpd_req_get_hdr_value_str(r, "X-Allow-Older", older, sizeof(older));
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    if (!slot || r->content_len > slot->size || r->content_len < 288)
        return fail(r, "400 Bad Request", "Invalid image size");
    if (strcmp(op, uploaded_op) == 0)
        return fail(r, "409 Conflict", "Operation already staged; query info");
    esp_err_t err = panel_ota_begin((uint32_t)r->content_len, sha, strcmp(older, "yes") == 0);
    if (err != ESP_OK)
        return fail(r, "409 Conflict", "OTA writer busy or probation active");
    uploaded_op[0] = 0;
    uploaded_elf[0] = 0;
    atomic_store(&receiving, true);
    if (A.control)
        A.control(false);
    controller_until = 0;
    controller[0] = 0;
    record("upload started");
    uint8_t *buf = malloc(4096);
    size_t left = r->content_len;
    int64_t deadline = esp_timer_get_time() + 300000000;
    if (!buf)
        err = ESP_ERR_NO_MEM;
    while (err == ESP_OK && left) {
        if (esp_timer_get_time() > deadline || !authorized(r)) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        int n = httpd_req_recv(r, (char *)buf, left < 4096 ? left : 4096);
        if (n <= 0) {
            err = ESP_FAIL;
            break;
        }
        err = panel_ota_write(buf, (size_t)n, reason, sizeof(reason));
        left -= (size_t)n;
    }
    free(buf);
    if (err == ESP_OK)
        err = panel_ota_end(version, sizeof(version), reason, sizeof(reason));
    else
        panel_ota_abort();
    atomic_store(&receiving, false);
    if (err != ESP_OK) {
        record("upload failed; running image unchanged");
        return fail(r, "400 Bad Request", reason);
    }
    esp_app_desc_t d;
    if (esp_ota_get_partition_description(slot, &d) != ESP_OK ||
        strcmp(d.project_name, esp_app_get_description()->project_name) != 0) {
        panel_ota_discard(); /* not left "ready" for the console's `ota apply` either */
        record("network update refused: firmware profile does not match");
        return fail(r, "409 Conflict",
                    "Network updates must match the running board and firmware profile");
    }
    memcpy(uploaded_op, op, 33);
    hex(d.app_elf_sha256, 32, uploaded_elf);
    record("signed image verified; awaiting explicit install");
    return info(r);
}
/* ---- the voice pack (docs/features/08 voice-first-build.md D12): PUT installs a whole signed
 * .robovoice file, GET reads the voice's state. The install never changes voice_enabled or the
 * bench silence (AUD-025); panel_voice_install() refuses while armed, in an alarm or an update. */
#define VOICE_MIN 8192u
#define VOICE_MAX 970752u
static esp_err_t voice_json(httpd_req_t *r, const char *status, const char *json)
{
    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, json, HTTPD_RESP_USE_STRLEN);
}
static esp_err_t voice_put(httpd_req_t *r)
{
    if (!authorized(r))
        return fail(r, "403 Forbidden", "Unauthorized");
    const size_t n = r->content_len;
    if (n > VOICE_MAX)
        return fail(r, "413 Payload Too Large", "Voice pack too big");
    if (n < VOICE_MIN || n % 4096)
        return fail(r, "400 Bad Request", "Voice pack size must be a multiple of 4096, 8192..970752");
    uint8_t *file = heap_caps_aligned_alloc(4, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!file)
        return voice_json(r, "507 Insufficient Storage", "{\"result\":\"no_space\"}");
    size_t got = 0;
    int64_t deadline = esp_timer_get_time() + 300000000;
    while (got < n) {
        if (esp_timer_get_time() > deadline || !authorized(r))
            break;
        int k = httpd_req_recv(r, (char *)file + got, n - got);
        if (k <= 0)
            break;
        got += (size_t)k;
    }
    if (got != n) {
        heap_caps_free(file);
        record("voice pack upload failed");
        return fail(r, "400 Bad Request", "Voice pack upload incomplete");
    }
    const panel_voice_install_t result = panel_voice_install(file, n);
    heap_caps_free(file);
    record(result == PANEL_VOICE_INSTALL_OK ? "voice pack installed" : "voice pack refused");
    const char *status = "500 Internal Server Error";
    switch (result) {
    case PANEL_VOICE_INSTALL_OK: status = "200 OK"; break;
    case PANEL_VOICE_INSTALL_LOCKED:
    case PANEL_VOICE_INSTALL_BUSY: status = "409 Conflict"; break;
    case PANEL_VOICE_INSTALL_TOO_BIG:
    case PANEL_VOICE_INSTALL_BAD_SIGNATURE:
    case PANEL_VOICE_INSTALL_BAD_PACK: status = "400 Bad Request"; break;
    case PANEL_VOICE_INSTALL_NO_SPACE: status = "507 Insufficient Storage"; break;
    case PANEL_VOICE_INSTALL_UNAVAILABLE: status = "503 Service Unavailable"; break;
    default: break;
    }
    char json[64];
    snprintf(json, sizeof(json), "{\"result\":\"%s\"}", panel_voice_install_name(result));
    return voice_json(r, status, json);
}
/* ---- the asset packs (docs/features/39 asset-packs.md "The panel", OPT-006): PUT installs a
 * whole signed .roboasset file (letters, picture or certificates; the kind is in its header), GET
 * reads each kind's state. asset_packs_install() refuses while armed, in an alarm or an update. */
#define ASSET_MIN 8192u
#define ASSET_MAX PA_PICTURE_MAX_FILE /* the largest kind's bound; each kind's own is the service's */
static void asset_put_json(char *out, size_t cap, const char *result, uint8_t kind)
{
    asset_info_t list[ASSET_PACKS_KINDS];
    const size_t n = asset_packs_list(list, ASSET_PACKS_KINDS);
    const char *state = "";
    for (size_t i = 0; i < n; i++)
        if (list[i].kind == kind)
            state = asset_state_name(list[i].state);
    snprintf(out, cap, "{\"result\":\"%s\",\"kind\":\"%s\",\"state\":\"%s\"}", result,
             asset_kind_name(kind), state);
}
static esp_err_t asset_put(httpd_req_t *r)
{
    if (!authorized(r))
        return fail(r, "403 Forbidden", "Unauthorized");
    const size_t n = r->content_len;
    if (n > ASSET_MAX)
        return voice_json(r, "413 Payload Too Large", "{\"result\":\"size\"}");
    if (n < ASSET_MIN || n % 4096)
        return voice_json(r, "400 Bad Request", "{\"result\":\"size\"}");
    uint8_t *file = heap_caps_aligned_alloc(4, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!file)
        return voice_json(r, "507 Insufficient Storage", "{\"result\":\"no_space\"}");
    size_t got = 0;
    int64_t deadline = esp_timer_get_time() + 300000000;
    while (got < n) {
        if (esp_timer_get_time() > deadline || !authorized(r))
            break;
        int k = httpd_req_recv(r, (char *)file + got, n - got);
        if (k <= 0)
            break;
        got += (size_t)k;
    }
    if (got != n) {
        heap_caps_free(file);
        record("asset pack upload failed");
        return fail(r, "400 Bad Request", "Asset pack upload incomplete");
    }
    uint8_t kind = 0;
    const asset_result_t result = asset_packs_install(file, n, &kind);
    heap_caps_free(file);
    record(result == ASSET_OK ? "asset pack installed" : "asset pack refused");
    const char *status = "500 Internal Server Error";
    switch (result) {
    case ASSET_OK: status = "200 OK"; break;
    case ASSET_ERR_LOCKED:
    case ASSET_ERR_BUSY: status = "409 Conflict"; break;
    case ASSET_ERR_SIZE: status = "413 Payload Too Large"; break; /* over its kind's bound */
    case ASSET_ERR_SIGNATURE:
    case ASSET_ERR_FORMAT:
    case ASSET_ERR_KIND:
    case ASSET_ERR_DRAFT: status = "400 Bad Request"; break;
    case ASSET_ERR_NO_SPACE: status = "507 Insufficient Storage"; break;
    case ASSET_ERR_OFF: status = "503 Service Unavailable"; break;
    default: break;
    }
    char json[128];
    asset_put_json(json, sizeof(json), asset_result_name(result), kind);
    return voice_json(r, status, json);
}

/* src as a JSON string's content: `"` and `\` escaped (a pack's version is any printable text). */
static void json_text(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (; *src != '\0' && o + 2u < cap; src++) {
        if (*src == '"' || *src == '\\')
            dst[o++] = '\\';
        dst[o++] = *src;
    }
    dst[o] = '\0';
}

/* GET /dev/v1/asset: {"kinds":[{"kind","name","state","version","match","bytes","draft","why"}
 * ... letters, picture, certificates], "free_bytes"}; "kinds" is empty without a pack store. */
static esp_err_t asset_get(httpd_req_t *r)
{
    if (!authorized(r))
        return fail(r, "403 Forbidden", "Unauthorized");
    asset_info_t list[ASSET_PACKS_KINDS];
    const size_t n = asset_packs_list(list, ASSET_PACKS_KINDS);
    char json[1024];
    size_t o = (size_t)snprintf(json, sizeof(json), "{\"kinds\":[");
    for (size_t i = 0; i < n && o < sizeof(json); i++) {
        const asset_info_t *a = &list[i];
        char version[2u * sizeof(a->version)];
        json_text(version, sizeof(version), a->version);
        o += (size_t)snprintf(json + o, sizeof(json) - o,
                              "%s{\"kind\":\"%s\",\"name\":\"%s\",\"state\":\"%s\",\"version\":\"%s\","
                              "\"match\":\"0x%08" PRIx32 "\",\"bytes\":%" PRIu32 ",\"draft\":%s,\"why\":\"%s\"}",
                              i ? "," : "", asset_kind_name(a->kind), a->name, asset_state_name(a->state),
                              version, a->match, a->bytes, a->draft ? "true" : "false", a->why);
    }
    if (o < sizeof(json))
        snprintf(json + o, sizeof(json) - o, "],\"free_bytes\":%" PRIu32 "}", asset_packs_free_bytes());
    return voice_json(r, "200 OK", json);
}
#if CONFIG_ALARM_BUILD_DEV
/* PUT /dev/v1/play: 16 kHz mono PCM16 (little-endian, no header) heard through the panel's
 * speech path (panel_voice_bench_play, docs/features/08 AUD-030), for trying a voice's joins and
 * levels on the panel without a pack. Answers at once; the audio plays after what speaks now. */
static esp_err_t play(httpd_req_t *r)
{
    if (!authorized(r))
        return fail(r, "403 Forbidden", "Unauthorized");
    const size_t n = r->content_len;
    if (n == 0 || n % 2 || n > 2u * PANEL_VOICE_BENCH_MAX_SAMPLES)
        return voice_json(r, "413 Payload Too Large", "{\"result\":\"too_long\"}");
    int16_t *pcm = heap_caps_aligned_alloc(4, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm)
        return voice_json(r, "507 Insufficient Storage", "{\"result\":\"no_space\"}");
    size_t got = 0;
    int64_t deadline = esp_timer_get_time() + 60000000;
    while (got < n) {
        if (esp_timer_get_time() > deadline || !authorized(r))
            break;
        int k = httpd_req_recv(r, (char *)pcm + got, n - got);
        if (k <= 0)
            break;
        got += (size_t)k;
    }
    if (got != n) {
        heap_caps_free(pcm);
        return fail(r, "400 Bad Request", "Audio upload incomplete");
    }
    const panel_voice_bench_t result = panel_voice_bench_play(pcm, n / 2u);
    if (result != PANEL_VOICE_BENCH_OK)
        heap_caps_free(pcm);
    char json[64];
    snprintf(json, sizeof(json), "{\"result\":\"%s\",\"ms\":%u}",
             result == PANEL_VOICE_BENCH_OK ? "ok" : result == PANEL_VOICE_BENCH_TOO_LONG ? "too_long"
                                                                                          : "unavailable",
             (unsigned)(n / 32u));
    return voice_json(r, result == PANEL_VOICE_BENCH_OK ? "200 OK" : "503 Service Unavailable", json);
}
/* POST /dev/v1/say "<gap_ms> <clip-id>...": the panel says up to eight clip IDs (decimal or 0x
 * hex) from its installed pack as a Preview does, through its own renderer, word gap and Voice
 * volume (panel_voice_bench_say, docs/features/08 AUD-030); a gap of 0 is the pack's, any other
 * stays until the next one. Answers at once with the gap and the volume it plays with. */
static esp_err_t say(httpd_req_t *r)
{
    if (!authorized(r))
        return fail(r, "403 Forbidden", "Unauthorized");
    char b[96], *p, *end = b;
    uint16_t ids[8];
    size_t n = 0;
    bool ok = body(r, b, sizeof(b));
    const unsigned long gap = ok ? strtoul(b, &end, 10) : 0;
    ok = ok && end != b && gap <= PANEL_VOICE_BENCH_GAP_MAX_MS;
    for (p = ok ? end : b; ok;) {
        p += strspn(p, " \r\n");
        if (*p == 0)
            break;
        const unsigned long id = strtoul(p, &end, 0);
        ok = end != p && id != 0 && id <= 0xFFFF && n < 8;
        if (ok)
            ids[n++] = (uint16_t)id;
        p = end;
    }
    if (!ok || n == 0)
        return voice_json(r, "400 Bad Request", "{\"result\":\"bad_words\"}");
    uint16_t used = 0;
    uint8_t volume = 0;
    const panel_voice_preview_t result = panel_voice_bench_say(ids, n, (uint16_t)gap, &used, &volume);
    static const char *const names[] = {"ok", "unavailable", "bad_words", "too_long", "quiet"};
    char json[96];
    snprintf(json, sizeof(json), "{\"result\":\"%s\",\"gap_ms\":%u,\"volume\":%u}",
             (unsigned)result < 5u ? names[result] : "unavailable", (unsigned)used, (unsigned)volume);
    return voice_json(r,
                      result == PANEL_VOICE_PREVIEW_OK ? "200 OK"
                      : result == PANEL_VOICE_PREVIEW_BAD_WORDS || result == PANEL_VOICE_PREVIEW_TOO_LONG
                          ? "400 Bad Request"
                          : "503 Service Unavailable",
                      json);
}
#endif
static esp_err_t voice_get(httpd_req_t *r)
{
    if (!authorized(r))
        return fail(r, "403 Forbidden", "Unauthorized");
    static const char *const states[] = {"off", "none", "loading", "ready", "unavailable"};
    panel_voice_status_t st;
    panel_voice_status(&st);
    char why[96] = "";
    for (size_t i = 0, o = 0; st.why && st.why[i] && o + 2 < sizeof(why); i++)
        if ((unsigned char)st.why[i] >= 32 && st.why[i] != '"' && st.why[i] != '\\')
            why[o++] = st.why[i], why[o] = 0;
    char json[512];
    snprintf(json, sizeof(json),
             "{\"state\":\"%s\",\"draft\":%s,\"locale\":\"%s\",\"voice_id\":\"%s\","
             "\"pack_revision\":%" PRIu32 ",\"clips\":%u,\"file_bytes\":%" PRIu32 ",\"why\":\"%s\","
             "\"utterances\":%" PRIu32 ",\"completed\":%" PRIu32 ",\"cancelled\":%" PRIu32 ","
             "\"underruns\":%" PRIu32 ",\"decode_errors\":%" PRIu32 ",\"enabled\":%s,"
             "\"volume\":%u,\"word_gap_ms\":%u}",
             (unsigned)st.state < 5 ? states[st.state] : "unknown", st.draft ? "true" : "false",
             st.locale, st.voice_id, st.pack_revision, (unsigned)st.clips, st.file_bytes, why,
             st.utterances, st.completed, st.cancelled, st.underruns, st.decode_errors,
             st.enabled ? "true" : "false", (unsigned)st.volume, (unsigned)st.word_gap_ms);
    return voice_json(r, "200 OK", json);
}
#if CONFIG_ALARM_BUILD_DEV
/* POST /dev/v1/voice "volume <0-100>": the panel's Voice volume (AUD-032), saved and applied as
 * its Settings slider does, for the voice bench; answers as GET does, with the volume after it. */
static esp_err_t voice_post(httpd_req_t *r)
{
    if (!authorized(r))
        return fail(r, "403 Forbidden", "Unauthorized");
    char b[24], extra;
    unsigned volume;
    if (!body(r, b, sizeof(b)) || sscanf(b, "volume %u %c", &volume, &extra) != 1 || volume > 100)
        return voice_json(r, "400 Bad Request", "{\"result\":\"bad_volume\"}");
    panel_prefs_t p;
    panel_prefs_load(&p);
    p.voice_volume = (uint8_t)volume;
    if (panel_prefs_save(&p) != ESP_OK)
        return voice_json(r, "500 Internal Server Error", "{\"result\":\"failed\"}");
    sounder_set_voice_volume((uint8_t)volume);
    panel_voice_set_volume((uint8_t)volume);
    if (A.preferences_changed)
        A.preferences_changed(); /* the touchscreen's own copy follows, as after the Panel
                                  * Manager's display save: its next change can't put it back */
    return voice_get(r);
}
#endif
static esp_err_t install(httpd_req_t *r)
{
    if (!authorized(r))
        return fail(r, "403 Forbidden", "Unauthorized");
    char b[40];
    const char *why = NULL;
    if (!body(r, b, sizeof(b)) || strcmp(b, uploaded_op) != 0 || !uploaded_op[0])
        return fail(r, "409 Conflict", "No matching staged operation");
    if (!panel_ota_system_idle(&why))
        return fail(r, "409 Conflict", "Panel is not idle");
    uint8_t elf[32];
    if (!unhex(uploaded_elf, elf, 32) || panel_ota_remote_prepare(b, elf) != ESP_OK)
        return fail(r, "409 Conflict", "Staged image changed");
    if (panel_ota_apply(NULL, 0) != ESP_OK)
        return fail(r, "409 Conflict", "Cannot install");
    record("install requested; reboot still gated by alarm task");
    return fail(r, "202 Accepted", "Restart requested; reconnect and confirm expected image");
}
static esp_err_t confirm(httpd_req_t *r)
{
    if (!authorized(r))
        return fail(r, "403 Forbidden", "Unauthorized");
    char b[80], op[33], bboot[33], extra;
    if (!body(r, b, sizeof(b)) || sscanf(b, "%32s %32s %c", op, bboot, &extra) != 2 ||
        strcmp(bboot, boot) != 0 || !panel_ota_remote_confirm(op))
        return fail(r, "409 Conflict", "Wrong operation or boot");
    record("authenticated reconnect confirmed; health probation continues");
    return fail(r, "200 OK", "confirmed");
}
static esp_err_t cancel(httpd_req_t *r)
{
    if (!authorized(r))
        return fail(r, "403 Forbidden", "Unauthorized");
    char b[40];
    if (!body(r, b, sizeof(b)) || strcmp(b, uploaded_op) != 0)
        return fail(r, "409 Conflict", "Wrong operation");
    panel_ota_cancel();
    uploaded_op[0] = 0;
    uploaded_elf[0] = 0;
    return fail(r, "200 OK", "cancelled");
}
/* ---- Z-Wave (docs/features/30 "Z-Wave"): the adapter's JSON, bounded both ways. What a request
 * may do is the adapter's allowlist and the panel's own rules; nothing here forwards bytes to the
 * radio, and the routes exist only when the adapter has Z-Wave. */
static esp_err_t send_json(httpd_req_t *r, const char *json, size_t length)
{
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, json, (ssize_t)length);
}
/* A sequence number: decimal digits only, within 64 bits. */
static bool parse_u64(const char *s, uint64_t *out)
{
    uint64_t v = 0;
    if (!*s)
        return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return false;
        const uint64_t d = (uint64_t)(*s - '0');
        if (v > (UINT64_MAX - d) / 10u)
            return false;
        v = v * 10u + d;
    }
    *out = v;
    return true;
}
static esp_err_t zwave_state(httpd_req_t *r)
{
    if (!authorized(r))
        return fail(r, "403 Forbidden", "Unauthorized");
    if (!A.zwave_state)
        return fail(r, "404 Not Found", "No Z-Wave in this firmware");
    char *json = alarm_mem_slow_malloc(DEV_ZWAVE_STATE_MAX);
    if (!json)
        return fail(r, "503 Service Unavailable", "Memory busy");
    const size_t n = A.zwave_state(json, DEV_ZWAVE_STATE_MAX);
    esp_err_t err = n && n < DEV_ZWAVE_STATE_MAX
                        ? send_json(r, json, n)
                        : fail(r, "503 Service Unavailable", "Z-Wave state unavailable; retry");
    free(json);
    return err;
}
static esp_err_t zwave_trace(httpd_req_t *r)
{
    if (!authorized(r))
        return fail(r, "403 Forbidden", "Unauthorized");
    if (!A.zwave_trace)
        return fail(r, "404 Not Found", "No Z-Wave frame trace in this firmware");
    /* ?since=N, the only parameter; none: from the oldest entry kept */
    uint64_t since = 0;
    char query[64], value[24];
    const size_t query_len = httpd_req_get_url_query_len(r);
    if (query_len > 0 &&
        (query_len >= sizeof(query) ||
         httpd_req_get_url_query_str(r, query, sizeof(query)) != ESP_OK ||
         httpd_query_key_value(query, "since", value, sizeof(value)) != ESP_OK ||
         !parse_u64(value, &since)))
        return fail(r, "400 Bad Request", "Expected ?since=<sequence number>");
    char *json = alarm_mem_slow_malloc(DEV_ZWAVE_TRACE_MAX);
    if (!json)
        return fail(r, "503 Service Unavailable", "Memory busy");
    const size_t n = A.zwave_trace(since, json, DEV_ZWAVE_TRACE_MAX);
    esp_err_t err = n && n < DEV_ZWAVE_TRACE_MAX
                        ? send_json(r, json, n)
                        : fail(r, "503 Service Unavailable", "Z-Wave trace unavailable; retry");
    free(json);
    return err;
}
static esp_err_t zwave_request(httpd_req_t *r)
{
    if (!authorized(r))
        return fail(r, "403 Forbidden", "Unauthorized");
    if (!A.zwave_request)
        return fail(r, "404 Not Found", "No Z-Wave in this firmware");
    char b[DEV_ZWAVE_BODY_MAX], answer[DEV_ZWAVE_ANSWER_MAX];
    if (!body(r, b, sizeof(b)))
        return fail(r, "400 Bad Request", "Invalid body");
    answer[0] = 0;
    if (!A.zwave_request(b, strlen(b), answer, sizeof(answer)))
        return fail(r, "400 Bad Request", "Malformed request or unknown operation");
    const size_t n = strnlen(answer, sizeof(answer));
    if (!n || n >= sizeof(answer))
        return fail(r, "500 Internal Server Error", "No answer");
    return send_json(r, answer, n);
}
/* GET /dev/v1/zigbee: the radio, the devices, the operation, the install codes (zigbee_service.h
 * zigbee_service_state_json). */
static esp_err_t zigbee_state(httpd_req_t *r)
{
    if (!authorized(r))
        return fail(r, "403 Forbidden", "Unauthorized");
    if (!zigbee_present())
        return fail(r, "404 Not Found", "No Zigbee in this firmware");
    char *json = alarm_mem_slow_malloc(ZIGBEE_SERVICE_STATE_JSON_MAX);
    if (!json)
        return fail(r, "503 Service Unavailable", "Memory busy");
    const size_t n = zigbee_service_state_json(json, ZIGBEE_SERVICE_STATE_JSON_MAX);
    esp_err_t err = n && n < ZIGBEE_SERVICE_STATE_JSON_MAX
                        ? send_json(r, json, n)
                        : fail(r, "503 Service Unavailable", "Zigbee state unavailable; retry");
    free(json);
    return err;
}
/* A body that may hold an install code: wiped with stores the compiler keeps. */
static void wipe_body(char *b, size_t n)
{
    volatile char *v = b;
    for (size_t i = 0; i < n; i++)
        v[i] = 0;
}
/* POST /dev/v1/zigbee: {"op":...} as the console takes it (zigbee_service_request_json: every
 * partition disarmed, "confirm" for forget and channel, no network reset over the network). */
static esp_err_t zigbee_request(httpd_req_t *r)
{
    if (!authorized(r))
        return fail(r, "403 Forbidden", "Unauthorized");
    if (!zigbee_present())
        return fail(r, "404 Not Found", "No Zigbee in this firmware");
    char b[ZIGBEE_SERVICE_BODY_JSON_MAX], answer[ZIGBEE_SERVICE_ANSWER_JSON_MAX];
    if (!body(r, b, sizeof(b))) {
        wipe_body(b, sizeof(b)); /* a body cut short may still hold part of an install code */
        return fail(r, "400 Bad Request", "Invalid body");
    }
    answer[0] = 0;
    const bool ok = zigbee_service_request_json(b, strlen(b), answer, sizeof(answer));
    wipe_body(b, sizeof(b)); /* an install code may have come in it */
    if (!ok)
        return fail(r, "400 Bad Request", "Malformed request or unknown operation");
    const size_t n = strnlen(answer, sizeof(answer));
    if (!n || n >= sizeof(answer))
        return fail(r, "500 Internal Server Error", "No answer");
    return send_json(r, answer, n);
}
static void add_uri(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *))
{
    const httpd_uri_t u = {.uri = uri, .method = method, .handler = handler};
    if (httpd_register_uri_handler(server, &u) != ESP_OK)
        record("route registration failed");
}
static int __attribute__((unused)) console(int argc, char **argv)
{
    /* The console runs one command at a time on a 6 KiB stack: the 2 KiB texts below share
     * one buffer in PSRAM rather than living on that stack. */
    static char text[CERT_MAX * 2 + 1] ALARM_SLOW_BSS;
    if (argc == 2 && strcmp(argv[1], "describe") == 0) {
        describe(text, sizeof(text));
        printf("DEV INFO %s\n", text);
        return 0;
    }
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "status") == 0)) {
        net_wifi_status_t wifi;
        net_wifi_get_status(&wifi);
        hex(cert, cert_len, text);
        printf("DEV ID %s IP %s PORT %d ENABLED %d\nDEV CERT %s\n", identity, wifi.ip, PORT,
               atomic_load(&enabled), text);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "revoke") == 0) {
        atomic_store(&enabled, false);
        nvs_handle_t h;
        esp_err_t e = nvs_open("devrem", NVS_READWRITE, &h);
        if (e == ESP_OK) {
            e = nvs_erase_key(h, "peer");
            if (e == ESP_ERR_NVS_NOT_FOUND)
                e = ESP_OK;
            if (e == ESP_OK)
                e = nvs_commit(h);
            nvs_close(h);
        }
        atomic_store(&revoke_pending, e != ESP_OK);
        atomic_fetch_add(&generation, 1);
        printf("DEV REVOKE %s\n", esp_err_to_name(e));
        return e == ESP_OK ? 0 : 1;
    }
    if (argc == 2 && strcmp(argv[1], "begin") == 0) {
        pending_len = 0;
        puts("DEV BEGIN OK");
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "chunk") == 0) {
        size_t n = strlen(argv[2]) / 2;
        if (n && n <= 96 && pending_len + n <= CERT_MAX &&
            unhex(argv[2], pending + pending_len, n)) {
            pending_len += n;
            printf("DEV CHUNK %u\n", (unsigned)pending_len);
            return 0;
        }
    }
    if (argc == 2 && strcmp(argv[1], "commit") == 0) {
        mbedtls_x509_crt p;
        mbedtls_x509_crt_init(&p);
        uint8_t fp[32];
        /* Exactly one certificate and nothing after it: the fingerprint is compared with the
         * TLS peer's raw certificate, so trailing bytes here would never authorize anyone. */
        bool ok = pending_len > 0 && mbedtls_x509_crt_parse_der(&p, pending, pending_len) == 0 &&
                  p.raw.len == pending_len && digest(pending, pending_len, fp);
        mbedtls_x509_crt_free(&p);
        if (ok) {
            nvs_handle_t h;
            esp_err_t err = nvs_open("devrem", NVS_READWRITE, &h);
            if (err == ESP_OK) {
                err = nvs_set_blob(h, "peer", pending, pending_len);
                if (err == ESP_OK)
                    err = nvs_commit(h);
                nvs_close(h);
            }
            if (err == ESP_OK) {
                xSemaphoreTake(trust_lock, portMAX_DELAY);
                memcpy(peer, pending, pending_len);
                peer_len = pending_len;
                memcpy(peer_fp, fp, 32);
                atomic_store(&revoke_pending, false);
                atomic_store(&enabled, true);
                xSemaphoreGive(trust_lock);
                atomic_fetch_add(&generation, 1);
                puts("DEV ENROLLED");
                return 0;
            }
        }
    }
    puts("dev [status|describe|begin|chunk <public certificate hex>|commit|revoke]");
    return 1;
}
static void maintenance(void *arg)
{
    (void)arg;
    unsigned seen = 0;
    bool advertised = false;
    uint32_t tick = 0;
    for (;;) {
        if (atomic_load(&revoke_pending)) {
            nvs_handle_t h;
            esp_err_t e = nvs_open("devrem", NVS_READWRITE, &h);
            if (e == ESP_OK) {
                e = nvs_erase_key(h, "peer");
                if (e == ESP_ERR_NVS_NOT_FOUND)
                    e = ESP_OK;
                if (e == ESP_OK)
                    e = nvs_commit(h);
                nvs_close(h);
            }
            if (e == ESP_OK)
                atomic_store(&revoke_pending, false);
        }
        unsigned gen = atomic_load(&generation);
        if (gen != seen) {
            if (server) {
                httpd_ssl_stop(server);
                server = NULL;
            }
            if (advertised) {
                mdns_service_remove("_roboalarms-dev", "_tcp");
                advertised = false;
            }
            if (!(atomic_load(&enabled) && atomic_load(&allowed))) {
                /* Access revoked or switched off: an install still waiting for idle would
                 * restart the panel into an image nobody can confirm any more. The server is
                 * stopped, so the worker's staging record can be cleared from here. */
                panel_ota_cancel_remote();
                uploaded_op[0] = 0;
                uploaded_elf[0] = 0;
            }
            if (A.control)
                A.control(false);
            if (atomic_load(&enabled) && atomic_load(&allowed)) {
                xSemaphoreTake(trust_lock, portMAX_DELAY);
                httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();
                cfg.port_secure = PORT;
                cfg.servercert = cert;
                cfg.servercert_len = cert_len;
                cfg.prvtkey_pem = key;
                cfg.prvtkey_len = key_len;
                cfg.cacert_pem = peer;
                cfg.cacert_len = peer_len;
                /* Three, not two (docs/bugs/B016): the screen viewer's connection, an upload's,
                 * and room for the viewer to reconnect after a request timed out behind a long
                 * upload. With two, that reconnect made the purge below close the upload's
                 * connection right after its reply, and the workstation never saw the reply. */
                cfg.httpd.max_open_sockets = 3;
                cfg.httpd.lru_purge_enable = true; /* a dropped Wi-Fi link leaves stale sessions */
                /* the ten below, Z-Wave's three, Zigbee's two, the voice pack's two, the asset
                 * packs' two, and the speaker's two and the audio bench's three (dev builds) */
                cfg.httpd.max_uri_handlers = 24;
                cfg.httpd.recv_wait_timeout = 3;
                cfg.httpd.send_wait_timeout = 3;
                cfg.httpd.task_priority = 2;
                cfg.httpd.stack_size = 16384; /* injected taps run UI handlers here: same as the LVGL task */
                cfg.tls_handshake_timeout_ms = 5000;
                cfg.user_cb = tls_session;
                esp_err_t start_result = httpd_ssl_start(&server, &cfg);
                xSemaphoreGive(trust_lock);
                if (start_result == ESP_OK) {
                    add_uri("/dev/v1/info", HTTP_GET, info);
                    add_uri("/dev/v1/logs", HTTP_GET, log_read);
                    add_uri("/dev/v1/browser-grant", HTTP_POST, browser_grant);
                    add_uri("/dev/v1/diagnostics", HTTP_GET, diagnostics);
                    add_uri("/dev/v1/frame", HTTP_GET, frame);
                    add_uri("/dev/v1/control", HTTP_POST, control);
                    add_uri("/dev/v1/upload", HTTP_PUT, upload);
                    add_uri("/dev/v1/install", HTTP_POST, install);
                    add_uri("/dev/v1/confirm", HTTP_POST, confirm);
                    add_uri("/dev/v1/cancel", HTTP_POST, cancel);
                    add_uri("/dev/v1/voice", HTTP_GET, voice_get);
                    add_uri("/dev/v1/voice", HTTP_PUT, voice_put);
                    add_uri("/dev/v1/asset", HTTP_GET, asset_get);
                    add_uri("/dev/v1/asset", HTTP_PUT, asset_put);
#if CONFIG_ALARM_BUILD_DEV
                    add_uri("/dev/v1/speaker", HTTP_GET, speaker);
                    add_uri("/dev/v1/speaker", HTTP_POST, speaker);
                    add_uri("/dev/v1/play", HTTP_PUT, play);
                    add_uri("/dev/v1/say", HTTP_POST, say);
                    add_uri("/dev/v1/voice", HTTP_POST, voice_post);
#endif
                    if (zwave_present()) {
                        add_uri("/dev/v1/zwave", HTTP_GET, zwave_state);
                        add_uri("/dev/v1/zwave", HTTP_POST, zwave_request);
                    }
                    if (A.zwave_trace)
                        add_uri("/dev/v1/zwave/trace", HTTP_GET, zwave_trace);
                    /* always served: they answer 404 on a panel without Zigbee */
                    add_uri("/dev/v1/zigbee", HTTP_GET, zigbee_state);
                    add_uri("/dev/v1/zigbee", HTTP_POST, zigbee_request);
                    record("authenticated development service started");
                } else {
                    record("TLS listener failed to start");
                }
            }
            seen = gen;
        }
        net_wifi_status_t wifi;
        net_wifi_get_status(&wifi);
        if (server && !advertised && wifi.state == NET_WIFI_CONNECTED && mdns_init() == ESP_OK) {
            char hostname[32];
            snprintf(hostname, sizeof(hostname), "roboalarms-%s", identity);
            mdns_hostname_set(hostname);
            mdns_txt_item_t txt[] = {{"id", identity}, {"api", "1"}, {"model", A.board}};
            advertised = mdns_service_add("RoboAlarms development", "_roboalarms-dev", "_tcp", PORT,
                                          txt, 3) == ESP_OK;
        }
        if (++tick % 10 == 0) {
            char m[160];
            snprintf(m, sizeof(m), "health=%lu internal=%u largest=%u crypto_faults=%lu wifi=%d",
                     (unsigned long)panel_ota_health_stale(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned long)code_hash_faults(), wifi.state);
            record(m);
        }
        if (!server && atomic_load(&enabled) && atomic_load(&allowed) && tick % 5 == 0)
            atomic_fetch_add(&generation, 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
static void delete_locks(SemaphoreHandle_t a, SemaphoreHandle_t b)
{
    if (a)
        vSemaphoreDelete(a);
    if (b)
        vSemaphoreDelete(b);
}

esp_err_t dev_remote_start(const dev_remote_adapter_t *adapter)
{
    if (atomic_load(&started))
        return ESP_ERR_INVALID_STATE;
    /* The locks are built privately and published as a pair once the identity and the trust
     * store have loaded; a failure before then deletes them (M02). Once published, enrolment and
     * the log copy may hold them, so a start that fails later keeps them and the retry uses the
     * same pair (and the same frame buffer) instead of making new ones. */
    SemaphoreHandle_t new_trust = NULL, new_log = NULL;
    if (!trust_lock) {
        new_trust = xSemaphoreCreateMutex();
        new_log = xSemaphoreCreateMutex();
        if (!new_trust || !new_log) {
            delete_locks(new_trust, new_log);
            return ESP_ERR_NO_MEM;
        }
    }
    A = *adapter;
    uint8_t mac[6], nonce[16];
    if (esp_efuse_mac_get_default(mac) != ESP_OK) {
        delete_locks(new_trust, new_log);
        return ESP_FAIL;
    }
    hex(mac, 6, identity);
    esp_fill_random(nonce, sizeof(nonce));
    hex(nonce, 16, boot);
    nvs_handle_t h;
    esp_err_t err = nvs_open("devrem", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        delete_locks(new_trust, new_log);
        return err;
    }
    err = dev_cert_ensure(h, identity, cert, &cert_len, key, &key_len);
    peer_len = sizeof(peer);
    bool paired = nvs_get_blob(h, "peer", peer, &peer_len) == ESP_OK && peer_len > 0 &&
                  digest(peer, peer_len, peer_fp);
    nvs_close(h);
    if (err != ESP_OK) {
        delete_locks(new_trust, new_log);
        return err;
    }
    if (new_trust) {
        log_lock = new_log;
        trust_lock = new_trust;
    }
    atomic_store(&enabled, paired);
    atomic_store(&generation, 1);
    if (!pixels && A.frame) /* a headless build has no screen to send (S09) */
        pixels = alarm_mem_slow_malloc((size_t)(A.width / 2) * (A.height / 2) * 2);
#if CONFIG_ALARM_SERVICE_CONSOLE
    const esp_console_cmd_t cmd = {
        .command = "dev",
        .help = "Developer enrollment/status/revoke (public certificates only)",
        .func = console};
    err = esp_console_cmd_register(&cmd);
    if (err != ESP_OK)
        return err;
#endif
    /* esp_console_cmd_register replaces a command of the same name: a retry registers it again. */
    if (xTaskCreate(maintenance, "dev_remote", 6144, NULL, 1, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    atomic_store(&started, true);
    return ESP_OK;
}
