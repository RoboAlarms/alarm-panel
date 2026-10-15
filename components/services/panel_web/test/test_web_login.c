/* web_login.c on the PC (code review finding S01, docs/features/31 WEB-016 "fail closed"):
 * a browser password set, changed or turned off is reported done only once flash has it; a
 * change flash refused leaves the old password; a sign-in turned off stops at once, ends its
 * sessions and keeps trying to leave flash, so it can't come back after a restart; a sign-in
 * turned off or replaced while a password is being checked opens no session.
 *
 * The production file is compiled here as it is (#include below), against the fakes in this
 * file: NVS that fails on demand (open, set, erase or commit) and "restarts" by reloading what
 * flash holds, a deterministic hash in place of PSA and PBKDF2 (with a hook that runs while a
 * password is hashed: the touchscreen acting meanwhile), and a clock the test moves.
 * test/CMakeLists.txt writes the ESP-IDF headers it names as empty files. */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alarm/alarm_config.h"
#include "alarm/alarm_types.h"
#include "unity.h"

/* ---- ESP-IDF, FreeRTOS, PSA and the alarm service, as far as web_login.c uses them ------- */
typedef int esp_err_t;
#define ESP_OK                0
#define ESP_FAIL              (-1)
#define ESP_ERR_NO_MEM        0x101
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ALARM_SLOW_BSS
static void fake_log(const char *tag, const char *fmt, ...)
{
    (void)tag;
    (void)fmt;
}
#define ESP_LOGE(...) fake_log(__VA_ARGS__)
#define ESP_LOGW(...) fake_log(__VA_ARGS__)
#define ESP_LOGI(...) fake_log(__VA_ARGS__)
const char *esp_err_to_name(esp_err_t e)
{
    (void)e;
    return "error";
}

typedef void *SemaphoreHandle_t;
#define portMAX_DELAY 0u
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    static int m;
    return &m;
}
int xSemaphoreTake(SemaphoreHandle_t s, unsigned wait)
{
    (void)s;
    (void)wait;
    return 1;
}
int xSemaphoreGive(SemaphoreHandle_t s)
{
    (void)s;
    return 1;
}

static int64_t s_now_us = 1000000;
int64_t esp_timer_get_time(void)
{
    return s_now_us;
}
void esp_fill_random(void *p, size_t n)
{
    static uint8_t next = 1;
    for (size_t i = 0; i < n; i++)
        ((uint8_t *)p)[i] = next++;
}
esp_err_t esp_efuse_mac_get_default(uint8_t mac[6])
{
    memset(mac, 0x5A, 6);
    return ESP_OK;
}
void mbedtls_platform_zeroize(void *p, size_t n)
{
    memset(p, 0, n);
}
void *alarm_mem_slow_calloc(size_t n, size_t size)
{
    return calloc(n, size);
}

/* A deterministic stand-in for SHA-256 and PBKDF2: different input, different 32 bytes. */
static void mix(uint8_t st[32], const void *data, size_t n)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < n; i++)
        for (unsigned j = 0; j < 32; j++)
            st[j] = (uint8_t)((st[j] ^ p[i]) * 167u + j + st[(j + 31u) % 32u]);
}
typedef int psa_status_t;
#define PSA_SUCCESS     0
#define PSA_ALG_SHA_256 1
typedef struct {
    uint8_t st[32];
} psa_hash_operation_t;
#define PSA_HASH_OPERATION_INIT {{0}}
psa_status_t psa_hash_setup(psa_hash_operation_t *op, int alg)
{
    (void)alg;
    memset(op->st, 0x11, sizeof(op->st));
    return PSA_SUCCESS;
}
psa_status_t psa_hash_update(psa_hash_operation_t *op, const uint8_t *p, size_t n)
{
    mix(op->st, p, n);
    return PSA_SUCCESS;
}
psa_status_t psa_hash_finish(psa_hash_operation_t *op, uint8_t *out, size_t size, size_t *len)
{
    memcpy(out, op->st, size < 32 ? size : 32);
    *len = 32;
    return PSA_SUCCESS;
}
psa_status_t psa_hash_abort(psa_hash_operation_t *op)
{
    memset(op, 0, sizeof(*op));
    return PSA_SUCCESS;
}
psa_status_t psa_hash_compute(int alg, const uint8_t *p, size_t n, uint8_t *out, size_t size,
                              size_t *len)
{
    psa_hash_operation_t op;
    psa_hash_setup(&op, alg);
    psa_hash_update(&op, p, n);
    return psa_hash_finish(&op, out, size, len);
}
/* Runs once, during the next password hash (which takes a second on a panel, without the lock):
 * what the touchscreen or the console does meanwhile. */
