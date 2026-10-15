/*
 * zbz_ace.h - Zigbee keypads and key fobs arming and disarming the panel (docs/features/20
 * alarm-devices.md §3; ZB-024, BL-148): the panel is the IAS ACE server (cluster 0x0501), each
 * assigned keypad or fob its client.
 *
 * The rules that make it safe:
 *   - Codes are always the panel's to check, by the engine, in the alarm task. A keypad's Arm is
 *     turned into an alarm_cmd_t with the code it carried, posted (zba_post_t) and answered with
 *     the engine's result (zba_result) - nothing here compares a code, keeps one or logs one; the
 *     owner wipes the posted copy. Stack callbacks never reach this: the Zigbee task does, and
 *     never calls the engine.
 *   - A keypad is ALARM_SRC_KEYPAD with its engine device number as id, so the engine's lockout
 *     (five wrong codes) applies to it like to any keypad; a locked-out Arm is "invalid code".
 *     The number is the panel's to give, never typed (ZBA_DEVICE_FIRST..ZBA_DEVICE_LAST, below
 *     the Z-Wave keypads' top eight: alarm-devices.md §4), and it is only a source id: nothing
 *     here writes the engine's device table (alarm_config_t.devices[]), as Z-Wave's keypads
 *     don't (zw_keyctl.h ZWKC_DEVICE).
 *   - A key fob carries no code: it acts as the user it belongs to (alarm_engine_command_as, the
 *     same rules as that user's code), arms the partition it was given, and disarms only when the
 *     installer turned may_disarm on for it (off by default); its panic button only when `panic`
 *     is on (off by default). Its source is alarm_types.h's "keyfob button" (ZBA_FOB_SOURCE_*),
 *     never a keypad id.
 *   - Only assigned devices are heard: an ACE frame from any other device gets a Default Response
 *     NOT_AUTHORIZED and nothing else.
 *   - A retransmission (the same device, sequence number and command - an Arm: and its arm mode -
 *     within ZBA_RETRY_WINDOW_MS) is not run again: its first answer is sent again (fobs repeat an
 *     unanswered Arm).
 *
 * Mapping (alarm-devices.md §3): arm modes 0 disarm, 1 day/home -> STAY, 2 night -> NIGHT,
 * 3 all zones -> AWAY. Arm notifications 0 all disarmed, 1 day armed, 2 night armed, 3 all armed,
 * 4 invalid code (also: locked out, not authorized), 5 not ready, 6 already disarmed. Panel status
 * 0 disarmed, 1 armed stay, 2 armed night, 3 armed away, 4 exit delay, 5 entry delay, 6 not ready,
 * 7 in alarm, 8..10 arming stay/night/away; delays carry the seconds left and audible 1; in alarm
 * the alarm status (1 burglar, 2 fire, 3 emergency, 4 police panic, 5 fire panic, 6 emergency
 * panic). Bypass and the zone-map questions are answered, never ignored: an empty map, and a
 * Bypass Response "not allowed" for every zone asked (bypassing stays the panel's own keypads').
 *
 * Golden (testing.md §3): Arm 01 57 00 | 00 06 31 32 33 34 35 36 00 is disarm, code 123456, zone
 * 0; Arm Response all armed 19 57 00 | 03; invalid code 19 57 00 | 04; Panel Status Changed exit
 * delay 30 s with sound 19 58 04 | 04 1E 01 00; Get Panel Status 01 57 07; its answer in alarm,
 * emergency 19 57 05 | 07 00 01 03. Every frame the panel sends as the ACE server asks for no
 * Default Response (frame control 0x19, decided in ZB-M11: alarm-devices.md §3 "Frames"); the
 * rows testing.md first wrote with 0x09 are these.
 *
 * Built in src/zbz_ace.c (ZB-M11), tested by test/test_zbz_ace.c with a frozen v1 blob
 * (test/ace_fixture_v1.h). zbz_panel calls it: zbz_panel_partition -> zba_panel,
 * zbz_panel_ace_result -> zba_result, the radio's ACE frames -> zba_receive, its poll ->
 * zba_poll. Portable C11, caller-owned state, no heap. The owner serializes calls (the Zigbee
 * task).
 */
