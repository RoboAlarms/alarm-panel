/*
 * ui_remote.h - the touch UI's backend over a byte channel (docs/features/11 "The master's settings
 * and installer menu on keypads", LINK-057..LINK-065; BL-223).
 *
 * A keypad's screen calls the same ui_backend_t entries as an alarm panel's. For the alarm panel's
 * own pages (users, integrations, automations, Smart Home's settings, the installer menu) a keypad's
 * entries are proxies (ui_remote_proxy): each one encodes its arguments, hands them to a port that
 * carries them to the alarm panel, and decodes the answer. The alarm panel runs the call on its own
 * ui_backend_t (ui_remote_serve), the backend its own screen and its Panel Manager use, under the
 * grant it holds for that keypad: it is the only judge.
 *
 * The call ids (ui_remote_call_t) and every call's arguments and answer are an external contract
 * between firmware versions, frozen in test/ui_remote_fixture_v1.h: never renumber a call, never
 * reorder or remove a field. Each struct crosses as a record (u16 length, then its fields in its
 * table's order, little endian); a reader takes the fields it knows and reads missing ones as zero,
 * so fields are only appended. Strings carry their length and are cleaned before they are shown.
 *
 * Portable C11: no heap, no globals. The channel (the panel link, a test) is the caller's.
 */
#ifndef UI_REMOTE_H
#define UI_REMOTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui_backend/ui_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The calls (LINK-058). Grouped by area; a group's ids are its own block of 16. Never renumbered. */
typedef enum {
    UR_CALL_NONE = 0x0000,
    /* Session (0x0001-0x000F) */
    UR_GRANT_END = 0x0001,         /* the person left the alarm panel's pages */
    UR_PROGRAM_ENTER = 0x0002,     /* code -> result, user */
    UR_PROGRAM_EXIT = 0x0003,
    UR_PROGRAM_KEEPALIVE = 0x0004,
    UR_PROGRAM_SET_CODE = 0x0005,  /* user, new code -> result */
    /* Configuration (0x0010-0x001F) */
    UR_SET_CONFIG = 0x0010,        /* base cfg_crc, blob -> result */
    /* Users (0x0020-0x002F) */
    UR_USER_SET = 0x0020,
    UR_USER_UPDATE = 0x0021,
    UR_USER_DELETE = 0x0022,
    UR_WEB_LOGIN_GET = 0x0023,
    UR_WEB_LOGIN_SET = 0x0024,
    UR_WEB_LOGIN_CLEAR = 0x0025,
    /* Zones' sensors (0x0030-0x003F) */
    UR_ZONE_SOURCES = 0x0030,
    UR_ZONE_LEARN = 0x0031,
    UR_ZONE_CANDIDATES = 0x0032,
    UR_ZONE_BIND = 0x0033,
    UR_ZONE_UNBIND = 0x0034,
    UR_ZONE_INFO = 0x0035,
    /* Z-Wave (0x0040-0x004F) */
    UR_ZW_STATUS = 0x0040,
    UR_ZW_DEVICES = 0x0041,
    UR_ZW_DEVICE = 0x0042,
    UR_ZW_REQUEST = 0x0043,
    UR_ZW_PROGRESS = 0x0044,
    UR_ZW_PROV = 0x0045,
    UR_ZW_PROV_REGISTER = 0x0046,
    UR_ZW_PROV_CHANGE = 0x0047,
    UR_ZW_RADIO_SD_INSTALL = 0x0048,
    UR_ZW_RADIO_SD_STATE = 0x0049,
    /* Zigbee (0x0050-0x005F) */
    UR_ZB_STATUS = 0x0050,
    UR_ZB_DEVICES = 0x0051,
    UR_ZB_DEVICE = 0x0052,
    UR_ZB_REQUEST = 0x0053,
    UR_ZB_PROGRESS = 0x0054,
    UR_ZB_CODES = 0x0055,
    /* Sirens, outputs and 433 MHz (0x0060-0x006F) */
    UR_OUT_STATUS = 0x0060,
    UR_OUT_WIRED = 0x0061,
    UR_OUT_WIRED_SET = 0x0062,
    UR_OUT_WIRED_TEST = 0x0063,
    UR_OUT_TERMINALS = 0x0064,
    UR_OUT_RF = 0x0065,
    UR_OUT_RF_ADD = 0x0066,
    UR_OUT_RF_SET = 0x0067,
    UR_OUT_RF_CLEAR = 0x0068,
    UR_OUT_RF_PAIR = 0x0069,
    UR_OUT_RF_TEST = 0x006A,
} ui_remote_call_t;

