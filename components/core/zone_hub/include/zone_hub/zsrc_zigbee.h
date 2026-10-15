/*
 * zsrc_zigbee.h - Zigbee sensors as zones (docs/features/20, docs/features/22 ZSRC-005/-008): door and
 * window contacts, motion, water, smoke, CO, temperature and panic buttons paired straight to
 * the panel's own 802.15.4 radio, with no Wi-Fi and no Home Assistant in the way.
 *
 * This half is portable and host-tested: the device table, the IAS Zone enrollment state
 * machine (including the retries sleepy devices need), the cluster-to-zone-event mapping, the
 * quirks table and per-device supervision. The radio itself lives behind two small interfaces,
 * exactly as the wired source reaches pins through zsrc_gpio_port_t:
 *
 *   inbound   zsrc_zb_* functions below, called by components/core/zigbee_host (on the panel
 *             once its ESP32-H2 radio backend exists, docs/features/20) and directly by the host
 *             tests and the PC preview;
 *   outbound  zsrc_zb_radio_t, what the source asks the radio to do.
 *
 * Calls come with the platform's lock held (zone_service), like every other source.
 *
 * Since BL-143 (docs/features/20 devices.md §2-§3) the same header is the whole radio contract:
 * what the radio side reports about a device beyond its sensor traffic (zsrc_zb_report_t: its
 * descriptors, Basic attributes, link quality, how it joined, the end of each request) goes to
 * components/core/zigbee_zones (zbz_panel_on_report), which keeps the device registry and hands
 * the source what it needs through the calls below; and what may be asked of the radio grew by
 * the hooks appended to zsrc_zb_radio_t (leave, forget, identify, reads, bind and reporting, IAS
 * test mode, cluster commands, install codes, channel, network reset). The calls that existed
 * before are unchanged.
 *
 * Supervision, and why it is two numbers (ZB-005):
 *   the source declares a device unreachable once it has said nothing for its supervision
 *   window (2 h for a sleepy end device, 30 min for a mains-powered router); the hub then runs
 *   its grace - the binding's, else this source's ZSRC_ZB_GRACE_S - before the zone shows
 *   CHECK. The same grace is what every sensor gets to speak up again after the radio link
 *   comes back (ZSRC-015), which is why it is an hour and not a minute: a sleepy contact that
 *   checks in hourly must not turn its zone into CHECK just because the panel restarted.
 *   Both numbers are meant to be settled against real sensors on a bench.
 */
#ifndef ZSRC_ZIGBEE_H
#define ZSRC_ZIGBEE_H

#include "zone_hub/zone_source.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZSRC_ZB_DEV_MAX   128 /* device endpoints the source keeps track of (one device with three
                               * endpoints takes three); the devices themselves are zbz_registry's
                               * (ZBZ_DEV_MAX, 64): two endpoints each, and at least one per device
                               * (test_zbz_contract.c asserts it). In PSRAM on the panel. */
#define ZSRC_ZB_WATCH_MAX 128 /* device endpoints bound to zones: one per zone (ALARM_MAX_ZONES) */

#define ZSRC_ZB_SUPERVISION_S        7200u /* a sleepy end device silent this long: unreachable */
#define ZSRC_ZB_ROUTER_SUPERVISION_S 1800u /* a mains-powered router is expected to answer sooner */
#define ZSRC_ZB_GRACE_S              3600u /* the hub's grace after that, and after a link break */

#define ZSRC_ZB_LEARN_S      180u /* how long permit-join stays open (ZB-002) */
#define ZSRC_ZB_ENROLL_TRIES 5u   /* IAS enroll attempts before we give up and just listen */
#define ZSRC_ZB_ENROLL_GAP_MS 5000u /* ... and no faster than this, however chatty the device */
#define ZSRC_ZB_JAM_HOLD_MS  30000u /* channel energy high this long is jamming (ZB-006) */
#define ZSRC_ZB_JAM_DBM      (-35)  /* ... above this level */
#define ZSRC_ZB_SCAN_MS      5000u  /* request fresh energy; a stale sample cannot prove a jam */
#define ZSRC_ZB_CHECKIN_EVERY_MS 60000u /* an unchanged report is a check-in this often at most */

#define ZSRC_ZB_BATTERY_LOW_PCT 20u  /* below this, without the device saying so itself */
#define ZSRC_ZB_BATTERY_UNKNOWN 0xFFu

/* ZB-M2 (devices.md §7, testing.md §8): a sleepy device whose Poll Control check-in interval is
 * known is given at least two check-ins and this slack before it is unreachable - never less
 * than ZSRC_ZB_SUPERVISION_S, never more than ZSRC_ZB_SUPERVISION_MAX_S. */
#define ZSRC_ZB_CHECKIN_SLACK_S   600u
#define ZSRC_ZB_SUPERVISION_MAX_S 86400u
/* The hub's grace for a zone whose device the installer forgot (ZB-013): CHECK at once, as a
 * Z-Wave device that left (grace_of). */
#define ZSRC_ZB_GONE_GRACE_S      1u
#define ZSRC_ZB_FORGOTTEN_MAX     64u /* forgotten devices remembered until a restart: every
                                       * registry device (ZBZ_DEV_MAX), so a network reset or a run
                                       * of Forget leaves no zone on the hour's grace (ZB-013) */
#define ZSRC_ZB_SETUP_MAX         8u  /* devices whose setup runs at once (ADD's rows are 4) */
/* Walk-test mode (ZB-022): what each Initiate Test Mode asks for, in seconds (the ZCL field is
 * one byte of seconds: ZCL 8.2.2.3.2, esp-zigbee-lib's ias_zone.h), and how often a device still
 * in the walk test is asked again - well inside those 60 s, so it stays in test mode for the whole
 * walk test and leaves it by itself within a minute of the last ask. */
#define ZSRC_ZB_TEST_MODE_LEN     60u
#define ZSRC_ZB_TEST_REFRESH_MS   45000u
/* Tags of the requests the source makes itself (zsrc_zb_radio_t's tagged hooks): this bit set.
 * zbz_panel's own tags never set it, so a REP_DONE with it is the source's (zsrc_zb_on_report). */
#define ZSRC_ZB_TAG_SOURCE        0x80000000u
/* ZB-028 (B020): a contact whose magnet bounces sends open, closed, open... and may leave out its
 * last change. Two changes of its alarm bits this close together are a burst; once it has been
 * quiet for ZSRC_ZB_REREAD_MS the source reads its zoneStatus, and the answer - the newest state -
 * wins. An answer not in by ZSRC_ZB_REREAD_WAIT_MS (asleep, or lost) is asked again at its next
 * frame, ZSRC_ZB_REREAD_TRIES times in all. */