#ifndef ZBZ_ACE_H
#define ZBZ_ACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_types.h"
#include "zigbee_zones/zbz_zcl.h"
#include "zone_hub/zsrc_zigbee.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Client to server (the keypad to the panel). */
#define ZBA_CMD_ARM               0x00u
#define ZBA_CMD_BYPASS            0x01u
#define ZBA_CMD_EMERGENCY         0x02u
#define ZBA_CMD_FIRE              0x03u
#define ZBA_CMD_PANIC             0x04u
#define ZBA_CMD_GET_ZONE_ID_MAP   0x05u
#define ZBA_CMD_GET_ZONE_INFO     0x06u
#define ZBA_CMD_GET_PANEL_STATUS  0x07u
#define ZBA_CMD_GET_BYPASSED      0x08u
#define ZBA_CMD_GET_ZONE_STATUS   0x09u
/* Server to client (the panel to the keypad). */
#define ZBA_RSP_ARM               0x00u
#define ZBA_RSP_ZONE_ID_MAP       0x01u
#define ZBA_RSP_ZONE_INFO         0x02u
#define ZBA_RSP_ZONE_STATUS_CHANGED 0x03u
#define ZBA_RSP_PANEL_STATUS_CHANGED 0x04u
#define ZBA_RSP_PANEL_STATUS      0x05u
#define ZBA_RSP_SET_BYPASSED      0x06u
#define ZBA_RSP_BYPASS            0x07u
#define ZBA_RSP_ZONE_STATUS       0x08u

/* Arm modes. */
#define ZBA_ARM_DISARM 0u
#define ZBA_ARM_DAY    1u
#define ZBA_ARM_NIGHT  2u
#define ZBA_ARM_ALL    3u
/* Arm notifications. */
#define ZBA_NOTE_DISARMED         0u
#define ZBA_NOTE_DAY_ARMED        1u
#define ZBA_NOTE_NIGHT_ARMED      2u
#define ZBA_NOTE_ALL_ARMED        3u
#define ZBA_NOTE_INVALID_CODE     4u
#define ZBA_NOTE_NOT_READY        5u
#define ZBA_NOTE_ALREADY_DISARMED 6u
/* Panel status. */
#define ZBA_PS_DISARMED     0u
#define ZBA_PS_ARMED_STAY   1u
#define ZBA_PS_ARMED_NIGHT  2u
#define ZBA_PS_ARMED_AWAY   3u
#define ZBA_PS_EXIT_DELAY   4u
#define ZBA_PS_ENTRY_DELAY  5u
#define ZBA_PS_NOT_READY    6u
#define ZBA_PS_IN_ALARM     7u
#define ZBA_PS_ARMING_STAY  8u
#define ZBA_PS_ARMING_NIGHT 9u
#define ZBA_PS_ARMING_AWAY  10u
#define ZBA_PS_ARMED_80     0x80u /* ZBZ_MODEL_ACE_ARMED_80's "armed" */
/* Audible notification and alarm status. */
#define ZBA_AUDIBLE_MUTE    0u
#define ZBA_AUDIBLE_DEFAULT 1u
#define ZBA_ALARM_NONE            0u
#define ZBA_ALARM_BURGLAR         1u
#define ZBA_ALARM_FIRE            2u
#define ZBA_ALARM_EMERGENCY       3u
#define ZBA_ALARM_POLICE_PANIC    4u
#define ZBA_ALARM_FIRE_PANIC      5u
#define ZBA_ALARM_EMERGENCY_PANIC 6u
/* Bypass results. */
#define ZBA_BYPASS_NOT_ALLOWED 0x02u

/* A keypad's device number (alarm-devices.md §4; decided 2026-10-02 without a review).
 * The engine has ALARM_MAX_DEVICES (16) of them. The top ZBA_ZWAVE_DEVICES are the Z-Wave
 * keypads' (zw_keyctl.h ZWKC_SLOTS and ZWKC_DEVICE: 9..16), never given here. The rest,
 * ZBA_DEVICE_FIRST..ZBA_DEVICE_LAST (1..8), are shared with the panel-link keypads
 * (docs/features/11 LINK-001) and with any number the installer turned on in the engine's own
 * device table: the owner says which of them are taken (zbz_panel_port_t.devices_taken), and a
 * keypad being assigned gets the lowest free one (zba_free_device) - none free: the page's
 * UI_ZB_FIT_NO_DEVICE_SLOT, the request's FULL. Sirens and fobs take none. */
