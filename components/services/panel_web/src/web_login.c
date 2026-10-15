/* Panel Manager sign-in: user name + password (see web_login.h). */
#include "web_login.h"
#include "alarm_mem.h"
#include "code_hash.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "i18n/i18n.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "web_mem.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PBKDF2 rounds for a new password. Online guessing is held back by the sign-in limits in
 * panel_web.c; this makes a copy of the flash expensive to search. Stored with each password,
 * so it can be raised later without invalidating the ones already set. */
#define ITERATIONS 20000u
#define NS         "weblogin"

static const char *TAG = "web_login";

/* One stored sign-in; version 0 = none. The NVS blob "uNN" is this struct. */
typedef struct {
    uint8_t version; /* 1 */
    uint8_t user;
    uint8_t flags; /* REC_TEMPORARY */
    uint8_t reserved;
    uint32_t iterations;
    uint8_t salt[16];
    uint8_t hash[32]; /* PBKDF2-HMAC-SHA256(password, salt, iterations) */
    uint8_t bind[32]; /* bind_of(): whose sign-in this is */
} rec_t;

#define REC_TEMPORARY 0x01u /* sign in only to change it (web_login_set_temporary) */

static rec_t s_rec[ALARM_MAX_USERS] ALARM_SLOW_BSS;
static SemaphoreHandle_t s_lock;
/* Set by the listener on the alarm task (which must not wait for anyone), taken by whoever
 * next looks at the sign-ins. Bit = user - 1. */
static uint32_t s_erase[2], s_rebind[2];
/* Sign-ins turned off here whose erase flash refused (code review S01): off in RAM at once,
 * erased by retry_unerased() once flash takes it. Under s_lock. Bit = user - 1. */
static uint32_t s_unerased[2];
/* No flash retry (an erase above, a rebind in reconcile()) before this esp_timer time: a
 * broken flash is not written on every request. Under s_lock. */
static int64_t s_retry_at;
#define RETRY_US 5000000
static char s_id[13];

static bool equal(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++)
        d |= x[i] ^ y[i];
    return d == 0;
}

/* Binds a sign-in to one person: their slot and their code record. Someone else in the slot
 * later (a new user, a factory default) has another code record, so the password stops. */
static bool bind_of(const alarm_config_t *cfg, uint8_t user, uint8_t out[32])
{
    static const char tag[] = "RoboAlarms Panel Manager sign-in v1";
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    size_t n = 0;
    const bool ok =
        psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS &&
        psa_hash_update(&op, (const uint8_t *)tag, sizeof(tag)) == PSA_SUCCESS &&
        psa_hash_update(&op, &user, 1) == PSA_SUCCESS &&
        psa_hash_update(&op, cfg->code_salt, sizeof(cfg->code_salt)) == PSA_SUCCESS &&
        psa_hash_update(&op, cfg->users[user - 1].code_hash, ALARM_CODE_HASH_LEN) ==
            PSA_SUCCESS &&
        psa_hash_finish(&op, out, 32, &n) == PSA_SUCCESS && n == 32;
    psa_hash_abort(&op);
    return ok;
}

/* Writes user's record (r) or erases it (NULL); ESP_OK only once flash has it (open, write or
 * erase, and commit). */
static esp_err_t persist(uint8_t user, const rec_t *r)
{
    char key[8];
    snprintf(key, sizeof(key), "u%02u", (unsigned)user);
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = r ? nvs_set_blob(h, key, r, sizeof(*r)) : nvs_erase_key(h, key);
        if (!r && e == ESP_ERR_NVS_NOT_FOUND)
            e = ESP_OK; /* nothing to erase */
        if (e == ESP_OK)
            e = nvs_commit(h);
        nvs_close(h);
    }
    if (e != ESP_OK)
        ESP_LOGE(TAG, "user %u: could not store the sign-in (%s)", (unsigned)user,
                 esp_err_to_name(e));
    return e;
}

