/*
 * notify_http.c - the request each notification service wants (docs/features/27 NTFY-001,
 * docs/features/28 PUSH-001, PUSH-005).
 *
 * No sockets: it fills a caller-owned notify_request_t and components/services/net_notify posts it.
 */
#include "notify/notify_http.h"

#include <stdio.h>
#include <string.h>

#define PUSHOVER_API "https://api.pushover.net/1"
/* Pushover's own limits; a message is cut to fit rather than refused. */
#define PUSHOVER_MSG_MAX   1024u
#define PUSHOVER_TITLE_MAX 250u

static bool empty(const char *s)
{
    return s == NULL || s[0] == '\0';
}

/* Append to a buffer, keeping track of whether everything fit. */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool over;
} sink_t;

static void sink_init(sink_t *s, char *buf, size_t cap)
{
    s->buf = buf;
    s->cap = cap;
    s->len = 0u;
    s->over = false;
    if (cap > 0u) {
        buf[0] = '\0';
    }
}

static void put(sink_t *s, const char *text, size_t n)
{
    if (s->over || s->len + n + 1u > s->cap) {
        s->over = true;
        return;
    }
    memcpy(s->buf + s->len, text, n);
    s->len += n;
    s->buf[s->len] = '\0';
}

static void puts_(sink_t *s, const char *text)
{
    put(s, text, strlen(text));
}

/* How much of 'text' can be kept without cutting a UTF-8 character in half: a title chopped
 * mid-sequence would reach the phone as a replacement character, or be rejected outright. */
static size_t utf8_fit(const char *text, size_t max)
{
    const size_t n = strlen(text);
    if (n <= max) {
        return n;
    }
    size_t cut = max;
    while (cut > 0u && ((unsigned char)text[cut] & 0xC0u) == 0x80u) {
        cut--; /* step back off a continuation byte to the start of the character */
    }
    return cut;
}

/* ---- JSON, for ntfy ------------------------------------------------------------------------ */

/* A JSON string body. UTF-8 goes through untouched - it is legal in JSON and is exactly why the
 * JSON form is used instead of HTTP headers. */
static void json_string(sink_t *s, const char *text, size_t n)
{
    puts_(s, "\"");
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)text[i];
        switch (c) {
        case '"': puts_(s, "\\\""); break;
        case '\\': puts_(s, "\\\\"); break;
        case '\n': puts_(s, "\\n"); break;
        case '\r': puts_(s, "\\r"); break;
        case '\t': puts_(s, "\\t"); break;
        case '\b': puts_(s, "\\b"); break;
        case '\f': puts_(s, "\\f"); break;
        default:
            if (c < 0x20u) {
                char esc[7];
                snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)c);
                puts_(s, esc);
            } else {
                put(s, (const char *)&c, 1u);
            }
            break;
        }
    }
    puts_(s, "\"");
}

static void json_field(sink_t *s, const char *key, const char *value, size_t max, bool *first)
{
    if (!*first) {
        puts_(s, ",");
    }
    *first = false;
    json_string(s, key, strlen(key));
    puts_(s, ":");
    json_string(s, value, utf8_fit(value, max));
}

notify_prio_t notify_priority(const alarm_event_t *ev)
{
    if (ev->type == ALARM_EV_AUTOMATION_NOTICE) {
        /* NOTE-042: asked for, so it sounds as an arming does; an alarm's a step louder, never
         * urgent (the alarm's own notification is) */
        return (ev->arg == (uint8_t)ALARM_AUTO_NOTICE_ALARM ||
                ev->arg == (uint8_t)ALARM_AUTO_NOTICE_FIRE)
                   ? NOTIFY_PRIO_HIGH
                   : NOTIFY_PRIO_NORMAL;
    }
    switch (notify_category(ev->type)) {
    case NOTIFY_CAT_ALARM:
        /* Only a live alarm is urgent: the ones that say it is over are not. */
        return (ev->type == ALARM_EV_ALARM || ev->type == ALARM_EV_DURESS ||
                ev->type == ALARM_EV_EXIT_ERROR || ev->type == ALARM_EV_RECENT_CLOSING)
                   ? NOTIFY_PRIO_URGENT
                   : NOTIFY_PRIO_HIGH;
    case NOTIFY_CAT_TROUBLE: return NOTIFY_PRIO_HIGH;
    case NOTIFY_CAT_ARMING: return NOTIFY_PRIO_NORMAL;
    default: return NOTIFY_PRIO_LOW;
    }
}