#define ZBA_ZWAVE_DEVICES   8u
#define ZBA_DEVICE_FIRST    1u
#define ZBA_DEVICE_LAST     ((uint8_t)(ALARM_MAX_DEVICES - ZBA_ZWAVE_DEVICES))
#if ALARM_MAX_DEVICES <= 8
#error "zbz_ace.h: Zigbee keypads need device numbers below the Z-Wave keypads' top eight"
#endif

#define ZBA_KEYPADS_MAX     8u      /* at most the numbers there are (test_zbz_contract) */
#define ZBA_FOBS_MAX        16u
#define ZBA_RECENT          16u
#define ZBA_RETRY_WINDOW_MS 60000u
#define ZBA_PENDING_MAX     4u      /* Arms waiting for the alarm task's answer. A panic always
                                     * finds room (decided in the ZB-M11 wiring): with every slot
                                     * taken it takes the oldest waiting Arm's, whose retransmission
                                     * is then neither answered nor run again; BUSY for a panic only
                                     * when ZBA_PENDING_MAX panics wait */
#define ZBA_ANSWER_MS       5000u   /* ... no answer by then: none is sent (the keypad shows its
                                     * own error), the post is forgotten */
/* A fob's alarm_source_t (decided 2026-10-02, alarm-devices.md §3): alarm_types.h's "keyfob
 * button" kind, ALARM_SRC_ZONE, with id 0 - it feeds no zone, and 0 is "none". The command runs
 * as its user (alarm_engine_command_as), so the log says who ("By Alex"), the engine's keyfob
 * rules hold (no auto-stay: that is keypad arming's) and no keypad id or device number is taken.
 * Replaces the provisional ZBA_FOB_SOURCE_BASE (keypad ids 0x2000 + slot), which broke the rule
 * that a keypad id is a device number. */
#define ZBA_FOB_SOURCE_KIND ALARM_SRC_ZONE
#define ZBA_FOB_SOURCE_ID   0u
#define ZBA_KEY             "ace"   /* stored in the registry's namespace */
#define ZBA_COMPAT          1u
#define ZBA_TAG             0x41000000u /* the keypads' sends: top byte 'A' */
#define ZBA_TAG_MASK        0xFF000000u
/* The stored value at its longest (zba_encode): compat, the two counts, keypads of 17 bytes and
 * fobs of 16 with their length bytes - for the NVS budget (zbz_panel.h ZBZ_NVS_WORST). */
#define ZBA_KEYPAD_ENTRY    17u
#define ZBA_FOB_ENTRY       16u
#define ZBA_BLOB_MAX        (3u + ZBA_KEYPADS_MAX * ZBA_KEYPAD_ENTRY + ZBA_FOBS_MAX * ZBA_FOB_ENTRY)

/* ---- the frames (pure; golden-tested, testing.md §3) ------------------------------------------- */

typedef struct {
    uint8_t mode;              /* ZBA_ARM_* (others: refused as not ready) */
    char code[ALARM_CODE_MAX_LEN + 1]; /* as sent; "" none */
    bool code_bad;             /* its length byte runs past the frame, or it is longer than any
                                * code the panel has: answered "invalid code", never truncated */
    uint8_t zone_id;           /* ignored */
} zba_arm_t;
zbz_zcl_result_t zba_decode_arm(const zsrc_zb_zcl_t *f, zba_arm_t *out);
void zba_arm_response(uint8_t note, uint8_t tsn, zsrc_zb_zcl_t *out);

typedef struct {
    uint8_t status;            /* ZBA_PS_* */
    uint8_t seconds;           /* exit or entry seconds left (0 otherwise) */
    uint8_t audible;           /* ZBA_AUDIBLE_* */
    uint8_t alarm;             /* ZBA_ALARM_* */
} zba_status_t;
void zba_panel_status_changed(const zba_status_t *s, zsrc_zb_zcl_t *out);
void zba_panel_status_response(const zba_status_t *s, uint8_t tsn, zsrc_zb_zcl_t *out);
zbz_zcl_result_t zba_decode_panel_status(const zsrc_zb_zcl_t *f, zba_status_t *out);

