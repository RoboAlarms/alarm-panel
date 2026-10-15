/*
 * backup_service.c - the backup file on the panel (backup_service.h, docs/features/14 CFG-005,
 * CFG-006).
 *
 * Making one: every NVS entry the policy keeps (backup_policy_keeps), read as NVS stores it,
 * into the payload; sealed with the installer's password. A namespace that isn't there is
 * skipped; one that can't be opened, listed or read makes no file at all (backup_collect, S04). Restoring one: decrypted and checked
 * in full first (backup_check: this kind of panel, only entries the policy keeps, a
 * configuration the engine takes); only then is the restore logged, the engine's own writes
 * stopped, and namespace by namespace every entry the policy keeps erased and the file's
 * written (backup_restore_entry: an update server comes back without its accepted key, S05);
 * then the panel restarts. Entries the policy doesn't keep - the panel's identity, its
 * keys, the developer enrollment, counters - are never read, written or erased.
 *
 * The file's configuration is not stored as it is (docs/features/08 voice-first-build.md D07,
 * CORE-CFG-021): once the engine's writes are stopped, the running configuration is captured
 * and backup_restore_config() makes the file's safe to store over it - no spoken-name approval
 * comes back, and the voice revision counter never goes back. That blob replaces alarm/config
 * first, in place (never erased first) and committed, before the zones' sensors (zones/bind)
 * and everything else; if any of this fails before it is committed, nothing is written.
 *
 * A power cut while the entries are written leaves some namespaces restored and others not:
 * the panel starts with that mix and the installer restores again (the file is still at
 * hand). The window is the time NVS takes for a few kilobytes, well under a second.
 */
#include "backup_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "alarm/alarm_codec.h"
#include "alarm_proto/proto_fwsrv.h"
#include "alarm_service.h"
#include "backup_internal.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "panel_log.h"
#include "zone_service.h"

static const char *TAG = "backup";

#define RESTART_AFTER_US   (2000u * 1000u) /* the Panel Manager's answer goes out first */
#define LOG_FLUSH_MS       2000u
#define VALUE_MAX          BACKUP_BLOB_MAX /* the largest value an entry may have */
#define MIN_CREATED_S      1735689600u     /* 2025-01-01: a clock before this isn't set */

static const char *s_board = "";

void backup_service_set_board(const char *board_id)
{
    s_board = board_id != NULL ? board_id : "";
}

static void *big(size_t n)
{
    return heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM);
}

/* ---- NVS types --------------------------------------------------------------------------- */

static uint8_t to_backup_type(nvs_type_t t)
{
    switch (t) {
    case NVS_TYPE_U8: return BACKUP_T_U8;
    case NVS_TYPE_I8: return BACKUP_T_I8;
    case NVS_TYPE_U16: return BACKUP_T_U16;
    case NVS_TYPE_I16: return BACKUP_T_I16;
    case NVS_TYPE_U32: return BACKUP_T_U32;
    case NVS_TYPE_I32: return BACKUP_T_I32;
    case NVS_TYPE_U64: return BACKUP_T_U64;
    case NVS_TYPE_I64: return BACKUP_T_I64;
    case NVS_TYPE_STR: return BACKUP_T_STR;
    case NVS_TYPE_BLOB: return BACKUP_T_BLOB;
    default: return 0u;
    }
}

/* Reads one entry into buf (little-endian integers, a string without its NUL). */
static esp_err_t read_entry(nvs_handle_t h, const char *key, uint8_t type, uint8_t *buf,
                            size_t cap, uint32_t *len)
{
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    uint64_t v = 0;
    size_t n = cap;
    switch (type) {
    case BACKUP_T_U8: err = nvs_get_u8(h, key, (uint8_t *)&v); n = 1; break;
    case BACKUP_T_I8: err = nvs_get_i8(h, key, (int8_t *)&v); n = 1; break;
    case BACKUP_T_U16: err = nvs_get_u16(h, key, (uint16_t *)&v); n = 2; break;
    case BACKUP_T_I16: err = nvs_get_i16(h, key, (int16_t *)&v); n = 2; break;
    case BACKUP_T_U32: err = nvs_get_u32(h, key, (uint32_t *)&v); n = 4; break;
    case BACKUP_T_I32: err = nvs_get_i32(h, key, (int32_t *)&v); n = 4; break;
    case BACKUP_T_U64: err = nvs_get_u64(h, key, &v); n = 8; break;
    case BACKUP_T_I64: err = nvs_get_i64(h, key, (int64_t *)&v); n = 8; break;
    case BACKUP_T_STR:
        err = nvs_get_str(h, key, (char *)buf, &n); /* n counts the NUL */
        if (err == ESP_OK) {
            *len = n > 0u ? (uint32_t)(n - 1u) : 0u;
        }
        return err;
    case BACKUP_T_BLOB:
        err = nvs_get_blob(h, key, buf, &n);
        if (err == ESP_OK) {
            *len = (uint32_t)n;
        }
        return err;
    default: return err;
    }
    if (err == ESP_OK) {
        for (size_t i = 0; i < n; i++) {
            buf[i] = (uint8_t)(v >> (8u * i)); /* the P4 is little-endian; spelled out anyway */
        }
        *len = (uint32_t)n;
    }
    return err;
}

