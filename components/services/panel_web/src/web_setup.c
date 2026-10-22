/* web_setup.c - setting up a panel without a screen from a phone, and getting back into one
 * (docs/features/31 headless-setup.md, design approved 2026-10-18 by the owner; docs/features/42).
 * The adapter of the policy in web_setup_flow.c:
 *
 *   1. The panel opens its setup Wi-Fi (WPA2, the password SETUP_WIFI_PASSWORD, the same on every
 *      board: decided by the owner 2026-10-21, as nothing is printed on a board yet). A phone joins
 *      it and opens http://192.168.4.1/ - plain HTTP, phone to board, for the first setup only (the
 *      owner, 2026-10-21; panel_web.c plain_serve) - or any plain-HTTP address, which the setup
 *      Wi-Fi's name server (web_setup_dns.c) and the redirect bring there. Whoever is on the setup Wi-Fi claims the new panel - no
 *      setup code (the owner, 2026-10-21); a request from the home network never does.
 *   2. Installer access (user 1) gets its browser name and password, required; household access
 *      (the system master, user 2) may come with it or later, from the Panel Manager's People
 *      (the owner, 2026-10-21: the installer finishes the household's account on a computer).
 *      The ordinary Panel Manager turns on and the browser signs in as the installer.
 *      The panel makes a recovery key, shows it once, and waits until the person says it is kept.
 *   3. The home Wi-Fi is tried (net_wifi's two-minute trial) with the setup Wi-Fi still open. The
 *      person moves the phone to the home network, signs in at the panel's new address and
 *      confirms there; then the setup Wi-Fi closes. Unconfirmed, the panel goes back.
 *
 * Afterwards the BOOT button held five seconds and let go opens recovery for five minutes: the
 * setup Wi-Fi again, a sign-in, and a lost browser password replaced with the recovery key.
 * Nothing here arms, disarms or silences anything, and while a partition is armed or an alarm
 * sounds nothing here changes.
 *
 * Stored in NVS "websetup": "rec" (the record, a versioned 8-byte form, never the struct),
 * "rkey"
 * (the recovery key's salt and SHA-256, never the key). The first builds' "ap" (a random setup
 * Wi-Fi password) and "code" (a setup code) are erased at start. */
#include "web_setup.h"

#include "alarm/alarm_config.h"
#include "alarm_service.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/platform_util.h"
#include "net_wifi.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "web_extra.h"
#include "web_login.h"
#include "web_mem.h"
#include "web_setup_flow.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "web_setup";
static const char *NS = "websetup";

#define REC_VERSION   1u
#define REC_KEY_PEND  0x01u
#define REC_NET_OK    0x02u
/* The setup Wi-Fi's password: fixed, open only in setup and recovery (owner, 2026-10-21). */
#define SETUP_WIFI_PASSWORD "Robo1234"
#define KEY_LEN       25u /* the recovery key: five groups of five, about 125 bits */
#define SALT_LEN      16u
#define SESSION_SIGNED_IN UINT64_C(1) /* the trial's owner: whoever signs in with these accounts */
#define SESSION_RECOVERY  UINT64_C(2) /* a password replaced with the recovery key */

/* Letters and digits nobody misreads (no 0/O, 1/I/L). */
static const char LABEL_ALPHABET[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";

static SemaphoreHandle_t s_lock;
static web_setup_flow_t s_flow;
static bool s_headless, s_started;
static char s_ap_ssid[33], s_ap_pass[sizeof(SETUP_WIFI_PASSWORD)], s_fingerprint[65];
static esp_err_t (*s_enable_web)(void);
static web_gate_t s_gate; /* wrong recovery keys */
static uint64_t s_claim_id;
static char s_trial_token[33];
static bool s_ap_open;
static bool (*s_button_down)(void);
static web_setup_button_t s_button;
static esp_timer_handle_t s_button_timer;

/* What the commit of the write in progress does besides the record (web_setup_flow.h: one
 * transaction). */
static struct {
    const char *owner, *owner_pw, *installer, *installer_pw;
    uint8_t reset_user;
    const char *reset_pw;
    const char *why;
} s_op;

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static void random_text(char *out, size_t n, const char *alphabet)
{
    const size_t k = strlen(alphabet);
    for (size_t i = 0; i < n; i++) {
        uint32_t r;
        do {
            r = esp_random() & 0xFFu;
        } while (r >= 256u - 256u % k); /* no bias */
        out[i] = alphabet[r % k];
    }
    out[n] = '\0';
}

/* Upper case, without spaces and dashes: how a typed code or key is compared. */
static size_t normalise(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (; in && *in && n + 1 < cap; in++) {
        if (*in == ' ' || *in == '-')
            continue;
        out[n++] = (char)toupper((unsigned char)*in);
    }
    out[n] = '\0';
    return n;
}

static bool same(const char *a, const char *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++)
        d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

static bool sha256(const void *p, size_t n, uint8_t out[32])
{
    size_t got = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, p, n, out, 32, &got) == PSA_SUCCESS && got == 32;
}

