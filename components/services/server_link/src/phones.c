#include "sdkconfig.h"
#if CONFIG_ALARM_FEATURE_SERVER
/*
 * phones.c - direct mode: phones paired at the panel by QR code, signing in with their own key and
 * signing the guarded actions themselves (alarm-server's direct-mode.md). The rules are alarm_proto's
 * (proto_phone.h); the Panel Manager calls these from its worker, the console and the touchscreen
 * from theirs, so everything here takes the lock.
 */
#include <stdio.h>
#include <string.h>

#include "alarm_mem.h"
#include "alarm_proto/proto_server.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "server_link.h"
#include "server_link_internal.h"
#include "sodium.h"

static const char *TAG = "phones";

#define CHALLENGES 4u

typedef struct {
    char value[PROTO_SRV_B64_KEY_LEN];
    char phone[PROTO_PHONE_ID_LEN];
    char action[20];
    alarm_ms_t until;
} challenge_t;

static SemaphoreHandle_t s_plock;
static proto_phone_t s_phones[PROTO_PHONE_MAX] ALARM_SLOW_BSS;
static proto_phone_window_t s_window;
static proto_phone_t s_asking ALARM_SLOW_BSS; /* waiting for Allow on the panel */
static bool s_asked;
static uint8_t s_answered_pub[PROTO_PHONE_KEY_LEN];
static int s_answer; /* for that key: 0 none, 1 allowed, -1 not allowed */
static challenge_t s_challenges[CHALLENGES];
static uint8_t s_next_challenge;
static uint32_t s_revision; /* the table changed: the push service reads it again */