#define ZSRC_ZB_BURST_MS          3000u
#define ZSRC_ZB_REREAD_MS         1000u
#define ZSRC_ZB_REREAD_WAIT_MS    10000u
#define ZSRC_ZB_REREAD_TRIES      2u

/* ZCL cluster ids we speak (ZB-003, ZB-004). */
#define ZSRC_ZB_ZCL_POWER_CFG  0x0001u
#define ZSRC_ZB_ZCL_POLL_CTRL  0x0020u
#define ZSRC_ZB_ZCL_ON_OFF     0x0006u
#define ZSRC_ZB_ZCL_TEMP_MEAS  0x0402u
#define ZSRC_ZB_ZCL_OCCUPANCY  0x0406u
#define ZSRC_ZB_ZCL_IAS_ZONE   0x0500u
#define ZSRC_ZB_ZCL_TUYA       0xEF00u /* a manufacturer cluster we do not speak (ZB-007) */
/* ... and since BL-143 (devices.md §2): the device page, the setup, sirens and keypads. */
#define ZSRC_ZB_ZCL_BASIC      0x0000u
#define ZSRC_ZB_ZCL_IDENTIFY   0x0003u
#define ZSRC_ZB_ZCL_OTA        0x0019u /* a device's own updates (Tier D, BL-150) */
#define ZSRC_ZB_ZCL_IAS_ACE    0x0501u /* keypads and key fobs are its clients (ZB-024) */
#define ZSRC_ZB_ZCL_IAS_WD     0x0502u /* sirens and strobes (ZB-023) */
#define ZSRC_ZB_ZCL_DIAGNOSTICS 0x0B05u

/* Which of them an endpoint has (a bitmap, so one uint16 describes an endpoint). The same bits
 * describe an endpoint's output (client) clusters, where a keypad's IAS ACE sits. In RAM, and in
 * zbz_registry's stored records (devices.md §4): never renumber a bit, only add. */
#define ZSRC_ZB_CL_IAS_ZONE  0x0001u
#define ZSRC_ZB_CL_ON_OFF    0x0002u
#define ZSRC_ZB_CL_OCCUPANCY 0x0004u
#define ZSRC_ZB_CL_TEMP      0x0008u
#define ZSRC_ZB_CL_POWER_CFG 0x0010u
#define ZSRC_ZB_CL_POLL_CTRL 0x0020u
#define ZSRC_ZB_CL_TUYA      0x0040u
#define ZSRC_ZB_CL_IAS_WD    0x0080u
#define ZSRC_ZB_CL_IAS_ACE   0x0100u
#define ZSRC_ZB_CL_IDENTIFY  0x0200u
#define ZSRC_ZB_CL_BASIC     0x0400u
#define ZSRC_ZB_CL_OTA       0x0800u
#define ZSRC_ZB_CL_DIAGNOSTICS 0x1000u

/* IAS Zone status word (cluster 0x0500, attribute 0x0002). */
#define ZSRC_ZB_IAS_ALARM1      0x0001u
#define ZSRC_ZB_IAS_ALARM2      0x0002u
#define ZSRC_ZB_IAS_TAMPER      0x0004u
#define ZSRC_ZB_IAS_BATTERY     0x0008u
#define ZSRC_ZB_IAS_SUPERVISION 0x0010u /* the device sends supervision reports */
#define ZSRC_ZB_IAS_RESTORE     0x0020u /* the device sends restore reports */
#define ZSRC_ZB_IAS_TROUBLE     0x0040u
#define ZSRC_ZB_IAS_AC_MAINS    0x0080u
#define ZSRC_ZB_IAS_TEST        0x0100u
#define ZSRC_ZB_IAS_BATT_DEFECT 0x0200u

/* IAS Zone Type (attribute 0x0001); 0xFFFF means "not an IAS device". */
#define ZSRC_ZB_ZONE_TYPE_NONE        0xFFFFu
#define ZSRC_ZB_ZONE_TYPE_CIE         0x0000u
#define ZSRC_ZB_ZONE_TYPE_MOTION      0x000Du
#define ZSRC_ZB_ZONE_TYPE_CONTACT     0x0015u
#define ZSRC_ZB_ZONE_TYPE_HANDLE      0x0016u
#define ZSRC_ZB_ZONE_TYPE_FIRE        0x0028u
#define ZSRC_ZB_ZONE_TYPE_WATER       0x002Au
#define ZSRC_ZB_ZONE_TYPE_CO          0x002Bu
#define ZSRC_ZB_ZONE_TYPE_PERSONAL    0x002Cu /* personal emergency device */
#define ZSRC_ZB_ZONE_TYPE_VIBRATION   0x002Du
#define ZSRC_ZB_ZONE_TYPE_REMOTE      0x010Fu
#define ZSRC_ZB_ZONE_TYPE_KEYFOB      0x0115u
#define ZSRC_ZB_ZONE_TYPE_KEYPAD      0x021Du
#define ZSRC_ZB_ZONE_TYPE_WARNING     0x0225u
#define ZSRC_ZB_ZONE_TYPE_GLASS_BREAK 0x0226u

/* What a brand gets wrong, as data rather than code (ZB-007). */
#define ZSRC_ZB_QUIRK_NO_ENROLL   0x0001u /* reports zone status without ever enrolling */
#define ZSRC_ZB_QUIRK_ALARM2      0x0002u /* the alarm is on Alarm2, not Alarm1 */
#define ZSRC_ZB_QUIRK_BOTH_ALARMS 0x0004u /* either alarm bit means faulted (the default) */
#define ZSRC_ZB_QUIRK_NO_TAMPER   0x0008u /* the tamper bit is noise on this model: ignore it */
#define ZSRC_ZB_QUIRK_ENROLL_WAKE 0x0010u /* only answers enrollment inside a wake window */
#define ZSRC_ZB_QUIRK_UNSUPPORTED 0x0020u /* speaks a manufacturer cluster: use Home Assistant */
#define ZSRC_ZB_QUIRK_INVERT      0x0040u /* the alarm bit is the other way round on this model */

/* How a device joined (ZB-021, the per-device trust record). Stored in zbz_registry's records:
 * never renumber, only add. Every way may feed any zone, fire, CO and panic included (decided
 * 2026-10-02): it is shown on the device's page, not used to refuse it. */
