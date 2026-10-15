/*
 * zw_host.h - the Z-Wave host: the link to the ZAC93 and everything that keeps it up
 * (docs/features/36 serial-api.md §4 "What must be supported", §5 "Start-up sequence", §7 "When
 * the module stops answering"; requirements ZW-001, ZW-004).
 *
 * It owns the Serial API transport (zw_transport.h) and the link's state. At start it runs §5's
 * sequence - a pulse on the reset line when there is one, a NAK and 500 ms of listening for the
 * bootloader, then GetSerialApiCapabilities to SetSerialApiTimeouts - checking the module
 * against §4 on the way; when every step has passed the link is up and the dispatch gate opens.
 * A module that stops answering is recovered as §7 says (soft reset, the reset line, then link
 * down and a new start-up every 30 s, every 5 min after 10 failures); one that restarts by
 * itself gets steps 13-19 again. Requests of the classes that need the link wait behind the gate
 * meanwhile, and fail with ZW_TX_LINK_DOWN when the link goes down.
 *
 * It lives in the zwave task like the transport: the zw_link task's news goes in through
 * zw_host_on_control/_on_frame/_on_text, zw_host_poll() does the work and writes, and
 * zw_host_next_deadline() says when to poll again. The owner hears through the port: link up
 * (with the module's facts), link down (with a reason), the module restarting by itself, and
 * the module's own frames. Every owner callback runs from zw_host_poll() or one of the link
 * calls; it may submit and cancel requests, never feed link news or poll.
 *
 * Portable C11: caller-owned state, no heap, no globals.
 */
#ifndef ZW_HOST_H
#define ZW_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zwave_host/zw_transport.h"
#include "zwave_proto/zw_api.h"
#include "zwave_proto/zw_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- What the owner configures ------------------------------------------------------------ */

typedef struct {
    bool set_region;    /* false: keep the module's region */
    uint8_t region;     /* the installer's region (serial-api.md §6.7 codes) when set_region */
    bool long_range;    /* Long Range (phase 3): 16-bit node IDs and step 13 when the region has
                         * it; false (default): 8-bit node IDs, step 13 skipped */
} zw_host_config_t;

/* ---- What the host tells the owner -------------------------------------------------------- */

#define ZW_HOST_REGIONS_MAX 16u

/* The module as start-up found it (steps 2-18). Fields a step skipped keep 0 / false. */
typedef struct {
    /* step 2, GetSerialApiCapabilities */
    uint8_t firmware_major, firmware_minor; /* the module's application (Zooz firmware) */
    uint16_t manufacturer, product_type, product_id;
    uint8_t function_mask[ZW_FUNC_MASK_BYTES]; /* zw_func_mask_test() reads it */
    /* step 3, GetSerialApiInitData */
    uint8_t api_version;
    bool is_secondary, is_sis;
    uint8_t node_list[ZW_NODE_LIST_BYTES];  /* zw_node_list_test() and friends read it */
    uint8_t chip_type, chip_version;
    /* step 4, GetControllerVersion */
    char library[24];                       /* "Z-Wave 7.22", NUL-terminated */
    uint8_t library_type;                   /* 1 static controller, 7 bridge controller */
    /* step 5, GetProtocolVersion: the SDK version */
    uint8_t protocol_major, protocol_minor, protocol_patch;
    bool update_note;                       /* before 7.22.1: "Update the Z-Wave radio's
                                             * software" (§4) */
    /* step 6, GetControllerCapabilities */
    zw_api_controller_caps_rsp_t controller;
    /* step 7, SerialAPISetup GetSupportedCommands: the legacy mask and the extended one */
    uint8_t setup_legacy;
    uint8_t setup_extended[32], setup_extended_len;
    /* step 8 */
    uint8_t max_payload, lr_max_payload;    /* bytes of CC payload in one SendData; 0 unknown */
    /* step 9 (and 10) */
    uint8_t regions[ZW_HOST_REGIONS_MAX], region_count; /* the regions the module offers */
    uint8_t region_flags[ZW_HOST_REGIONS_MAX]; /* GetRegionInfo: 01 classic, 02 Long Range */
    bool region_known;
    uint8_t region;                         /* the module's region now (the wish, once set) */
    /* step 12, SerialAPIStarted */
    bool long_range_available;              /* in this region */
    /* step 13 */
    uint16_t lr_node_count;
    /* step 14 */
    uint8_t node_id_bits;                   /* 8, or 16 with Long Range */
    /* step 15, GetControllerId */
    uint32_t home_id;
    uint16_t node_id;                       /* the panel's own */
    /* step 16 */
    bool tx_status_report;
    /* step 17 */
    uint16_t suc_node_id;                   /* after SetSUCNodeId when there was none */
    /* step 18: the module's timeouts before SetSerialApiTimeouts, in 10 ms; 0 when skipped */
    uint8_t old_ack_timeout, old_byte_timeout;
} zw_module_facts_t;