static void set_bit(uint32_t *bits, uint8_t user)
{
    if (user >= 1 && user <= ALARM_MAX_USERS)
        __atomic_fetch_or(&bits[(user - 1u) / 32u], 1u << ((user - 1u) % 32u), __ATOMIC_RELAXED);
}
static bool has_bit(const uint32_t *bits, uint8_t user)
{
    return (bits[(user - 1u) / 32u] >> ((user - 1u) % 32u)) & 1u;
}
static void clear_bit(uint32_t *bits, uint8_t user)
{
    bits[(user - 1u) / 32u] &= ~(1u << ((user - 1u) % 32u));
}
static void retry_later(void)
{
    s_retry_at = esp_timer_get_time() + RETRY_US;
}

/* Turns user's sign-in off: in RAM at once (fail closed: the password stops working, and the
 * sessions it opened end with web_login_fingerprint()), on flash if flash takes it, else later
 * (retry_unerased()). false: flash refused it for now. s_lock held. */
static bool revoke_locked(uint8_t user)
{
    mbedtls_platform_zeroize(&s_rec[user - 1], sizeof(s_rec[0]));
    if (persist(user, NULL) == ESP_OK) {
        clear_bit(s_unerased, user);
        return true;
    }
    set_bit(s_unerased, user);
    retry_later();
    return false;
}
/* The erases flash refused before, once their wait is over. s_lock held. */
static void retry_unerased(void)
{
    if (!(s_unerased[0] | s_unerased[1]) || esp_timer_get_time() < s_retry_at)
        return;
    for (uint8_t u = 1; u <= ALARM_MAX_USERS; u++)
        if (has_bit(s_unerased, u) && !revoke_locked(u))
            return; /* still refused: the rest wait with it */
}
/* Carries user's sign-in over to their new code record (bind), in RAM only once flash has it.
 * false: not now (refused, or waiting to retry); the record stays as flash has it. s_lock held. */
static bool rebind_locked(uint8_t user, const uint8_t bind[32])
{
    if (esp_timer_get_time() < s_retry_at)
        return false;
    rec_t next = s_rec[user - 1];
    memcpy(next.bind, bind, sizeof(next.bind));
    const bool ok = persist(user, &next) == ESP_OK;
    if (ok)
        s_rec[user - 1] = next;
    else
        retry_later();
    mbedtls_platform_zeroize(&next, sizeof(next));
    return ok;
}

bool web_login_eligible(const alarm_config_t *cfg, uint8_t user)
{
    if (user < 1 || user > ALARM_MAX_USERS)
        return false;
    const alarm_authority_t a = cfg->users[user - 1].authority;
    if (user == ALARM_USER_INSTALLER)
        return a == ALARM_AUTH_INSTALLER;
    /* The people who can do something at the panel with their code beyond arming: not an
     * arm-only or guest code, and never a duress code (made to be typed under threat). */
    return a == ALARM_AUTH_MASTER || a == ALARM_AUTH_PARTITION_MASTER || a == ALARM_AUTH_STANDARD;
}

/* Brings the stored sign-ins in line with the users. A deleted user's sign-in goes, and one
 * whose person no longer matches (another user in the slot, a code that can't sign in) goes
 * too; a code change carries the sign-in over to the new code record. */