typedef enum {
    ZSRC_ZB_JOIN_UNKNOWN = 0,    /* joined before the panel recorded it, or found again by its
                                  * traffic after the record was lost */
    ZSRC_ZB_JOIN_INSTALL_CODE,   /* its link key came from the install code on its label */
    ZSRC_ZB_JOIN_STANDARD_KEY,   /* the Zigbee 3.0 well-known link key */
} zsrc_zb_join_t;

typedef enum {
    ZSRC_ZB_ENROLL_NONE = 0, /* not an IAS device, or nothing to do yet */
    ZSRC_ZB_ENROLL_CIE,      /* the CIE address still has to be written */
    ZSRC_ZB_ENROLL_RESPOND,  /* an enroll response is owed: send it in a wake window */
    ZSRC_ZB_ENROLL_DONE,
    ZSRC_ZB_ENROLL_FAILED, /* out of tries; the device may still report its status */
} zsrc_zb_enroll_t;

/* One row of the quirks table. NULL or "" matches anything; otherwise it is a prefix match, so
 * "_TZ3000_" catches a whole family of Tuya radios. */
typedef struct {
    const char *manufacturer;
    const char *model;
    uint32_t quirks;         /* ZSRC_ZB_QUIRK_* */
    uint8_t device_class;    /* alarm_device_class_t; GENERIC keeps what the device said */
    uint16_t supervision_s;  /* 0 = the default for this device's power source */
    const char *note;        /* one line for the zone page and the console */
} zsrc_zb_quirk_t;

/* What zigbee_host tells us about a device endpoint when it joins or is rediscovered. */
typedef struct {
    uint64_t ieee;
    uint16_t nwk;      /* short address, for logs */
    uint8_t endpoint;  /* 1..240 */
    uint16_t clusters; /* ZSRC_ZB_CL_* found on it */
    uint16_t zone_type; /* IAS Zone Type, ZSRC_ZB_ZONE_TYPE_NONE when it is not an IAS device */
    bool router;       /* mains powered (a router), not a sleepy end device */
    char manufacturer[32];
    char model[32];
    uint8_t join;      /* zsrc_zb_join_t (BL-143; ZSRC_ZB_JOIN_UNKNOWN from older callers) */
} zsrc_zb_desc_t;

typedef struct {
    bool used;
    uint64_t ieee;
    uint16_t nwk;
    uint8_t endpoint;
    uint16_t clusters;
    uint16_t zone_type;
    bool router;
    char manufacturer[32];
    char model[32];
    uint8_t device_class; /* alarm_device_class_t, after the quirks table has had its say */
    uint32_t quirks;
    const char *note; /* from the quirks table, NULL when the device is ordinary */
    uint16_t supervision_s;

    /* live */
    uint8_t enroll;     /* zsrc_zb_enroll_t */
    uint8_t tries;      /* enrollment attempts so far */
    uint8_t zone_id;    /* the IAS zone id we gave it */
    uint16_t status;    /* last IAS Zone status word */
    bool status_known;
    bool on;            /* last On/Off or Occupancy state */
    bool on_known;
    int32_t value_x100; /* last measured value (temperature) */
    bool has_value;
    uint8_t battery_pct; /* ZSRC_ZB_BATTERY_UNKNOWN when it has never said */
    bool battery_low;    /* what the engine was told */
    uint8_t lqi;         /* 0 = unknown */
    uint64_t heard_at;   /* last frame, 0 = nothing since the link came up */
    uint64_t checkin_at; /* last check-in passed on to the hub */
    uint64_t next_try;   /* earliest next enrollment attempt */
    bool unreachable;    /* the sink was told so */
    bool new_join;       /* joined during this learn window: offer it first (ZB-002) */
    uint8_t join;        /* zsrc_zb_join_t, from its zsrc_zb_desc_t (BL-143) */
    bool test_mode;      /* asked into IAS test mode for a walk test (ZB-022) */

    /* ---- ZB-M2: what the radio's reports added (zsrc_zb_on_report); RAM only ---- */
    int8_t rssi;          /* dBm of the last frame, 0 = not measured (REP_LINK) */
    bool node_known;      /* a node descriptor (or an announce) said what it is */
    uint8_t logical_type; /* ZSRC_ZB_LOGICAL_*, when node_known */
    uint8_t mac_caps;     /* ZSRC_ZB_MAC_*, when node_known */
    uint8_t power_source; /* Basic 0x0007 (1 mains, 3 battery, 4 DC...; bit 7 a backup battery);
                           * 0 not known */
    uint32_t check_in_s;  /* Poll Control's check-in interval, 0 none or not read (ZB-005) */
    uint8_t test;         /* zsrc_zb_test_t: where its walk-test mode stands (ZB-022) */
    uint64_t test_at;     /* when it was last asked into or out of test mode */
    uint32_t test_tag;    /* the tag of that request (ZSRC_ZB_TAG_SOURCE set) */
    bool test_masked;     /* a trip was told as ALARM_ZEV_TEST: the zone is told where it
                           * stands once its trips are no longer tests (ZB-022) */

    /* ---- ZB-028 (B020): a burst's re-read; RAM only ---- */
    uint8_t burst_n;      /* alarm-bit changes in the current burst */
    uint8_t reread;       /* zsrc_zb_reread_t */
    uint8_t reread_tries; /* reads sent for this burst */
    uint64_t burst_at;    /* the burst's last change */
    uint64_t reread_at;   /* REREAD_DUE: when to ask; REREAD_ASKED: when it was asked */
} zsrc_zb_device_t;

/* Where a burst's re-read of a device's zoneStatus stands (ZB-028). */
typedef enum {
    ZSRC_ZB_REREAD_NONE = 0,   /* nothing owed */
    ZSRC_ZB_REREAD_DUE,        /* ask at reread_at, once the burst has settled */
    ZSRC_ZB_REREAD_ASKED,      /* asked at reread_at; the next status is the answer */
    ZSRC_ZB_REREAD_NEXT_FRAME, /* no answer: ask again at its next frame (a router: now) */
} zsrc_zb_reread_t;