static esp_err_t write_entry(nvs_handle_t h, const backup_entry_t *e, char *str)
{
    uint64_t v = 0;
    for (uint32_t i = 0; i < e->len && i < 8u; i++) {
        v |= (uint64_t)e->value[i] << (8u * i);
    }
    switch (e->type) {
    case BACKUP_T_U8: return nvs_set_u8(h, e->key, (uint8_t)v);
    case BACKUP_T_I8: return nvs_set_i8(h, e->key, (int8_t)(uint8_t)v);
    case BACKUP_T_U16: return nvs_set_u16(h, e->key, (uint16_t)v);
    case BACKUP_T_I16: return nvs_set_i16(h, e->key, (int16_t)(uint16_t)v);
    case BACKUP_T_U32: return nvs_set_u32(h, e->key, (uint32_t)v);
    case BACKUP_T_I32: return nvs_set_i32(h, e->key, (int32_t)(uint32_t)v);
    case BACKUP_T_U64: return nvs_set_u64(h, e->key, v);
    case BACKUP_T_I64: return nvs_set_i64(h, e->key, (int64_t)v);
    case BACKUP_T_STR:
        if (e->len > BACKUP_STR_MAX) {
            return ESP_ERR_INVALID_SIZE; /* backup_check refused it already */
        }
        memcpy(str, e->value, e->len);
        str[e->len] = '\0';
        return nvs_set_str(h, e->key, str);
    case BACKUP_T_BLOB: return nvs_set_blob(h, e->key, e->value, e->len);
    default: return ESP_ERR_INVALID_ARG;
    }
}

/* ---- NVS as backup_store_t (components/core/backup decides what each answer means, S04) ------ */

typedef struct {
    nvs_handle_t h;
    nvs_iterator_t it;
} nvs_port_t;

/* Only ESP_ERR_NVS_NOT_FOUND is absence: no such namespace, no entries, past the last one. Any
 * other error - out of memory, a damaged page, NVS not started - is a failure, never "empty". */
static backup_store_status_t store_status(esp_err_t err)
{
    if (err == ESP_OK) {
        return BACKUP_STORE_OK;
    }
    return err == ESP_ERR_NVS_NOT_FOUND ? BACKUP_STORE_ABSENT : BACKUP_STORE_FAILED;
}

static backup_store_status_t port_open(void *ctx, const char *ns)
{
    nvs_port_t *p = ctx;
    return store_status(nvs_open(ns, NVS_READONLY, &p->h));
}

static backup_store_status_t port_entry(nvs_port_t *p, esp_err_t err, char *key, uint8_t *type)
{
    if (err != ESP_OK) {
        return store_status(err);
    }
    nvs_entry_info_t info;
    if (nvs_entry_info(p->it, &info) != ESP_OK) {
        return BACKUP_STORE_FAILED;
    }
    snprintf(key, BACKUP_NAME_MAX + 1u, "%s", info.key);
    *type = to_backup_type(info.type);
    return BACKUP_STORE_OK;
}

static backup_store_status_t port_first(void *ctx, const char *ns, char *key, uint8_t *type)
{
    nvs_port_t *p = ctx;
    p->it = NULL;
    return port_entry(p, nvs_entry_find(NVS_DEFAULT_PART_NAME, ns, NVS_TYPE_ANY, &p->it), key,
                      type);
}