/* ---- the stored record ------------------------------------------------------------------- */

static bool record_write(const web_setup_record_t *r)
{
    const uint8_t b[8] = {(uint8_t)REC_VERSION,
                          (uint8_t)r->stage,
                          (uint8_t)((r->recovery_key_pending ? REC_KEY_PEND : 0u) |
                                    (r->network_confirmed ? REC_NET_OK : 0u)),
                          0,
                          (uint8_t)(r->revision & 0xFFu),
                          (uint8_t)((r->revision >> 8) & 0xFFu),
                          (uint8_t)((r->revision >> 16) & 0xFFu),
                          (uint8_t)((r->revision >> 24) & 0xFFu)};
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_blob(h, "rec", b, sizeof(b));
        if (e == ESP_OK)
            e = nvs_commit(h);
        nvs_close(h);
    }
    if (e != ESP_OK)
        ESP_LOGE(TAG, "setup record not saved: %s", esp_err_to_name(e));
    return e == ESP_OK;
}

static bool record_read(nvs_handle_t h, web_setup_record_t *r)
{
    uint8_t b[8];
    size_t n = sizeof(b);
    memset(r, 0, sizeof(*r));
    if (nvs_get_blob(h, "rec", b, &n) != ESP_OK)
        return false;
    if (n != sizeof(b) || b[0] != REC_VERSION || b[1] > WEB_SETUP_READY)
        return false;
    r->stage = (web_setup_stage_t)b[1];
    r->recovery_key_pending = (b[2] & REC_KEY_PEND) != 0;
    r->network_confirmed = (b[2] & REC_NET_OK) != 0;
    r->revision = (uint32_t)b[4] | ((uint32_t)b[5] << 8) | ((uint32_t)b[6] << 16) |
                  ((uint32_t)b[7] << 24);
    return true;
}

/* ---- accounts ---------------------------------------------------------------------------- */

static bool name_ok(const char *name)
{
    const size_t n = name ? strlen(name) : 0;
    if (n == 0 || n >= ALARM_NAME_LEN || name[0] == ' ' || name[n - 1] == ' ')
        return false;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)name[i] < 0x20u)
            return false;
    return true;
}

/* Renames the user (as the installer, who looks after everyone) keeping its role and partitions. */
static bool rename_user(const alarm_config_t *cfg, uint8_t user, const char *name)
{
    static const alarm_source_t SRC = {.kind = ALARM_SRC_REMOTE, .id = ALARM_SRC_ID_PANEL_MANAGER};
    const alarm_user_t *u = &cfg->users[user - 1];
    if (!strcmp(u->name, name))
        return true;
    return alarm_service_user_update_as(&SRC, ALARM_USER_INSTALLER, user, u->authority,
                                        u->partitions, name) == ALARM_OK;
}