/* ntfy's 1-5 (NTFY-009). */
static int ntfy_prio(notify_prio_t p)
{
    switch (p) {
    case NOTIFY_PRIO_URGENT: return 5;
    case NOTIFY_PRIO_HIGH: return 4;
    case NOTIFY_PRIO_NORMAL: return 3;
    default: return 2;
    }
}

/* One tag per category, so a phone-side filter can tell an alarm from a trouble without reading
 * the title. Frozen once shipped: never rename, only add. */
static const char *ntfy_tag(uint8_t cat)
{
    switch ((notify_cat_t)cat) {
    case NOTIFY_CAT_ALARM: return "rotating_light";
    case NOTIFY_CAT_TROUBLE: return "warning";
    case NOTIFY_CAT_ARMING: return "lock";
    case NOTIFY_CAT_BYPASS: return "door";
    default: return "information_source";
    }
}

bool notify_build_ntfy(const notify_ntfy_cfg_t *cfg, const notify_words_t *w, notify_prio_t prio,
                       notify_request_t *out)
{
    if (cfg == NULL || w == NULL || out == NULL || empty(cfg->base) || empty(cfg->topic)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    snprintf(out->url, sizeof(out->url), "%s/", cfg->base);
    snprintf(out->content_type, sizeof(out->content_type), "application/json");
    if (!empty(cfg->token)) {
        snprintf(out->auth, sizeof(out->auth), "Bearer %s", cfg->token);
    }

    sink_t s;
    sink_init(&s, out->body, sizeof(out->body));
    bool first = true;
    puts_(&s, "{");
    json_field(&s, "topic", cfg->topic, 64u, &first);
    json_field(&s, "title", w->title, NOTIFY_TITLE_LEN - 1u, &first);
    json_field(&s, "message", w->body[0] != '\0' ? w->body : w->title, NOTIFY_BODY_LEN - 1u,
               &first);
    puts_(&s, ",\"priority\":");
    char n[8];
    snprintf(n, sizeof(n), "%d", ntfy_prio(prio));
    puts_(&s, n);
    puts_(&s, ",\"tags\":[");
    json_string(&s, ntfy_tag(w->cat), strlen(ntfy_tag(w->cat)));
    puts_(&s, "]}");
    if (s.over) {
        return false;
    }
    out->body_len = s.len;
    return true;
}

/* ---- form encoding, for Pushover ------------------------------------------------------------ */

static void form_value(sink_t *s, const char *text, size_t n)
{
    static const char HEX[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)text[i];
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            put(s, (const char *)&c, 1u);
        } else {
            char esc[4] = {'%', HEX[c >> 4], HEX[c & 0x0Fu], '\0'};
            put(s, esc, 3u);
        }
    }
}

static void form_field(sink_t *s, const char *key, const char *value, size_t max, bool *first)
{
    if (!*first) {
        puts_(s, "&");
    }
    *first = false;
    puts_(s, key);
    puts_(s, "=");
    form_value(s, value, utf8_fit(value, max));
}

static void form_int(sink_t *s, const char *key, int value, bool *first)
{
    char n[12];
    snprintf(n, sizeof(n), "%d", value);
    if (!*first) {
        puts_(s, "&");
    }
    *first = false;
    puts_(s, key);
    puts_(s, "=");
    puts_(s, n);
}

/* Pushover's -2..2 (PUSH-009). */
static int pushover_prio(notify_prio_t p, bool emergency)
{
    switch (p) {
    case NOTIFY_PRIO_URGENT: return emergency ? 2 : 1;
    case NOTIFY_PRIO_HIGH: return 1;
    case NOTIFY_PRIO_NORMAL: return 0;
    default: return -1;
    }
}