static backup_store_status_t port_next(void *ctx, char *key, uint8_t *type)
{
    nvs_port_t *p = ctx;
    return port_entry(p, nvs_entry_next(&p->it), key, type);
}

static void port_end(void *ctx)
{
    nvs_port_t *p = ctx;
    nvs_release_iterator(p->it); /* NULL after the last entry: nothing to release */
    p->it = NULL;
}

static backup_store_status_t port_read(void *ctx, const char *key, uint8_t type, uint8_t *buf,
                                       size_t cap, uint32_t *len)
{
    nvs_port_t *p = ctx;
    const esp_err_t err = read_entry(p->h, key, type, buf, cap, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s could not be read (%s)", key, esp_err_to_name(err));
        return BACKUP_STORE_FAILED; /* listed a moment ago: gone or unreadable is a failure */
    }
    return BACKUP_STORE_OK;
}

static void port_close(void *ctx)
{
    nvs_port_t *p = ctx;
    nvs_close(p->h);
}

static backup_store_t nvs_port(nvs_port_t *p)
{
    memset(p, 0, sizeof(*p));
    const backup_store_t s = {.ctx = p, .open = port_open, .first = port_first,
                              .next = port_next, .end = port_end, .read = port_read,
                              .close = port_close};
    return s;
}

/* ---- making a backup ---------------------------------------------------------------------- */

esp_err_t backup_service_make(const char *password, const alarm_source_t *src, uint8_t user,
                              uint8_t **file, size_t *len, backup_result_t *why)
{
    *file = NULL;
    *len = 0;
    *why = BACKUP_OK;
    if (!backup_password_ok(password)) {
        *why = BACKUP_ERR_PASSWORD;
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t *payload = big(BACKUP_PAYLOAD_MAX);
    uint8_t *value = big(VALUE_MAX + 1u);
    if (payload == NULL || value == NULL) {
        free(payload);
        free(value);
        return ESP_ERR_NO_MEM;
    }
    backup_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    const time_t now = time(NULL);
    meta.created_s = now >= (time_t)MIN_CREATED_S ? (uint64_t)now : 0u;
    snprintf(meta.firmware, sizeof(meta.firmware), "%s", esp_app_get_description()->version);
    snprintf(meta.board, sizeof(meta.board), "%s", s_board);

    backup_writer_t w;
    backup_writer_init(&w, payload, BACKUP_PAYLOAD_MAX, &meta);
    nvs_port_t port_ctx;
    const backup_store_t port = nvs_port(&port_ctx);
    bool fits = true;
    const char *unread = NULL; /* a namespace that couldn't be read: no file at all (S04) */
    for (const char *const *ns = backup_policy_namespaces(); fits && *ns != NULL; ns++) {
        if (backup_collect(&w, &port, *ns, value, VALUE_MAX, &fits) != BACKUP_STORE_OK) {
            unread = *ns;
            break;
        }
    }
    mbedtls_platform_zeroize(value, VALUE_MAX + 1u);
    free(value);
    size_t plen = 0;
    esp_err_t err = ESP_OK;
    if (unread != NULL) {
        ESP_LOGE(TAG, "the settings in %s could not be read: no backup made", unread);
        *why = BACKUP_ERR_CONTENT;
        err = ESP_FAIL;
    } else if (!fits || backup_writer_finish(&w, &plen) != BACKUP_OK) {
        ESP_LOGE(TAG, "the backup would not fit (%u entries)", (unsigned)w.count);
        *why = BACKUP_ERR_ARG;
        err = ESP_ERR_NO_MEM;
    }
    uint8_t *out = err == ESP_OK ? big(plen + BACKUP_OVERHEAD) : NULL;
    if (err == ESP_OK && out == NULL) {
        err = ESP_ERR_NO_MEM;
    }
    size_t flen = 0;
    if (err == ESP_OK) {
        const int64_t t0 = esp_timer_get_time();
        *why = backup_seal(backup_psa_port(), password, 0u, payload, plen, out,
                           plen + BACKUP_OVERHEAD, &flen);
        ESP_LOGI(TAG, "%u entries, %u bytes, sealed in %u ms", (unsigned)w.count,
                 (unsigned)flen, (unsigned)((esp_timer_get_time() - t0) / 1000));
        if (*why != BACKUP_OK) {
            err = ESP_FAIL;
        }
    }
    mbedtls_platform_zeroize(payload, BACKUP_PAYLOAD_MAX);
    free(payload);
    if (err != ESP_OK) {
        free(out);
        return err;
    }
    (void)alarm_service_backup_event(false, src, user);
    *file = out;
    *len = flen;
    return ESP_OK;
}

/* ---- restoring one ------------------------------------------------------------------------- */

static void restart_now(void *arg)
{
    (void)arg;
    esp_restart();
}

static void restart_soon(void)
{
    static esp_timer_handle_t t;
    const esp_timer_create_args_t args = {.callback = restart_now, .name = "restore"};
    if (t == NULL && esp_timer_create(&args, &t) != ESP_OK) {
        esp_restart();
    }
    (void)esp_timer_start_once(t, RESTART_AFTER_US);
}

/* The configuration: written in place by write_config(), never erased or written from the
 * file's entries. */
static bool is_config(const char *ns, const char *key)
{
    return strcmp(ns, "alarm") == 0 && strcmp(key, "config") == 0;
}

/* Every key of ns the policy keeps, erased: what the file doesn't have is gone afterwards. A
 * listing that fails fails the restore (S04): a namespace is never taken as cleared when it
 * couldn't be listed. */
#define CLEAR_ROUND 64u
static esp_err_t clear_namespace(nvs_handle_t h, const char *ns)
{
    char keys[CLEAR_ROUND][BACKUP_NAME_MAX + 1];
    nvs_port_t port_ctx;
    const backup_store_t port = nvs_port(&port_ctx);
    for (;;) {
        size_t n = 0;
        if (backup_erasable(&port, ns, keys, CLEAR_ROUND, &n) != BACKUP_STORE_OK) {
            ESP_LOGE(TAG, "%s could not be listed: not restored", ns);
            return ESP_FAIL;
        }
        for (size_t i = 0; i < n; i++) {
            const esp_err_t e = nvs_erase_key(h, keys[i]);
            if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) {
                return e;
            }
        }
        if (n < CLEAR_ROUND) {
            return ESP_OK; /* all of them; otherwise look again for the rest */
        }
    }
}