static bool accounts_commit(void)
{
    alarm_config_t *cfg = web_alloc(sizeof(*cfg));
    bool ok = cfg && alarm_service_get_config(cfg) == ESP_OK &&
              cfg->users[ALARM_USER_INSTALLER - 1].authority == ALARM_AUTH_INSTALLER &&
              cfg->users[ALARM_USER_MASTER - 1].authority == ALARM_AUTH_MASTER;
    if (cfg) {
        mbedtls_platform_zeroize(cfg, sizeof(*cfg));
        free(cfg);
    }
    if (!ok) {
        s_op.why = "setup_try_again";
        return false;
    }
    /* Each user's own name and password; the passwords are the people's own (not temporary). */
    const bool owner = s_op.owner[0] != '\0';
    cfg = web_alloc(sizeof(*cfg));
    ok = cfg && alarm_service_get_config(cfg) == ESP_OK &&
         (!owner || rename_user(cfg, ALARM_USER_MASTER, s_op.owner)) &&
         rename_user(cfg, ALARM_USER_INSTALLER, s_op.installer);
    if (cfg) {
        mbedtls_platform_zeroize(cfg, sizeof(*cfg));
        free(cfg);
    }
    if (!ok) {
        s_op.why = "bad_name";
        return false;
    }
    char why[96];
    if ((owner && !web_login_set(ALARM_USER_MASTER, s_op.owner_pw, false, why, sizeof(why))) ||
        !web_login_set(ALARM_USER_INSTALLER, s_op.installer_pw, false, why, sizeof(why))) {
        ESP_LOGW(TAG, "accounts: %s", why);
        s_op.why = web_login_why_busy(why) ? "setup_try_again" : "weak_password";
        return false;
    }
    if (s_enable_web && s_enable_web() != ESP_OK) {
        s_op.why = "setup_try_again";
        return false;
    }
    return true;
}

static bool commit(void *ctx, web_setup_write_t op, const web_setup_record_t *next)
{
    (void)ctx;
    if (op == WEB_SETUP_ACCOUNTS && !accounts_commit())
        return false;
    if (op == WEB_SETUP_RESET_PASSWORD) {
        char why[96];
        if (!web_login_set(s_op.reset_user, s_op.reset_pw, false, why, sizeof(why))) {
            s_op.why = web_login_why_busy(why) ? "setup_try_again" : "weak_password";
            return false;
        }
        /* The old key is spent: a new one is made and shown before setup is whole again. */
        nvs_handle_t h;
        if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
            (void)nvs_erase_key(h, "rkey");
            (void)nvs_commit(h);
            nvs_close(h);
        }
        ESP_LOGW(TAG, "browser password of user %u replaced with the recovery key",
                 (unsigned)s_op.reset_user);
    }
    return record_write(next);
}

/* ---- the recovery key -------------------------------------------------------------------- */

static bool key_store(const char *key)
{
    uint8_t blob[SALT_LEN + 32];
    uint8_t buf[SALT_LEN + KEY_LEN];
    esp_fill_random(blob, SALT_LEN);
    memcpy(buf, blob, SALT_LEN);
    memcpy(buf + SALT_LEN, key, KEY_LEN);
    bool ok = sha256(buf, sizeof(buf), blob + SALT_LEN);
    mbedtls_platform_zeroize(buf, sizeof(buf));
    nvs_handle_t h;
    if (ok && nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        ok = nvs_set_blob(h, "rkey", blob, sizeof(blob)) == ESP_OK && nvs_commit(h) == ESP_OK;
        nvs_close(h);
    } else
        ok = false;
    return ok;
}

static bool key_verify(const char *typed)
{
    char key[KEY_LEN + 8];
    if (normalise(typed, key, sizeof(key)) != KEY_LEN)
        return false;
    uint8_t blob[SALT_LEN + 32], buf[SALT_LEN + KEY_LEN], hash[32];
    size_t n = sizeof(blob);
    nvs_handle_t h;
    bool ok = nvs_open(NS, NVS_READONLY, &h) == ESP_OK;
    if (ok) {
        ok = nvs_get_blob(h, "rkey", blob, &n) == ESP_OK && n == sizeof(blob);
        nvs_close(h);
    }
    if (ok) {
        memcpy(buf, blob, SALT_LEN);
        memcpy(buf + SALT_LEN, key, KEY_LEN);
        ok = sha256(buf, sizeof(buf), hash) && same((const char *)hash, (const char *)blob + SALT_LEN, 32);
    }
    mbedtls_platform_zeroize(key, sizeof(key));
    mbedtls_platform_zeroize(buf, sizeof(buf));
    return ok;
}

/* ---- the setup Wi-Fi --------------------------------------------------------------------- */

/* The setup Wi-Fi and the listener: while setup isn't finished, and during recovery. */
static uint64_t s_linger_until; /* setup finished: the setup Wi-Fi stays this long for the Done page */