bool notify_build_pushover(const notify_pushover_cfg_t *cfg, const notify_words_t *w,
                           notify_prio_t prio, const char *tag, notify_request_t *out)
{
    if (cfg == NULL || w == NULL || out == NULL || empty(cfg->user_key) || empty(cfg->app_token)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    snprintf(out->url, sizeof(out->url), "%s/messages.json", PUSHOVER_API);
    snprintf(out->content_type, sizeof(out->content_type), "application/x-www-form-urlencoded");

    const bool emergency = cfg->emergency && prio == NOTIFY_PRIO_URGENT;
    sink_t s;
    sink_init(&s, out->body, sizeof(out->body));
    bool first = true;
    form_field(&s, "token", cfg->app_token, 64u, &first);
    form_field(&s, "user", cfg->user_key, 64u, &first);
    form_field(&s, "title", w->title, PUSHOVER_TITLE_MAX, &first);
    form_field(&s, "message", w->body[0] != '\0' ? w->body : w->title, PUSHOVER_MSG_MAX, &first);
    form_int(&s, "priority", pushover_prio(prio, emergency), &first);
    if (emergency) {
        /* Pushover requires both, and refuses the message without them. */
        form_int(&s, "retry", cfg->retry_s >= 30u ? (int)cfg->retry_s : 30, &first);
        form_int(&s, "expire", cfg->expire_s > 0u && cfg->expire_s <= 10800u ? (int)cfg->expire_s
                                                                            : 1800,
                 &first);
        if (!empty(tag)) {
            form_field(&s, "tag", tag, 64u, &first); /* so it can be cancelled (PUSH-005) */
        }
    }
    /* Only an alarm takes the chosen sound: a trouble at midnight shouldn't sound like a
     * break-in (PUSH-007). */
    if (!empty(cfg->sound) && prio == NOTIFY_PRIO_URGENT) {
        form_field(&s, "sound", cfg->sound, 16u, &first);
    }
    if (!empty(cfg->device)) {
        form_field(&s, "device", cfg->device, 25u, &first);
    }
    if (s.over) {
        return false;
    }
    out->body_len = s.len;
    return true;
}

bool notify_build_pushover_cancel(const notify_pushover_cfg_t *cfg, const char *tag,
                                  notify_request_t *out)
{
    if (cfg == NULL || out == NULL || empty(cfg->app_token) || empty(tag)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    snprintf(out->url, sizeof(out->url), "%s/receipts/cancel_by_tag/%s.json", PUSHOVER_API, tag);
    snprintf(out->content_type, sizeof(out->content_type), "application/x-www-form-urlencoded");

    sink_t s;
    sink_init(&s, out->body, sizeof(out->body));
    bool first = true;
    form_field(&s, "token", cfg->app_token, 64u, &first);
    if (s.over) {
        return false;
    }
    out->body_len = s.len;
    return true;
}

notify_http_outcome_t notify_http_outcome(int status)
{
    if (status >= 200 && status < 300) {
        return NOTIFY_HTTP_TAKEN;
    }
    if (status >= 400 && status < 500 && status != 429) {
        return NOTIFY_HTTP_REFUSED;
    }
    return NOTIFY_HTTP_RETRY;
}

uint64_t notify_backoff(uint8_t *fails, uint64_t now, uint32_t random)
{
    uint32_t delay = NOTIFY_RETRY_MS;
    for (uint8_t i = 0; i < *fails && delay < NOTIFY_RETRY_MAX_MS; i++) {
        delay *= 2u;
    }
    if (delay > NOTIFY_RETRY_MAX_MS) {
        delay = NOTIFY_RETRY_MAX_MS;
    }
    delay += random % (delay / 4u);
    if (*fails < 255u) {
        (*fails)++;
    }
    return now + delay;
}

bool notify_may_send(uint64_t now, uint64_t retry_at, uint64_t last_send)
{
    if (now < retry_at) {
        return false;
    }
    return now >= last_send && now - last_send >= NOTIFY_SEND_GAP_MS;
}

notify_prio_t notify_priority_dealt(notify_prio_t prio, uint32_t seq, uint32_t dealt)
{
    /* Sequence numbers only grow, and survive a restart; compared as a difference so the
     * wrap after four billion events doesn't turn the answer round. */
    if (prio == NOTIFY_PRIO_URGENT && dealt != 0u && (int32_t)(dealt - seq) > 0) {
        return NOTIFY_PRIO_HIGH;
    }
    return prio;
}