static void reconcile(const alarm_config_t *given)
{
    uint32_t erase[2], rebind[2];
    for (unsigned i = 0; i < 2; i++) {
        erase[i] = __atomic_exchange_n(&s_erase[i], 0u, __ATOMIC_RELAXED);
        rebind[i] = __atomic_exchange_n(&s_rebind[i], 0u, __ATOMIC_RELAXED);
    }
    const alarm_config_t *cfg = given;
    alarm_config_t *fresh = NULL;
    if (erase[0] | erase[1] | rebind[0] | rebind[1]) {
        /* The configuration read after taking the flags includes whatever raised them. */
        fresh = web_alloc(sizeof(*fresh));
        if (!fresh || alarm_service_get_config(fresh) != ESP_OK) {
            free(fresh);
            for (unsigned i = 0; i < 2; i++) { /* try again next time; nobody signs in now */
                __atomic_fetch_or(&s_erase[i], erase[i], __ATOMIC_RELAXED);
                __atomic_fetch_or(&s_rebind[i], rebind[i], __ATOMIC_RELAXED);
            }
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (unsigned u = 1; u <= ALARM_MAX_USERS; u++)
                if (has_bit(erase, (uint8_t)u) && s_rec[u - 1].version) {
                    mbedtls_platform_zeroize(&s_rec[u - 1], sizeof(s_rec[0]));
                    set_bit(s_unerased, (uint8_t)u); /* off flash too, by retry_unerased() */
                }
            xSemaphoreGive(s_lock);
            return;
        }
        cfg = fresh;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    retry_unerased();
    for (uint8_t u = 1; u <= ALARM_MAX_USERS; u++) {
        rec_t *r = &s_rec[u - 1];
        if (!r->version)
            continue;
        bool drop = has_bit(erase, u) || !web_login_eligible(cfg, u);
        uint8_t b[32];
        if (!drop && bind_of(cfg, u, b) && !equal(b, r->bind, sizeof(b))) {
            if (!has_bit(rebind, u))
                drop = true;
            else if (!rebind_locked(u, b))
                set_bit(s_rebind, u); /* flash refused it: again next time */
        }
        if (drop) {
            ESP_LOGI(TAG, "user %u: browser sign-in removed with the user", (unsigned)u);
            (void)revoke_locked(u); /* off now; a refused erase is retried */
        }
    }
    xSemaphoreGive(s_lock);
    if (fresh) {
        mbedtls_platform_zeroize(fresh, sizeof(*fresh));
        free(fresh);
    }
}

static void on_event(void *ctx, const alarm_event_t *ev)
{
    (void)ctx;
    if (ev->type == ALARM_EV_USER_DELETED)
        set_bit(s_erase, ev->arg);
    else if (ev->type == ALARM_EV_USER_CHANGED)
        set_bit(s_rebind, ev->arg);
}

const alarm_listener_t *web_login_listener(void)
{
    static const alarm_listener_t l = {.on_event = on_event};
    return &l;
}

const char *web_login_panel_id(void)
{
    if (!s_id[0]) {
        uint8_t mac[6];
        if (esp_efuse_mac_get_default(mac) == ESP_OK)
            snprintf(s_id, sizeof(s_id), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2],
                     mac[3], mac[4], mac[5]);
    }
    return s_id;
}

esp_err_t web_login_init(void)
{
    if (s_lock)
        return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock)
        return ESP_ERR_NO_MEM;
    web_login_panel_id();
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND)
        return ESP_OK; /* nobody has a sign-in yet */
    if (e != ESP_OK)
        return e;
    unsigned n = 0;
    for (unsigned u = 1; u <= ALARM_MAX_USERS; u++) {
        char key[8];
        snprintf(key, sizeof(key), "u%02u", u);
        rec_t r;
        size_t len = sizeof(r);
        if (nvs_get_blob(h, key, &r, &len) == ESP_OK && len == sizeof(r) && r.version == 1 &&
            r.user == u && r.iterations >= 1000u && r.iterations <= 1000000u) {
            s_rec[u - 1] = r;
            n++;
        }
        mbedtls_platform_zeroize(&r, sizeof(r));
    }
    nvs_close(h);
    ESP_LOGI(TAG, "%u browser sign-in%s", n, n == 1 ? "" : "s");
    return ESP_OK;
}

bool web_login_enabled(const alarm_config_t *cfg, uint8_t user)
{
    if (!s_lock || user < 1 || user > ALARM_MAX_USERS)
        return false;
    reconcile(cfg);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool on = s_rec[user - 1].version != 0;
    xSemaphoreGive(s_lock);
    return on;
}