static bool want_open(uint64_t now)
{
    return web_setup_listener_allowed(&s_flow, s_headless, s_started, now) ||
           (s_headless && now < s_linger_until);
}

static void follow_ap(uint64_t now)
{
    const bool open = want_open(now);
    if (open == s_ap_open)
        return;
    if (net_wifi_setup_ap(open ? s_ap_ssid : NULL, s_ap_pass) == ESP_OK)
        s_ap_open = open;
}

/* ---- start ------------------------------------------------------------------------------- */

esp_err_t web_setup_init(bool headless_master, const char *fingerprint,
                         esp_err_t (*enable_web)(void))
{
    s_headless = headless_master;
    s_enable_web = enable_web;
    snprintf(s_fingerprint, sizeof(s_fingerprint), "%s", fingerprint ? fingerprint : "");
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock)
        return ESP_ERR_NO_MEM;
    web_setup_record_t rec = {0};
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK)
        return e;
    const bool stored = record_read(h, &rec);
    if (headless_master) {
        snprintf(s_ap_pass, sizeof(s_ap_pass), "%s", SETUP_WIFI_PASSWORD);
        if (nvs_erase_key(h, "ap") == ESP_OK) /* a random one from the first builds */
            (void)nvs_commit(h);
        if (nvs_erase_key(h, "code") == ESP_OK) /* the first builds' setup code */
            (void)nvs_commit(h);
    }
    nvs_close(h);
    if (e != ESP_OK)
        return e;
    const char *id = web_login_panel_id();
    const size_t idn = strlen(id);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "RoboAlarms-%c%c%c%c", toupper((unsigned char)id[idn - 4]),
             toupper((unsigned char)id[idn - 3]), toupper((unsigned char)id[idn - 2]),
             toupper((unsigned char)id[idn - 1]));
    if (!web_setup_flow_init(&s_flow, &rec, commit, NULL)) {
        /* A record another firmware wrote, or a damaged one: start the setup again rather than
         * lock the owner out (the accounts, if any, still sign in and resume it). */
        ESP_LOGE(TAG, "setup record unreadable: setup starts again");
        memset(&rec, 0, sizeof(rec));
        web_setup_flow_init(&s_flow, &rec, commit, NULL);
    }
    if (!stored && headless_master)
        (void)record_write(&s_flow.record);
    s_started = headless_master;
    follow_ap(now_ms());
    if (headless_master) {
        web_setup_dns_start();
        web_setup_usb_start();
    }
    if (headless_master)
        ESP_LOGI(TAG, "setup stage %d; setup Wi-Fi %s", (int)s_flow.record.stage,
                 s_ap_open ? s_ap_ssid : "closed");
    return ESP_OK;
}

bool web_setup_listener(void)
{
    if (!s_lock)
        return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool open = want_open(now_ms());
    xSemaphoreGive(s_lock);
    return open;
}

bool web_setup_unclaimed(void)
{
    return s_headless && s_started && s_flow.record.stage == WEB_SETUP_UNCLAIMED;
}

/* ---- status ------------------------------------------------------------------------------ */

static const char *stage_word(const web_setup_record_t *r)
{
    switch (r->stage) {
    case WEB_SETUP_UNCLAIMED: return "unclaimed";
    case WEB_SETUP_KEY_PENDING: return "key";
    case WEB_SETUP_NETWORK: return "network";
    default: return "ready";
    }
}

