/* web_net_confirm.c - see web_net_confirm.h. */
#include "web_net_confirm.h"

#include <string.h>

bool web_net_confirm_method_ok(const char *method)
{
    return method && !strcmp(method, "POST");
}

bool web_net_confirm_operation_ok(const char *op)
{
    return op && strlen(op) == 32 && strspn(op, "0123456789abcdef") == 32;
}

/* Every byte looked at, so the time doesn't say how much of a guess was right. */
static bool same32(const char *a, const char *b)
{
    unsigned char d = 0;
    for (size_t i = 0; i < 32; i++)
        d |= (unsigned char)((unsigned char)a[i] ^ (unsigned char)b[i]);
    return d == 0;
}

web_net_confirm_t web_net_confirm(web_net_confirm_memo_t *memo, const char *method, const char *op,
                                  int64_t now_us, web_net_confirm_fn confirm, void *ctx)
{
    if (!web_net_confirm_method_ok(method))
        return WEB_NET_CONFIRM_BAD_METHOD;
    if (!web_net_confirm_operation_ok(op))
        return WEB_NET_CONFIRM_BAD_OPERATION;
    if (confirm && confirm(op, ctx)) {
        memcpy(memo->op, op, 33);
        memo->at_us = now_us;
        return WEB_NET_CONFIRM_OK;
    }
    /* Asked again after it confirmed: the trial is over and kept, so the same answer. */
    if (memo->op[0] && now_us >= memo->at_us && now_us - memo->at_us < WEB_NET_CONFIRM_MEMO_US &&
        same32(memo->op, op))
        return WEB_NET_CONFIRM_OK;
    return WEB_NET_CONFIRM_NOT_CONFIRMED;
}

const char *web_net_confirm_status(web_net_confirm_t r)
{
    switch (r) {
    case WEB_NET_CONFIRM_OK:
        return "200 OK";
    case WEB_NET_CONFIRM_NOT_CONFIRMED:
        return "409 Conflict";
    case WEB_NET_CONFIRM_BAD_OPERATION:
        return "400 Bad Request";
    default:
        return "405 Method Not Allowed";
    }
}

const char *web_net_confirm_error(web_net_confirm_t r)
{
    switch (r) {
    case WEB_NET_CONFIRM_NOT_CONFIRMED:
        return "not_confirmed";
    case WEB_NET_CONFIRM_BAD_OPERATION:
        return "bad_operation";
    default:
        return NULL;
    }
}