/* The reconciled configuration replaces the stored one in one NVS write (the old value stays
 * until the new one is complete), committed before anything else is touched. */
static esp_err_t write_config(const uint8_t *blob, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("alarm", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, "config", blob, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t apply(const uint8_t *payload, size_t len, const uint8_t *config,
                       size_t config_len)
{
    char *str = big(VALUE_MAX + 1u);
    if (str == NULL) {
        return ESP_ERR_NO_MEM;
    }
    uint8_t trust[PROTO_FWSRV_TRUST_BLOB_MAX]; /* the update server's record, without a key */
    esp_err_t err = write_config(config, config_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "alarm/config: %s: nothing restored", esp_err_to_name(err));
    }
    for (const char *const *ns = backup_policy_namespaces(); err == ESP_OK && *ns != NULL; ns++) {
        nvs_handle_t h;
        err = nvs_open(*ns, NVS_READWRITE, &h);
        if (err != ESP_OK) {
            break;
        }
        err = clear_namespace(h, *ns);
        backup_reader_t r;
        backup_meta_t meta;
        if (err == ESP_OK && backup_reader_init(&r, payload, len, &meta) != BACKUP_OK) {
            err = ESP_ERR_INVALID_ARG; /* checked already: can't happen */
        }
        while (err == ESP_OK) {
            backup_entry_t e;
            bool done;
            if (backup_reader_next(&r, &e, &done) != BACKUP_OK) {
                err = ESP_ERR_INVALID_ARG;
                break;
            }
            if (done) {
                break;
            }
            if (strcmp(e.ns, *ns) == 0 && !is_config(e.ns, e.key)) {
                /* as it is, left out (retired, or an update server that doesn't decode), or
                 * the update server without its accepted key (S05) */
                backup_entry_t out;
                bool write = false;
                if (backup_restore_entry(&e, trust, sizeof(trust), &out, &write) != BACKUP_OK) {
                    err = ESP_ERR_INVALID_ARG; /* can't happen: the buffer fits any record */
                } else if (write) {
                    err = write_entry(h, &out, str);
                } else {
                    ESP_LOGW(TAG, "%s/%s left out", e.ns, e.key);
                }
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "%s/%s: %s", e.ns, e.key, esp_err_to_name(err));
                }
            }
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    mbedtls_platform_zeroize(str, VALUE_MAX + 1u);
    free(str);
    return err;
}

esp_err_t backup_service_restore(const uint8_t *file, size_t len, const char *password,
                                 const alarm_source_t *src, uint8_t user,
                                 backup_summary_t *summary, backup_result_t *why)
{
    *why = BACKUP_OK;
    alarm_snapshot_t *snap = big(sizeof(*snap)); /* too big for the worker's stack */
    if (snap == NULL) {
        return ESP_ERR_NO_MEM;
    }
    alarm_service_get_snapshot(snap);
    const bool locked = alarm_snapshot_locked(snap);
    free(snap);
    if (!zone_service_all_disarmed() || locked) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t plen = 0;
    *why = backup_peek(file, len, &plen, NULL);
    if (*why != BACKUP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t *payload = big(plen);
    backup_scratch_t *scratch = big(sizeof(*scratch));
    backup_summary_t *sum = big(sizeof(*sum));
    esp_err_t err = (payload && scratch && sum) ? ESP_OK : ESP_ERR_NO_MEM;
    size_t got = 0;
    if (err == ESP_OK) {
        *why = backup_open(backup_psa_port(), password, file, len, payload, plen, &got);
        if (*why == BACKUP_OK) {
            *why = backup_check(payload, got, s_board, scratch, sum);
        }
        if (summary != NULL) {
            *summary = *sum;
        }
        if (*why != BACKUP_OK) {
            ESP_LOGW(TAG, "restore refused: %s%s%s%s", backup_result_name(*why),
                     sum->bad_ns[0] ? " (" : "", sum->bad_ns, sum->bad_ns[0] ? ")" : "");
            err = ESP_ERR_INVALID_ARG;
        }
    }
    if (scratch != NULL) {
        mbedtls_platform_zeroize(scratch, sizeof(*scratch));
    }
    free(scratch);
    free(sum);
    if (err == ESP_OK) {
        /* Checked in full: from here on the panel is committed to the file. */
        ESP_LOGW(TAG, "restoring a backup of %s made by firmware %s", s_board,
                 summary != NULL ? summary->meta.firmware : "?");
        /* Logged, and the engine writes no more: from here on the stored configuration is
         * the running one's last word, and its counter is captured after that. */
        err = alarm_service_backup_event(true, src, user);
        (void)panel_log_flush(LOG_FLUSH_MS);
        alarm_config_t *current = err == ESP_OK ? big(sizeof(*current)) : NULL;
        uint8_t *config = err == ESP_OK ? big(ALARM_CONFIG_BLOB_MAX) : NULL;
        scratch = err == ESP_OK ? big(sizeof(*scratch)) : NULL;
        size_t config_len = 0;
        if (err == ESP_OK && (current == NULL || config == NULL || scratch == NULL)) {
            err = ESP_ERR_NO_MEM;
        }
        if (err == ESP_OK) {
            err = alarm_service_get_config(current);
        }
        if (err == ESP_OK) {
            const backup_result_t r = backup_restore_config(payload, got, current, scratch, config,
                                                            ALARM_CONFIG_BLOB_MAX, &config_len);
            if (r != BACKUP_OK) {
                ESP_LOGE(TAG, "the file's configuration: %s", backup_result_name(r));
                err = ESP_FAIL;
            }
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "restore abandoned before writing: %s", esp_err_to_name(err));
            err = ESP_FAIL;
        } else {
            err = apply(payload, got, config, config_len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "restore stopped part way: %s", esp_err_to_name(err));
                err = ESP_FAIL;
            }
        }
        if (scratch != NULL) {
            mbedtls_platform_zeroize(scratch, sizeof(*scratch));
        }
        free(scratch);
        if (config != NULL) {
            mbedtls_platform_zeroize(config, ALARM_CONFIG_BLOB_MAX);
        }
        free(config);
        if (current != NULL) {
            mbedtls_platform_zeroize(current, sizeof(*current));
        }
        free(current);
        restart_soon(); /* the engine's writes are stopped: start again either way */
    }
    if (payload != NULL) {
        mbedtls_platform_zeroize(payload, plen);
    }
    free(payload);
    return err;
}

bool backup_service_self_test(void)
{
    return backup_psa_known_answers();
}