static void (*s_during_hash)(void);
esp_err_t code_hash_derive(const void *secret, size_t secret_len, const uint8_t *salt,
                           size_t salt_len, uint32_t iterations, uint8_t out[32])
{
    if (s_during_hash) {
        void (*meanwhile)(void) = s_during_hash;
        s_during_hash = NULL;
        meanwhile();
    }
    uint8_t st[32];
    memset(st, 0x22, sizeof(st));
    mix(st, secret, secret_len);
    mix(st, salt, salt_len);
    mix(st, &iterations, sizeof(iterations));
    memcpy(out, st, 32);
    return ESP_OK;
}

typedef struct {
    void (*on_event)(void *ctx, const alarm_event_t *ev);
} alarm_listener_t;
static alarm_config_t s_cfg;
esp_err_t alarm_service_get_config(alarm_config_t *out)
{
    *out = s_cfg;
    return ESP_OK;
}

/* ---- NVS: the "weblogin" namespace, one blob per user ------------------------------------
 * As ESP-IDF's: a set or an erase reaches flash at once, and nvs_commit() writes nothing more
 * (it can still answer an error, which web_login.c reports as a failure). */
typedef unsigned nvs_handle_t;
#define NVS_READONLY  0
#define NVS_READWRITE 1
#define SLOTS         (ALARM_MAX_USERS + 1)
typedef struct {
    bool has;
    uint8_t data[96];
    size_t len;
} blob_t;
static blob_t s_flash[SLOTS]; /* what a restart reads */
/* Fail the next n calls of each (a large n: flash stays broken). */
static int s_fail_open, s_fail_set, s_fail_erase, s_fail_commit;
static unsigned s_writes; /* set and erase calls that reached the fake */

static unsigned slot(const char *key)
{
    return (unsigned)strtoul(key + 1, NULL, 10) % SLOTS;
}
static bool fail(int *n)
{
    if (*n <= 0)
        return false;
    (*n)--;
    return true;
}
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h)
{
    (void)ns;
    *h = 1;
    if (mode == NVS_READWRITE && fail(&s_fail_open))
        return ESP_FAIL;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *p, size_t n)
{
    (void)h;
    s_writes++;
    if (fail(&s_fail_set) || n > sizeof(s_flash[0].data))
        return ESP_FAIL;
    const unsigned u = slot(key);
    s_flash[u].has = true;
    memcpy(s_flash[u].data, p, n);
    s_flash[u].len = n;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    (void)h;
    s_writes++;
    if (fail(&s_fail_erase))
        return ESP_FAIL;
    const unsigned u = slot(key);
    if (!s_flash[u].has)
        return ESP_ERR_NVS_NOT_FOUND;
    memset(&s_flash[u], 0, sizeof(s_flash[u]));
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    return fail(&s_fail_commit) ? ESP_FAIL : ESP_OK;
}
void nvs_close(nvs_handle_t h)
{
    (void)h;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    (void)h;
    const blob_t *b = &s_flash[slot(key)];
    if (!b->has)
        return ESP_ERR_NVS_NOT_FOUND;
    if (*len < b->len)
        return ESP_FAIL;
    memcpy(out, b->data, b->len);
    *len = b->len;
    return ESP_OK;
}

/* ---- the production file ----------------------------------------------------------------- */
#include "../src/web_login.c"

#define ALEX 2u /* a system master */
#define PAT  3u /* a standard user */
static const char OLD[] = "harbor house lantern";
static const char NEW[] = "quiet morning tide 42";