/* Why the link is down (serial-api.md §4, §7), for the Z-Wave page and `zwave status`. */
typedef enum {
    ZW_DOWN_NONE = 0,
    ZW_DOWN_NOT_ANSWERING,    /* §7: silent through the soft reset (and the reset line) */
    ZW_DOWN_BAD_ANSWER,       /* a step's answers could not be used, even after a soft reset */
    ZW_DOWN_NOT_CONTROLLER,   /* §4: an end node, not a controller */
    ZW_DOWN_WRONG_CHIP,       /* §4: not an 800-series chip (detail: the chip type) */
    ZW_DOWN_MISSING_FUNCTION, /* §4: a required function missing (detail: which) */
    ZW_DOWN_WRONG_LIBRARY,    /* §4: not a static or bridge controller (detail: library type) */
    ZW_DOWN_BOOTLOADER,       /* stuck in its bootloader */
    ZW_DOWN_RESTARTING,       /* restarted by itself again and again while starting */
    ZW_DOWN_UPDATING          /* its own firmware is being updated (zw_host_otw_begin) */
} zw_down_reason_t;

typedef struct {
    zw_down_reason_t reason;
    uint8_t step;   /* the §5 step it happened at; 0 when none */
    uint8_t detail; /* see the reason; 0 otherwise */
} zw_link_down_t;

typedef enum {
    ZW_LINK_OFF = 0,     /* not started */
    ZW_LINK_STARTING,    /* §5 running: boot, retry, or steps 13-19 after a restart */
    ZW_LINK_UP,
    ZW_LINK_RECOVERING,  /* §7 running */
    ZW_LINK_DOWN         /* waiting for the next start-up */
} zw_link_state_t;

typedef struct {
    void *ctx;
    /* Bytes to the UART, through the zw_link task; false when they could not leave. */
    bool (*write)(void *ctx, const uint8_t *bytes, size_t len);
    /* The module's reset line: asserted (low) or released. NULL: none is wired. */
    void (*reset_line)(void *ctx, bool asserted);
    /* §5 step 19: the link is up. This is the hook for what follows - comparing the home ID
     * with the stored network, reconciling the node list, interviews (Z5), SmartStart (Z10). */
    void (*link_up)(void *ctx, const zw_module_facts_t *facts);
    /* The link is down; the host tries again by itself (30 s, 5 min after 10 failures). */
    void (*link_down)(void *ctx, const zw_link_down_t *why);
    /* The module restarted by itself while the link was up (serial-api.md §2): `reason` is
     * SerialAPIStarted's wake-up reason. Steps 13-19 follow; link_up (or link_down) after.
     * ZW_HOST_RESTART_BOOTLOADER: it fell into its bootloader (its menu arrived unasked) - the
     * host chose "run", and the start-up from step 1 follows (§8, step 1). */
    void (*restarted)(void *ctx, uint8_t reason);
    /* A module frame no command waited for: ApplicationCommandHandler (04, A8),
     * ApplicationUpdate (49), AddNode/RemoveNode reports (4A, 4B)... zw_link has ACKed it. */
    void (*frame)(void *ctx, const zw_frame_t *frame);
    /* Optional: the module's own firmware update ended (zw_host_otw_begin): zw_host_otw() says
     * how. The start-up follows at once (link_up, or link_down). */
    void (*otw_done)(void *ctx);
} zw_host_port_t;

/* ---- The module's own firmware, over the wire (serial-api.md §8, network.md §14; ZW-025) ---- */

typedef enum {
    ZW_OTW_IDLE = 0,
    ZW_OTW_ENTERING,   /* EnterBootloader written: its menu awaited (5 s) */
    ZW_OTW_STARTING,   /* "upload gbl" chosen: "begin upload", then 'C' awaited (5 s) */
    ZW_OTW_SENDING,    /* the file's blocks: `sent` of `total` */
    ZW_OTW_ABORTING,   /* the bootloader cancelled: its reason awaited (1 s) */
    ZW_OTW_FINISHING,  /* EOT written: "Serial upload complete" and its menu awaited (10 s) */
    ZW_OTW_RUNNING,    /* "run" chosen: SerialAPIStarted awaited (10 s) */
    ZW_OTW_DONE        /* ended: `result` */
} zw_otw_state_t;

