/*
 * fake_zdev.h - a simulated Z-Wave device behind the fake ZAC93 (docs/features/36 README "Test
 * and acceptance"), for the host tests and, later, the PC simulator: its node information, and a
 * real joining-side S2 (security.md §5 "The joining side") - its own key pair and printed DSK,
 * the key exchange, then reports encrypted with the keys it got. Its S2 runtime is a zw_s2_t in
 * device mode with the panel (node 1) as its only peer: the SPAN protocol is the same on both
 * sides (as Z-Wave JS's mock nodes share its security manager).
 *
 * It lives on the fake module's radio (fake_zac93_add_device): the module hands it the panel's
 * SendData commands (fake_zdev_hear) and sends its frames on to the host as
 * ApplicationCommandHandler (fake_zac93_from_device); fake_zac93_press() is its button in the
 * panel's inclusion window.
 *
 * Faults: silence at a step of the key exchange (the panel's timers), a KEX Fail of any type
 * instead of a step, a CSA request, other schemes or curves (set the KEX Report fields), a man in
 * the middle changing its KEX Report or the panel's KEX Set, frames lost or repeated either way,
 * a report crossing the panel's next frame, a restart (its SPAN forgotten, its keys kept). What
 * devices do in the field, each off by default: a lifeline Report in two parts, Supervision Reports
 * with a stale or wrong session, Application Status instead of carrying out a Set, and its node
 * information falling back to every CC in the clear after a failed key exchange.
 *
 * Portable C11, no heap; no test framework.
 */
#ifndef FAKE_ZDEV_H
#define FAKE_ZDEV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zw_host_crypto.h"
#include "zwave_host/zw_s2.h"