/* A restart: RAM forgets everything, the sign-ins load from what flash committed. */
static void restart(void)
{
    memset(s_rec, 0, sizeof(s_rec));
    s_lock = NULL;
    memset(s_erase, 0, sizeof(s_erase));
    memset(s_rebind, 0, sizeof(s_rebind));
    memset(s_unerased, 0, sizeof(s_unerased));
    s_retry_at = 0;
    TEST_ASSERT_EQUAL(ESP_OK, web_login_init());
}
static void flash_works(void)
{
    s_fail_open = s_fail_set = s_fail_erase = s_fail_commit = 0;
}
static bool signs_in(const char *name, const char *pw, uint8_t want)
{
    uint8_t user = 0, verified[32];
    return web_login_check(name, pw, &user, verified) && user == want;
}
static bool zero32(const uint8_t b[32])
{
    for (unsigned i = 0; i < 32; i++)
        if (b[i])
            return false;
    return true;
}

void setUp(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.users[ALEX - 1].authority = ALARM_AUTH_MASTER;
    snprintf(s_cfg.users[ALEX - 1].name, sizeof(s_cfg.users[0].name), "Alex");
    memset(s_cfg.users[ALEX - 1].code_hash, 0xA1, sizeof(s_cfg.users[0].code_hash));
    s_cfg.users[PAT - 1].authority = ALARM_AUTH_STANDARD;
    snprintf(s_cfg.users[PAT - 1].name, sizeof(s_cfg.users[0].name), "Pat");
    memset(s_cfg.users[PAT - 1].code_hash, 0xB2, sizeof(s_cfg.users[0].code_hash));
    memset(s_flash, 0, sizeof(s_flash));
    flash_works();
    s_during_hash = NULL;
    s_writes = 0;
    s_now_us = 1000000;
    restart();
    char why[128];
    TEST_ASSERT_TRUE(web_login_set(ALEX, OLD, false, why, sizeof(why)));
    TEST_ASSERT_TRUE(web_login_set(PAT, OLD, false, why, sizeof(why)));
}
void tearDown(void)
{
}

/* S01, WEB-016: the control - set, change and turn off reach flash and survive a restart. */
static void test_changes_that_flash_takes_survive_a_restart(void)
{
    char why[128];
    bool wrong = true;
    uint8_t fp[32], now[32];
    TEST_ASSERT_TRUE(web_login_change(ALEX, OLD, NEW, &wrong, fp, why, sizeof(why)));
    TEST_ASSERT_FALSE(wrong);
    TEST_ASSERT_TRUE(web_login_fingerprint_verified(&s_cfg, ALEX, fp, now)); /* its session */
    TEST_ASSERT_TRUE(web_login_clear(PAT));
    restart();
    TEST_ASSERT_TRUE(signs_in("Alex", NEW, ALEX));
    TEST_ASSERT_FALSE(signs_in("Alex", OLD, ALEX));
    TEST_ASSERT_FALSE(web_login_has(PAT));
    TEST_ASSERT_FALSE(signs_in("Pat", OLD, PAT));
}

/* S01: a change flash refuses (open, write or commit) is reported - as the panel's trouble,
 * answered 503 (web_login_why_busy()) - and the old password stays the one that works. */
static void change_refused(int *which)
{
    uint8_t before[32], after[32], fp[32];
    web_login_fingerprint(&s_cfg, ALEX, before);
    *which = 1;
    char why[128] = "";
    bool wrong = true;
    TEST_ASSERT_FALSE(web_login_change(ALEX, OLD, NEW, &wrong, fp, why, sizeof(why)));
    TEST_ASSERT_FALSE(wrong); /* not counted as a wrong current password */
    TEST_ASSERT_EQUAL_STRING("The panel could not store it. Try again", why);
    TEST_ASSERT_TRUE(web_login_why_busy(why));
    TEST_ASSERT_TRUE(zero32(fp)); /* nothing a session could be built on */
    TEST_ASSERT_TRUE(signs_in("Alex", OLD, ALEX));
    TEST_ASSERT_FALSE(signs_in("Alex", NEW, ALEX));
    web_login_fingerprint(&s_cfg, ALEX, after); /* the user's sessions go on as they were */
    TEST_ASSERT_EQUAL_MEMORY(before, after, 32);
    flash_works();
}
/* Refused before anything reached flash: the old password after a restart too. */
static void test_change_refused_at_open_keeps_the_old_password(void)
{
    change_refused(&s_fail_open);
    restart();
    TEST_ASSERT_TRUE(signs_in("Alex", OLD, ALEX));
    TEST_ASSERT_FALSE(signs_in("Alex", NEW, ALEX));
}
static void test_change_refused_at_write_keeps_the_old_password(void)
{
    change_refused(&s_fail_set);
    restart();
    TEST_ASSERT_TRUE(signs_in("Alex", OLD, ALEX));
    TEST_ASSERT_FALSE(signs_in("Alex", NEW, ALEX));
}
/* A commit that fails comes after nvs_set_blob() has already written the new record: RAM keeps
 * the old password until a restart, and after it the new one works - the person's own choice,
 * so the harmless direction (no password nobody chose, none that was turned off). */