/* Z-Wave JS's OTWFirmwareUpdateStatus, with the panel's own refusals added. */
typedef enum {
    ZW_OTW_OK = 0,           /* uploaded, and the new firmware started */
    ZW_OTW_TIMEOUT,          /* the bootloader stopped answering */
    ZW_OTW_RETRIES,          /* a block refused 3 times (damaged on the wire) */
    ZW_OTW_ABORTED,          /* the bootloader refused the file: `error` its code */
    ZW_OTW_NOT_SUPPORTED,    /* no bootloader menu, or no "upload gbl" in it */
    ZW_OTW_NOT_STARTED       /* the uploaded firmware did not start (SerialAPIStarted missing) */
} zw_otw_result_t;

typedef struct {
    uint8_t state;           /* zw_otw_state_t */
    uint8_t result;          /* zw_otw_result_t, when DONE */
    uint8_t error;           /* ABORTED: the bootloader's error code (0x18...); 0 unknown */
    uint8_t attempts;        /* uploads begun (§8: an error 0x2X is tried again, 3 in all) */
    uint32_t sent, total;    /* blocks acknowledged, of all */
    uint32_t resent;         /* blocks sent again (NAK or no answer) */
} zw_otw_progress_t;

typedef enum {
    ZW_OTW_BEGUN = 0,
    ZW_OTW_BUSY,             /* the link is starting or recovering, or an update runs */
    ZW_OTW_NO_MODULE,        /* the link is down, not with the module in its bootloader */
    ZW_OTW_BAD_FILE          /* not a whole Gecko bootloader file (zw_gbl_check) */
} zw_otw_begin_t;

/* restarted()'s reason when the module fell into its bootloader while up (SerialAPIStarted's
 * "unknown": no SerialAPIStarted told it). */
#define ZW_HOST_RESTART_BOOTLOADER 0xFFu

typedef struct {
    uint32_t attempts;      /* start-up attempts begun */
    uint32_t link_ups, link_downs;
    uint32_t restarts;      /* the module restarted by itself while up */
    uint32_t recoveries;    /* §7 begun */
    uint32_t reset_pulses;  /* the reset line pulsed */
    uint32_t bootloader;    /* the bootloader left with "run" */
    uint32_t step_retries;  /* start-up commands sent again */
    uint32_t text_bytes;    /* bytes of text heard (outside frames) */
    uint32_t bootloader_falls; /* its bootloader's menu heard while the link was up */
    uint32_t probes;        /* idle probes sent (ZW_HOST_PROBE_IDLE_MS) */
    uint32_t otw_updates;   /* its firmware updates begun */
} zw_host_stats_t;

/* ---- State (caller-owned; read it through the functions below) --------------------------- */

#define ZW_HOST_TEXT_MAX 256u
/* serial-api.md §7 (ZW-032): with the link up and nothing heard from the module this long, one
 * MemoryGetId asks whether it is still there - a pulled or hung module is found by this even when
 * nothing else talks, and its failure is §7's recovery (then link down: the radio trouble). An
 * answer with another home ID is a swapped module: recovered the same way (the start-up checks
 * the network again). */
#define ZW_HOST_PROBE_IDLE_MS 60000u

typedef struct {
    zw_host_port_t port;
    zw_host_config_t cfg;
    zw_transport_t tr;
    zw_link_state_t link;
    uint8_t phase;            /* what the host waits for (zw_host.c) */
    uint64_t timer;           /* when that wait ends; UINT64_MAX: no timer */
    bool recovering_line;     /* the reset line is pulsed for §7, not for step 0 */
    /* the start-up cursor */
    uint8_t step, sub, tries;
    bool resume_at_13;        /* steps 2-11 are done: a restart resumes at step 13 */
    uint16_t gen;             /* start-up attempt: results of an older one are ignored */
    /* news from inside the transport's calls, handled in zw_host_poll() */
    bool result_ready, not_answering, restart_heard;
    zw_tx_outcome_t result_status;
    size_t result_len, restart_len;
    uint8_t result[ZW_PAYLOAD_MAX];
    uint8_t restart[ZW_PAYLOAD_MAX];
    /* bounds for one start-up attempt */
    uint8_t boot_escapes, recoveries, restarts_seen;
    /* the retry schedule */
    uint32_t failures;        /* start-ups failed since the link was last up */
    zw_link_down_t down;
    zw_module_facts_t facts;
    size_t text_len;          /* the newest text heard (the bootloader's menu is looked for in it) */
    uint8_t text[ZW_HOST_TEXT_MAX];
    bool text_new;            /* text came since the last poll */
    zw_host_stats_t stats;
    uint64_t heard_ms;        /* the module's last frame or ACK/NAK/CAN (the probe's clock) */
    bool probing;             /* its MemoryGetId is out */
    /* its own firmware update (zw_host_otw_begin): the file is the owner's until it ends */
    struct {
        zw_otw_progress_t p;
        const uint8_t *image;
        size_t len;
        uint8_t tries;         /* sends of the block out */
        uint8_t upload_key;    /* the menu's "upload gbl" digit */
        bool control_new;      /* an ACK/NAK/CAN arrived since the last poll... */
        uint8_t control;       /* ...this one */
        bool ready;            /* 'C' heard after "begin upload" */
    } otw;
} zw_host_t;

