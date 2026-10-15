/*
 * zsrc_zwave.h - Z-Wave devices as zones (docs/features/36 zones.md; ZW-013, ZW-014, ZW-015): door
 * and window contacts, motion, water, smoke, CO, heat, freeze, gas, glass break, tilt, panic
 * buttons and temperature sensors added to the panel's own Z-Wave network, each input of a device
 * a zone of its own, with tamper, low battery and life-safety trouble reaching every zone of the
 * device.
 *
 * This half is portable and host-tested and knows nothing of the Z-Wave stack: it depends on
 * neither zwave_host nor zwave_proto (a firmware without Z-Wave carries none of it). What it is
 * told comes decoded, through the zsrc_zw_* calls below: a device's facts (what its interview
 * found, zsrc_zw_desc_t), its reports (zsrc_zw_report_t), whether it can be reached, that it
 * left, and the network's link. components/core/zwave_zones turns zw_net's events and commands
 * into these calls on the panel and in the PC simulator; the host tests call them directly.
 * Calls come with the platform's lock held (zone_service), like every other source's.
 *
 * Addresses (zones.md §1, stored in bindings): kind ZSRC_ZWAVE, id = home ID << 32 | generation
 * << 16 | node ID, input = endpoint << 5 | slot. The home ID ties a binding to one network (a
 * reset or replaced module's new home ID matches none of the old bindings), the generation to
 * one inclusion (a node ID the module gives out again is another device): an old binding never
 * follows a new device, its zone shows CHECK.
 *
 * Slots (zones.md §2, stored in bindings: append only) are the inputs a device can offer, one
 * per endpoint each: a slot is offered when the interview shows the device can report it (the
 * event in its Event Supported Report, the type in its Binary Sensor Supported Report, air
 * temperature in its Multilevel Supported Sensor Report, Basic when it has nothing better) or
 * the quirks table says so; devices that do not report by themselves (pull mode, no lifeline)
 * offer nothing. Tamper, low battery and trouble are the whole device's (zones.md §4).
 *
 * Reports (zones.md §3): an input's state goes to the hub as "open"/"closed" (contacts) or
 * "on"/"off", temperature as its value in hundredths of a degree; a momentary input (glass break,
 * impact, emergency) is "on" for the binding's hold_s (ZSRC_ZW_HOLD_S when 0), restarted by a
 * repeat; tamper and low battery go to every watched input of the device, trouble to its smoke,
 * CO, heat and gas inputs; a Wake Up Notification, a heartbeat, a test or any frame that changes
 * nothing is a check-in, at most once a minute per device. Reachability is network.md §8's
 * (zw_health's): the source passes it on. When the link comes up, and when an input starts to be
 * watched, the device's state is asked for afresh (zsrc_zw_take_refresh): listening devices at
 * once, sleeping ones at their next wake-up; until it answers, the input has no state and the
 * hub's grace runs (ZSRC-015).
 *
 * Trust (zones.md §6) differs per device: caps_of() adds ZSRC_CAP_AUTH for a device with
 * Security 2 or S0, and ZSRC_CAP_LIFE_SAFETY for one with Security 2 that is supervised; a
 * sleeping device that never checks in (interval 0) loses ZSRC_CAP_SUPERVISED. The grace of a
 * binding that names none is the device's own (grace_of(), network.md §8): a sleeper's check-in
 * + 10 min, a listening device's 30 min; never for a device that never checks in; at once for
 * one that left the network.
 */
#ifndef ZSRC_ZWAVE_H
#define ZSRC_ZWAVE_H

#include "zone_hub/zone_source.h"