/* ---- the other commands (added in ZB-M11) ---------------------------------------------------------
 * The decoders answer ERR_TYPE for another cluster or direction, a manufacturer-specific frame
 * and a command they don't read (zba_decode_request: Arm, which is zba_decode_arm's, and ids past
 * ZBA_CMD_GET_ZONE_STATUS); SHORT for a frame shorter than its fields; LONG past
 * ZSRC_ZB_ZCL_PAYLOAD_MAX. Trailing bytes are ignored. The encoders build server-to-client
 * frames that ask for no Default Response, with the request's sequence number where they answer
 * one. */
#define ZBA_ZCL_MALFORMED   0x80u   /* ZCL MALFORMED_COMMAND: the Default Response to a frame
                                     * shorter than its fields */
#define ZBA_ZONE_MAP_SECTIONS 16u   /* Get Zone ID Map Response: 16 map16 sections */
#define ZBA_ZONE_TYPE_NONE  0xFFFFu /* Get Zone Information Response: no such zone */
#define ZBA_IEEE_NONE       0xFFFFFFFFFFFFFFFFull
#define ZBA_LABEL_MAX       32u     /* a zone label's characters sent (longer ones are cut) */
#define ZBA_ZONE_STATUS_MAX 15u     /* zone records one Get Zone Status Response carries */

typedef struct {
    uint8_t command;           /* ZBA_CMD_* (not ARM) */
    uint8_t zone_id;           /* GET_ZONE_INFO; GET_ZONE_STATUS: the first zone id */
    uint8_t max_zones;         /* GET_ZONE_STATUS */
    bool mask_flag;            /* GET_ZONE_STATUS: only zones whose status matches the mask */
    uint16_t status_mask;      /* GET_ZONE_STATUS */
    uint8_t count;             /* BYPASS: the zone ids asked */
    uint8_t zones[ZSRC_ZB_ZCL_PAYLOAD_MAX]; /* BYPASS. Its code is skipped, never copied. */
} zba_request_t;
zbz_zcl_result_t zba_decode_request(const zsrc_zb_zcl_t *f, zba_request_t *out);

/* Get Zone ID Map Response: `sections` (ZBA_ZONE_MAP_SECTIONS of them), NULL an empty map. */
void zba_zone_id_map_response(const uint16_t *sections, uint8_t tsn, zsrc_zb_zcl_t *out);
typedef struct {
    uint8_t zone_id;
    uint16_t zone_type;        /* ZBA_ZONE_TYPE_NONE: no such zone */
    uint64_t ieee;             /* ZBA_IEEE_NONE with it */
    const char *label;         /* NULL or "": none; cut at ZBA_LABEL_MAX */
} zba_zone_info_t;
void zba_zone_info_response(const zba_zone_info_t *z, uint8_t tsn, zsrc_zb_zcl_t *out);
/* Zone Status Changed (unsolicited): zone id, zone status, audible notification, label. */
void zba_zone_status_changed(uint8_t zone_id, uint16_t zone_status, uint8_t audible,
                             const char *label, zsrc_zb_zcl_t *out);
/* Set Bypassed Zone List: the answer to Get Bypassed Zone List (`tsn` the request's) or
 * unsolicited (ZSRC_ZB_TSN_NEW). `count` is cut to what a payload holds. */
void zba_set_bypassed(const uint8_t *zones, uint8_t count, uint16_t tsn, zsrc_zb_zcl_t *out);
/* Bypass Response: one ZBA_BYPASS_* result per zone asked. */
void zba_bypass_response(const uint8_t *results, uint8_t count, uint8_t tsn, zsrc_zb_zcl_t *out);
typedef struct {
    uint8_t zone_id;
    uint16_t status;           /* ZSRC_ZB_IAS_* bits */
} zba_zone_status_t;
/* Get Zone Status Response: zone status complete, the records (at most ZBA_ZONE_STATUS_MAX). */
void zba_zone_status_response(bool complete, const zba_zone_status_t *z, uint8_t count,
                              uint8_t tsn, zsrc_zb_zcl_t *out);

/* ---- the panel's side, as data ------------------------------------------------------------------- */