/* Where a device endpoint's IAS test mode stands during and after a walk test (ZB-022). */
typedef enum {
    ZSRC_ZB_TEST_NONE = 0,  /* normal operation, nothing owed */
    ZSRC_ZB_TEST_OWED_ON,   /* to be asked into test mode at its next chance */
    ZSRC_ZB_TEST_ASKED_ON,  /* asked; no answer yet (a radio without REP_DONE stays here) */
    ZSRC_ZB_TEST_ON,        /* it confirmed */
    ZSRC_ZB_TEST_OWED_OFF,  /* to be asked back to normal operation */
    ZSRC_ZB_TEST_ASKED_OFF, /* asked back; no answer yet */
    ZSRC_ZB_TEST_REFUSED,   /* it has no test mode (UNSUPPORTED/REFUSED): not asked again */
} zsrc_zb_test_t;

typedef struct {
    bool used;
    uint64_t ieee;
    uint8_t endpoint;
    bool invert;   /* from the binding's mapping: flips the alarm bit */
    uint8_t mode;  /* zsrc_map_mode_t: BINARY/STATES want the alarm, ABOVE/BELOW the value */
    bool walk;     /* the running walk test covers every zone this endpoint feeds
                    * (zsrc_zb_walk_test_input; ZB-022): only then is it asked into test mode */
} zsrc_zb_watch_t;

/* ==== The radio contract's additions (BL-143; docs/features/20 devices.md §2-§3) ===============
 * The radio side (zigbee_service's radio_port.c, the host tests' fake radio, the preview's
 * pretend one) reports what it learns as zsrc_zb_report_t to zbz_panel_on_report
 * (components/core/zigbee_zones), which hands each one to zsrc_zb_on_report first; from the
 * service's task with the zone lock held - never from a stack callback, which only queues
 * (docs/features/20 Design). */

/* The node descriptor's logical type and MAC capability flags (Zigbee spec 2.3.2.3, 2.3.2.3.6). */
#define ZSRC_ZB_LOGICAL_COORDINATOR 0u
#define ZSRC_ZB_LOGICAL_ROUTER      1u
#define ZSRC_ZB_LOGICAL_END_DEVICE  2u
#define ZSRC_ZB_MAC_ALT_PAN     0x01u
#define ZSRC_ZB_MAC_FFD         0x02u /* a full-function device: it can route */
#define ZSRC_ZB_MAC_MAINS       0x04u
#define ZSRC_ZB_MAC_RX_ON_IDLE  0x08u /* listens all the time: not a sleepy device */
#define ZSRC_ZB_MAC_SECURITY    0x40u
#define ZSRC_ZB_MAC_ALLOC_ADDR  0x80u

#define ZSRC_ZB_EP_MAX           8u  /* endpoints a report lists (the Green Power proxy's 242 too) */
#define ZSRC_ZB_EP_CLUSTERS_MAX  16u /* clusters per list a report carries; more: `truncated` */
#define ZSRC_ZB_ZCL_PAYLOAD_MAX  48u /* a cluster command's payload (an ACE Arm with a 10-digit code
                                      * is 13 bytes; a Zone Info Response 20) */
#define ZSRC_ZB_TSN_NEW          0xFFFFu /* zsrc_zb_zcl_t.tsn: the stack's next sequence number */

typedef struct {
    uint8_t logical_type;       /* ZSRC_ZB_LOGICAL_* */
    uint8_t mac_caps;           /* ZSRC_ZB_MAC_* */
    uint16_t manufacturer_code; /* the Zigbee manufacturer code (Frient 0x1015) */
    uint16_t server_mask;
    uint16_t max_in_transfer;   /* the largest incoming transfer it takes, bytes */
} zsrc_zb_node_desc_t;

typedef struct {
    uint8_t current_mode;       /* 0 receiver on when idle, 1 periodic, 2 stimulated */
    uint8_t available_sources;  /* bit 0 mains, bit 1 rechargeable, bit 2 disposable battery */
    uint8_t current_source;     /* the same bits */
    uint8_t level;              /* 0 critical, 4 = 33 %, 8 = 66 %, 12 = 100 % (spec 2.3.2.4) */
} zsrc_zb_power_desc_t;

/* One endpoint's simple descriptor, its cluster lists as they came (ZCL ids). */
typedef struct {
    uint8_t endpoint;
    uint16_t profile;           /* 0x0104 Home Automation; others recorded, not used */
    uint16_t device_id;         /* 0x0402 IAS Zone, 0x0403 IAS Warning Device, 0x0401 IAS ACE */
    uint8_t version;
    uint8_t in_count, out_count;
    uint16_t in[ZSRC_ZB_EP_CLUSTERS_MAX];
    uint16_t out[ZSRC_ZB_EP_CLUSTERS_MAX];
    bool truncated;             /* it listed more than ZSRC_ZB_EP_CLUSTERS_MAX in a list */
} zsrc_zb_simple_desc_t;

/* The Basic cluster's attributes the device page and the registry keep. */
#define ZSRC_ZB_BASIC_ZCL_VERSION 0x0001u /* zsrc_zb_basic_t.have */
#define ZSRC_ZB_BASIC_APP_VERSION 0x0002u
#define ZSRC_ZB_BASIC_HW_VERSION  0x0004u
#define ZSRC_ZB_BASIC_MANUFACTURER 0x0008u
#define ZSRC_ZB_BASIC_MODEL       0x0010u
#define ZSRC_ZB_BASIC_DATE_CODE   0x0020u
#define ZSRC_ZB_BASIC_POWER       0x0040u
#define ZSRC_ZB_BASIC_SW_BUILD    0x0080u
typedef struct {
    uint16_t have;              /* ZSRC_ZB_BASIC_*: what the answer held */
    uint8_t zcl_version;        /* 0x0000 */
    uint8_t app_version;        /* 0x0001 */
    uint8_t hw_version;         /* 0x0003 */
    uint8_t power_source;       /* 0x0007: 1 mains, 3 battery, 4 DC...; bit 7 a backup battery */
    char manufacturer[32];      /* 0x0004 */
    char model[32];             /* 0x0005 */
    char date_code[16];         /* 0x0006 */
    char sw_build[24];          /* 0x4000 */
} zsrc_zb_basic_t;

/* The steps of a device's setup (its interview, devices.md §5), in order. Stored in
 * zbz_registry's records (where a setup stopped; registry_fixture_v1.h): the numbers are fixed,
 * never renumber, only add after DONE. */