/* "Front Door " and "front door" are the same name. */
static bool same_name(const char *a, const char *b)
{
    while (*a == ' ')
        a++;
    while (*b == ' ')
        b++;
    size_t la = strlen(a), lb = strlen(b);
    while (la && a[la - 1] == ' ')
        la--;
    while (lb && b[lb - 1] == ' ')
        lb--;
    return la && lb && i18n_utf8_equal_fold(a, la, b, lb); /* "ZOË" is "zoë" (I18N-009) */
}

static bool blank(const char *s)
{
    while (*s == ' ')
        s++;
    return *s == 0;
}

/* The name a person signs in with: their name on the Users screen, the installer's too (decided
 * 2026-09-29: the panel ID they used before was hard to sign in with). The installer's slot is
 * called "Installer" from the factory; a blank one still answers to that. Never translated: it is
 * a user name, typed the same in every language. The touchscreen shows the same rule
 * (ui_login_name() in ui_settings.c). */
static const char *login_name(const alarm_config_t *cfg, uint8_t user)
{
    const char *name = cfg->users[user - 1].name;
    return user == ALARM_USER_INSTALLER && blank(name) ? "Installer" : name;
}

static bool contains_folded(const char *hay, const char *needle)
{
    return i18n_utf8_contains_fold(hay, needle);
}

/* NIST SP 800-63B: length is what matters, no composition rules, any printable character. */
static bool password_ok(const char *pw, const char *username, char *why, size_t why_len)
{
    const size_t n = strlen(pw);
    bool same = true;
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)pw[i] < 0x20 || pw[i] == 0x7f) {
            snprintf(why, why_len, "%s", I18N_NOOP_C("why", "Use letters, numbers, spaces and symbols only"));
            return false;
        }
        same &= pw[i] == pw[0];
    }
    if (n < WEB_PASSWORD_MIN) {
        snprintf(why, why_len, "Use at least %u characters", (unsigned)WEB_PASSWORD_MIN);
        return false;
    }
    if (n > WEB_PASSWORD_MAX) {
        snprintf(why, why_len, "Use at most %u characters", (unsigned)WEB_PASSWORD_MAX);
        return false;
    }
    if (same) {
        snprintf(why, why_len, "%s", I18N_NOOP_C("why", "Not one character over and over"));
        return false;
    }
    char name[ALARM_NAME_LEN];
    i18n_utf8_copy(name, sizeof(name), username);
    for (char *p = name; *p; p++)
        if (*p == ' ')
            *p = 0; /* the first word of the name is enough to refuse */
    if (strlen(name) >= 3 && contains_folded(pw, name)) {
        snprintf(why, why_len, "%s", I18N_NOOP_C("why", "The password can't contain the user name"));
        return false;
    }
    return true;
}

/* What a session opened by this sign-in depends on (web_login_fingerprint()). false, out all
 * zero: no sign-in, or the hash failed. s_lock held, or r a private copy. */
static bool fingerprint_of(const rec_t *r, uint8_t out[32])
{
    size_t n = 0;
    const bool ok = r->version &&
                    psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)r, sizeof(*r), out, 32,
                                     &n) == PSA_SUCCESS &&
                    n == 32;
    if (!ok)
        memset(out, 0, 32);
    return ok;
}

/* expect: a password change - stored only if the sign-in is still the one whose current
 * password was checked (none turned it off or replaced it during the hashing). fp: the new
 * sign-in's fingerprint, made in the same hold of the lock. */