/* One partition's state as the alarm task last published it (from its snapshot). */
typedef struct {
    uint8_t state;             /* alarm_arm_state_t */
    uint8_t level;             /* alarm_arm_level_t */
    bool ready;                /* disarmed and nothing faulted */
    uint16_t delay_s;          /* exit or entry seconds left at `at` */
    uint64_t at;
    bool in_alarm;
    uint8_t cause;             /* in alarm: alarm_cause_t of the first alarm */
} zba_view_t;

/* Who a key fob may belong to (alarm-devices.md §3, ZB-024): a master, partition master,
 * standard, arm-only or guest user - never the installer, a duress code or an empty slot. The
 * pages offer only these, and a fob's command is refused if its user is no longer one. Pure. */
bool zba_fob_may_be(alarm_authority_t authority);

/* The panel status a keypad of the model is shown at `now` (the seconds counting down from
 * `at`). Pure. */
void zba_status_for(const zba_view_t *v, uint64_t now, uint16_t model_flags, zba_status_t *out);
/* The Arm Response for an Arm of `mode` that the engine answered `result`, the partition being
 * `before` when it was asked. Pure. */
uint8_t zba_note_for(uint8_t mode, alarm_result_t result, const zba_view_t *before);

/* ---- the policy ---------------------------------------------------------------------------------- */

typedef struct {
    bool used;
    uint16_t id;               /* the registry's id */
    uint64_t ieee;
    uint8_t endpoint;          /* its IAS ACE client endpoint */
    uint8_t partition;         /* 1..ALARM_MAX_PARTITIONS */
    uint8_t panic_mask;        /* 1 << alarm_panic_t its Emergency/Fire/Panic buttons may raise */
    bool show_state;           /* Panel Status Changed sent to it */
    uint8_t device;            /* its engine device number, ZBA_DEVICE_FIRST..ZBA_DEVICE_LAST, as
                                * zba_free_device gave it: its source id (lockout and log) only,
                                * never turned on in the engine's device table; kept while it
                                * stays assigned */
    uint16_t model_flags;      /* ZBZ_MODEL_* (not stored) */
    /* Kept here, not stored (added in ZB-M11; zba_assign_keypad clears them): */
    bool greeted;              /* a Panel Status Changed reached the radio since it was assigned
                                * (ZBZ_MODEL_ACE_INIT keypads take Arm Responses only after one) */
    bool shown_valid;
    zba_status_t shown;        /* the last Panel Status Changed sent: an unchanged state isn't
                                * sent again (a delay is) */
} zba_keypad_t;

typedef struct {
    bool used;
    uint16_t id;
    uint64_t ieee;
    uint8_t endpoint;
    uint8_t user;              /* 1..ALARM_MAX_USERS: whose fob it is */
    uint8_t partition;
    bool may_disarm;           /* off by default (ZB-024) */
    bool panic;                /* off by default */
    uint16_t model_flags;
} zba_fob_t;

/* What zba_receive asks of the owner. */
typedef struct {
    alarm_cmd_t cmd;           /* with the keypad's code: wipe it once posted */
    alarm_source_t source;     /* a keypad: ALARM_SRC_KEYPAD, its device number; a fob:
                                * ZBA_FOB_SOURCE_KIND, ZBA_FOB_SOURCE_ID */
    uint8_t user;              /* non-zero: run as this user without a code (a fob) */
    uint32_t ticket;           /* give it back with the answer (zba_result) */
} zba_post_t;

typedef enum {
    ZBA_NOTHING = 0,           /* nothing for the owner (answered here, or nothing to answer) */
    ZBA_POST,                  /* post `post` to the alarm task, then call zba_result */
    ZBA_DUPLICATE,             /* a retransmission: its first answer was sent again */
    ZBA_NOT_ASSIGNED,          /* not an assigned keypad or fob: refused NOT_AUTHORIZED */
    ZBA_INVALID,               /* a frame that doesn't decode: refused (Default Response) */
    ZBA_BUSY,                  /* ZBA_PENDING_MAX Arms wait already: no answer (it retries); a
                                * panic only when ZBA_PENDING_MAX panics wait */
} zba_action_t;

