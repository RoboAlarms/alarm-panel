/*
 * backup_policy.c - which NVS entries a backup holds (docs/features/14 CFG-005), as data.
 *
 * A namespace missing from the table is never backed up and never touched by a restore: that
 * is where the panel's identity lives (its certificates and keys: "halink", "panelweb"; the
 * developer enrollment: "devrem", "devota"), and whatever is only about this board or this
 * moment (the event log's counters "log", a network change on trial "nettrial", the touch
 * calibration of one screen "touch", ESP-IDF's own). Within a namespace, `only` narrows it to
 * the keys listed and `except` leaves the keys listed with the board.
 *
 * Add a namespace here when a service stores something an installer would want back on a
 * replacement board, and say why in its row.
 */
#include "backup/backup.h"

#include <string.h>

#include "alarm_proto/proto_fwsrv.h"

typedef struct {
    const char *ns;
    const char *const *only;   /* NULL: every key but `except` */
    const char *const *except; /* NULL: none */
} rule_t;

static const char *const ALARM_ONLY[] = {"config", NULL}; /* not "rt": arming, bypass, memory */
static const char *const NET_EXCEPT[] = {"previous", NULL}; /* a change on trial's saved profile */
static const char *const MQTT_EXCEPT[] = {
    "id",     /* the panel ID: the new board keeps its own */
    "did",    /* discovery as published under that ID */
    "dmap",
    "zwdisc",
    "acked",  /* the event log position the broker has: this board's log, not the file's */
    NULL,
};
static const char *const NOTIFY_EXCEPT[] = {"ackedn", "ackedp", NULL}; /* log positions, too */
/* The update server and its channel (docs/features/13 OTA-020), not an update in flight, its
 * attempt records or the developer enrollment. "trust" holds the server and the signing key the
 * installer accepted for it; a restore keeps the server and drops the key (backup_restore_entry). */
static const char *const OTA_ONLY[] = {"trust", "chan", NULL};

static const rule_t RULES[] = {
    {"alarm", ALARM_ONLY, NULL},     /* the configuration: options, partitions, zones, users */
    {"zones", NULL, NULL},           /* which sensor feeds each zone */
    {"net", NULL, NET_EXCEPT},       /* Wi-Fi, addresses, time zone, time server */
    {"ui", NULL, NULL},              /* display and sound */
    {"sys", NULL, NULL},             /* the setup is done */
    {"mqtt", NULL, MQTT_EXCEPT},     /* the broker, its account and certificate authority */
    {"notify", NULL, NOTIFY_EXCEPT}, /* phone notifications: what and when */
    {"ntfy", NULL, NULL},
    {"pushover", NULL, NULL},
    {"weblogin", NULL, NULL},        /* the people's browser sign-ins */
    {"zwave", NULL, NULL},           /* the network keys, node cache, SmartStart list, radio */
    /* Zigbee (docs/features/20 devices.md §13, ZB-020): the panel's own record of its devices -
     * registry, install codes, channel, siren and keypad assignments - whole. A restore is for a
     * replacement board.
     * TODO(BL-036 step 5): the Zigbee stack's own namespace (network key, PAN, its device table,
     * frame counter) joins once its name is recorded on the bench; its restore must raise the
     * network frame counter well past the backup's, and with it each IAS device's setup is to be
     * set STOPPED at CIE_WRITE so it is enrolled again with the new board's address (planned, not
     * built: until then the restored devices are not in the new network and are added again). */
    {"zbpanel", NULL, NULL},
    {"rf", NULL, NULL},              /* the 433 MHz receiver's place and settings */
    {"clock", NULL, NULL},           /* where the clock module is */
    {"automations", NULL, NULL},     /* what the house does by itself (docs/features/41 AUT-008):
                                      * household choices, not identity; a restored device that
                                      * isn't listed yet shows as missing until its source lists it */
    {"ota", OTA_ONLY, NULL},
};

static bool listed(const char *const *list, const char *key)
{
    for (; list != NULL && *list != NULL; list++) {
        if (strcmp(*list, key) == 0) {
            return true;
        }
    }
    return false;
}

bool backup_policy_keeps(const char *ns, const char *key)
{
    if (ns == NULL || key == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(RULES) / sizeof(RULES[0]); i++) {
        if (strcmp(RULES[i].ns, ns) != 0) {
            continue;
        }
        if (RULES[i].only != NULL) {
            return listed(RULES[i].only, key);
        }
        return !listed(RULES[i].except, key);
    }
    return false;
}

const char *const *backup_policy_namespaces(void)
{
    static const char *const NAMES[] = {"alarm", "zones", "net",      "ui",    "sys",
                                        "mqtt",  "notify", "ntfy",    "pushover", "weblogin",
                                        "zwave", "zbpanel", "rf",     "clock", "automations",
                                        "ota",   NULL};
    _Static_assert(sizeof(NAMES) / sizeof(NAMES[0]) == sizeof(RULES) / sizeof(RULES[0]) + 1u,
                   "every rule's namespace is listed");
    return NAMES;
}

/* Entries older firmware kept and this one doesn't: still accepted in a file, never written. */
static const struct {
    const char *ns;
    const char *key;
} RETIRED[] = {
    {"ota", "src"}, /* the old manifest source, before the trust record (BL-186): not read */
};

bool backup_policy_retired(const char *ns, const char *key)
{
    if (ns == NULL || key == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(RETIRED) / sizeof(RETIRED[0]); i++) {
        if (strcmp(RETIRED[i].ns, ns) == 0 && strcmp(RETIRED[i].key, key) == 0) {
            return true;
        }
    }
    return false;
}

backup_result_t backup_restore_entry(const backup_entry_t *e, uint8_t *buf, size_t cap,
                                     backup_entry_t *out, bool *write)
{
    if (e == NULL || out == NULL || write == NULL) {
        return BACKUP_ERR_ARG;
    }
    *out = *e;
    *write = false;
    if (backup_policy_retired(e->ns, e->key)) {
        return BACKUP_OK;
    }
    if (strcmp(e->ns, "ota") != 0 || strcmp(e->key, "trust") != 0) {
        *write = true;
        return BACKUP_OK;
    }
    /* The update server: a key the installer accepted on another panel (or one a file made
     * anywhere names) is never trusted here without its fingerprint being confirmed (OTA-020,
     * OTA-021); the server and the channel come back. */
    proto_fwsrv_trust_t t;
    if (e->type != BACKUP_T_BLOB || !proto_fwsrv_trust_decode(e->value, e->len, &t)) {
        return BACKUP_OK; /* damaged, or a later version's: the panel's own server */
    }
    if (!t.has_key) {
        *write = true;
        return BACKUP_OK;
    }
    if (buf == NULL) {
        return BACKUP_ERR_ARG;
    }
    t.has_key = false;
    memset(t.key_name, 0, sizeof(t.key_name));
    memset(t.key_sha256, 0, sizeof(t.key_sha256));
    const size_t n = proto_fwsrv_trust_encode(&t, buf, cap);
    if (n == 0u) {
        return BACKUP_ERR_ARG; /* buf too small: it decoded, so it encodes */
    }
    out->value = buf;
    out->len = (uint32_t)n;
    *write = true;
    return BACKUP_OK;
}