static bool store(uint8_t user, const char *password, uint8_t flags, const rec_t *expect,
                  uint8_t fp[32], char *why, size_t why_len)
{
    snprintf(why, why_len, "%s", I18N_NOOP_C("why", "The panel is busy. Try again"));
    if (fp)
        memset(fp, 0, 32);
    if (!s_lock || !password || user < 1 || user > ALARM_MAX_USERS)
        return false;
    alarm_config_t *cfg = web_alloc(sizeof(*cfg));
    if (!cfg || alarm_service_get_config(cfg) != ESP_OK) {
        free(cfg);
        return false;
    }
    reconcile(cfg);
    bool ok = false;
    const char *username = login_name(cfg, user);
    rec_t r = {.version = 1, .user = user, .flags = flags, .iterations = ITERATIONS};
    if (!web_login_eligible(cfg, user)) {
        snprintf(why, why_len, "%s", I18N_NOOP_C("why", "Arm-only, guest and duress codes can't have a browser sign-in"));
    } else if (blank(username)) {
        snprintf(why, why_len, "%s", I18N_NOOP_C("why", "Give this user a name first"));
    } else {
        /* One sign-in per name, the installer's included: a master also called "Installer"
         * would make the installer's user name ambiguous. */
        ok = true;
        for (unsigned u = 1; u <= ALARM_MAX_USERS && ok; u++)
            if (u != user && cfg->users[u - 1].authority != ALARM_AUTH_NONE &&
                same_name(login_name(cfg, (uint8_t)u), username)) {
                snprintf(why, why_len, "Another user is also called %s. Rename one first",
                         username);
                ok = false;
            }
        ok = ok && password_ok(password, username, why, why_len);
        if (ok) {
            esp_fill_random(r.salt, sizeof(r.salt));
            const int64_t t0 = esp_timer_get_time();
            ok = bind_of(cfg, user, r.bind) &&
                 code_hash_derive(password, strlen(password), r.salt, sizeof(r.salt),
                                  r.iterations, r.hash) == ESP_OK;
            ESP_LOGI(TAG, "user %u: password hashed in %lld ms", (unsigned)user,
                     (long long)((esp_timer_get_time() - t0) / 1000));
            if (!ok)
                snprintf(why, why_len, "%s", I18N_NOOP_C("why", "The panel could not store it. Try again"));
        }
    }
    if (ok) {
        /* Flash first, then RAM: a password flash refused never works, not even until the
         * next restart, and a change refused leaves the old one (code review S01). */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (expect && !equal(&s_rec[user - 1], expect, sizeof(*expect))) {
            /* Turned off or set anew while the current password was checked: it was no longer
             * the user's to change (code review S01). */
            ok = false;
            snprintf(why, why_len, "%s",
                     s_rec[user - 1].version
                         ? I18N_NOOP_C("why", "The panel is busy. Try again")
                         : I18N_NOOP_C("why", "This user has no browser sign-in"));
        } else if (persist(user, &r) == ESP_OK) {
            s_rec[user - 1] = r;
            clear_bit(s_unerased, user); /* the new record replaced the one still due to go */
            if (fp)
                (void)fingerprint_of(&s_rec[user - 1], fp);
        } else {
            ok = false;
            snprintf(why, why_len, "%s", I18N_NOOP_C("why", "The panel could not store it. Try again"));
        }
        xSemaphoreGive(s_lock);
    }
    mbedtls_platform_zeroize(&r, sizeof(r));
    mbedtls_platform_zeroize(cfg, sizeof(*cfg));
    free(cfg);
    return ok;
}

bool web_login_set(uint8_t user, const char *password, bool temporary, char *why, size_t why_len)
{
    return store(user, password, temporary ? REC_TEMPORARY : 0, NULL, NULL, why, why_len);
}

bool web_login_set_temporary(uint8_t user, char *out, size_t out_len, char *why, size_t why_len)
{
    /* No 0/o, 1/l/i: read over a phone or out of a chat without mistakes. */
    static const char set[] = "abcdefghjkmnpqrstuvwxyz23456789";
    char pw[20];
    if (out_len < sizeof(pw)) {
        snprintf(why, why_len, "%s", I18N_NOOP_C("why", "No room for the password"));
        return false;
    }
    size_t n = 0;
    for (unsigned i = 0; i < 16; i++) {
        uint8_t b;
        do /* rejection sampling: every character equally likely */
            esp_fill_random(&b, 1);
        while (b >= 248u); /* 248 = 8 * 31 */
        if (i && i % 4 == 0)
            pw[n++] = '-';
        pw[n++] = set[b % 31u];
    }
    pw[n] = 0;
    const bool ok = store(user, pw, REC_TEMPORARY, NULL, NULL, why, why_len);
    if (ok)
        snprintf(out, out_len, "%s", pw);
    mbedtls_platform_zeroize(pw, sizeof(pw));
    return ok;
}