cJSON *web_setup_status(bool via_ap, uint8_t user)
{
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return NULL;
    if (!s_lock || !s_headless) {
        cJSON_AddBoolToObject(o, "setup", false);
        return o;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint64_t now = now_ms();
    const web_setup_record_t rec = s_flow.record;
    const bool open = want_open(now);
    const bool recovery = web_setup_recovery_active(&s_flow, now);
    const uint64_t trial_until = s_flow.trial_until;
    xSemaphoreGive(s_lock);
    cJSON_AddBoolToObject(o, "setup", open);
    cJSON_AddStringToObject(o, "stage", stage_word(&rec));
    cJSON_AddBoolToObject(o, "key_pending", rec.recovery_key_pending);
    cJSON_AddBoolToObject(o, "recovery", recovery);
    cJSON_AddBoolToObject(o, "via_setup_wifi", via_ap);
    cJSON_AddStringToObject(o, "panel_id", web_login_panel_id());
    cJSON_AddStringToObject(o, "setup_wifi", s_ap_ssid);
    cJSON_AddStringToObject(o, "setup_address", NET_WIFI_SETUP_AP_IP);
    cJSON_AddStringToObject(o, "fingerprint", s_fingerprint);
    alarm_config_t *cfg = web_alloc(sizeof(*cfg));
    if (cfg && alarm_service_get_config(cfg) == ESP_OK)
        cJSON_AddStringToObject(o, "name", cfg->partitions[0].name);
    if (cfg) {
        mbedtls_platform_zeroize(cfg, sizeof(*cfg));
        free(cfg);
    }
    /* The home network: to whoever is on the setup Wi-Fi (its password is on the label) or signed
     * in, never to anyone else on the home network. */
    if (via_ap || user) {
        net_wifi_status_t w;
        net_wifi_get_status(&w);
        cJSON *wifi = cJSON_AddObjectToObject(o, "wifi");
        const char *state = w.state == NET_WIFI_CONNECTED    ? "connected"
                            : w.state == NET_WIFI_CONNECTING ? "connecting"
                            : w.state == NET_WIFI_UNCONFIGURED ? "none"
                            : w.fail == NET_WIFI_FAIL_PASSWORD ? "password"
                            : w.fail == NET_WIFI_FAIL_NOT_FOUND ? "not_found"
                                                                 : "disconnected";
        cJSON_AddStringToObject(wifi, "state", state);
        cJSON_AddStringToObject(wifi, "ssid", w.ssid);
        cJSON_AddStringToObject(wifi, "ip", w.ip);
        cJSON_AddNumberToObject(o, "trial_s",
                                trial_until > now ? (double)((trial_until - now + 999u) / 1000u) : 0);
    }
    return o;
}

/* ---- actions ----------------------------------------------------------------------------- */

static const char *str(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

static const char *refusal(web_setup_result_t r, const char **status)
{
    switch (r) {
    case WEB_SETUP_LOCKED: *status = "409 Conflict"; return "locked";
    case WEB_SETUP_UNAUTHORIZED: *status = "403 Forbidden"; return "not_allowed";
    case WEB_SETUP_BUSY: *status = "409 Conflict"; return "setup_busy";
    case WEB_SETUP_STALE: *status = "409 Conflict"; return "setup_stale";
    case WEB_SETUP_WRONG_STAGE: *status = "409 Conflict"; return "setup_stage";
    case WEB_SETUP_STORAGE: *status = "503 Service Unavailable"; return "setup_try_again";
    default: *status = "404 Not Found"; return "not_found";
    }
}

static cJSON *ok_reply(void)
{
    cJSON *o = cJSON_CreateObject();
    if (o)
        cJSON_AddBoolToObject(o, "ok", true);
    return o;
}

static uint8_t user_named(const char *name)
{
    alarm_config_t *cfg = web_alloc(sizeof(*cfg));
    uint8_t found = 0;
    if (cfg && alarm_service_get_config(cfg) == ESP_OK)
        for (uint8_t i = 1; i <= ALARM_MAX_USERS && !found; i++)
            if (cfg->users[i - 1].authority != ALARM_AUTH_NONE &&
                !strcasecmp(cfg->users[i - 1].name, name) && web_login_eligible(cfg, i))
                found = i;
    if (cfg) {
        mbedtls_platform_zeroize(cfg, sizeof(*cfg));
        free(cfg);
    }
    return found;
}

static cJSON *act(const cJSON *o, uint8_t user, alarm_authority_t authority, bool via_ap,
                  bool locked, const char **status, const char **why)
{
    const uint64_t now = now_ms();
    const char *op = str(o, "op");
    web_setup_actor_t a = {
        .provisioned = s_started,
        .safe_to_change = !locked,
        .authenticated = user != 0,
        .may_network = user != 0 && (authority == ALARM_AUTH_INSTALLER || authority == ALARM_AUTH_MASTER),
        .on_trial_network = !via_ap,
        .session = user != 0 ? SESSION_SIGNED_IN : 0,
    };
    web_setup_result_t r;
    memset(&s_op, 0, sizeof(s_op));

    if (!strcmp(op, "claim")) {
        if (!web_gate_open(&s_gate)) {
            *status = "429 Too Many Requests";
            *why = "wait";
            return NULL;
        }
        /* No setup code (the owner, 2026-10-21: nothing printed on the board to type): being on
         * the setup Wi-Fi is what claims a new panel, never a request from the home network. */
        if (!via_ap) {
            *status = "403 Forbidden";
            *why = "setup_from_setup_wifi";
            return NULL;
        }
        const bool right = true;
        uint64_t id;
        do {
            esp_fill_random(&id, sizeof(id));
        } while (id < 0x100u);
        a.session = id;
        r = web_setup_claim(&s_flow, &a, right, now);
        if (r != WEB_SETUP_OK) {
            *why = r == WEB_SETUP_UNAUTHORIZED ? "wrong_code" : refusal(r, status);
            if (r == WEB_SETUP_UNAUTHORIZED)
                *status = "403 Forbidden";
            return NULL;
        }
        if (s_flow.claimant != id) { /* the same claimant again: its id stands */
            *status = "409 Conflict";
            *why = "setup_busy";
            return NULL;
        }
        s_claim_id = id;
        cJSON *out = ok_reply();
        char hex[17];
        snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)id);
        cJSON_AddStringToObject(out, "claim", hex);
        return out;
    }
    if (!strcmp(op, "accounts")) {
        a.session = strtoull(str(o, "claim"), NULL, 16);
        s_op.owner = str(o, "owner");
        s_op.owner_pw = str(o, "owner_password");
        s_op.installer = str(o, "installer");
        s_op.installer_pw = str(o, "installer_password");
        /* The installer's access is required; the household's may wait (the installer adds it
         * later under People), but a name without a password, or the reverse, is a mistake. */
        const bool owner = s_op.owner[0] != '\0' || s_op.owner_pw[0] != '\0';
        if (!name_ok(s_op.installer) || (owner && (!name_ok(s_op.owner) ||
                                                   !strcasecmp(s_op.owner, s_op.installer)))) {
            *status = "400 Bad Request";
            *why = "bad_name";
            return NULL;
        }
        if (owner && !strcmp(s_op.owner_pw, s_op.installer_pw)) {
            *status = "400 Bad Request";
            *why = "same_password";
            return NULL;
        }
        r = web_setup_write(&s_flow, &a, WEB_SETUP_ACCOUNTS, s_flow.record.revision, now);
        if (r == WEB_SETUP_STORAGE && s_op.why) {
            *status = strcmp(s_op.why, "setup_try_again") ? "400 Bad Request" : "503 Service Unavailable";
            *why = s_op.why;
            return NULL;
        }
        if (r != WEB_SETUP_OK) {
            *why = refusal(r, status);
            return NULL;
        }
        s_claim_id = 0;
        ESP_LOGI(TAG, "setup: installer%s access created", owner ? " and household" : "");
        return ok_reply();
    }
    if (!strcmp(op, "key")) {
        /* A new key every time it is asked for while one is owed: one never seen (a closed tab, a
         * restart) is replaced, never shown again. */
        if (!a.authenticated || !a.may_network) {
            *status = "403 Forbidden";
            *why = "not_allowed";
            return NULL;
        }
        if (!s_flow.record.recovery_key_pending) {
            *status = "409 Conflict";
            *why = "setup_stage";
            return NULL;
        }
        char key[KEY_LEN + 1];
        random_text(key, KEY_LEN, LABEL_ALPHABET);
        if (!key_store(key)) {
            mbedtls_platform_zeroize(key, sizeof(key));
            *status = "503 Service Unavailable";
            *why = "setup_try_again";
            return NULL;
        }
        char shown[KEY_LEN + KEY_LEN / 5];
        size_t k = 0;
        for (size_t i = 0; i < KEY_LEN; i++) {
            if (i && i % 5 == 0)
                shown[k++] = '-';
            shown[k++] = key[i];
        }
        shown[k] = '\0';
        cJSON *out = ok_reply();
        cJSON_AddStringToObject(out, "key", shown);
        mbedtls_platform_zeroize(key, sizeof(key));
        mbedtls_platform_zeroize(shown, sizeof(shown));
        return out;
    }
    if (!strcmp(op, "key-saved")) {
        r = web_setup_write(&s_flow, &a, WEB_SETUP_KEY_SAVED, s_flow.record.revision, now);
        if (r != WEB_SETUP_OK) {
            *why = refusal(r, status);
            return NULL;
        }
        return ok_reply();
    }
    if (!strcmp(op, "network")) {
        const char *ssid = str(o, "ssid"), *pw = str(o, "password");
        if (!ssid[0] || strlen(ssid) > 32 || strlen(pw) > 64 || (pw[0] && strlen(pw) < 8)) {
            *status = "400 Bad Request";
            *why = "bad_request";
            return NULL;
        }
        r = web_setup_trial_begin(&s_flow, &a, now);
        if (r != WEB_SETUP_OK) {
            *why = refusal(r, status);
            return NULL;
        }
        static const char HEX[] = "0123456789abcdef";
        uint8_t raw[16];
        esp_fill_random(raw, sizeof(raw));
        for (size_t i = 0; i < 16; i++) {
            s_trial_token[2 * i] = HEX[raw[i] >> 4];
            s_trial_token[2 * i + 1] = HEX[raw[i] & 15u];
        }
        s_trial_token[32] = '\0';
        if (net_wifi_trial_credentials(ssid, pw, s_trial_token) != ESP_OK) {
            (void)web_setup_trial_cancel(&s_flow, &a);
            s_trial_token[0] = '\0';
            *status = "409 Conflict";
            *why = "setup_busy";
            return NULL;
        }
        ESP_LOGI(TAG, "setup: trying the home Wi-Fi \"%s\"", ssid);
        return ok_reply();
    }
    if (!strcmp(op, "cancel")) {
        r = web_setup_trial_cancel(&s_flow, &a);
        if (r != WEB_SETUP_OK) {
            *why = refusal(r, status);
            return NULL;
        }
        if (s_trial_token[0])
            (void)net_wifi_trial_cancel(s_trial_token);
        s_trial_token[0] = '\0';
        return ok_reply();
    }
    if (!strcmp(op, "confirm")) {
        /* Signed in: the trial is kept and setup is finished. From the setup Wi-Fi too (the owner,
         * 2026-10-21: setup ends in the phone's pop-up, never a trip to the home network): then the
         * board's own check stands for the confirmation from the new network - it joined it and
         * has its address there (net_wifi_trial_confirm refuses otherwise). */
        if (!a.authenticated || !a.may_network) {
            *status = "403 Forbidden";
            *why = "not_allowed";
            return NULL;
        }
        if (via_ap)
            a.on_trial_network = true;
        if (!s_flow.record.network_confirmed) {
            if (!s_trial_token[0] || !net_wifi_trial_confirm(s_trial_token)) {
                *status = "409 Conflict";
                *why = "setup_stale";
                return NULL;
            }
            s_trial_token[0] = '\0';
            r = web_setup_trial_confirm(&s_flow, &a, now);
            if (r != WEB_SETUP_OK) {
                *why = refusal(r, status);
                return NULL;
            }
        }
        if (s_flow.record.stage == WEB_SETUP_NETWORK) {
            r = web_setup_write(&s_flow, &a, WEB_SETUP_FINISH, s_flow.record.revision, now);
            if (r != WEB_SETUP_OK) {
                *why = refusal(r, status);
                return NULL;
            }
            ESP_LOGI(TAG, "setup finished: the setup Wi-Fi closes in a minute and a half");
            s_linger_until = now + 90000u; /* the phone reads its Done page first */
        }
        cJSON *out = ok_reply();
        net_wifi_status_t w;
        net_wifi_get_status(&w);
        cJSON_AddStringToObject(out, "ssid", w.ssid);
        cJSON_AddStringToObject(out, "ip", w.ip);
        return out;
    }
    if (!strcmp(op, "reset")) {
        /* A lost browser password: the recovery key, while recovery is open at the panel. */
        if (!web_gate_open(&s_gate)) {
            *status = "429 Too Many Requests";
            *why = "wait";
            return NULL;
        }
        a.session = SESSION_RECOVERY;
        a.recovery_key_verified = web_setup_recovery_active(&s_flow, now) &&
                                  key_verify(str(o, "key"));
        s_op.reset_user = user_named(str(o, "username"));
        s_op.reset_pw = str(o, "password");
        if (!a.recovery_key_verified || !s_op.reset_user) {
            if (web_setup_recovery_active(&s_flow, now))
                web_gate_failed(&s_gate);
            *status = "403 Forbidden";
            *why = web_setup_recovery_active(&s_flow, now) ? "wrong_key" : "setup_recovery";
            return NULL;
        }
        r = web_setup_write(&s_flow, &a, WEB_SETUP_RESET_PASSWORD, s_flow.record.revision, now);
        if (r == WEB_SETUP_STORAGE && s_op.why) {
            *status = strcmp(s_op.why, "setup_try_again") ? "400 Bad Request" : "503 Service Unavailable";
            *why = s_op.why;
            return NULL;
        }
        if (r != WEB_SETUP_OK) {
            *why = refusal(r, status);
            return NULL;
        }
        return ok_reply();
    }
    *status = "404 Not Found";
    *why = "not_found";
    return NULL;
}