typedef enum {
    ZSRC_ZB_STEP_NONE = 0,
    ZSRC_ZB_STEP_NODE_DESC = 1,     /* Node_Desc_req: router or sleepy, manufacturer code */
    ZSRC_ZB_STEP_ACTIVE_EP = 2,     /* Active_EP_req */
    ZSRC_ZB_STEP_SIMPLE_DESC = 3,   /* Simple_Desc_req, once per endpoint */
    ZSRC_ZB_STEP_BASIC = 4,         /* Basic: manufacturer, model, versions, power source */
    ZSRC_ZB_STEP_POWER_DESC = 5,    /* Power_Desc_req: asked only when Basic has no power source */
    ZSRC_ZB_STEP_IAS_ZONE = 6,      /* IAS Zone: zone type, zone state, the CIE address it holds */
    ZSRC_ZB_STEP_CIE_WRITE = 7,     /* the panel's address written as its CIE address */
    ZSRC_ZB_STEP_ENROLL = 8,        /* the enroll answered (zsrc_zb_enroll_t DONE or NO_ENROLL) */
    ZSRC_ZB_STEP_BIND = 9,          /* its clusters bound to the panel's endpoint */
    ZSRC_ZB_STEP_REPORTING = 10,     /* Configure Reporting for battery, temperature, on/off */
    ZSRC_ZB_STEP_POLL_CTRL = 11,     /* a sleepy device's check-in interval read */
    ZSRC_ZB_STEP_DONE = 12,
} zsrc_zb_step_t;

/* How a request the source made ended (ZSRC_ZB_REP_DONE). */
typedef enum {
    ZSRC_ZB_DONE_OK = 0,
    ZSRC_ZB_DONE_TIMEOUT,       /* no answer in the stack's time (a sleepy device, out of range) */
    ZSRC_ZB_DONE_UNSUPPORTED,   /* it answered: no such attribute, command or cluster */
    ZSRC_ZB_DONE_REFUSED,       /* it answered with another failure status (`zcl_status`) */
    ZSRC_ZB_DONE_SEND_FAILED,   /* the stack couldn't send it (no route, queue full) */
    ZSRC_ZB_DONE_NO_RADIO,      /* the radio went away meanwhile */
} zsrc_zb_done_t;

/* A cluster-specific command, either way (ZB-023, ZB-024): what the siren and keypad policies
 * build (components/core/zigbee_zones zbz_zcl.h encodes the bytes) and what a keypad sends. */
typedef struct {
    uint16_t cluster;
    uint8_t command;
    bool server_to_client;      /* the direction bit: the panel answers as the cluster's server */
    bool want_response;         /* ask for a Default Response (off for ACE answers) */
    uint16_t manufacturer;      /* 0: a standard command */
    uint16_t tsn;               /* out: ZSRC_ZB_TSN_NEW, or the request's (an Arm Response); in:
                                 * the sender's 0..255 */
    uint8_t len;
    uint8_t payload[ZSRC_ZB_ZCL_PAYLOAD_MAX];
} zsrc_zb_zcl_t;

/* One attribute's reporting, as Configure Reporting sets it (devices.md §5 step REPORTING). */
typedef struct {
    uint16_t cluster;
    uint16_t attr;
    uint8_t type;               /* the ZCL data type (0x20 uint8, 0x29 int16, 0x10 bool...) */
    uint16_t min_s, max_s;      /* max_s is the device's check-in for this attribute */
    uint32_t change;            /* the reportable change, for analog types; 0 for discrete */
} zsrc_zb_reporting_t;

/* What the radio side reports (zbz_panel_on_report). */
typedef enum {
    ZSRC_ZB_REP_NONE = 0,
    ZSRC_ZB_REP_JOINED,         /* a device joined or rejoined through the trust center:
                                 * u.joined (its key, rejoin, parent) */
    ZSRC_ZB_REP_ANNOUNCE,       /* Device_annce: `nwk` is its address now (u.announce) */
    ZSRC_ZB_REP_LEFT,           /* it left by itself, or was told to (u.left) */
    ZSRC_ZB_REP_JOIN_REFUSED,   /* the trust center turned a device away (u.refused) */
    ZSRC_ZB_REP_NODE_DESC,      /* u.node */
    ZSRC_ZB_REP_POWER_DESC,     /* u.power */
    ZSRC_ZB_REP_ACTIVE_EP,      /* u.active */
    ZSRC_ZB_REP_SIMPLE_DESC,    /* u.simple */
    ZSRC_ZB_REP_BASIC,          /* u.basic: a read answer or a report of Basic attributes */
    ZSRC_ZB_REP_LINK,           /* the last frame's link quality (u.link) */
    ZSRC_ZB_REP_CHECK_IN,       /* a Poll Control check-in: the device listens for a moment */
    ZSRC_ZB_REP_ZCL,            /* a cluster command the source doesn't decode (u.zcl: an IAS
                                 * ACE command from a keypad, a Default Response to the panel's) */
    ZSRC_ZB_REP_DONE,           /* the request `tag` ended (u.done) */
    ZSRC_ZB_REP_NETWORK,        /* the network as formed or changed (u.network); ieee 0 */
    ZSRC_ZB_REP_SURVEY,         /* the channels' energy (u.survey); ieee 0 */
    ZSRC_ZB_REP_AUTHORIZED,     /* the trust center saw a joined device's own key exchange end
                                 * (after a JOINED with key_pending; B021) */
} zsrc_zb_report_kind_t;

#define ZSRC_ZB_CHANNELS 4u     /* the panel's channels: 11, 15, 20, 25 (ALARM_ZB_CHANNEL_MASK) */