esp_err_t phones_init(void)
{
    s_plock = xSemaphoreCreateMutex();
    if (s_plock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    nvs_handle_t h;
    if (nvs_open(SERVER_LINK_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_phones);
        if (nvs_get_blob(h, "phones", s_phones, &len) != ESP_OK || len != sizeof(s_phones)) {
            memset(s_phones, 0, sizeof(s_phones));
        }
        nvs_close(h);
    }
    return ESP_OK;
}

static esp_err_t store(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(SERVER_LINK_NVS_NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = nvs_set_blob(h, "phones", s_phones, sizeof(s_phones));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    s_revision++;
    return err;
}

esp_err_t server_link_phone_pair_begin(uint8_t user, const uint8_t cert_sha256[32],
                                       const char *hosts, char *uri, size_t cap)
{
    if (s_plock == NULL || user < 1u || user > ALARM_MAX_USERS) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t token[PROTO_PHONE_TOKEN_LEN];
    esp_fill_random(token, sizeof(token));
    xSemaphoreTake(s_plock, portMAX_DELAY);
    proto_phone_window_open(&s_window, token, user, alarm_service_now());
    s_asked = false;
    s_answer = 0;
    xSemaphoreGive(s_plock);
    char id[PROTO_SRV_ID_LEN];
    server_link_panel_id(id);
    const bool ok = proto_phone_pair_uri(id, hosts, cert_sha256, token, uri, cap);
    sodium_memzero(token, sizeof(token));
    return ok ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

void server_link_phone_pair_cancel(void)
{
    if (s_plock != NULL) {
        xSemaphoreTake(s_plock, portMAX_DELAY);
        s_window.open = false;
        s_asked = false;
        xSemaphoreGive(s_plock);
    }
}

server_link_phone_ask_t server_link_phone_pair_request(const uint8_t token[PROTO_PHONE_TOKEN_LEN],
                                                       const proto_phone_t *phone)
{
    if (s_plock == NULL) {
        return SERVER_LINK_PHONE_REFUSED;
    }
    server_link_phone_ask_t r = SERVER_LINK_PHONE_REFUSED;
    xSemaphoreTake(s_plock, portMAX_DELAY);
    if (s_asked) {
        r = SERVER_LINK_PHONE_BUSY;
    } else if (proto_phone_window_use(&s_window, token, alarm_service_now())) {
        s_asking = *phone;
        s_asking.used = 1u;
        s_asking.user = s_window.user;
        s_asked = true;
        s_answer = 0;
        memcpy(s_answered_pub, phone->pub, sizeof(s_answered_pub));
        r = SERVER_LINK_PHONE_ASKING;
    }
    xSemaphoreGive(s_plock);
    return r;
}

bool server_link_phone_pair_asking(char *name, size_t cap, uint8_t *user)
{
    if (s_plock == NULL) {
        return false;
    }
    xSemaphoreTake(s_plock, portMAX_DELAY);
    const bool asked = s_asked;
    if (asked) {
        snprintf(name, cap, "%s", s_asking.name);
        *user = s_asking.user;
    }
    xSemaphoreGive(s_plock);
    return asked;
}

esp_err_t server_link_phone_pair_decide(bool allow)
{
    if (s_plock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_plock, portMAX_DELAY);
    if (!s_asked) {
        err = ESP_ERR_INVALID_STATE;
    } else if (!allow) {
        s_answer = -1;
    } else {
        char id[PROTO_PHONE_ID_LEN];
        proto_phone_id(s_asking.pub, id);
        proto_phone_t *slot = proto_phone_find(s_phones, id);
        if (slot == NULL) {
            slot = proto_phone_free_slot(s_phones);
        }
        if (slot == NULL) {
            err = ESP_ERR_NO_MEM; /* 16 phones already */
            s_answer = -1;
        } else {
            *slot = s_asking;
            err = store(); /* in one NVS write (DIR-001) */
            s_answer = err == ESP_OK ? 1 : -1;
        }
    }
    s_asked = false;
    sodium_memzero(&s_asking, sizeof(s_asking));
    xSemaphoreGive(s_plock);
    return err;
}

int server_link_phone_pair_result(const uint8_t pub[PROTO_PHONE_KEY_LEN])
{
    if (s_plock == NULL) {
        return -1;
    }
    xSemaphoreTake(s_plock, portMAX_DELAY);
    const int r = sodium_memcmp(pub, s_answered_pub, PROTO_PHONE_KEY_LEN) == 0 ? s_answer : -1;
    xSemaphoreGive(s_plock);
    return r;
}

bool server_link_phone_challenge(const char *phone_id, const char *action, char out[PROTO_SRV_B64_KEY_LEN])
{
    if (s_plock == NULL) {
        return false;
    }
    bool ok = false;
    uint8_t raw[PROTO_PHONE_CHALLENGE_LEN];
    esp_fill_random(raw, sizeof(raw));
    xSemaphoreTake(s_plock, portMAX_DELAY);
    if (proto_phone_find(s_phones, phone_id) != NULL) {
        challenge_t *c = &s_challenges[s_next_challenge];
        s_next_challenge = (uint8_t)((s_next_challenge + 1u) % CHALLENGES);
        (void)proto_srv_b64url(raw, sizeof(raw), c->value, sizeof(c->value));
        snprintf(c->phone, sizeof(c->phone), "%s", phone_id);
        snprintf(c->action, sizeof(c->action), "%s", action != NULL ? action : "");
        c->until = alarm_service_now() + PROTO_PHONE_CHALLENGE_MS;
        snprintf(out, PROTO_SRV_B64_KEY_LEN, "%s", c->value);
        ok = true;
    }
    xSemaphoreGive(s_plock);
    return ok;
}

/* A challenge works once, for that phone and that action. Under the lock. */
static bool take_challenge(const char *phone_id, const char *challenge, const char *action)
{
    const alarm_ms_t now = alarm_service_now();
    for (size_t i = 0; i < CHALLENGES; i++) {
        challenge_t *c = &s_challenges[i];
        if (c->value[0] != '\0' && strcmp(c->value, challenge) == 0) {
            const bool ok = now < c->until && strcmp(c->phone, phone_id) == 0 &&
                            strcmp(c->action, action) == 0;
            memset(c, 0, sizeof(*c));
            return ok;
        }
    }
    return false;
}

bool server_link_phone_verify(const char *phone_id, const char *challenge, const char *action,
                              const uint8_t sig[PROTO_PHONE_SIG_LEN], uint8_t *user)
{
    if (s_plock == NULL) {
        return false;
    }
    char panel[PROTO_SRV_ID_LEN];
    server_link_panel_id(panel);
    uint8_t msg[PROTO_PHONE_SIGNED_MAX];
    const size_t n = action[0] != '\0'
                         ? proto_phone_action_bytes(panel, challenge, action, msg, sizeof(msg))
                         : proto_phone_session_bytes(panel, challenge, msg, sizeof(msg));
    bool ok = false;
    xSemaphoreTake(s_plock, portMAX_DELAY);
    const proto_phone_t *p = proto_phone_find(s_phones, phone_id);
    if (p != NULL && n != 0u && take_challenge(phone_id, challenge, action) &&
        crypto_sign_verify_detached(sig, msg, n, p->pub) == 0) {
        *user = p->user;
        ok = true;
    }
    xSemaphoreGive(s_plock);
    return ok;
}

size_t server_link_phone_list(proto_phone_t *out, size_t max)
{
    size_t n = 0;
    if (s_plock == NULL) {
        return 0u;
    }
    xSemaphoreTake(s_plock, portMAX_DELAY);
    for (size_t i = 0; i < PROTO_PHONE_MAX && n < max; i++) {
        if (s_phones[i].used != 0u) {
            out[n++] = s_phones[i];
        }
    }
    xSemaphoreGive(s_plock);
    return n;
}

uint32_t server_link_phone_revision(void)
{
    return s_revision;
}

esp_err_t server_link_phone_remove(const char *phone_id)
{
    if (s_plock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    xSemaphoreTake(s_plock, portMAX_DELAY);
    proto_phone_t *p = proto_phone_find(s_phones, phone_id);
    if (p != NULL) {
        sodium_memzero(p, sizeof(*p));
        err = store();
    }
    xSemaphoreGive(s_plock);
    return err;
}

esp_err_t server_link_phone_set_push(const char *phone_id, const char *handle, const char *kid,
                                     const uint8_t key[PROTO_PHONE_KEY_LEN], uint8_t levels)
{
    if (s_plock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    xSemaphoreTake(s_plock, portMAX_DELAY);
    proto_phone_t *p = proto_phone_find(s_phones, phone_id);
    if (p != NULL) {
        snprintf(p->handle, sizeof(p->handle), "%s", handle != NULL ? handle : "");
        snprintf(p->kid, sizeof(p->kid), "%s", kid != NULL ? kid : "");
        if (key != NULL) {
            memcpy(p->push_key, key, sizeof(p->push_key));
        } else {
            sodium_memzero(p->push_key, sizeof(p->push_key));
        }
        p->levels = levels;
        err = store();
    }
    xSemaphoreGive(s_plock);
    return err;
}

bool phones_any(void)
{
    if (s_plock == NULL) {
        return false;
    }
    bool any = false;
    xSemaphoreTake(s_plock, portMAX_DELAY);
    for (size_t i = 0; i < PROTO_PHONE_MAX && !any; i++) {
        any = s_phones[i].used != 0u;
    }
    xSemaphoreGive(s_plock);
    return any;
}

bool server_link_phone_paired(const char *phone_id, uint8_t user)
{
    if (s_plock == NULL) {
        return false;
    }
    xSemaphoreTake(s_plock, portMAX_DELAY);
    const proto_phone_t *p = proto_phone_find(s_phones, phone_id);
    const bool ok = p != NULL && p->user == user;
    xSemaphoreGive(s_plock);
    return ok;
}

/* DIR-004: a person deleted, or made something that can't have a phone. */
void phones_forget_users(uint64_t users)
{
    if (s_plock == NULL) {
        return;
    }
    bool changed = false;
    xSemaphoreTake(s_plock, portMAX_DELAY);
    for (unsigned u = 1; u <= 64u; u++) {
        if ((users & ((uint64_t)1u << (u - 1u))) != 0u && proto_phone_remove_user(s_phones, (uint8_t)u) > 0u) {
            ESP_LOGW(TAG, "user %u was deleted: their phones removed", u);
            changed = true;
        }
    }
    if (changed) {
        (void)store();
    }
    xSemaphoreGive(s_plock);
}

void phones_review(const alarm_config_t *cfg)
{
    if (s_plock == NULL) {
        return;
    }
    bool changed = false;
    xSemaphoreTake(s_plock, portMAX_DELAY);
    for (size_t i = 0; i < PROTO_PHONE_MAX; i++) {
        proto_phone_t *p = &s_phones[i];
        if (p->used == 0u) {
            continue;
        }
        const alarm_authority_t a =
            p->user >= 1u && p->user <= ALARM_MAX_USERS ? cfg->users[p->user - 1u].authority
                                                        : ALARM_AUTH_NONE;
        if (a != ALARM_AUTH_INSTALLER && a != ALARM_AUTH_MASTER &&
            a != ALARM_AUTH_PARTITION_MASTER && a != ALARM_AUTH_STANDARD) {
            ESP_LOGW(TAG, "phone %s removed: user %u can't have one now", p->name, (unsigned)p->user);
            sodium_memzero(p, sizeof(*p));
            changed = true;
        }
    }
    if (changed) {
        (void)store();
    }
    xSemaphoreGive(s_plock);
}

#endif