bool web_login_has(uint8_t user)
{
    if (!s_lock || user < 1 || user > ALARM_MAX_USERS)
        return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool has = s_rec[user - 1].version != 0;
    xSemaphoreGive(s_lock);
    return has;
}

bool web_login_temporary(uint8_t user)
{
    if (!s_lock || user < 1 || user > ALARM_MAX_USERS)
        return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool temporary = s_rec[user - 1].version && (s_rec[user - 1].flags & REC_TEMPORARY);
    xSemaphoreGive(s_lock);
    return temporary;
}

/* Is this the user's password? *has: whether they have one at all. The same derivation and
 * constant-time comparison as web_login_check(); a user without one, or an empty or too long
 * password, costs the same work. seen (if not NULL): the record checked, which the caller
 * zeroizes. */
static bool password_right(uint8_t user, const char *password, bool *has, rec_t *seen)
{
    *has = false;
    if (!s_lock || !password || user < 1 || user > ALARM_MAX_USERS)
        return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    rec_t r = s_rec[user - 1];
    xSemaphoreGive(s_lock);
    if (seen)
        *seen = r;
    *has = r.version != 0;
    uint8_t out[32];
    const size_t len = strlen(password);
    bool right = false;
    if (!r.version || len == 0 || len > WEB_PASSWORD_MAX) {
        static const uint8_t salt[16] = {0};
        (void)code_hash_derive(password, len, salt, sizeof(salt), ITERATIONS, out);
    } else {
        right = code_hash_derive(password, len, r.salt, sizeof(r.salt), r.iterations, out) ==
                    ESP_OK &&
                equal(out, r.hash, sizeof(out));
    }
    mbedtls_platform_zeroize(out, sizeof(out));
    mbedtls_platform_zeroize(&r, sizeof(r));
    return right;
}

bool web_login_verify(uint8_t user, const char *password)
{
    bool has;
    return password_right(user, password, &has, NULL);
}

bool web_login_change(uint8_t user, const char *current, const char *next, bool *wrong_current,
                      uint8_t fingerprint[32], char *why, size_t why_len)
{
    *wrong_current = false;
    memset(fingerprint, 0, 32);
    snprintf(why, why_len, "%s", I18N_NOOP_C("why", "The panel is busy. Try again"));
    if (!s_lock || user < 1 || user > ALARM_MAX_USERS)
        return false;
    bool has, ok = false;
    rec_t seen = {0};
    const bool right = password_right(user, current, &has, &seen);
    if (!has) {
        snprintf(why, why_len, "%s", I18N_NOOP_C("why", "This user has no browser sign-in"));
    } else if (!right) {
        *wrong_current = true;
        snprintf(why, why_len, "%s", I18N_NOOP_C("why", "The current password is not right"));
    } else if (strcmp(current, next) == 0) {
        snprintf(why, why_len, "%s", I18N_NOOP_C("why", "Choose a new password, not the current one"));
    } else {
        ok = store(user, next, 0, &seen, fingerprint, why, why_len);
    }
    mbedtls_platform_zeroize(&seen, sizeof(seen));
    return ok;
}

bool web_login_why_busy(const char *why)
{
    /* The answers store() and web_login_change() give when the panel, not the person, stopped
     * it: a full memory, a busy configuration, a flash that refused. */
    return why && (!strcmp(why, "The panel is busy. Try again") ||
                   !strcmp(why, "The panel could not store it. Try again"));
}

bool web_login_clear(uint8_t user)
{
    if (!s_lock || user < 1 || user > ALARM_MAX_USERS)
        return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = true;
    /* Asked again after a refusal: flash is tried at once, whatever the retry wait. */
    if (s_rec[user - 1].version || has_bit(s_unerased, user))
        ok = revoke_locked(user);
    xSemaphoreGive(s_lock);
    return ok;
}