typedef struct {
    uint8_t kind;               /* zsrc_zb_report_kind_t */
    uint64_t ieee;              /* the device (0: the network itself) */
    uint16_t nwk;
    uint8_t endpoint;           /* where it applies (0: the device) */
    uint32_t tag;               /* the request it answers (0: unsolicited) */
    union {
        struct {
            uint8_t join;       /* zsrc_zb_join_t */
            bool rejoin;        /* a rejoin of a device the stack knew (its key kept) */
            uint16_t parent;    /* the router it joined through (0x0000 the panel) */
            bool key_pending;   /* a fresh join told from its announce, before the trust center
                                 * saw its key exchange end: the device may still be asking for
                                 * its own link key (AUTHORIZED follows if it ends; B021) */
        } joined;
        struct {
            uint8_t mac_caps;
        } announce;
        struct {
            bool rejoin;        /* it said it will come back (a rejoin request) */
            bool asked;         /* the answer to the panel's leave (tag says which) */
        } left;
        struct {
            uint8_t why;        /* 0 not said, 1 no install code matched while codes are
                                 * required, 2 an unknown device's rejoin refused */
        } refused;
        zsrc_zb_node_desc_t node;
        zsrc_zb_power_desc_t power;
        struct {
            uint8_t count;
            uint8_t ep[ZSRC_ZB_EP_MAX];
        } active;
        zsrc_zb_simple_desc_t simple;
        zsrc_zb_basic_t basic;
        struct {
            uint8_t lqi;        /* 0..255 as the radio measured it; 0 not measured */
            int8_t rssi;        /* dBm; 0 not measured */
        } link;
        zsrc_zb_zcl_t zcl;
        struct {
            uint8_t step;       /* zsrc_zb_step_t it belonged to (NONE: not a setup step) */
            uint8_t result;     /* zsrc_zb_done_t */
            uint8_t zcl_status; /* the status it answered (REFUSED), else 0 */
        } done;
        struct {
            uint8_t channel;
            uint16_t pan_id;
            uint64_t ext_pan_id;
            uint64_t panel_ieee; /* the coordinator's own address: every sensor's CIE address */
            uint8_t update_id;   /* the network's update id: raised by each channel change */
        } network;
        struct {
            uint8_t known;      /* bit i: dbm[i] was measured */
            int8_t dbm[ZSRC_ZB_CHANNELS]; /* channels 11, 15, 20, 25 in that order */
        } survey;
    } u;
} zsrc_zb_report_t;

/* What the source asks the radio to do. Every call may fail (the device is asleep, the radio is
 * busy): the source retries in the device's next wake window.
 *
 * The hooks after energy_scan are the BL-143 additions (devices.md §3): each takes a non-zero
 * `tag` that comes back in the ZSRC_ZB_REP_DONE report ending it (and in the answer's own report),
 * so zbz_panel can follow one request per device; false means it could not even be queued. A
 * radio that leaves one NULL can't do it: the request asking for it answers UNSUPPORTED. They are
 * called with the zone lock held and take the stack's lock briefly (never the other way round). */
typedef struct {
    void *ctx;
    /* Open the join window for 'seconds', or close it with 0 (ZB-002). */
    bool (*permit_join)(void *ctx, uint16_t seconds);
    /* Write the coordinator's IEEE into the device's IAS CIE Address attribute. */
    bool (*ias_cie_write)(void *ctx, uint64_t ieee, uint8_t endpoint);
    /* Answer a Zone Enroll Request with success and the zone id we assigned. */
    bool (*ias_enroll_response)(void *ctx, uint64_t ieee, uint8_t endpoint, uint8_t zone_id);
    /* Ask for one attribute now (used to prod a router that has gone quiet). */
    bool (*read_attribute)(void *ctx, uint64_t ieee, uint8_t endpoint, uint16_t cluster,
                           uint16_t attr);
    /* Tell a device to leave the network (unbinding it from its zone does not do this). */
    bool (*remove_device)(void *ctx, uint64_t ieee);
    /* Start one energy scan of the network's channel; the answer comes back as zsrc_zb_energy. */
    bool (*energy_scan)(void *ctx);

    /* ---- BL-143 (devices.md §3) ---- */
    /* The setup's ZDO questions (ZB-012); answers as REP_NODE_DESC, _POWER_DESC, _ACTIVE_EP,
     * _SIMPLE_DESC, then REP_DONE. */
    bool (*node_desc_req)(void *ctx, uint64_t ieee, uint16_t nwk, uint32_t tag);
    bool (*power_desc_req)(void *ctx, uint64_t ieee, uint16_t nwk, uint32_t tag);
    bool (*active_ep_req)(void *ctx, uint64_t ieee, uint16_t nwk, uint32_t tag);
    bool (*simple_desc_req)(void *ctx, uint64_t ieee, uint16_t nwk, uint8_t endpoint,
                            uint32_t tag);
    /* Read up to 8 attributes of one cluster in one frame (Basic for the setup; Basic's ZCL
     * version is "Check it now", ZB-014). Answers as REP_BASIC for Basic, else as the source's
     * own inbound calls (zsrc_zb_attr_*, zsrc_zb_battery), then REP_DONE. */
    bool (*read_attributes)(void *ctx, uint64_t ieee, uint8_t endpoint, uint16_t cluster,
                            const uint16_t *attrs, uint8_t count, uint32_t tag);
    /* Bind the device's `cluster` at `endpoint` to the panel's endpoint (ZDO Bind_req). */
    bool (*bind)(void *ctx, uint64_t ieee, uint8_t endpoint, uint16_t cluster, uint32_t tag);
    /* Configure Reporting of one attribute. */
    bool (*configure_reporting)(void *ctx, uint64_t ieee, uint8_t endpoint,
                                const zsrc_zb_reporting_t *r, uint32_t tag);
    /* Ask it to leave (Mgmt_Leave_req, rejoin off; ZB-013): REP_DONE OK when it answered, then
     * REP_LEFT; TIMEOUT when it didn't (the page offers Forget). Replaces remove_device for the
     * pages; remove_device stays for the source's own use. */
    bool (*leave)(void *ctx, uint64_t ieee, uint16_t nwk, uint32_t tag);
    /* Forget it without its answer (ZB-013): its entries in the stack's tables (address map,
     * link key, install code) go, as far as the stack allows (devices.md §6 says what remains).
     * Synchronous: true when the stack took it. */
    bool (*forget)(void *ctx, uint64_t ieee);
    /* Identify for `seconds` (ZB-014): the device blinks or beeps itself - never the panel. */
    bool (*identify)(void *ctx, uint64_t ieee, uint8_t endpoint, uint16_t seconds, uint32_t tag);
    /* IAS Zone Initiate Test Mode for `seconds` (ZB-022), or with 0 Initiate Normal Operation
     * Mode. */
    bool (*ias_test_mode)(void *ctx, uint64_t ieee, uint8_t endpoint, uint8_t seconds,
                          uint32_t tag);
    /* A cluster command the siren and keypad policies built (ZB-023, ZB-024). */
    bool (*zcl_command)(void *ctx, uint64_t ieee, uint8_t endpoint, const zsrc_zb_zcl_t *cmd,
                        uint32_t tag);
    /* Give the trust center an install code for `ieee` (ZB-015; `code` as on the label with its
     * CRC, 8, 10, 14 or 18 bytes), or take it back (len 0). Synchronous. */
    bool (*install_code)(void *ctx, uint64_t ieee, const uint8_t *code, uint8_t len);
    /* While true, the trust center admits only devices whose install code it holds (the "Add
     * with an install code" page, ZB-015); false: any Zigbee 3.0 device during the window. */
    bool (*codes_only)(void *ctx, bool on);
    /* Move the network to `channel` (11, 15, 20 or 25; ZB-016): Mgmt_NWK_Update_req to every
     * device, then the panel's own radio. REP_DONE when the panel's radio is on it; REP_NETWORK
     * follows. */
    bool (*set_channel)(void *ctx, uint8_t channel, uint32_t tag);
    /* Leave the network and form a new one (ZB-017): new keys, new PAN id; every device must be
     * added again. REP_DONE, then REP_NETWORK. */
    bool (*reset_network)(void *ctx, uint32_t tag);
    /* Energy of each of the panel's channels (11, 15, 20, 25) for the Channel page (ZB-016): one
     * REP_SURVEY, then REP_DONE. Not while devices are joining. */
    bool (*channel_survey)(void *ctx, uint32_t tag);
} zsrc_zb_radio_t;