typedef struct {
    void *ctx;
    /* A frame to the keypad (the radio's zcl_command); false: not queued. */
    bool (*send)(void *ctx, uint64_t ieee, uint8_t endpoint, const zsrc_zb_zcl_t *cmd,
                 uint32_t tag);
    /* A Default Response (a global command: zsrc_zb_zcl_t can't carry one through `send`) to the
     * client command `command` of sequence `tsn`, with ZCL `status` (added in ZB-M11). NULL: the
     * owner answers through the stack instead - zba_receive's NOT_ASSIGNED is NOT_AUTHORIZED,
     * INVALID is ZBA_ZCL_MALFORMED or UNSUP_COMMAND, and a refused panic button is
     * NOT_AUTHORIZED (alarm-devices.md §6, question 2). */
    bool (*default_rsp)(void *ctx, uint64_t ieee, uint8_t endpoint, uint8_t tsn, uint8_t command,
                        uint8_t status, uint32_t tag);
} zba_port_t;

typedef struct {
    bool used;
    uint64_t ieee;
    uint8_t tsn;
    uint64_t at;
    uint8_t note;              /* the answer sent: results only, never a code (an Arm: its
                                * ZBA_NOTE_*; Emergency, Fire, Panic: the Default Response's
                                * ZCL status) */
    /* Added in ZB-M11: */
    uint8_t endpoint;
    uint8_t command;           /* ZBA_CMD_ARM, _EMERGENCY, _FIRE, _PANIC */
    bool answered;             /* false: nothing was sent (a panic's success, no Default Response
                                * asked) - a retransmission is then not answered either */
    /* Added in the ZB-M11 wiring: */
    uint8_t mode;              /* an Arm's ZBA_ARM_*: a frame with the same sequence number and
                                * another mode is a new command, not a retransmission */
} zba_recent_t;

typedef struct {
    bool used;
    uint32_t ticket;
    uint64_t ieee;
    uint8_t endpoint;
    uint8_t tsn;
    uint8_t mode;
    zba_view_t before;
    uint64_t at;
    /* Added in ZB-M11: */
    uint8_t command;           /* ZBA_CMD_ARM, or the panic button's command */
    bool want_response;        /* a panic's frame asked for a Default Response */
} zba_pending_t;

typedef struct {
    zba_port_t port;
    zba_keypad_t keypad[ZBA_KEYPADS_MAX];
    zba_fob_t fob[ZBA_FOBS_MAX];
    zba_view_t view[ALARM_MAX_PARTITIONS];
    zba_recent_t recent[ZBA_RECENT];
    uint8_t next_recent;
    zba_pending_t pending[ZBA_PENDING_MAX];
    uint32_t ticket_seq;
    uint16_t seq;
    bool dirty;                /* the assignments are to be stored */
} zba_t;

typedef enum {
    ZBA_OK = 0,
    ZBA_ERR_FULL,
    ZBA_ERR_ARG,               /* no partition, user 0 for a fob, a keypad's device number outside
                                * ZBA_DEVICE_FIRST..ZBA_DEVICE_LAST */
    ZBA_ERR_IN_USE,            /* the device number is another keypad's, or that user's fob slot
                                * is taken */
    ZBA_ERR_UNKNOWN,
} zba_result_t;

void zba_init(zba_t *a, const zba_port_t *port);
/* The device number for a keypad being assigned: the lowest of ZBA_DEVICE_FIRST..ZBA_DEVICE_LAST
 * that no assigned keypad holds and `taken` doesn't name (bit n-1: device n is another part's -
 * a panel-link keypad, the installer's own device table); 0 when none is free. A keypad already
 * assigned keeps its own (the owner passes it on). Pure. */
uint8_t zba_free_device(const zba_t *a, uint32_t taken);
/* The device numbers the assigned keypads hold, bit n-1 for device n: for the panel's other users
 * of 1..ZBA_DEVICE_LAST (zbz_panel_devices_held). Pure. */
