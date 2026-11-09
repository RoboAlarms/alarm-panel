/*
 * web_thispanel.h - Setup's "This panel" and "Sensors and devices" pages through the server link
 * (alarm-server's website, 2026-11-09: everything a Setup page shows changes from the server once the
 * panel is connected): a backup handed over in parts and a restore taken in parts (the link carries
 * at most 48 KB out and 4 KB in per message), the sirens and outputs as settings, and Smart Home's
 * Owner-code devices. Portable (cJSON, sirens, hc_home): host test test_web_thispanel.c. The
 * handlers in panel_web.c hold the files and call the services.
 */
#ifndef WEB_THISPANEL_H
#define WEB_THISPANEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "home_control/hc_home.h"
#include "sirens/out_wired.h"
#include "sirens/rf_siren.h"

/* ---- files in parts ---- */

/* The most of a backup one answer carries (base64 makes it 40 000 characters, under the link's
 * 48 KB), and the most of a restore one request may carry (its 4 KB message). */
#define WEB_PART_OUT 30000u
#define WEB_PART_IN  2048u

/* Standard base64 with padding. Encode: the characters written (out holds 4*ceil(n/3)+1). Decode:
 * the bytes written, or -1 when it isn't base64 or doesn't fit in cap. */
size_t web_b64_encode(const uint8_t *in, size_t n, char *out);
int web_b64_decode(const char *in, uint8_t *out, size_t cap);

/* {"offset": off, "data": base64 of up to WEB_PART_OUT bytes from off}. NULL: off past the end
 * (or at it), or no memory. */
cJSON *web_part_json(const uint8_t *file, size_t len, size_t off);

/* A restore's next part: data (base64) appended at off, which must be what is held so far, without
 * going past size. NULL when taken (*have moved on), else why not: "backup_format" (out of order,
 * too long, not base64, more than declared). */
const char *web_part_take(uint8_t *buf, size_t size, size_t *have, size_t off, const char *data);

/* ---- sirens and outputs ---- */

/* One wired output's settings: what Setup › Sirens and outputs shows for it. */
typedef struct {
    ow_config_t c; /* function OW_FN_NONE: free */
    bool on;
    bool trouble;
} web_wired_t;

typedef struct {
    rfs_config_t c; /* used false: free */
    bool sounding;
} web_rf_t;

/* {"outputs":[{number, kind, name, on, trouble, partitions, also, burglary, fire, squawk,
 * terminal, active_low, sense} for wired, {number, kind:"rf_siren", ...} for 433 MHz],
 * "terminals":[gpio...]}: the used ones in order, and the terminals a new one may take. */
cJSON *web_sirens_json(const web_wired_t *w, size_t nw, const web_rf_t *r, size_t nr,
                       const uint8_t *free_terminals, size_t nt);

/* What a POST sirens asks for. */
typedef enum {
    WEB_SIREN_BAD = 0,
    WEB_SIREN_TEST,  /* kind + number */
    WEB_SIREN_SET,   /* kind + number, the fields given changing *wired or *rf (start: its now) */
    WEB_SIREN_CLEAR, /* kind + number */
} web_siren_op_t;

/* Reads {action:"test"|"set"|"remove", kind:"siren"|...|"rf_siren", number, fields...}. *rf_out:
 * a 433 MHz siren. *slot: 0-based. For SET, wired/rf hold the output's settings now and get the
 * fields the request names: function (by kind), partitions, also, burglary, fire, squawk, terminal,
 * active_low, sense, name. */
web_siren_op_t web_siren_request(const cJSON *o, bool *rf_out, size_t *slot, ow_config_t *wired,
                                 rfs_config_t *rf);

/* ---- Smart Home ---- */

typedef struct {
    const char *ref;  /* the registry's key */
    const char *name;
    const char *area; /* its room as the screens show it, may be "" */
} web_home_dev_t;

/* {"arrange_code": bool, "devices":[{ref, name, room, code}]}. */
cJSON *web_home_json(const hc_home_t *h, const web_home_dev_t *devs, size_t n);

/* Applies {"arrange_code"?: bool, "devices"?: [{ref, code}]} to h. False: nothing it could take
 * (no known field, a device not in devs, or the table full); h is then unchanged. */
bool web_home_apply(hc_home_t *h, const cJSON *o, const web_home_dev_t *devs, size_t n);

#endif