typedef struct {
    zsrc_sink_t sink;
    zsrc_zb_radio_t radio;
    bool link_up;   /* the radio answers and the network is formed */
    bool jammed;    /* the channel has been drowned out (ZB-006) */
    bool told_up;   /* what the hub was last told: link_up && !jammed */
    bool learning;  /* the join window is open */
    uint64_t learn_until;
    uint64_t link_at;   /* when the link last came up: where supervision counts from */
    uint64_t jam_since; /* when the energy first went over the threshold, 0 = quiet */
    uint64_t energy_at;
    uint64_t next_scan;
    bool energy_high;
    int8_t energy_dbm;
    uint16_t supervision_s;        /* sleepy end devices; 0 = ZSRC_ZB_SUPERVISION_S */
    uint16_t router_supervision_s; /* routers; 0 = ZSRC_ZB_ROUTER_SUPERVISION_S */
    zsrc_zb_device_t dev[ZSRC_ZB_DEV_MAX];
    zsrc_zb_watch_t watch[ZSRC_ZB_WATCH_MAX];

    /* ---- ZB-M2 ---- */
    bool walk_test;      /* a walk test is on (zsrc_zb_walk_test) */
    uint16_t tag_seq;    /* the source's own request tags */
    uint8_t channel;     /* the network's, from REP_NETWORK; 0 not known */
    uint64_t panel_ieee; /* the coordinator's address, from REP_NETWORK; 0 not known */
    uint64_t setup[ZSRC_ZB_SETUP_MAX];         /* devices whose setup runs (zsrc_zb_device_setup) */
    uint64_t forgotten[ZSRC_ZB_FORGOTTEN_MAX]; /* devices the installer forgot, newest last */
} zsrc_zb_t;

void zsrc_zb_init(zsrc_zb_t *z, const zsrc_zb_radio_t *radio, const zsrc_sink_t *sink);
const zone_source_ops_t *zsrc_zb_ops(void); /* register with the zsrc_zb_t as ctx */

/* ---- fed by zigbee_host (or a test) --------------------------------------------------------- */

/* The radio answers and the network is formed (up), or it has stopped answering (down). Down is
 * one supervision trouble for the source, never a CHECK per zone (ZONE-008, ZSRC-015). */
void zsrc_zb_link(zsrc_zb_t *z, bool up, uint64_t now);

/* A device endpoint joined, or was found again after a restart. Returns false when the table is
 * full. Called again for the same endpoint, it updates what we know without losing its state. */
bool zsrc_zb_device_seen(zsrc_zb_t *z, const zsrc_zb_desc_t *d, uint64_t now);

/* The device left the network (or was removed): its zones go unreachable at once. */
void zsrc_zb_device_gone(zsrc_zb_t *z, uint64_t ieee, uint64_t now);

/* Any frame from a device: it is alive, and it is awake now (ZB-003: the moment to retry a
 * sleepy device's enrollment). lqi 0 means "not measured". */
void zsrc_zb_frame_seen(zsrc_zb_t *z, uint64_t ieee, uint8_t lqi, uint64_t now);

/* A Zone Enroll Request (or the zone type read back from the device). */
void zsrc_zb_ias_enroll_request(zsrc_zb_t *z, uint64_t ieee, uint8_t endpoint, uint16_t zone_type,
                                uint64_t now);

/* A Zone Status Change Notification, or the status attribute reported. */
void zsrc_zb_ias_status(zsrc_zb_t *z, uint64_t ieee, uint8_t endpoint, uint16_t status,
                        uint64_t now);

/* On/Off (0x0006) or Occupancy (0x0406) for contacts and motion sensors that are not IAS. */
void zsrc_zb_attr_bool(zsrc_zb_t *z, uint64_t ieee, uint8_t endpoint, uint16_t cluster, bool on,
                       uint64_t now);

/* A measured value, in hundredths of its unit (Temperature Measurement 0x0402 reports 0.01 C). */
void zsrc_zb_attr_value(zsrc_zb_t *z, uint64_t ieee, uint8_t endpoint, uint16_t cluster,
                        int32_t value_x100, uint64_t now);

/* Power Configuration battery percentage, 0..100 (ZCL reports it doubled; zigbee_host halves
 * it). ZSRC_ZB_BATTERY_UNKNOWN forgets it. */
void zsrc_zb_battery(zsrc_zb_t *z, uint64_t ieee, uint8_t percent, uint64_t now);

/* The result of an energy scan of the network's channel (ZB-006). */
void zsrc_zb_energy(zsrc_zb_t *z, int8_t dbm, uint64_t now);

/* ---- since BL-143 (devices.md §5, §6; built in ZB-M2) ------------------------------------------ */

/* The device was removed or forgotten by the installer (ZB-013), not just gone: every endpoint
 * slot it had is freed, and the zones it fed go unreachable at once (their bindings stay until
 * the installer gives the zone another sensor, or Replace moves them) and CHECK after
 * ZSRC_ZB_GONE_GRACE_S (grace_of). Nothing it said replays, and its traffic no longer brings it
 * back as a candidate until it is described again (zsrc_zb_device_seen, after a new join). The
 * source does not call the radio's forget: zbz_panel does. */
void zsrc_zb_device_forget(zsrc_zb_t *z, uint64_t ieee, uint64_t now);