uint32_t zba_devices_held(const zba_t *a);
/* k->device comes from zba_free_device (or is the keypad's own when it was assigned already).
 * Settled in ZB-M11: an assignment is found by its registry id and replaced in place (a device
 * assigned as the other kind loses that assignment: a device is a keypad or a fob); ARG also for
 * id 0, address 0, an endpoint outside 1..240, a partition past ALARM_MAX_PARTITIONS, a fob's
 * user past ALARM_MAX_USERS; IN_USE also for an address another id's assignment has, and - a
 * fob - a user who has another fob (one fob each: ui_backend's UI_ZB_REQ_IN_USE). The RAM-only
 * fields start clear; a post or retransmission memory of a replaced device goes. */
zba_result_t zba_assign_keypad(zba_t *a, const zba_keypad_t *k);
zba_result_t zba_assign_fob(zba_t *a, const zba_fob_t *f);
/* Its assignment, its posts waiting and its retransmission memory go (its device number is
 * free). UNKNOWN: none. */
zba_result_t zba_clear(zba_t *a, uint16_t id);

/* An ACE command from `ieee` (the radio's ZSRC_ZB_REP_ZCL for cluster 0x0501). Settled in
 * ZB-M11 (alarm-devices.md §3): an assignment is its address and endpoint both; a frame of
 * another cluster or the server's direction is INVALID and unanswered; an assigned device's
 * frame that doesn't decode is INVALID with a Default Response MALFORMED (unknown or
 * manufacturer-specific commands: UNSUP_COMMAND); a retransmission still waiting for the alarm
 * task is DUPLICATE and sends nothing. `post` is zeroed unless the action is POST; the code in
 * `in`'s payload is the caller's to wipe. */
zba_action_t zba_receive(zba_t *a, uint64_t now, uint64_t ieee, uint8_t endpoint,
                         const zsrc_zb_zcl_t *in, zba_post_t *post);
/* The alarm task's answer to a post: the Arm Response goes out with the request's sequence
 * number. False: no such ticket (answered too late, or never posted). */
bool zba_result(zba_t *a, uint64_t now, uint32_t ticket, alarm_result_t result,
                const alarm_cmd_result_t *detail);
/* A partition's state changed: Panel Status Changed to each keypad of it that shows state. */
void zba_panel(zba_t *a, uint64_t now, uint8_t partition, const zba_view_t *view);
/* Panel Status Changed of its partition to keypad `id` now, whatever show_state says (added in
 * ZB-M11): the owner calls it once a keypad is assigned and when it rejoins - a
 * ZBZ_MODEL_ACE_INIT keypad takes Arm Responses only after one (zba_receive greets it first
 * otherwise). False: no such keypad, or the radio didn't take it. */
bool zba_greet(zba_t *a, uint64_t now, uint16_t id);
/* Expires the posts nobody answered in ZBA_ANSWER_MS and the retransmission memory. */
void zba_poll(zba_t *a, uint64_t now);
/* When zba_poll has work: the earliest expiry, UINT64_MAX none. */
uint64_t zba_next_deadline(const zba_t *a);

/* The store: compat (1) | keypads u8 | fobs u8 | entries, each with its length byte first:
 * keypad (kind 1 | id u16 | ieee u64 | endpoint | partition | panic mask | flags (bit 0 show
 * state) | device), fob (kind 2 | id u16 | ieee u64 | endpoint | user | partition | flags (bit 0
 * may disarm, bit 1 panic)). A keypad entry whose device number is outside ZBA_DEVICE_FIRST..
 * ZBA_DEVICE_LAST, or another entry's, is dropped as damaged (the others load).
 * Settled in ZB-M11: little-endian; an entry's length byte counts the bytes after it, so a later
 * format appends fields inside an entry and this reader skips them; the entries are the keypads'
 * then the fobs' (keypads + fobs of them, each by its own kind byte). An entry that can't be used
 * - an unknown kind, id 0, address 0, endpoint outside 1..240, no partition, a fob's user 0 or
 * past ALARM_MAX_USERS or another fob's, a keypad's device number as above, an id or address
 * already read - is dropped and the rest load (zba_decode true). False, and nothing assigned:
 * NULL, compat 0 or above ZBA_COMPAT, more entries than the arrays hold, an entry shorter than
 * its kind's fields or running past the blob, bytes left over. model_flags aren't stored: the
 * owner fills them from zbz_model_for after a decode. */
size_t zba_encode(const zba_t *a, uint8_t *out, size_t cap);
bool zba_decode(zba_t *a, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZBZ_ACE_H */