#ifdef __cplusplus
extern "C" {
#endif

struct fake_zac93;

#define FAKE_ZDEV_PANEL 1u
#define FAKE_ZDEV_GOT   16u
#define FAKE_ZDEV_CMD   48u

/* The steps of its side of the key exchange (§5 "The joining side"). */
typedef enum {
    FAKE_KEX_NONE = 0,
    FAKE_KEX_REPORT,       /* KEX Get -> KEX Report */
    FAKE_KEX_PUBKEY,       /* KEX Set -> Public Key Report */
    FAKE_KEX_ECHO,         /* the panel's key -> KEX Set echo (every 10 s until answered) */
    FAKE_KEX_NETKEY_GET,   /* KEX Report echo, or a Transfer End -> Network Key Get */
    FAKE_KEX_VERIFY,       /* Network Key Report -> Network Key Verify */
    FAKE_KEX_TRANSFER_END  /* the last key verified -> Transfer End, key request complete */
} fake_kex_step_t;

typedef enum {
    FAKE_ZDEV_OUT = 0,     /* not in a network */
    FAKE_ZDEV_JOINED,      /* in the network; no key exchange (yet) */
    FAKE_ZDEV_EXCHANGING,  /* its key exchange runs */
    FAKE_ZDEV_SECURE,      /* it holds `have` */
    FAKE_ZDEV_FAILED       /* its key exchange failed */
} fake_zdev_status_t;

/* ---- What it answers the interview with (network.md §5; command-classes.md §4) --------------- */

#define FAKE_ZDEV_EPS    3u   /* Multi Channel end points */
#define FAKE_ZDEV_CCS    24u
#define FAKE_ZDEV_TYPES  8u

typedef struct {
    uint8_t cc, version;
} fake_cc_version_t;

/* One Multi Channel end point. */
typedef struct {
    uint8_t generic, specific;
    uint8_t ccs[12];              /* its Capability Report */
    size_t cc_n;
    uint8_t secure_ccs[12];       /* its own Security 2 Commands Supported Report */
    size_t secure_n;
    uint8_t notif_types[FAKE_ZDEV_TYPES];
    size_t notif_n;
    uint8_t binary_types[4];      /* Binary Sensor types it has */
    size_t binary_n;
    bool temperature;             /* Multilevel Sensor air temperature */
} fake_ep_t;

typedef struct {
    bool answers;                 /* it answers questions (false, the default: it only records
                                   * them - the lower layers' tests) */
    bool listening, flirs;        /* GetNodeProtocolInfo (the module answers from it) */
    bool sleeps;                  /* a sleeping device's radio really sleeps between wake-ups;
                                   * false (the default): it hears everything, whatever it says
                                   * it is (the lower layers' tests) */
    uint16_t manufacturer, product_type, product_id;
    fake_cc_version_t versions[FAKE_ZDEV_CCS]; /* Version CC Report (a CC not here: 0) */
    size_t versions_n;
    uint8_t secure_ccs[FAKE_ZDEV_CCS];         /* its Security 2 Commands Supported Report */
    size_t secure_n;
    uint8_t zwp_role, zwp_node_type;
    uint16_t icon;
    /* Wake Up */
    uint32_t wake_interval, wake_min, wake_max, wake_default, wake_step;
    uint8_t wake_node;            /* where its Wake Up Notifications go (0: nowhere heard) */
    uint32_t awake_ms;            /* awake this long after a wake-up or joining... */
    uint32_t awake_extend_ms;     /* ...and at least this long after each frame it hears */
    bool auto_wake;               /* it wakes by itself at its interval */
    /* Association (group 1 is the lifeline) */
    uint8_t groups;
    uint8_t assoc[4][4];          /* groups 1..4: their members */
    size_t assoc_n[4];
    uint8_t mca_groups;           /* 0: no Multi Channel Association groups */
    uint8_t mca_dest[12];         /* Multi Channel Association group 1's destinations, raw */
    size_t mca_dest_n;
    uint16_t agi_profile;         /* AGI group 1's profile */
    uint8_t agi_cmds[8];          /* AGI group 1's command list: CC/command pairs */
    size_t agi_cmds_n;
    /* Notification (the device itself) */
    uint8_t notif_types[FAKE_ZDEV_TYPES];
    size_t notif_n;
    /* Notification Event Supported Report per type (fake_zdev_notification_events); a type with
     * no entry here answers every event 1..23, as before. */
    struct {
        uint8_t type;
        uint8_t mask[4];
        size_t len;
    } notif_events[FAKE_ZDEV_TYPES];
    size_t notif_events_n;
    bool pull;                    /* pull mode: Get answers status 00, a Set is ignored */
    uint8_t notif_status[FAKE_ZDEV_TYPES]; /* FF: enabled (push) */
    uint8_t binary_types[4];
    size_t binary_n;
    uint8_t alarm_types[4];       /* Alarm Sensor (legacy) types it has (the device itself) */
    size_t alarm_n;
    bool alarm_bad_report;        /* its Supported Report claims more mask bytes than it has */
    uint32_t alarm_active;        /* bit t: Alarm Sensor type t is in alarm (its Get answers FF) */
    bool temperature;
    int16_t temp_c10;             /* tenths of a degree */
    uint8_t battery;
    uint8_t fw_major, fw_minor;   /* its Version Report's firmware 0 (door_model: 1.10) */
    /* ZW-034: its switches - Binary or Multilevel Switch at the device (0) or its end points,
     * their values as the CC has them; sw_report: it reports a change the panel asked for
     * (as Zooz switches do), besides answering Gets; sw_ignore: it takes no Set (broken) */
    uint8_t sw[FAKE_ZDEV_EPS + 1u];
    bool sw_report;
    bool sw_ignore;
    uint32_t sw_sets;             /* Sets it took */
    uint8_t ep_n;                 /* Multi Channel end points */
    fake_ep_t eps[FAKE_ZDEV_EPS];
    /* what the panel set */
    struct {
        uint8_t parameter;
        int32_t value;
    } config[4];
    size_t config_n;
    /* faults */
    uint8_t mute_cc, mute_cmd;    /* it never answers this question (mute_cmd 0: any)... */
    uint8_t mute_n;               /* ...this many times (FAKE_MUTE_ALWAYS: always) */
    bool supervision_fail;        /* Supervision Gets answered "fail" */
    bool read_back_empty;         /* its lifeline Get answers without the panel */
    uint8_t wrong_ep_cc, wrong_ep_cmd; /* an end point's answer to this question comes from... */
    uint8_t wrong_ep_n;           /* ...another end point, this many times */
    /* its lifeline group's Association (and Multi Channel Association) Report in two parts, as a
     * device with more destinations than one Report holds sends it (reports to follow 1, then
     * 0): the first names split_nodes, the second the group as it is - split_ms after the Get,
     * and as much again after that (0: at once) */
    uint8_t split_nodes[4];
    size_t split_n;               /* 0 (the default): one Report */
    uint32_t split_ms;
    uint8_t split_lose_n;         /* its second part never goes, this many times */
    /* its Supervision Report for a Set of this CC carries another session... */
    uint8_t spv_wrong_cc;
    uint8_t spv_wrong_n;          /* ...this many times (0, the default: never)... */
    bool spv_stale;               /* ...the Supervision Get before's (a stale Report); false: the
                                   * next session's */
    /* a Set of this CC answered with Application Status instead of carried out (and without its
     * Supervision Report) */
    uint8_t app_status_cc;
    uint8_t app_status_n;         /* this many times (0, the default: never) */
    uint8_t app_status_cmd;       /* ZW_CMD_APPLICATION_BUSY or ZW_CMD_APPLICATION_REJECTED_REQUEST */
    uint8_t app_busy_status, app_busy_wait_s; /* Busy's status and wait time */
} fake_model_t;

#define FAKE_MUTE_ALWAYS 0xFFu

typedef struct fake_zdev {
    /* who it is (set before it joins; fake_zdev_init fills a door sensor) */
    uint8_t basic, generic, specific;
    uint8_t nif[24];              /* its node information's CC list */
    size_t nif_len;
    bool nif_fallback;            /* a failed key exchange makes it work without security and
                                   * list every CC it has in its node information, as the
                                   * protocol's fallback has it (its secure CCs join its NIF);
                                   * false (the default): its NIF stays as it was */
    bool controller;              /* a controller: the panel refuses it */
    fake_model_t m;               /* its answers to the interview */
    uint64_t awake_until;         /* a sleeping device listens while now < this */
    uint64_t next_wake;           /* its next wake-up by itself (auto_wake) */
    uint32_t wakeups;             /* Wake Up Notifications it sent */
    uint32_t nmi_heard;           /* No More Information it got */
    uint32_t answers;             /* questions it answered */
    uint32_t heard_n;             /* the panel's frames it heard */
    uint32_t heard_asleep;        /* the panel's frames that reached it asleep (a test failure) */
    uint32_t supervised;          /* commands it got inside a Supervision Get */
    uint32_t mc_secure, mc_clear; /* commands it got for an end point (Multi Channel), encrypted
                                   * and in the clear */
    uint32_t spv_reports;         /* Supervision Reports it got (the panel's answers to its
                                   * supervised reports); the last one's: */
    uint8_t spv_session, spv_status;
    zw_sec_class_t spv_sec;
    uint64_t spv_at;
    uint8_t spv_last;             /* the session of the last Supervision Get it got */
    struct {
        uint64_t at;
        zw_sec_class_t cls;
        size_t len;
        uint8_t cmd[FAKE_ZDEV_CMD];
    } later[4];                   /* answers it sends later (split_ms), in order */
    size_t later_n;
    struct {
        uint8_t cc, cmd;
        uint16_t n;
    } asked[48];                  /* questions it got, by CC and command (answered or not) */
    size_t asked_k;
    uint8_t watch_cc, watch_cmd;  /* this question's times are kept: */
    uint64_t watch_at[32];
    size_t watch_n;
    zw_s2_kex_t kex;              /* its KEX Report: flags (CSA, reserved), schemes, curves, keys */
    uint8_t priv[32], pub[32];    /* its key pair */
    char dsk[48];                 /* its label; the first group is the PIN */
    uint16_t pin;
    /* faults */
    uint8_t silent_at;            /* fake_kex_step_t: it stops answering there */
    uint8_t fail_at;              /* fake_kex_step_t: it sends KEX Fail fail_type instead */
    uint8_t fail_type;
    uint8_t mitm_report_keys;     /* != 0: the panel gets its KEX Report asking for these */
    uint8_t mitm_set_keys;        /* != 0: it gets the panel's KEX Set granting these */
    uint8_t ask_key;              /* != 0: its first Network Key Get asks for this key */
    bool verify_with_temp;        /* its Network Key Verify under the temporary key */
    bool damage_first_echo;       /* its first KEX Set echo arrives damaged (its tag) */
    bool damage_next;
    uint8_t echo_flags_or;        /* added to its KEX Set echo's flags (a reserved bit...) */
    bool netkey_get_plain;        /* its Network Key Get in the clear */
    bool te_flags_set;            /* its last Transfer End carries te_flags instead of "complete" */
    uint8_t te_flags;
    unsigned lose_in_after;       /* after this many of the panel's frames, lose the next one */
    unsigned lose_out;            /* its next frames never reach the module */
    unsigned lose_in;             /* the panel's next frames never reach it (no ACK) */
    uint8_t deaf_cc;              /* the panel's plain frames of this CC never reach it (no ACK) */
    bool silent;                  /* reset or dead: it hears nothing at all (never awake), while
                                   * the module still lists it */
    unsigned repeat_out;          /* its next frames reach the module twice */
    bool cross_next;              /* before it handles the panel's next frame, it sends cross */
    uint8_t cross[FAKE_ZDEV_CMD];
    size_t cross_len;
    /* state */
    fake_zdev_status_t status;
    uint16_t node;
    uint32_t home_id;
    uint8_t step;                 /* what it waits for: fake_kex_step_t of its next message */
    zw_s2_kex_t sent_report;      /* the KEX Report it sent (before any man in the middle) */
    zw_s2_kex_t set;              /* the KEX Set as it got it */
    uint8_t have;                 /* keys it holds */
    uint8_t netkeys[3][16];       /* S2 Unauthenticated, Authenticated, Access Control */
    uint64_t echo_due;
    uint32_t echoes;              /* KEX Set echoes sent */
    uint64_t now;
    zw_host_crypto_t hc;
    zw_crypto_port_t crypto;
    zw_s2_t s2;
    struct fake_zac93 *zac;
    uint32_t acks[8];             /* its frames' fate, for its S2 at its next poll */
    bool ack_ok[8];
    size_t acks_n;
    /* what reached it from the panel, after its S2 */
    uint8_t got[FAKE_ZDEV_GOT][FAKE_ZDEV_CMD];
    size_t got_len[FAKE_ZDEV_GOT];
    zw_sec_class_t got_sec[FAKE_ZDEV_GOT];
    size_t got_n;
    uint8_t fail_got;             /* the panel's KEX Fail type, if one came */
    uint32_t sent_frames;         /* frames it sent, lost ones included */
    /* its software update (ZW-033): Firmware Update Meta Data answered when fw.on (list 7A in its
     * CCs and versions too); its fragments asked two at a time */
    struct {
        bool on;
        uint16_t firmware_id;
        uint16_t max_fragment;    /* its MD Report's (0: says none) */
        bool not_upgradable;
        bool activation;          /* it waits for Activation Set before it runs the image */
        bool refuse_set;
        uint8_t refuse;           /* refuse_set: its Request Report's status */
        uint8_t verdict;          /* its Status Report for an image that checks out (0 unset: FF) */
        uint16_t wait_s;
        uint16_t drop_at;         /* this fragment is lost once: it asks for it again */
        uint16_t quiet_after;     /* after this fragment it stops asking (0: never) */
        uint8_t after_major, after_minor; /* the version an image it takes gives it (0.0: kept) */
        /* state */
        bool active;
        uint16_t fragment, checksum, next;
        uint8_t image[2048];
        size_t len;
        uint32_t fragments;       /* fragments it took */
        uint32_t bad, unexpected; /* fragments damaged (their CRC), and not the one it asked */
        uint16_t heard[16];       /* the first fragments' numbers, as they came */
        size_t heard_n;
        uint8_t status;           /* the Status Report it sent (0 before it sent one) */
        bool status_sent;
        uint32_t updates;         /* images it took (and runs) */
    } fw;
} fake_zdev_t;

/* A door sensor (Notification, Battery, Wake Up; S2 Access Control, Authenticated and
 * Unauthenticated requested) with a key pair from `seed`; its DSK and PIN printed. */
void fake_zdev_init(fake_zdev_t *d, uint64_t seed);
/* Without Security 2: its node information lists no 9F. */
void fake_zdev_no_s2(fake_zdev_t *d);

/* The module gives it a node ID in a network (at inclusion). */
void fake_zdev_joined(fake_zdev_t *d, uint32_t home_id, uint16_t node);
/* A frame from the panel reached it (the module's SendData). */
void fake_zdev_hear(fake_zdev_t *d, uint64_t now, const uint8_t *cmd, size_t len);
void fake_zdev_poll(fake_zdev_t *d, uint64_t now);
uint64_t fake_zdev_next_deadline(const fake_zdev_t *d);

/* A report to the panel at `cls` (ZW_SEC_AUTO: its highest class; ZW_SEC_NONE: in the clear). */
zw_s2_result_t fake_zdev_report(fake_zdev_t *d, uint64_t now, zw_sec_class_t cls,
                                const uint8_t *cmd, size_t len);
/* A restart: its SPAN and sequence numbers forgotten, its keys kept. */
void fake_zdev_restart(fake_zdev_t *d);

/* Whether it hears the radio now: listening, FLiRS, or a sleeping device awake. */
bool fake_zdev_awake(const fake_zdev_t *d, uint64_t now);
/* It wakes up (its button, or its interval): awake for awake_ms, a Wake Up Notification to the
 * panel when that is where they go. */
void fake_zdev_wake(fake_zdev_t *d, uint64_t now);
/* Factory reset by hand: Device Reset Locally to the panel, then it is out of the network. */
void fake_zdev_reset_locally(fake_zdev_t *d, uint64_t now);
/* Models: a mains-powered, always listening device (no Wake Up); insecure (no Security 2). */
void fake_zdev_listening(fake_zdev_t *d);
/* This model answers Notification Event Supported Get for `type` with exactly these events (raw
 * event numbers, e.g. 0x16, 0x17); unset for a type (the default): every event 1..23, unlike most
 * real sensors (fake_zdev_cc.c). */
void fake_zdev_notification_events(fake_zdev_t *d, uint8_t type, const uint8_t *events, size_t n);
/* Answers a question from the panel as the model says; true when it was one it knows
 * (fake_zdev_cc.c). */
bool fake_zdev_answer(fake_zdev_t *d, const zw_s2_rx_t *rx);
/* ZW-034: someone at the device switches end point `ep` (0: the device) to `value`: it reports
 * it to the panel (`cc` its switch CC). */
void fake_zdev_switch_press(fake_zdev_t *d, uint64_t now, uint8_t ep, uint8_t cc, uint8_t value);
/* How many times it was asked this (CC, command). */
unsigned fake_zdev_asked(const fake_zdev_t *d, uint8_t cc, uint8_t cmd);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_ZDEV_H */