bool web_login_check(const char *username, const char *password, uint8_t *user,
                     uint8_t verified[32])
{
    *user = 0;
    memset(verified, 0, 32);
    if (!s_lock)
        return false;
    alarm_config_t *cfg = web_alloc(sizeof(*cfg));
    if (!cfg || alarm_service_get_config(cfg) != ESP_OK) {
        free(cfg);
        return false;
    }
    reconcile(cfg);
    /* Candidates: the users with that name, the installer included (a rename can make two
     * alike; each is tried). */
    uint8_t cand[4];
    rec_t recs[4];
    unsigned n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint8_t u = 1; u <= ALARM_MAX_USERS && n < 4; u++) {
        if (s_rec[u - 1].version && same_name(login_name(cfg, u), username)) {
            recs[n] = s_rec[u - 1];
            cand[n++] = u;
        }
    }
    xSemaphoreGive(s_lock);
    const size_t len = strlen(password);
    bool ok = false;
    uint8_t out[32];
    if (n == 0 || len == 0 || len > WEB_PASSWORD_MAX) {
        /* The same work as a real check: the time taken says nothing about the name. */
        static const uint8_t salt[16] = {0};
        (void)code_hash_derive(password, len, salt, sizeof(salt), ITERATIONS, out);
        n = 0;
    }
    unsigned hit = 0;
    for (unsigned i = 0; i < n && !ok; i++)
        if (code_hash_derive(password, len, recs[i].salt, sizeof(recs[i].salt),
                             recs[i].iterations, out) == ESP_OK &&
            equal(out, recs[i].hash, sizeof(out))) {
            ok = true;
            hit = i;
        }
    if (ok) {
        /* The hashing took a second without the lock: the touchscreen or the console may have
         * turned this sign-in off or set another meanwhile. Only the very record checked signs
         * in, and verified ties the session to it (web_login_fingerprint_verified(); code
         * review S01). */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const rec_t *now = &s_rec[cand[hit] - 1];
        ok = equal(now, &recs[hit], sizeof(*now)) && fingerprint_of(now, verified);
        xSemaphoreGive(s_lock);
        if (ok)
            *user = cand[hit];
    }
    mbedtls_platform_zeroize(out, sizeof(out));
    mbedtls_platform_zeroize(recs, sizeof(recs));
    mbedtls_platform_zeroize(cfg, sizeof(*cfg));
    free(cfg);
    return ok;
}

bool web_login_fingerprint(const alarm_config_t *cfg, uint8_t user, uint8_t out[32])
{
    memset(out, 0, 32);
    if (!s_lock || user < 1 || user > ALARM_MAX_USERS)
        return false;
    reconcile(cfg);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool has = fingerprint_of(&s_rec[user - 1], out);
    xSemaphoreGive(s_lock);
    return has;
}

bool web_login_fingerprint_verified(const alarm_config_t *cfg, uint8_t user,
                                    const uint8_t verified[32], uint8_t out[32])
{
    static const uint8_t none[32] = {0};
    const bool ok = web_login_fingerprint(cfg, user, out) && !equal(verified, none, 32) &&
                    equal(out, verified, 32);
    if (!ok)
        memset(out, 0, 32); /* nothing a session could be built on */
    return ok;
}

bool web_login_owed(void)
{
    uint32_t owed = 0;
    for (unsigned i = 0; i < 2; i++)
        owed |= __atomic_load_n(&s_erase[i], __ATOMIC_RELAXED) |
                __atomic_load_n(&s_rebind[i], __ATOMIC_RELAXED) |
                __atomic_load_n(&s_unerased[i], __ATOMIC_RELAXED);
    return owed != 0;
}

void web_login_retry(void)
{
    if (!s_lock)
        return;
    alarm_config_t *cfg = web_alloc(sizeof(*cfg));
    if (cfg && alarm_service_get_config(cfg) == ESP_OK)
        reconcile(cfg); /* within the retry wait, as every other look at the sign-ins */
    if (cfg) {
        mbedtls_platform_zeroize(cfg, sizeof(*cfg));
        free(cfg);
    }
}