static void test_change_refused_at_commit_keeps_the_old_password(void)
{
    change_refused(&s_fail_commit);
    restart();
    TEST_ASSERT_TRUE(signs_in("Alex", NEW, ALEX));
    TEST_ASSERT_FALSE(signs_in("Alex", OLD, ALEX));
}

/* S01: a temporary password set for somebody else is not handed out when flash refused it. */
static void test_set_refused_is_not_reported_done(void)
{
    char why[128] = "", pw[24] = "";
    TEST_ASSERT_TRUE(web_login_clear(PAT));
    s_fail_set = 1;
    TEST_ASSERT_FALSE(web_login_set_temporary(PAT, pw, sizeof(pw), why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("", pw);
    TEST_ASSERT_FALSE(web_login_has(PAT));
}

/* S01, WEB-016 fail closed: turning a sign-in off that flash refuses (an unchanged standard
 * user's, so nothing else ends it) stops the password and its sessions at once, says it
 * failed, and keeps trying until flash takes it: no restart after that brings it back. */
static void turn_off_refused(int *which, bool erased_already)
{
    uint8_t before[32], after[32];
    web_login_fingerprint(&s_cfg, PAT, before);
    TEST_ASSERT_FALSE(zero32(before));
    *which = 1000; /* flash stays broken for now */
    TEST_ASSERT_FALSE(web_login_clear(PAT));
    TEST_ASSERT_FALSE(web_login_has(PAT));
    TEST_ASSERT_FALSE(signs_in("Pat", OLD, PAT));
    TEST_ASSERT_FALSE(web_login_verify(PAT, OLD));
    web_login_fingerprint(&s_cfg, PAT, after); /* the proof of each of Pat's sessions changes */
    TEST_ASSERT_TRUE(zero32(after));
    /* Still on flash (or, a commit that failed after the erase, maybe not): a retry is due. */
    TEST_ASSERT_EQUAL(!erased_already, s_flash[PAT].has);
    TEST_ASSERT_TRUE(web_login_owed());
    /* No retry before its time: a broken flash isn't written on every request. */
    const unsigned writes = s_writes;
    s_now_us += 1000000;
    TEST_ASSERT_FALSE(web_login_enabled(&s_cfg, PAT));
    TEST_ASSERT_EQUAL_UINT(writes, s_writes);
    /* Flash works again: the next look at the sign-ins after the wait erases it. */
    flash_works();
    s_now_us += RETRY_US;
    TEST_ASSERT_FALSE(web_login_enabled(&s_cfg, PAT));
    TEST_ASSERT_FALSE(s_flash[PAT].has);
    restart();
    TEST_ASSERT_FALSE(web_login_has(PAT));
    TEST_ASSERT_FALSE(signs_in("Pat", OLD, PAT));
    TEST_ASSERT_TRUE(signs_in("Alex", OLD, ALEX)); /* nobody else's */
}
static void test_turn_off_refused_at_open_fails_closed_and_retries(void)
{
    turn_off_refused(&s_fail_open, false);
}
static void test_turn_off_refused_at_erase_fails_closed_and_retries(void)
{
    turn_off_refused(&s_fail_erase, false);
}
static void test_turn_off_refused_at_commit_fails_closed_and_retries(void)
{
    turn_off_refused(&s_fail_commit, true);
}

/* S01: turning it off again (the person retries after the error) tries flash at once, wait
 * or not, and says when it is done. */
static void test_turn_off_again_retries_at_once(void)
{
    s_fail_erase = 1;
    TEST_ASSERT_FALSE(web_login_clear(PAT));
    TEST_ASSERT_TRUE(web_login_clear(PAT));
    TEST_ASSERT_FALSE(s_flash[PAT].has);
    TEST_ASSERT_TRUE(web_login_clear(PAT)); /* nothing left to do */
}

/* S01: a new password stored while an erase was still due replaces the record on flash; the
 * old erase is no longer retried over it. */
static void test_new_password_after_a_refused_turn_off(void)
{
    char why[128];
    s_fail_erase = 1;
    TEST_ASSERT_FALSE(web_login_clear(PAT));
    TEST_ASSERT_TRUE(web_login_set(PAT, NEW, false, why, sizeof(why)));
    s_now_us += 2 * RETRY_US;
    TEST_ASSERT_TRUE(web_login_enabled(&s_cfg, PAT));
    restart();
    TEST_ASSERT_TRUE(signs_in("Pat", NEW, PAT));
    TEST_ASSERT_FALSE(signs_in("Pat", OLD, PAT));
}

/* S01: a deleted user's sign-in that flash wouldn't erase (reconcile) is gone at once and
 * erased once flash works. */
static void test_deleted_user_erase_refused_is_retried(void)
{
    const alarm_event_t ev = {.type = ALARM_EV_USER_DELETED, .arg = PAT};
    s_cfg.users[PAT - 1].authority = ALARM_AUTH_NONE;
    web_login_listener()->on_event(NULL, &ev);
    s_fail_erase = 1000;
    TEST_ASSERT_FALSE(web_login_enabled(&s_cfg, PAT));
    TEST_ASSERT_FALSE(web_login_has(PAT));
    TEST_ASSERT_TRUE(s_flash[PAT].has);
    flash_works();
    s_now_us += RETRY_US;
    (void)web_login_enabled(&s_cfg, ALEX);
    TEST_ASSERT_FALSE(s_flash[PAT].has);
}

/* S01: a code change carries the sign-in over (reconcile's rebind). When flash refuses the
 * new binding, RAM keeps the one flash has and tries again: after a restart the password still
 * works instead of being dropped as somebody else's. */
static void test_rebind_refused_is_retried(void)
{
    const alarm_event_t ev = {.type = ALARM_EV_USER_CHANGED, .arg = PAT};
    memset(s_cfg.users[PAT - 1].code_hash, 0xC3, sizeof(s_cfg.users[0].code_hash));
    web_login_listener()->on_event(NULL, &ev);
    s_fail_set = 1000;
    TEST_ASSERT_TRUE(web_login_enabled(&s_cfg, PAT));
    TEST_ASSERT_TRUE(signs_in("Pat", OLD, PAT));
    flash_works();
    s_now_us += RETRY_US;
    TEST_ASSERT_TRUE(web_login_enabled(&s_cfg, PAT));
    restart();
    TEST_ASSERT_TRUE(web_login_enabled(&s_cfg, PAT));
    TEST_ASSERT_TRUE(signs_in("Pat", OLD, PAT));
}

/* ---- S01: a sign-in turned off or replaced while a password is checked ------------------- */
static void clear_pat(void)
{
    TEST_ASSERT_TRUE(web_login_clear(PAT));
}
static void reset_pat(void)
{
    char why[128];
    TEST_ASSERT_TRUE(web_login_set(PAT, NEW, true, why, sizeof(why)));
}

/* S01, WEB-016 fail closed: the touchscreen turns Pat's sign-in off during the second the
 * password is hashed (without the lock). The old password, checked against the copy taken
 * before, must not sign in - before, it did, and the session's proof was built on no sign-in
 * at all, so it matched for good. */
static void test_check_refuses_a_sign_in_turned_off_meanwhile(void)
{
    uint8_t user = 9, verified[32];
    s_during_hash = clear_pat;
    TEST_ASSERT_FALSE(web_login_check("Pat", OLD, &user, verified));
    TEST_ASSERT_EQUAL_UINT8(0, user);
    TEST_ASSERT_TRUE(zero32(verified));
    /* Replaced meanwhile (a temporary password set for Pat): not the old one's session either. */
    char why[128];
    TEST_ASSERT_TRUE(web_login_set(PAT, OLD, false, why, sizeof(why)));
    TEST_ASSERT_TRUE(signs_in("Pat", OLD, PAT)); /* the control */
    s_during_hash = reset_pat;
    TEST_ASSERT_FALSE(web_login_check("Pat", OLD, &user, verified));
    TEST_ASSERT_TRUE(zero32(verified));
}

/* S01, WEB-016: turned off after the check but before the session is made - the proof
 * (web_login_fingerprint_verified) is refused, and is never the all-zero "no sign-in" one. */
static void test_no_session_on_a_sign_in_gone_since_the_check(void)
{
    uint8_t user = 0, verified[32], out[32];
    TEST_ASSERT_TRUE(web_login_check("Pat", OLD, &user, verified));
    TEST_ASSERT_EQUAL_UINT8(PAT, user);
    TEST_ASSERT_FALSE(zero32(verified));
    TEST_ASSERT_TRUE(web_login_fingerprint_verified(&s_cfg, PAT, verified, out)); /* control */
    TEST_ASSERT_EQUAL_MEMORY(verified, out, 32);
    TEST_ASSERT_TRUE(web_login_clear(PAT));
    TEST_ASSERT_FALSE(web_login_fingerprint(&s_cfg, PAT, out));
    TEST_ASSERT_FALSE(web_login_fingerprint_verified(&s_cfg, PAT, verified, out));
    TEST_ASSERT_TRUE(zero32(out));
    /* A new password set in between: still not the sign-in that was checked. */
    char why[128];
    TEST_ASSERT_TRUE(web_login_set(PAT, NEW, true, why, sizeof(why)));
    TEST_ASSERT_FALSE(web_login_fingerprint_verified(&s_cfg, PAT, verified, out));
    TEST_ASSERT_TRUE(zero32(out));
    /* No sign-in at all can't be "verified" by an all-zero fingerprint. */
    TEST_ASSERT_TRUE(web_login_clear(PAT));
    memset(verified, 0, sizeof(verified));
    TEST_ASSERT_FALSE(web_login_fingerprint_verified(&s_cfg, PAT, verified, out));
}

/* S01: a password change whose sign-in is turned off while the current password is checked
 * stores nothing - the sign-in stays off, after a restart too. */
static void test_change_refused_when_turned_off_meanwhile(void)
{
    char why[128] = "";
    bool wrong = true;
    uint8_t fp[32];
    s_during_hash = clear_pat;
    TEST_ASSERT_FALSE(web_login_change(PAT, OLD, NEW, &wrong, fp, why, sizeof(why)));
    TEST_ASSERT_FALSE(wrong);
    TEST_ASSERT_EQUAL_STRING("This user has no browser sign-in", why);
    TEST_ASSERT_FALSE(web_login_why_busy(why)); /* 400, as for anyone without one */
    TEST_ASSERT_TRUE(zero32(fp));
    TEST_ASSERT_FALSE(web_login_has(PAT));
    restart();
    TEST_ASSERT_FALSE(web_login_has(PAT));
    TEST_ASSERT_FALSE(signs_in("Pat", NEW, PAT));
}

/* S01: the person's own mistakes stay 400; only the panel's trouble is 503. */
static void test_why_busy_tells_the_panel_from_the_person(void)
{
    char why[128] = "";
    bool wrong = false;
    uint8_t fp[32];
    TEST_ASSERT_FALSE(web_login_change(ALEX, "not the password at all", NEW, &wrong, fp, why,
                                       sizeof(why)));
    TEST_ASSERT_TRUE(wrong);
    TEST_ASSERT_FALSE(web_login_why_busy(why));
    TEST_ASSERT_FALSE(web_login_set(ALEX, "short", false, why, sizeof(why)));
    TEST_ASSERT_FALSE(web_login_why_busy(why));
    s_fail_set = 1;
    TEST_ASSERT_FALSE(web_login_set(ALEX, NEW, false, why, sizeof(why)));
    TEST_ASSERT_TRUE(web_login_why_busy(why));
    TEST_ASSERT_FALSE(web_login_why_busy(NULL));
}

/* ---- S01: the retry with nobody at the browser (panel_web's maintenance tick) ------------ */

/* S01, WEB-016: a refused erase is retried by web_login_retry() alone - no sign-in, session
 * check or users list needed - once its wait is over, and then nothing is owed. */
static void test_retry_erases_without_anyone_at_the_browser(void)
{
    TEST_ASSERT_FALSE(web_login_owed());
    s_fail_erase = 1000;
    TEST_ASSERT_FALSE(web_login_clear(PAT));
    TEST_ASSERT_TRUE(web_login_owed());
    TEST_ASSERT_TRUE(s_flash[PAT].has);
    const unsigned writes = s_writes;
    web_login_retry(); /* within the wait: flash isn't tried */
    TEST_ASSERT_EQUAL_UINT(writes, s_writes);
    s_now_us += RETRY_US;
    web_login_retry(); /* still broken: tried once, owed still */
    TEST_ASSERT_EQUAL_UINT(writes + 1u, s_writes);
    TEST_ASSERT_TRUE(web_login_owed());
    flash_works();
    s_now_us += RETRY_US;
    web_login_retry();
    TEST_ASSERT_FALSE(s_flash[PAT].has);
    TEST_ASSERT_FALSE(web_login_owed());
    restart();
    TEST_ASSERT_FALSE(signs_in("Pat", OLD, PAT));
    TEST_ASSERT_TRUE(signs_in("Alex", OLD, ALEX));
}

/* S01: a deleted user's sign-in that flash wouldn't erase before a restart comes back with it
 * from flash; the retry once at start erases it, before anyone looks. */
static void test_retry_at_start_erases_a_deleted_users_sign_in(void)
{
    const alarm_event_t ev = {.type = ALARM_EV_USER_DELETED, .arg = PAT};
    s_cfg.users[PAT - 1].authority = ALARM_AUTH_NONE;
    web_login_listener()->on_event(NULL, &ev);
    TEST_ASSERT_TRUE(web_login_owed());
    s_fail_erase = 1000;
    web_login_retry();
    TEST_ASSERT_FALSE(web_login_has(PAT));
    TEST_ASSERT_TRUE(s_flash[PAT].has);
    restart(); /* what RAM owed is forgotten; flash still has Pat's record */
    flash_works();
    TEST_ASSERT_TRUE(web_login_has(PAT)); /* loaded, nobody has looked yet */
    web_login_retry();
    TEST_ASSERT_FALSE(web_login_has(PAT));
    TEST_ASSERT_FALSE(s_flash[PAT].has);
    TEST_ASSERT_FALSE(web_login_owed());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_changes_that_flash_takes_survive_a_restart);
    RUN_TEST(test_change_refused_at_open_keeps_the_old_password);
    RUN_TEST(test_change_refused_at_write_keeps_the_old_password);
    RUN_TEST(test_change_refused_at_commit_keeps_the_old_password);
    RUN_TEST(test_set_refused_is_not_reported_done);
    RUN_TEST(test_turn_off_refused_at_open_fails_closed_and_retries);
    RUN_TEST(test_turn_off_refused_at_erase_fails_closed_and_retries);
    RUN_TEST(test_turn_off_refused_at_commit_fails_closed_and_retries);
    RUN_TEST(test_turn_off_again_retries_at_once);
    RUN_TEST(test_new_password_after_a_refused_turn_off);
    RUN_TEST(test_deleted_user_erase_refused_is_retried);
    RUN_TEST(test_rebind_refused_is_retried);
    RUN_TEST(test_check_refuses_a_sign_in_turned_off_meanwhile);
    RUN_TEST(test_no_session_on_a_sign_in_gone_since_the_check);
    RUN_TEST(test_change_refused_when_turned_off_meanwhile);
    RUN_TEST(test_why_busy_tells_the_panel_from_the_person);
    RUN_TEST(test_retry_erases_without_anyone_at_the_browser);
    RUN_TEST(test_retry_at_start_erases_a_deleted_users_sign_in);
    return UNITY_END();
}