/* What a call needs (the alarm panel checks it against its grant for the keypad; docs/features/14
 * "Who can change what", as ui_settings_allowed() on its own screen). */
typedef enum {
    UR_ACCESS_SESSION = 0, /* only a live session (the session group judges its own) */
    UR_ACCESS_USERS,       /* a system or partition master, or the installer */
    UR_ACCESS_SHARED,      /* a master or the installer, or installer mode */
    UR_ACCESS_SYSTEM,      /* the system master or the installer, or installer mode */
    UR_ACCESS_INSTALLER,   /* installer mode, held by this keypad */
} ui_remote_access_t;

/* Groups, as RB_GRANT's areas say which ones an alarm panel serves (LINK-065): bit = id >> 4. */
#define UR_GROUP(call) ((unsigned)(call) >> 4)
#define UR_AREA_SESSION (1u << 0)
#define UR_AREA_CONFIG  (1u << 1)
#define UR_AREA_USERS   (1u << 2)
#define UR_AREA_ZONES   (1u << 3)
#define UR_AREA_ZWAVE   (1u << 4)
#define UR_AREA_ZIGBEE  (1u << 5)
#define UR_AREA_OUTPUTS (1u << 6)

/* The answer's status (RB_ANSWER status, LINK-058). Never renumbered. */
typedef enum {
    UR_OK = 0,
    UR_UNAVAILABLE = 1,    /* an unknown call, or one this alarm panel doesn't serve */
    UR_NOT_AUTHORIZED = 2, /* no grant for it */
    UR_NEED_INSTALLER = 3, /* installer mode isn't this keypad's */
    UR_BUSY = 4,           /* installer mode is held elsewhere, or a call is already running */
    UR_LOCKED_OUT = 5,
    UR_STALE = 6,          /* the configuration changed since the keypad read it */
    UR_TOO_BIG = 7,
    UR_GONE = 8,           /* the answer is no longer kept: call again */
    UR_BAD = 9,            /* arguments that don't decode */
    UR_NO_ANSWER = 10,     /* the keypad's own: no answer came (link down, timed out) */
} ui_remote_status_t;

typedef struct {
    ui_remote_call_t call;
    ui_remote_access_t need;
    bool read;   /* answered from the keypad's copy at once, asked again in the background */
    bool secret; /* its arguments hold a code or a password: the channel wipes them after use
                  * and never keeps them (its answer cache keeps the answer only) */
} ui_remote_info_t;

/* The call's facts; NULL for an id this firmware doesn't know. */
const ui_remote_info_t *ui_remote_info(uint16_t call);

/* Largest arguments and answer this firmware sends (the configuration blob is the largest). */
#define UR_ARGS_MAX   (ALARM_CONFIG_BLOB_MAX + 16u)
#define UR_ANSWER_MAX 12288u

/* ---- the alarm panel's side ---- */

/* What the alarm panel holds for one keypad (LINK-059). Zero = nothing. */
typedef struct {
    uint8_t user;        /* whose code was accepted, 0 = none */
    uint8_t authority;   /* theirs (alarm_authority_t) when it was accepted */
    bool installer;      /* this keypad entered installer mode */
    uint8_t learning;    /* a zone source kind + 1 this keypad listens on (LINK-062), 0 = none */
    uint64_t last_ms;    /* the last call under it */
} ui_remote_grant_t;

#define UR_GRANT_IDLE_MS (5u * 60u * 1000u)

/* A code the alarm panel accepted for this keypad (its code check, CMD op 2). */
void ui_remote_grant_user(ui_remote_grant_t *g, uint8_t user, uint8_t authority, uint64_t now_ms);
/* Ends what is idle (UR_GRANT_IDLE_MS without a call; installer mode keeps it while held) and
 * what installer mode ended elsewhere took with it. Stops a learn mode the grant no longer
 * covers on be. Call it about once a second, and with gone = true once the keypad's session has
 * been lost for 60 s: the grant ends and so does this keypad's installer mode (LINK-060). */
void ui_remote_grant_tick(const ui_backend_t *be, ui_remote_grant_t *g, bool gone, uint64_t now_ms);

