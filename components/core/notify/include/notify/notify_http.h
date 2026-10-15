/*
 * notify_http.h - the request each notification service wants (docs/features/27, /28).
 *
 * Building the URL, the headers and the body is the part that is easy to get quietly wrong:
 * a zone called Dad's Office, a trouble title with a "·" in it, a message at the length limit.
 * So it happens here, in portable C11 with no sockets, and every shape is a host test.
 * components/services/net_notify does nothing but post what comes out.
 */
#ifndef NOTIFY_HTTP_H
#define NOTIFY_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "notify/notify_words.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How loud this one should be. Each service maps it to its own scale: ntfy 1-5, Pushover -2..2,
 * which is why it is neither of those here. */
typedef enum {
    NOTIFY_PRIO_LOW = 0,
    NOTIFY_PRIO_NORMAL,
    NOTIFY_PRIO_HIGH,
    NOTIFY_PRIO_URGENT,
} notify_prio_t;

/* Pushover caps the message at 1024 characters and the title at 250; ntfy's server refuses a
 * body over 4 KB. One buffer serves both, sized for the larger. */
#define NOTIFY_URL_LEN     256
#define NOTIFY_AUTH_LEN    160
#define NOTIFY_REQ_BODY    1024

typedef struct {
    char url[NOTIFY_URL_LEN];
    char content_type[40];
    char auth[NOTIFY_AUTH_LEN]; /* the Authorization header's value, "" when there is none */
    char body[NOTIFY_REQ_BODY];
    size_t body_len;
} notify_request_t;

typedef struct {
    const char *base;  /* "https://ntfy.sh", or the owner's own server; no trailing slash */
    const char *topic;
    const char *token; /* NULL or "" when the server needs none */
} notify_ntfy_cfg_t;

typedef struct {
    const char *user_key;
    const char *app_token;
    const char *sound;  /* NULL or "": whatever the account uses */
    const char *device; /* NULL or "": every device on the account */
    bool emergency;     /* alarms repeat until acknowledged (PUSH-004) */
    uint16_t retry_s;   /* how often the repeat fires (Pushover's minimum is 30) */
    uint16_t expire_s;  /* how long it keeps trying (Pushover's maximum is 10800) */
} notify_pushover_cfg_t;

/* NOTE-014 (and PUSH-005 for a cancel): what an answer means for trying again. */
typedef enum {
    NOTIFY_HTTP_TAKEN = 0, /* a 2xx */
    NOTIFY_HTTP_RETRY,     /* no answer (a timeout, no route), a 429, a 5xx, anything unexpected */
    NOTIFY_HTTP_REFUSED,   /* a 4xx other than 429: it will never succeed, so not again */
} notify_http_outcome_t;

/* status: the HTTP status, or negative when there was no answer. */
notify_http_outcome_t notify_http_outcome(int status);

/* NOTE-014/035: the pace of one service. A request that failed waits a growing, jittered gap
 * (2 s doubling to a minute, plus up to a quarter more) before the next one, and no two requests
 * to the same service go within NOTIFY_SEND_GAP_MS of each other. */
#define NOTIFY_SEND_GAP_MS  1500u
#define NOTIFY_RETRY_MS     2000u
#define NOTIFY_RETRY_MAX_MS 60000u

/* The time of the next attempt after one more failure; *fails counts the failures in a row and
 * goes up by one (it stops at 255). random: any random number, for the jitter. */
uint64_t notify_backoff(uint8_t *fails, uint64_t now, uint32_t random);

/* May a request go to this service now? retry_at: not before this, after a failure (0: no
 * wait); last_send: when the last request went (one later than now went in this pass: no). */
bool notify_may_send(uint64_t now, uint64_t retry_at, uint64_t last_send);

/* How loud an event should be, before either service's own scale. */
notify_prio_t notify_priority(const alarm_event_t *ev);

/* PUSH-005: an alarm whose disarm, cancel or silence has already happened is not urgent any
 * more. Sent late (its first request failed, or the network was down), it still reaches the
 * phone, but not as an emergency that repeats with nothing left to cancel it: URGENT becomes
 * HIGH when seq (the alarm's event, or the newest one in a rollup) is older than dealt (the
 * event that dealt with its partition's alarm, 0 = none yet). Anything else is returned as is. */
notify_prio_t notify_priority_dealt(notify_prio_t prio, uint32_t seq, uint32_t dealt);

/* One ntfy publish. Posts JSON to the base URL rather than to <base>/<topic> with headers: our
 * own titles contain "·", and a non-ASCII HTTP header would have to be RFC 2047 encoded, while
 * a JSON body takes UTF-8 as it is (NTFY-001).
 * false: something is missing (no base or no topic) and nothing was written. */
bool notify_build_ntfy(const notify_ntfy_cfg_t *cfg, const notify_words_t *w, notify_prio_t prio,
                       notify_request_t *out);

/* One Pushover message, as the form POST its API takes. 'tag' groups emergency messages so they
 * can all be cancelled at once when the alarm is dealt with; NULL or "" leaves it off.
 * emergency only applies to NOTIFY_PRIO_URGENT, and only when the config asked for it. */
bool notify_build_pushover(const notify_pushover_cfg_t *cfg, const notify_words_t *w,
                           notify_prio_t prio, const char *tag, notify_request_t *out);

/* Where to tell Pushover that an alarm is over, so the phones stop repeating (PUSH-005).
 * Writes the URL and a body carrying the application token. */
bool notify_build_pushover_cancel(const notify_pushover_cfg_t *cfg, const char *tag,
                                  notify_request_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NOTIFY_HTTP_H */
