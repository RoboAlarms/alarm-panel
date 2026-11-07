/*
 * web_readouts.h - two read-only answers for the app (through the server link) and the Panel
 * Manager: the panel's sirens and outputs (GET outputs) and the last backup (GET backup), the
 * system master's to read without installer mode. Nothing here changes anything. Portable (cJSON):
 * host test test_web_readouts.c.
 */
#ifndef WEB_READOUTS_H
#define WEB_READOUTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_types.h"
#include "cJSON.h"

typedef struct {
    bool used;
    uint8_t number;    /* 1-based within its kind */
    const char *kind;  /* web_output_kind() for a wired one, "rf_siren" */
    const char *name;  /* as typed, may be "" */
    bool on;
    bool trouble;
} web_output_t;

/* A wired output's function (out_wired.h ow_function_t) as the answer names it: "siren",
 * "strobe", "smoke_reset", "armed", "automation"; NULL for a free channel. */
const char *web_output_kind(uint8_t function);
/* {"outputs":[{number, kind, name, on, trouble}, ...]}, the used ones in order. NULL: no memory. */
cJSON *web_outputs_json(const web_output_t *outs, size_t n);
/* {"last_at": ISO 8601 UTC or null, "by": user number or null, "by_name": name or null,
 * "size": bytes or null}: have false, all null (no backup made since the record began). */
cJSON *web_backup_json(bool have, uint64_t at_s, uint8_t user, const char *user_name,
                       uint32_t size);

/* What the app finds the panel by on the home network (mDNS _roboalarms-app._tcp, port 443, while the
 * Panel Manager is on, all the time as HomeKit and Matter devices do: a paired phone finds the panel
 * again by its id after the router gives it a new address): id (the panel id its pairing QR code
 * carries), name and, when it is linked, srv - the linked server's base URL. No firmware version and
 * no model (decided by the owner 2026-11-06): a version tells an attacker which known bugs to try.
 * It grants nothing: pairing still needs the code shown at the panel (alarm-server direct-mode.md
 * DIR-001). */
#define WEB_APP_TXT_ITEMS 3
#define WEB_APP_TXT_VALUE 80
typedef struct {
    const char *key[WEB_APP_TXT_ITEMS];
    char value[WEB_APP_TXT_ITEMS][WEB_APP_TXT_VALUE];
    unsigned n;
} web_app_txt_t;
/* host NULL or "": not linked, no srv. port 0 or 443 leaves the port out. */
void web_app_txt(web_app_txt_t *out, const char *id, const char *name, const char *host,
                 uint16_t port);

/* Panic from the app (WEB-032): the types the installer allows (alarm_options_t.app_panic_mask,
 * 1 << alarm_panic_t) as names, police, fire, medical, silent in that order, the ones this build
 * doesn't know left out. NULL: no memory. */
cJSON *web_app_panic_json(uint8_t mask);
/* A request's "type": false when it names no panic type. */
bool web_app_panic_type(const char *name, alarm_panic_t *out);
bool web_app_panic_allowed(uint8_t mask, alarm_panic_t type);

#endif