/* The alarm panel's scratch for serving: the caller's (PSRAM is fine). One per alarm panel: the
 * calls of all its keypads run one at a time. */
typedef struct {
    alarm_snapshot_t snap;
    alarm_config_t cfg, mine;
    union {
        ui_zone_source_t sources[16];
        zsrc_candidate_t candidates[32];
        zone_hub_info_t info;
        ui_zw_status_t zw_status;
        ui_zw_device_t zw_devices[UI_ZW_DEVICES_MAX];
        ui_zw_device_page_t zw_page;
        ui_zw_progress_t zw_progress;
        ui_zw_prov_t zw_prov[UI_ZW_PROV_MAX];
        ui_zb_status_t zb_status;
        ui_zb_device_t zb_devices[UI_ZB_DEVICES_MAX];
        ui_zb_device_page_t zb_page;
        ui_zb_progress_t zb_progress;
        ui_zb_code_t zb_codes[UI_ZB_CODES_MAX];
        ui_out_status_t out_status;
        ui_out_wired_t out_wired[UI_OUT_WIRED_MAX];
        ui_out_rf_t out_rf[UI_OUT_RF_MAX];
        uint8_t terminals[64];
    } u;
} ui_remote_server_t;

/* What the alarm panel can serve with be (UR_AREA_*; RB_GRANT's areas, LINK-065). */
uint32_t ui_remote_areas(const ui_backend_t *be);

/* Runs one call on be for a keypad: decodes args, checks the call's need against *g, runs it and
 * encodes the answer into out (cap bytes, *out_len). Returns the status. cfg_crc is the alarm
 * panel's keypad configuration CRC now (UR_SET_CONFIG is refused STALE when the keypad's base
 * differs); same_fw says the keypad runs this firmware's version (a configuration from another
 * version could lose fields it doesn't know: refused UR_UNAVAILABLE). Never a factory reset, an
 * update, developer access, or the alarm panel's own network, time or Screen and sound: those are
 * no calls. */
ui_remote_status_t ui_remote_serve(ui_remote_server_t *s, const ui_backend_t *be,
                                   ui_remote_grant_t *g, uint32_t cfg_crc, bool same_fw,
                                   uint16_t call, const uint8_t *args, size_t args_len,
                                   uint8_t *out, size_t cap, size_t *out_len, uint64_t now_ms);

/* ---- the keypad's side ---- */

/* The channel: sends a call and gives its answer (into ans, cap bytes, *ans_len). A read may be
 * answered from the keypad's copy. Runs on whatever task the backend entry was called on. */
typedef struct {
    void *ctx;
    ui_remote_status_t (*call)(void *ctx, uint16_t call, const uint8_t *args, size_t len,
                               uint8_t *ans, size_t cap, size_t *ans_len);
    /* The cfg_crc of the keypad's configuration copy (UR_SET_CONFIG's base). */
    uint32_t (*cfg_crc)(void *ctx);
} ui_remote_port_t;

/* The proxy's scratch: the caller's (PSRAM is fine). Its calls run one at a time (LINK-054's
 * queue on a keypad). */
typedef struct {
    ui_remote_port_t port;
    void *inner;             /* be->ctx before ui_remote_proxy: the other entries' context */
    ui_remote_status_t last; /* the last call's status (the screen's "Not confirmed") */
    char out_where[48];      /* out_status's where: valid until the next out_status */
    uint8_t args[UR_ARGS_MAX];
    uint8_t ans[UR_ANSWER_MAX];
} ui_remote_proxy_t;

/* Fills the entries of the groups in areas (UR_AREA_*) with proxies over p's port and leaves every
 * other entry as it is. The proxies find p through the backend's context, so be->ctx becomes p
 * and the entries that are not proxied get p as their context too: an entry that uses its context
 * takes the old one back with ui_remote_inner(ctx). */
void ui_remote_proxy(ui_backend_t *be, ui_remote_proxy_t *p, uint32_t areas);
void *ui_remote_inner(void *ctx);

/* ---- the codec, for the tests and the frozen fixture ---- */
size_t ui_remote_enc_binding(const zone_hub_binding_t *b, uint8_t *out, size_t cap);
bool ui_remote_dec_binding(const uint8_t *p, size_t len, zone_hub_binding_t *b);

#ifdef __cplusplus
}
#endif

#endif /* UI_REMOTE_H */