cJSON *web_setup_action(const cJSON *o, uint8_t user, alarm_authority_t authority, bool via_ap,
                        bool locked, const char **status, const char **why)
{
    *status = "404 Not Found";
    *why = "not_found";
    if (!s_lock || !s_headless)
        return NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *out = act(o, user, authority, via_ap, locked, status, why);
    follow_ap(now_ms());
    memset(&s_op, 0, sizeof(s_op));
    xSemaphoreGive(s_lock);
    return out;
}

void web_setup_poll(void)
{
    if (!s_lock || !s_headless)
        return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint64_t now = now_ms();
    if (web_setup_tick(&s_flow, now) && s_trial_token[0]) {
        /* Not confirmed in time: net_wifi's own deadline (the same two minutes) brings the
         * previous network back (none on a new panel) on its task; this task's stack is slow
         * memory and never writes flash. A restart rolls an unconfirmed trial back too. */
        s_trial_token[0] = '\0';
        ESP_LOGW(TAG, "the home Wi-Fi wasn't confirmed in time: back to the previous network");
    }
    if (!s_flow.claimant)
        s_claim_id = 0;
    follow_ap(now);
    xSemaphoreGive(s_lock);
}

/* ---- the BOOT button --------------------------------------------------------------------- */

static void button_sample(void *arg)
{
    (void)arg;
    if (!s_button_down || xSemaphoreTake(s_lock, 0) != pdTRUE)
        return; /* busy: the gesture's own gap rule cancels a hold not seen */
    const uint64_t now = now_ms();
    if (web_setup_button_sample(&s_button, s_button_down(), now)) {
        web_setup_recovery_open(&s_flow, now);
        ESP_LOGW(TAG, "recovery open for five minutes (BOOT button): setup Wi-Fi %s", s_ap_ssid);
        follow_ap(now);
    }
    xSemaphoreGive(s_lock);
}

