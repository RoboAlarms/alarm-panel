/*
 * web_net_confirm.h - POST network-confirm through the server link (docs/features/43): keeping a
 * Wi-Fi or address change that alarm-server started through the tunnel. The server retries from
 * about 5 s to 115 s into the two-minute trial, so the answer must be safe to repeat: a token that
 * confirmed stays confirmed (200 again) for WEB_NET_CONFIRM_MEMO_US, in case the first 200 was lost
 * on the way back. On the home network the same path is continuity() in panel_web.c.
 * Portable: time and the confirmation come in as arguments (host test: test_web_net_confirm.c).
 * HTTP worker only (callers hold panel_web's serve lock).
 */
#ifndef WEB_NET_CONFIRM_H
#define WEB_NET_CONFIRM_H

#include <stdbool.h>
#include <stdint.h>

#define WEB_NET_CONFIRM_MEMO_US (130LL * 1000000) /* longer than a trial (120 s) */

typedef enum {
    WEB_NET_CONFIRM_OK,            /* 200 {"ok":true} */
    WEB_NET_CONFIRM_NOT_CONFIRMED, /* 409 {"error":"not_confirmed"} */
    WEB_NET_CONFIRM_BAD_OPERATION, /* 400 {"error":"bad_operation"} */
    WEB_NET_CONFIRM_BAD_METHOD,    /* 405 */
} web_net_confirm_t;

typedef struct {
    char op[33]; /* the last operation that confirmed, "" for none */
    int64_t at_us;
} web_net_confirm_memo_t;

/* What net_wifi_trial_confirm() does on the panel. */
typedef bool (*web_net_confirm_fn)(const char *op, void *ctx);

/* Only POST. */
bool web_net_confirm_method_ok(const char *method);
/* Exactly 32 lowercase hex digits, as the panel makes and the server sends (trial_begin). */
bool web_net_confirm_operation_ok(const char *op);
/* The whole decision for one request; confirm is called only for a well-formed operation. */
web_net_confirm_t web_net_confirm(web_net_confirm_memo_t *memo, const char *method, const char *op,
                                  int64_t now_us, web_net_confirm_fn confirm, void *ctx);
/* Its HTTP status line and its "error" word (NULL for OK and for 405, which has no body). */
const char *web_net_confirm_status(web_net_confirm_t r);
const char *web_net_confirm_error(web_net_confirm_t r);

#endif