/* A device's setup is under way (true) or over (false): while it runs, the source does not start
 * its own enrollment for it - zbz_panel's setup does the CIE write and enrollment in order
 * (devices.md §5) - but still takes its reports. It may be called before the device is described
 * (zsrc_zb_device_seen); at most ZSRC_ZB_SETUP_MAX at once (beyond that the source enrolls the
 * device itself, which only repeats a CIE write). Starting, each IAS endpoint gets its zone id
 * (zsrc_zb_device()->zone_id: the one the setup's Enroll Response carries). Over: an IAS endpoint
 * the setup did not enroll (zsrc_zb_ias_enrolled) goes back to the source's own patient
 * enrollment, as before BL-143. */
void zsrc_zb_device_setup(zsrc_zb_t *z, uint64_t ieee, bool running, uint64_t now);

/* A walk test started or ended (ZB-022, docs/features/40 WALK-007). The walk test has a scope -
 * the zones of the partitions testing, of the types it covers (alarm_zone_type_walk_counts) - so
 * on alone asks nothing: each input the scope covers is named with zsrc_zb_walk_test_input. Such a
 * bound and enrolled IAS sensor is asked into IAS test mode (radio ias_test_mode; a listening one
 * at once, a sleepy one at its next frame; again every ZSRC_ZB_TEST_REFRESH_MS while the test
 * lasts) and its TEST bit is reported as ALARM_ZEV_TEST. Off clears every input's scope, and each
 * device asked in is asked back to normal operation. Only while the walk test lasts, its input is
 * in the scope and the source has asked the device into test mode (ZSRC_ZB_TEST_ASKED_ON or
 * ZSRC_ZB_TEST_ON) does a status word with the TEST bit turn a fault into ALARM_ZEV_TEST. Outside
 * that - another partition's sensor, a fire or CO detector, after the walk test while the device
 * has not yet heard its Normal Operation request, or a device that set the bit by itself - a trip
 * is ALARM_ZEV_FAULT whatever the TEST bit says, and the engine decides (a covered zone is marked,
 * fire and CO stay live, an armed partition alarms): a fire, CO or panic sensor left in test mode
 * still sounds its alarm (fail-safe). Once its trips are no
 * longer tests, a zone told a test while its sensor is still open is told the fault at once. */
void zsrc_zb_walk_test(zsrc_zb_t *z, bool on, uint64_t now);

/* One input in or out of the running walk test's scope (ZB-022; since the review of wave 6,
 * additive): `on` only when every zone the endpoint feeds is covered (zbz_panel_walk_scope works
 * it out from the hub's bindings). Not bound, or no walk test running: nothing is asked, and the
 * scope is kept only while the binding lasts (a new binding starts out of it). */
void zsrc_zb_walk_test_input(zsrc_zb_t *z, const zsrc_address_t *a, bool on, uint64_t now);

/* ---- since ZB-M2 (additive) -------------------------------------------------------------------- */

/* A report from the radio side, as zbz_panel_on_report gets it (zbz_panel forwards each one
 * here first). The source takes what concerns its endpoints: LINK (LQI and RSSI), NODE_DESC and
 * ANNOUNCE (router or sleepy), POWER_DESC, SIMPLE_DESC (an endpoint's clusters), BASIC
 * (manufacturer, model, power source; the quirks are looked up again), JOINED (how it joined; a
 * fresh join of a known address forgets what it said before), LEFT (with a rejoin: its zones
 * lose it until it speaks again; without: as zsrc_zb_device_gone), CHECK_IN and ZCL (it is
 * awake), NETWORK (channel, the panel's address); ACTIVE_EP is it answering. JOIN_REFUSED and
 * SURVEY are not the source's. True when the report was the source's own (a REP_DONE for a tag
 * with ZSRC_ZB_TAG_SOURCE): nothing else need look at it. */
bool zsrc_zb_on_report(zsrc_zb_t *z, const zsrc_zb_report_t *r, uint64_t now);

/* The setup's ENROLL step finished (the device took its Enroll Response, or it needs none:
 * ZSRC_ZB_QUIRK_NO_ENROLL): the source does not chase it again. */
void zsrc_zb_ias_enrolled(zsrc_zb_t *z, uint64_t ieee, uint8_t endpoint, uint64_t now);

/* Poll Control's Check-in Interval, in seconds (the attribute is quarter-seconds: the caller
 * divides), 0 none: a sleepy device's supervision window follows it (ZB-005). */
void zsrc_zb_check_in_interval(zsrc_zb_t *z, uint64_t ieee, uint32_t seconds, uint64_t now);

/* How long this endpoint may be silent before the source says it is unreachable (ZB-005): the
 * quirk's window, else 30 min for a listening device, else 2 h for a sleepy one, stretched to
 * two check-ins and ZSRC_ZB_CHECKIN_SLACK_S when its check-in interval is known. */
uint32_t zsrc_zb_supervision_s(const zsrc_zb_t *z, const zsrc_zb_device_t *d);

/* The ZSRC_ZB_CL_* bit of a ZCL cluster id, 0 for one the source has no bit for. */
uint16_t zsrc_zb_cluster_bit(uint16_t cluster);

/* ---- queries for the zone page, the console and zigbee_host -------------------------------- */

const zsrc_zb_device_t *zsrc_zb_device(const zsrc_zb_t *z, uint64_t ieee, uint8_t endpoint);
size_t zsrc_zb_devices(const zsrc_zb_t *z, const zsrc_zb_device_t **out, size_t max);
bool zsrc_zb_jammed(const zsrc_zb_t *z);

/* The address of a device endpoint. */
zsrc_address_t zsrc_zb_address(uint64_t ieee, uint8_t endpoint);
/* "0x00158D0001A2B3C4" into buf; returns buf. */
const char *zsrc_zb_ieee_str(uint64_t ieee, char *buf, size_t len);

/* The built-in quirks table, and the row that matches a manufacturer and model (NULL = none). */
const zsrc_zb_quirk_t *zsrc_zb_quirk_table(size_t *count);
const zsrc_zb_quirk_t *zsrc_zb_quirk_for(const char *manufacturer, const char *model);

/* What an IAS Zone Type means to us (ZB-003); ALARM_DEVCLASS_GENERIC when we don't know it. */
alarm_device_class_t zsrc_zb_class_of_zone_type(uint16_t zone_type);
/* ... and what a plain endpoint's clusters suggest when there is no IAS Zone cluster (ZB-004). */
alarm_device_class_t zsrc_zb_class_of_clusters(uint16_t clusters);

#ifdef __cplusplus
}
#endif

#endif /* ZSRC_ZIGBEE_H */