void web_setup_button(bool (*down)(void))
{
    if (!s_lock || !s_headless || !down || s_button_timer)
        return;
    s_button_down = down;
    const esp_timer_create_args_t t = {.callback = button_sample, .name = "setup_button"};
    if (esp_timer_create(&t, &s_button_timer) != ESP_OK ||
        esp_timer_start_periodic(s_button_timer, 50000) != ESP_OK)
        ESP_LOGE(TAG, "BOOT button not watched: no recovery from it");
}

void web_setup_print_label(void)
{
    if (!s_headless) {
        printf("SETUP: this panel has a screen: no setup Wi-Fi\n");
        return;
    }
    printf("SETUP stage %s%s, setup Wi-Fi %s\n", stage_word(&s_flow.record),
           s_flow.record.recovery_key_pending ? " (recovery key owed)" : "",
           s_ap_open ? "open" : "closed");
    printf("SETUP label: Wi-Fi \"%s\", password %s; open http://%s/\n", s_ap_ssid, s_ap_pass,
           NET_WIFI_SETUP_AP_IP);
    printf("SETUP label: panel %s\n", web_login_panel_id());
    printf("SETUP label: certificate SHA-256 %.16s %.16s %.16s %.16s\n", s_fingerprint,
           s_fingerprint + 16, s_fingerprint + 32, s_fingerprint + 48);
}