/* ---- Functions ---------------------------------------------------------------------------- */

/* Ready, not started; the transport's gate closed. port->write is required. */
void zw_host_init(zw_host_t *h, const zw_host_port_t *port, const zw_host_config_t *cfg);

/* Runs the start-up sequence from step 0 - at boot, after a region change or a network reset
 * (§5). Requests already queued wait behind the gate. */
void zw_host_start(zw_host_t *h, uint64_t now_ms);

/* A new configuration (the installer's region, network.md §12): in effect from the next
 * start-up. */
void zw_host_set_config(zw_host_t *h, const zw_host_config_t *cfg);

/* The zw_link task's news, as for zw_transport_on_control/_on_frame/_on_text. */
void zw_host_on_control(zw_host_t *h, uint64_t now_ms, uint8_t byte);
void zw_host_on_frame(zw_host_t *h, uint64_t now_ms, const zw_frame_t *frame);
void zw_host_on_text(zw_host_t *h, uint64_t now_ms, const uint8_t *bytes, size_t len);

/* Everything due at now_ms; the only call that writes. Call it after the link's news and at
 * zw_host_next_deadline(). */
void zw_host_poll(zw_host_t *h, uint64_t now_ms);

/* When zw_host_poll() has work next: 0 = at once, UINT64_MAX = not before news. */
uint64_t zw_host_next_deadline(const zw_host_t *h);

/* The owner's requests (see zw_request_t). Refused with INVALID for the MODULE class (the
 * module's own commands are the host's) and for tags 0x5A57xxxx (the host's own), whatever the
 * link; with LINK_DOWN while the link is down (or not started); with FULL when only the queue's
 * last slot is left - it is the host's, so a recovery always finds room. While the link is
 * starting or recovering they are queued and wait behind the gate. */
ZW_TRANSPORT_MUST_CHECK
zw_submit_t zw_host_submit(zw_host_t *h, const zw_request_t *req);

/* Drops the owner's queued requests with this tag (zw_transport_cancel). */
size_t zw_host_cancel(zw_host_t *h, uint32_t tag);

/* The module's own firmware, over the wire (serial-api.md §8, as Z-Wave JS's firmwareUpdateOTW):
 * `image` a whole .gbl (checked here first: nothing is written for a bad one), kept by the owner
 * until otw_done. From a link that is up, or down with the module in its bootloader (a failed
 * update tried again). The link goes down at once (link_down, ZW_DOWN_UPDATING: the owner's
 * requests fail as for any link down), EnterBootloader is written, the menu's "upload gbl" chosen,
 * the file sent in XMODEM-CRC blocks (a block NAKed or unanswered for 1 s sent again, 3 sends at
 * most), EOT, then "run"; an abort whose code is 0x2X starts the upload again (3 uploads in all).
 * Whatever the end, otw_done, then the start-up from step 0 - after a failure, the start-up's own
 * bootloader handling ("run", then link down "bootloader" when the old firmware is gone too).
 * The caller's checks come first: installer, every partition disarmed, arming held (§14). */
zw_otw_begin_t zw_host_otw_begin(zw_host_t *h, uint64_t now_ms, const uint8_t *image, size_t len);
const zw_otw_progress_t *zw_host_otw(const zw_host_t *h);

/* §7's recovery asked by the owner while the link is up (serial-api.md §6.12: a module jammed
 * for 5 transmissions): the gate closes, SoftReset, steps 13-19 again - then the reset line and
 * link down if it does not answer, as for a module that stops answering. The owner's requests
 * stay queued behind the gate meanwhile. False (nothing done) unless the link is up and the host
 * idle (not probing). */
bool zw_host_recover(zw_host_t *h, uint64_t now_ms);

zw_link_state_t zw_host_link(const zw_host_t *h);

/* The module's facts; complete while the link is up. */
const zw_module_facts_t *zw_host_facts(const zw_host_t *h);

/* Why the link went down last (reason NONE while it has not). */
const zw_link_down_t *zw_host_down_reason(const zw_host_t *h);

const zw_host_stats_t *zw_host_stats(const zw_host_t *h);

/* The transport's counters (zw_transport_stats_t). */
const zw_transport_stats_t *zw_host_transport_stats(const zw_host_t *h);

#ifdef __cplusplus
}
#endif

#endif /* ZW_HOST_H */