#ifdef __cplusplus
extern "C" {
#endif

/* zones.md §7: the source's timers and limits. */
#define ZSRC_ZW_DEV_MAX          64u    /* devices (the node cache's ZW_NODES_MAX) */
#define ZSRC_ZW_WATCH_MAX        96u    /* inputs bound to zones */
#define ZSRC_ZW_GRACE_S          4200u  /* the source's grace: the default 1 h check-in + 10 min */
#define ZSRC_ZW_LINK_GRACE_S     90u    /* the radio down this long: its trouble (ZW-032); a
                                         * module's own recovery takes seconds */
#define ZSRC_ZW_LEARN_S          60u    /* the add/remove window (network.md §4) */
#define ZSRC_ZW_HOLD_S           30u    /* a momentary input's "on" (hold_s 0) */
#define ZSRC_ZW_PING_IDLE_S      900u   /* a quiet listening device is pinged (zw_health) */
#define ZSRC_ZW_BATTERY_LOW_PCT  20u    /* below this: low battery */
#define ZSRC_ZW_BATTERY_OK_PCT   30u    /* from this, without replace flags: restored */
#define ZSRC_ZW_JAM_HOLD_MS      30000u /* jamming (network.md §9, phase 2) */
#define ZSRC_ZW_JAM_DBM          (-60)
/* network.md §8's grace per device (the binding's default). */
#define ZSRC_ZW_SLEEP_GRACE_S    600u   /* a sleeping device: its check-in + this */
#define ZSRC_ZW_LISTEN_GRACE_S   1800u  /* a listening device */
#define ZSRC_ZW_GONE_GRACE_S     1u     /* a device no longer in the network: CHECK at once */
#define ZSRC_ZW_MOVED_S          30u    /* "product moved" with a cover: tamper this long */
#define ZSRC_ZW_CHECKIN_EVERY_MS 60000u /* check-ins passed on, at most, per device */
#define ZSRC_ZW_IDLE_RESET_MAX   8u     /* notification variables a quirk's idle_after_s times
                                         * at once, per device (zones.md §8) */

#define ZSRC_ZW_EP_MAX    4u  /* endpoint 0 (the device) and Multi Channel endpoints 1-3: the node
                               * cache's limit (zones.md §1 allows 7 in the address) */
#define ZSRC_ZW_TYPES_MAX 8u  /* notification types per endpoint (the node cache's) */
#define ZSRC_ZW_BATTERY_UNKNOWN 0xFFu
#define ZSRC_ZW_NROW_WORDS 3u /* words for one bit per notification event the source knows */

/* zones.md §2: the inputs a device can offer. Stored in bindings: append only. */
typedef enum {
    ZSRC_ZW_SLOT_NONE = 0,
    ZSRC_ZW_SLOT_CONTACT = 1,      /* door/window contact: open/closed */
    ZSRC_ZW_SLOT_INTRUSION = 2,
    ZSRC_ZW_SLOT_MOTION = 3,
    ZSRC_ZW_SLOT_GLASS_BREAK = 4,  /* momentary */
    ZSRC_ZW_SLOT_TILT = 5,
    ZSRC_ZW_SLOT_IMPACT = 6,       /* impact / moved: momentary */
    ZSRC_ZW_SLOT_WATER = 7,
    ZSRC_ZW_SLOT_SMOKE = 8,
    ZSRC_ZW_SLOT_CO = 9,
    ZSRC_ZW_SLOT_HEAT = 10,
    ZSRC_ZW_SLOT_FREEZE = 11,
    ZSRC_ZW_SLOT_GAS = 12,
    ZSRC_ZW_SLOT_EMERGENCY = 13,   /* momentary */
    ZSRC_ZW_SLOT_TEMPERATURE = 14, /* a value, hundredths of a degree Celsius */
    ZSRC_ZW_SLOT_MAINS = 15,       /* active = mains lost */
    ZSRC_ZW_SLOT_GENERAL = 16,     /* Binary Sensor general / aux, Basic (zones.md §5) */
    ZSRC_ZW_SLOT_COUNT
} zsrc_zw_slot_t;

/* The address of a device's input, and back. False: not a Z-Wave input address. */
zsrc_address_t zsrc_zwave_address(uint32_t home_id, uint16_t generation, uint16_t node,
                                  uint8_t endpoint, uint8_t slot);
bool zsrc_zw_address_parse(const zsrc_address_t *a, uint32_t *home_id, uint16_t *generation,
                           uint16_t *node, uint8_t *endpoint, uint8_t *slot);

/* ---- what the interview found (zwave_zones builds it from the node cache) -------------------- */

/* A device's security (security.md §1): its highest granted class. */
typedef enum {
    ZSRC_ZW_SEC_NONE = 0,
    ZSRC_ZW_SEC_S0,
    ZSRC_ZW_SEC_S2_UNAUTH,
    ZSRC_ZW_SEC_S2_AUTH,
    ZSRC_ZW_SEC_S2_ACCESS,
    ZSRC_ZW_SEC_UNKNOWN /* not known yet: trusted as not encrypted */
} zsrc_zw_security_t;

/* What an endpoint offers the zones, as the interview judged it (zw_iv_inputs, same numbers). */
typedef enum {
    ZSRC_ZW_INPUTS_OFFERED = 0,
    ZSRC_ZW_INPUTS_NOT_READY,   /* still being interviewed */
    ZSRC_ZW_INPUTS_NO_LIFELINE, /* "Does not report to the panel" */
    ZSRC_ZW_INPUTS_PULL,        /* it would have to be polled */
    ZSRC_ZW_INPUTS_NOT_USABLE   /* another controller */
} zsrc_zw_inputs_t;

typedef struct {
    uint8_t inputs;             /* zsrc_zw_inputs_t */
    uint8_t notif_count;
    struct {
        uint8_t type;           /* notification type (Smoke 01 ... Gas 12) */
        uint32_t events;        /* its Event Supported Report: bit e = event e; 0 = none (v2) */
    } notif[ZSRC_ZW_TYPES_MAX];
    uint32_t binary_types;      /* Binary Sensor Supported Report: bit t = sensor type t */
    bool binary;                /* has Binary Sensor at all (v1: no list, a general input) */
    bool air_temperature;       /* Multilevel Sensor air temperature (or v1-4: can't tell) */
    uint16_t alarm_types;       /* Alarm Sensor (legacy): bit t = sensor type t */
    bool basic;                 /* lists Basic */
} zsrc_zw_endpoint_desc_t;

typedef struct {
    uint16_t node;
    uint16_t generation;        /* the panel's inclusion counter when it was added (zones.md §1) */
    bool has_identity;
    uint16_t manufacturer, product_type, product_id;
    bool has_firmware;
    uint16_t firmware;          /* major << 8 | minor */
    uint8_t security;           /* zsrc_zw_security_t */
    bool listening;             /* mains (or FLiRS): always reachable, pinged when quiet */
    uint32_t check_in_s;        /* a sleeper's wake-up interval; 0 = only woken by hand */
    uint16_t installer_icon;    /* Z-Wave Plus Info, 0 = none */
    uint8_t endpoint_count;     /* Multi Channel endpoints described in ep[1..] */
    zsrc_zw_endpoint_desc_t ep[ZSRC_ZW_EP_MAX]; /* [0] the device itself */
} zsrc_zw_desc_t;

/* ---- what a device said (decoded by zwave_zones, command-classes.md §4) ---------------------- */

typedef enum {
    ZSRC_ZW_RPT_FRAME = 0,     /* any other frame: the device is alive */
    ZSRC_ZW_RPT_NOTIFICATION,  /* Notification Report (§4.5) */
    ZSRC_ZW_RPT_BINARY,        /* Binary Sensor Report (§4.3) */
    ZSRC_ZW_RPT_BATTERY,       /* Battery Report (§4.6) */
    ZSRC_ZW_RPT_MULTILEVEL,    /* Multilevel Sensor Report (§4.4) */
    ZSRC_ZW_RPT_ALARM_SENSOR,  /* Alarm Sensor Report (§4.22, legacy) */
    ZSRC_ZW_RPT_BASIC,         /* Basic Set or Report (§4.2) */
    ZSRC_ZW_RPT_WAKE_UP,       /* Wake Up Notification (§4.7) */
    ZSRC_ZW_RPT_CENTRAL_SCENE  /* Central Scene Notification (§4.21): a button, by quirk */
} zsrc_zw_report_kind_t;

typedef struct {
    uint8_t kind;              /* zsrc_zw_report_kind_t */
    uint8_t endpoint;          /* the Multi Channel source endpoint; 0 = the device itself */
    /* NOTIFICATION */
    bool notif_v1;             /* a v1 alarm (shorter than 7 bytes): read only through the
                                * quirk's alarm map or the supported types (ZW-043) */
    uint8_t notif_status;      /* FF on, 00 reports switched off, FE nothing pending */
    uint8_t notif_type;
    uint8_t notif_event;       /* 00 = idle */
    uint8_t notif_param_len;
    uint8_t notif_params[4];   /* the first event parameters (idle: the event that ended) */
    uint8_t notif_v1_type;     /* the V1 Alarm Type and Level every report starts with (ZW-043) */
    uint8_t notif_v1_level;
    /* BINARY */
    bool binary_has_type;
    uint8_t binary_type;
    bool binary_on;
    /* BATTERY */
    uint8_t battery_level;     /* 0..100, FF = the low warning */
    uint8_t battery_replace;   /* v2: 00 none, 01 replace soon, 02 replace now */
    bool battery_disconnected; /* v2 flags2 bit 0: treated like "replace now" (zones.md §3) */
    /* MULTILEVEL */
    uint8_t ml_type;           /* 01 = air temperature */
    uint8_t ml_scale;          /* 0 Celsius, 1 Fahrenheit */
    int32_t ml_value_x100;     /* in hundredths of the scale's unit */
    /* ALARM_SENSOR */
    uint8_t alarm_type;
    uint8_t alarm_state;       /* 00 idle, else active */
    /* BASIC */
    uint8_t basic_value;       /* 00 inactive, else active */
    /* CENTRAL_SCENE */
    uint8_t scene_seq;         /* its sequence number: a repeat is a retransmission */
    uint8_t scene_attr;        /* key attribute, bits 2..0: 0 pressed, 1 released, 2 held,
                                * 3..6 pressed 2..5 times */
    uint8_t scene_number;
} zsrc_zw_report_t;

/* ---- the quirks table (zones.md §8; data, zw_quirks.c) --------------------------------------- */

#define ZSRC_ZW_QUIRK_ANY 0xFFFFu
#define ZSRC_ZW_QUIRK_CONFIG_MAX 4u
/* "Product moved" is this model's shock sensor (an input), not a tamper, whatever its cover. */
#define ZSRC_ZW_QUIRK_MOVED_IS_IMPACT 0x01u
#define ZSRC_ZW_ALARM_MAP_MAX   10u      /* alarm map rows per model (ZW-043) */
#define ZSRC_ZW_ALARM_LEVEL_ANY 0xFFFFu  /* an alarm map row for any V1 Alarm Level */

/* One row of a model's alarm map (zones.md §8, ZW-043; Z-Wave JS's compat.alarmMapping): a
 * Notification Report whose V1 Alarm Type and Level match is read as this notification type and
 * event (event 00: idle). The first matching row wins; alarm type 0 ends the list (it is never
 * mapped). */
typedef struct {
    uint8_t alarm_type;
    uint16_t alarm_level;           /* 0..FF, or ZSRC_ZW_ALARM_LEVEL_ANY */
    uint8_t type, event;
} zsrc_zw_alarm_map_t;

typedef struct {
    uint16_t manufacturer, product_type, product_id; /* ZSRC_ZW_QUIRK_ANY matches any */
    const char *name;               /* the model for people: "Zooz ZSE41 open/close sensor" */
    uint8_t device_class;           /* alarm_device_class_t for its contact, intrusion and general
                                     * inputs; GENERIC keeps the slot's own */
    uint32_t offer[ZSRC_ZW_EP_MAX]; /* slots offered besides the interview's (bit s = slot s) */
    uint32_t hide[ZSRC_ZW_EP_MAX];  /* slots never offered */
    uint8_t remap_from, remap_to;   /* what reports for one slot feed another (0: none) */
    uint8_t basic_slot;             /* Basic feeds this slot (0: zones.md §5's rule) */
    uint16_t idle_after_s;          /* states it never ends (0: none): each notification
                                     * state, tamper, trouble and battery variable (zones.md §8;
                                     * not a door's open/closed, mains or the CO test result) is
                                     * idled this long after it was last set, and motion, Binary
                                     * Sensor's too, this long after its last report (Z-Wave JS's
                                     * forceNotificationIdleReset); momentary events and product
                                     * moved keep their own holds. A condition's timer outlives a
                                     * link break, a state's does not */
    uint8_t lifeline_group;         /* its lifeline when not group 1 (0: the interview's) */
    uint16_t no_supervision_before; /* firmware below this (major << 8 | minor) answers supervised
                                     * commands "fail": don't supervise them (0: never); unknown
                                     * firmware counts as below */
    uint8_t flags;                  /* ZSRC_ZW_QUIRK_* */
    uint8_t config_count;
    struct {
        uint8_t number;
        uint8_t size;               /* 1, 2 or 4 */
        int32_t value;
    } config[ZSRC_ZW_QUIRK_CONFIG_MAX]; /* set during the interview (network.md §5 step 15) */
    const char *add_words;          /* what to press to add it, in the model's words */
    const char *wake_words;         /* ... to wake it */
    const char *note;               /* one line for the device page */
    uint32_t panic_scenes;          /* Central Scene (zones.md §4 slot 13, "by quirk"): bit n =
                                     * scene n (1..31) is its emergency button - pressed, held or
                                     * pressed several times, never its release; offered at the
                                     * device itself */
    zsrc_zw_alarm_map_t alarm_map[ZSRC_ZW_ALARM_MAP_MAX]; /* how it reports through the V1
                                     * Alarm Type and Level (ZW-043): its inputs are offered
                                     * from these rows too, whatever the interview found */
} zsrc_zw_quirk_t;

const zsrc_zw_quirk_t *zsrc_zw_quirk_table(size_t *count);
/* Whether a row is for a model (ZSRC_ZW_QUIRK_ANY matches any). */
bool zsrc_zw_quirk_matches(const zsrc_zw_quirk_t *q, uint16_t manufacturer,
                           uint16_t product_type, uint16_t product_id);
/* The row for a model, NULL when none. */
const zsrc_zw_quirk_t *zsrc_zw_quirk_for(uint16_t manufacturer, uint16_t product_type,
                                         uint16_t product_id);
/* Whether commands to a device of that row and firmware go without Supervision. */
bool zsrc_zw_quirk_no_supervision(const zsrc_zw_quirk_t *q, bool has_firmware, uint16_t firmware);

/* ---- the source ------------------------------------------------------------------------------ */

typedef struct {
    bool used;
    bool gone;                  /* it left the network: its zones show CHECK */
    bool ready;                 /* any endpoint offers inputs */
    uint16_t node;
    uint16_t generation;
    bool has_identity;
    uint16_t manufacturer, product_type, product_id;
    uint8_t security;           /* zsrc_zw_security_t */
    bool listening;
    uint32_t check_in_s;
    uint8_t inputs[ZSRC_ZW_EP_MAX];   /* zsrc_zw_inputs_t per endpoint */
    uint32_t base[ZSRC_ZW_EP_MAX];    /* slots its Notification, Binary and Alarm Sensor report */
    uint32_t offered[ZSRC_ZW_EP_MAX]; /* bit s: slot s offered at the endpoint */
    uint8_t binary_single[ZSRC_ZW_EP_MAX]; /* its one Binary Sensor type (a v1 report's), 0 */
    uint32_t v1_events[ZSRC_ZW_EP_MAX][ZSRC_ZW_NROW_WORDS]; /* bit r: zsrc_zwave.c's NROWS[r] is
                                    * an event its Supported Reports list, so V1 Alarm Type and
                                    * Level naming it are read as it (ZW-043, Z-Wave JS GH#1034) */
    uint32_t binary_any_ambiguous; /* Binary Sensor Report type FF ("Any") with no single supported
                                    * type at that endpoint: dropped, counted (zones.md §2) */
    uint8_t air_temp_eps;       /* endpoints with air temperature (bit e) */
    uint8_t basic_eps;          /* endpoints that list or send Basic */
    uint8_t sends_basic_eps;    /* ... that sent a Basic Set or Report */
    bool has_cover;             /* it reports a cover tamper (07 03 or 09 06) */
    const zsrc_zw_quirk_t *quirk;

    /* live, since the link came up */
    uint32_t vars[ZSRC_ZW_EP_MAX];    /* active states (zsrc_zwave.c's variables) */
    uint32_t vknown[ZSRC_ZW_EP_MAX];  /* states reported */
    uint32_t cond[ZSRC_ZW_EP_MAX];    /* its tamper, trouble and battery conditions per endpoint
                                       * (ZW21): bit k: the k-th such notification event of
                                       * zsrc_zwave.c's NROWS is its variable's current event
                                       * there; the top bit: Binary Sensor tamper (type 08). The
                                       * *_src masks below add them up for the whole device */
    uint16_t alt[ZSRC_ZW_EP_MAX];     /* bit v: active state variable v holds the second event of
                                       * its pair (Smoke 02, not 01): what an idle must name (ZW22) */
    int32_t temp_x100[ZSRC_ZW_EP_MAX];
    uint8_t temp_known;               /* bit e: temp_x100[e] is known */
    uint64_t pulse_at[ZSRC_ZW_EP_MAX][4]; /* its last glass break, impact, emergency, heat's
                                           * rapid rise (Heat 03/04) */
    uint64_t motion_until[ZSRC_ZW_EP_MAX]; /* the quirk's idle_after: when its motion ends */
    struct {
        uint64_t at;                  /* 0: a free entry */
        uint8_t ep, type, var;        /* the notification variable (zsrc_zwave.c's NROWS) */
    } idle_reset[ZSRC_ZW_IDLE_RESET_MAX]; /* the quirk's idle_after: when the others end */
    uint8_t tamper_src;               /* why it is tampered (zsrc_zwave.c): any endpoint's, or moved */
    uint64_t moved_until;             /* "product moved": its tamper ends then */
    uint8_t lowbat_src;               /* any endpoint's, or its Battery Report */
    uint16_t trouble_src;             /* any endpoint's */
    uint8_t battery_pct;              /* ZSRC_ZW_BATTERY_UNKNOWN until reported */
    bool told_tamper, told_lowbat, told_trouble; /* what its watched inputs were told */
    bool unreachable;                 /* network.md §8 said so */
    uint64_t heard_at;                /* its last frame, 0 = none since the link came up */
    uint64_t checkin_at;              /* its last check-in passed on */
    uint64_t tested_at;               /* its last test event (smoke, CO, heat, gas) */
    bool scene_seq_known;             /* Central Scene: its last sequence number */
    uint8_t scene_seq;
    uint8_t refresh;                  /* endpoints whose state is to be asked for (bit e) */
    /* the picker */
    bool new_join;                    /* added while the picker was open (best) */
    uint32_t changed[ZSRC_ZW_EP_MAX]; /* slots that changed while it was open */
} zsrc_zw_device_t;

typedef struct {
    bool used;
    uint32_t home_id;
    uint16_t generation;
    uint16_t node;
    uint8_t endpoint;
    uint8_t slot;
    uint16_t hold_s;            /* the binding's; 0 = ZSRC_ZW_HOLD_S */
    uint64_t pulse_until;       /* a momentary input is "on" until then, 0 = off */
    bool pulse_cut;             /* a link break fell in its hold: its end is told at the device's
                                 * next check-in (heat's rapid rise, ZSRC-015) */
} zsrc_zw_watch_t;

typedef struct {
    zsrc_sink_t sink;
    bool link_up;               /* the network is up and ours */
    bool jammed;                /* network.md §9: the air is jammed (the hub hears the link down,
                                 * the source keeps listening) */
    uint32_t home_id;           /* the network's */
    bool learning;              /* the picker is open */
    uint64_t now;               /* the last time it was told */
    uint32_t emitted;           /* inputs and events told (a report that told none is a check-in) */
    const zsrc_zw_quirk_t *quirks; /* rows consulted before the built-in table (NULL: none) */
    size_t quirk_count;
    zsrc_zw_device_t dev[ZSRC_ZW_DEV_MAX];
    zsrc_zw_watch_t watch[ZSRC_ZW_WATCH_MAX];
} zsrc_zw_t;

void zsrc_zw_init(zsrc_zw_t *z, const zsrc_sink_t *sink);
const zone_source_ops_t *zsrc_zw_ops(void); /* register with the zsrc_zw_t as ctx */

/* Rows consulted before the built-in table (a bench's trial rows, a test's). */
void zsrc_zw_set_quirks(zsrc_zw_t *z, const zsrc_zw_quirk_t *rows, size_t count);
/* The row for a model: those rows, then the built-in table; NULL when none. */
const zsrc_zw_quirk_t *zsrc_zw_quirk_lookup(const zsrc_zw_t *z, uint16_t manufacturer,
                                            uint16_t product_type, uint16_t product_id);

/* ---- fed by zwave_zones (or a test) ---------------------------------------------------------- */

/* The network the radio runs (its home ID): a different one than before forgets every device,
 * whose bindings then match nothing (zones.md §1). Before the devices and the link. */
void zsrc_zw_network(zsrc_zw_t *z, uint32_t home_id);
/* The link: up once the network is checked and its devices told (zsrc_zw_device), down when the
 * radio stops answering, restarts or is not this panel's. Down is one trouble for the source,
 * never a CHECK per zone (ZSRC-015); up asks every watched device for its state afresh. */
void zsrc_zw_link(zsrc_zw_t *z, bool up, uint64_t now);
/* network.md §9: the radio is jammed, or no longer. Not a link down: the source keeps its link,
 * its devices' state and their reports (a door that opens during a jam is told, a supervised
 * report answered). Only the hub hears the link down, at once (sink.link_now: the jam detector
 * already waited); when the jam ends the hub hears it up - if the real link is - and every
 * watched device is asked for its state afresh, what it said meanwhile kept (ZW-024). */
void zsrc_zw_jammed(zsrc_zw_t *z, bool jammed, uint64_t now);
/* A device's facts: when it joined or was found, after its interview, when they change. Kept per
 * node ID; another generation for the same node ID is another device. False: the table is full. */
bool zsrc_zw_device(zsrc_zw_t *z, const zsrc_zw_desc_t *d, uint64_t now);
/* It left the network (removed, reset locally, no longer in the module): its zones show CHECK. */
void zsrc_zw_device_gone(zsrc_zw_t *z, uint16_t node, uint16_t generation, uint64_t now);
/* A sleeping device's wake-up interval changed (network.md §7-8: a check-in of its own it
 * confirmed, or the interval read again after a late wake-up), s: its grace (grace_of: the
 * check-in + 10 min) and what it promises (caps_of) follow at once. 0: it no longer checks in -
 * not supervised (ZW-015: no SUPERVISED, no LIFE_SAFETY, never CHECK for its silence), its zones
 * vouched for as reachable. Nothing else of it changes, and its watched inputs are not asked again
 * (zsrc_zw_device would ask them). False, nothing changed: not that device (another generation,
 * gone), or a listening one. */
bool zsrc_zw_check_in(zsrc_zw_t *z, uint16_t node, uint16_t generation, uint32_t check_in_s);
/* network.md §8's verdict (zw_health): it can (no longer) be reached. */
void zsrc_zw_reachable(zsrc_zw_t *z, uint16_t node, bool reachable, uint64_t now);
/* A command from it, at its highest class (security.md §8), decoded. */
void zsrc_zw_report(zsrc_zw_t *z, uint16_t node, const zsrc_zw_report_t *r, uint64_t now);
/* A device whose inputs want their state asked for (the link came up, an input is newly
 * watched): its node and endpoints (bit e); false when none. Each is handed out once. */
bool zsrc_zw_take_refresh(zsrc_zw_t *z, uint16_t *node, uint8_t *endpoints);

/* ---- queries for the zone page, the device page and zwave_zones ---------------------------- */

const zsrc_zw_device_t *zsrc_zw_find(const zsrc_zw_t *z, uint16_t node);
/* The model for people: the quirk's name, else its IDs, else "Z-Wave device 5". */
void zsrc_zw_device_name(const zsrc_zw_device_t *d, char *out, size_t len);
/* The input in words ("door contact", "input 2 · water"). */
void zsrc_zw_input_words(const zsrc_zw_device_t *d, uint8_t endpoint, uint8_t slot, char *out,
                         size_t len);
/* Its state in words ("Open", "Closed", "Motion", "21.5 °C"), "" when not known. */
void zsrc_zw_state_words(const zsrc_zw_device_t *d, uint8_t endpoint, uint8_t slot, uint64_t now,
                         char *out, size_t len);
/* The device class a slot suggests (zones.md §2, the quirk's override). */
alarm_device_class_t zsrc_zw_slot_class(const zsrc_zw_device_t *d, uint8_t slot);

#ifdef __cplusplus
}
#endif

#endif /* ZSRC_ZWAVE_H */
