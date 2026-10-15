/*
 * zw_incl.h - adding a device (docs/features/36 network.md §4, §4.1; security.md §5 "the panel
 * includes a device"; requirements ZW-005, ZW-006): the protocol inclusion through the module
 * (AddNodeToNetwork's status reports, the 60 s window, every timeout and refusal), the SUC return
 * route, then the S2 key exchange with the installer's consent and PIN.
 *
 * It runs beside the host (zw_host.h) and the security manager (zw_s2.h), usually inside zw_net
 * (zw_net.h), which feeds it: the module's AddNode status reports the transport did not claim
 * (zw_incl_module_frame), the new node's Security 2 commands after the manager decrypted them
 * (zw_incl_s2_command), the manager's decryption failures (zw_incl_s2_event: the wrong-PIN rule)
 * and the clock. It sends through its port: module commands to the host, Security 2 commands
 * to the manager.
 *
 * ZW-005's "installer mode with every partition disarmed" is the caller's check (the service and
 * the UI know the modes and partitions, this library does not): zw_incl_start() takes its answer
 * as `allowed` and refuses when it is false.
 *
 * Every S2 failure leaves the device in the network without security and says why (§4.1): the
 * manager keeps no class for it and its temporary key is deleted; nothing falls back silently.
 * A downgrade is flagged as one: a device whose node information lists Security 2 but that asks
 * for no S2 class, a KEX Set echo that differs from what the panel granted, the device refusing
 * the panel's KEX Report echo.
 *
 * Portable C11, caller-owned state, no heap. Callbacks may call the zw_incl functions.
 */
#ifndef ZW_INCL_H
#define ZW_INCL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zwave_host/zw_s2.h"
#include "zwave_host/zw_transport.h"
#include "zwave_proto/zw_api.h"
#include "zwave_proto/zw_dsk.h"
#include "zwave_proto/zw_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* network.md §4 timing */
#define ZW_INCL_READY_MS          10000u  /* AddNode start: status Ready */
#define ZW_INCL_WINDOW_MS         60000u  /* the window, restarted by "Keep listening" */
#define ZW_INCL_FOUND_MS          60000u  /* NodeFound: AddingEndNode / AddingController */
#define ZW_INCL_PROTOCOL_BASE_MS  76000u  /* AddingEndNode: ProtocolDone within this */
#define ZW_INCL_PROTOCOL_LISTENING_MS 217u  /* + this per listening node */
#define ZW_INCL_PROTOCOL_FLIRS_MS 3500u   /* + this per FLiRS node */
#define ZW_INCL_DONE_MS           10000u  /* AddNode stop: status Done */
#define ZW_INCL_ROUTE_MS          30000u  /* AssignSUCReturnRoute's callback */
/* security.md §5 timers */
#define ZW_INCL_TA_MS             10000u  /* TA1..TA5 */
#define ZW_INCL_TAI_MS           240000u  /* TAI1, TAI2 */

typedef enum {
    ZW_INCL_IDLE = 0,
    ZW_INCL_STARTING,     /* AddNode(any) sent: status Ready awaited */
    ZW_INCL_LISTENING,    /* the window: "Put the device in pairing mode" */
    ZW_INCL_FOUND,        /* NodeFound: the protocol goes on by itself */
    ZW_INCL_ADDING,       /* AddingEndNode: ProtocolDone awaited */
    ZW_INCL_FINISHING,    /* AddNode(stop): Done awaited */
    ZW_INCL_ROUTE,        /* AssignSUCReturnRoute */
    ZW_INCL_KEX_REPORT,   /* KEX Get sent: TA1 */
    ZW_INCL_GRANT,        /* the installer decides what to grant: TAI1 */
    ZW_INCL_PUBKEY,       /* KEX Set sent: the device's Public Key Report, TA2 */
    ZW_INCL_PIN,          /* the installer types the PIN: TAI2 */
    ZW_INCL_ECHO,         /* the KEX Set echo under the temporary key: TAI2 */
    ZW_INCL_NETKEY_GET,   /* Network Key Get: TA3 */
    ZW_INCL_VERIFY,       /* Network Key Verify: TA4 */
    ZW_INCL_TRANSFER_END  /* the device's Transfer End: TA5 */
} zw_incl_state_t;

/* How it ended (network.md §4): added, or why not. */
typedef enum {
    ZW_INCL_ADDED = 0,       /* the device is in the network: `security` says how */
    ZW_INCL_NOT_ALLOWED,     /* refused: not in installer mode with every partition disarmed */
    ZW_INCL_BUSY,            /* refused: a network operation is running */
    ZW_INCL_NOT_LISTENING,   /* "The Z-Wave radio did not start listening" */
    ZW_INCL_NO_DEVICE,       /* the window ended, nothing found */
    ZW_INCL_STOPPED,         /* the installer stopped it before a device joined */
    ZW_INCL_CONTROLLER,      /* "That is a controller, not a sensor": stopped at once */
    ZW_INCL_FAILED,          /* status Failed: "The device did not finish joining..." */
    ZW_INCL_TIMEOUT,         /* the protocol did not finish in time */
    ZW_INCL_ALREADY_ADDED,   /* its node ID is already in the cache */
    ZW_INCL_BAD_NODE_ID,     /* the broadcast node ID */
    ZW_INCL_LINK_DOWN,       /* the module went away */
    ZW_INCL_STARTED,         /* zw_incl_start(): listening begins, events follow */
    ZW_INCL_FOREIGN_RADIO,   /* (zw_net) refused: the radio belongs to another system - reset it
                              * first (network.md §2) */
    ZW_INCL_NOT_FAILED,      /* zw_incl_replace(): the radio doesn't count it as failed
                              * (IsFailedNode no, or ReplaceFailedNode's response 08) - nothing
                              * replaced */
    ZW_INCL_REFUSED,         /* zw_incl_replace(): the radio refused for another reason (not its
                              * primary, failed); its "busy" (10) is ZW_INCL_BUSY */
    ZW_INCL_STILL_ANSWERS,   /* zw_incl_replace(): the old device answered a ping, or the radio's
                              * report said so (00) - nothing replaced */
    ZW_INCL_RADIO_RESTARTED  /* the module restarted (by itself, or serial-api.md §7's soft
                              * reset) before the device joined: its window closed with it
                              * (zw_incl_radio_restarted) */
} zw_incl_result_t;

/* Its security (§4.1). */
typedef enum {
    ZW_INCL_SEC_NONE = 0,      /* the device has no security: "Not encrypted" */
    ZW_INCL_SEC_S2_UNAUTH,     /* "Secure (no PIN)" */
    ZW_INCL_SEC_S2_AUTH,       /* "Secure" */
    ZW_INCL_SEC_S2_ACCESS,     /* "Secure" */
    ZW_INCL_SEC_FAILED,        /* "Added without encryption: remove it and add it again" */
    ZW_INCL_SEC_DOWNGRADED     /* "Security was downgraded: remove it and add it again" */
} zw_incl_security_t;

/* Why the key exchange failed. */
typedef enum {
    ZW_INCL_S2_OK = 0,
    ZW_INCL_S2_REFUSED,        /* the panel sent KEX Fail (detail: its type) */
    ZW_INCL_S2_DEVICE_FAIL,    /* the device sent KEX Fail (detail: its type) */
    ZW_INCL_S2_TIMEOUT,        /* a timer ran out (detail: 1-5 TA1-TA5, 0x11/0x12 TAI1/TAI2) */
    ZW_INCL_S2_WRONG_PIN,
    ZW_INCL_S2_CANCELLED,      /* the installer cancelled */
    ZW_INCL_S2_TAMPERED,       /* a KEX message came back different from what was sent */
    ZW_INCL_S2_ERROR,          /* the crypto port or the radio failed */
    ZW_INCL_S2_NO_INFO,        /* its node information was empty and KEX Get got no answer:
                                * whether it has Security 2 is not known (not "no S2") */
    ZW_INCL_S2_WRONG_DEVICE    /* SmartStart: its public key is not the DSK its code gave, or it
                                * asked for none of the keys the code's entry grants */
} zw_incl_s2_failure_t;

#define ZW_INCL_TAI1 0x11u
#define ZW_INCL_TAI2 0x12u

typedef enum {
    ZW_INCL_EV_LISTENING = 1, /* the window is open until `deadline` */
    ZW_INCL_EV_FOUND,         /* a device answered: the countdown stops */
    ZW_INCL_EV_ADDING,        /* the protocol adds `node` (its CCs in `nif`) */
    ZW_INCL_EV_GRANT,         /* it asks for `requested`: zw_incl_grant() or zw_incl_cancel() by
                               * `deadline` (TAI1) */
    ZW_INCL_EV_PIN,           /* type the PIN of the label showing `dsk` (its first group is the
                               * PIN, sent as 00000): zw_incl_pin() by `deadline` (TAI2) */
    ZW_INCL_EV_DONE,          /* the end: `result`, `security`, `granted`, `failure`, `dsk` */
    ZW_INCL_EV_JOINED         /* network.md §4 step 5: `node` is in the network (its CCs in `nif`);
                               * the route and the key exchange follow, DONE ends it */
} zw_incl_event_kind_t;

typedef struct {
    zw_incl_event_kind_t kind;
    uint16_t node;
    uint64_t deadline;
    uint8_t requested;               /* GRANT: the classes it asks for that the panel has */
    char dsk[ZW_DSK_TEXT_SIZE];      /* PIN and DONE (S2): the DSK as text */
    const uint8_t *nif;              /* ADDING, DONE: its CC list */
    size_t nif_len;
    zw_incl_result_t result;         /* DONE */
    zw_incl_security_t security;     /* DONE */
    uint8_t granted;                 /* DONE: the keys it holds (ZW_S2_KEY_*) */
    zw_incl_s2_failure_t failure;    /* DONE: why security failed */
    uint8_t failure_detail;
    bool route_failed;               /* DONE: the SUC return route was not assigned */
    uint8_t dsk_bytes[16];           /* DONE (S2): the DSK, PIN included, for the node cache */
    bool replaced;                   /* LISTENING, JOINED, DONE: it takes a failed device's place
                                      * (its node ID: zw_incl_replace) */
    bool smartstart;                 /* DONE, JOINED: it joined by SmartStart (zw_incl_smartstart);
                                      * DONE carries the entry's DSK in dsk_bytes whatever the
                                      * outcome */
} zw_incl_event_t;

typedef struct {
    void *ctx;
    /* A module command (AddNodeToNetwork, AssignSUCReturnRoute) for the host (zw_host_submit). */
    zw_submit_t (*submit)(void *ctx, const zw_request_t *req);
    /* A Security 2 command to the node at a class (ZW_SEC_NONE or ZW_SEC_TEMP), through the
     * security manager (zw_s2_send). */
    zw_s2_result_t (*s2_send)(void *ctx, uint16_t node, zw_sec_class_t cls, const uint8_t *cmd,
                              size_t len);
    /* Whether a node ID is already in the node cache ("already added"). Optional. */
    bool (*known)(void *ctx, uint16_t node);
    void (*event)(void *ctx, const zw_incl_event_t *ev);
} zw_incl_port_t;

typedef struct {
    uint16_t listening_nodes;  /* in the network now: ProtocolDone's timeout grows with them */
    uint16_t flirs_nodes;
    zw_nid_width_t nid_width;  /* the node-ID width in effect (zw_host facts); 0: 8-bit */
} zw_incl_options_t;

/* A SmartStart entry that asked to join (network.md §4.2): the DSK its code gave, the keys to
 * grant (a subset of what the code requested), whether it joins by Long Range. */
typedef struct {
    uint8_t dsk[16];
    uint8_t grant;
    bool long_range;
} zw_incl_smartstart_t;

/* ---- State (caller-owned) ----------------------------------------------------------------- */

typedef struct {
    zw_incl_port_t port;
    zw_s2_t *s2;
    zw_incl_state_t state;
    zw_incl_options_t opt;
    uint64_t now;
    uint64_t deadline;          /* the current wait's end; UINT64_MAX: none */
    uint64_t tai2_until;        /* TAI2 runs through the PIN and the echo */
    uint16_t gen;               /* this inclusion: requests of an older one are ignored */
    uint8_t add_cb;             /* AddNode(any)'s callback ID: its status reports carry it */
    uint16_t node;
    uint8_t nif[ZW_PAYLOAD_MAX];
    size_t nif_len;
    bool route_failed;
    bool probing;               /* KEX Get sent without a node information to say it has S2 */
    /* the key exchange */
    zw_s2_kex_t report;         /* the device's KEX Report, as received */
    uint8_t granted;
    uint8_t delivered;          /* keys sent and verified */
    uint8_t exchanging;         /* the key whose Verify is awaited */
    uint8_t wrong_pin_count;    /* decryption failures on an exchanged nonce */
    bool temp_installed;
    uint8_t priv[32];           /* the panel's key pair for this exchange */
    uint8_t pub[32];
    uint8_t dev_pub[32];        /* the device's, with the PIN written into bytes 0-1 */
    /* SmartStart (zw_incl_smartstart): no window, no installer - the entry answers for them */
    bool smart;
    zw_incl_smartstart_t ss;
    bool replace;               /* zw_incl_replace (network.md §10) */
    bool checking;              /* zw_incl_replace: the old device's check runs, nothing sent yet */
} zw_incl_t;

/* ---- Functions ---------------------------------------------------------------------------- */

void zw_incl_init(zw_incl_t *in, const zw_incl_port_t *port, zw_s2_t *s2);

/* "Add a device" (network.md §4 step 1): `allowed` is ZW-005's check. ZW_INCL_STARTED, or the
 * refusal (nothing was sent). */
zw_incl_result_t zw_incl_start(zw_incl_t *in, uint64_t now, bool allowed,
                               const zw_incl_options_t *opt);
/* SmartStart (network.md §4.2): a device on the provisioning list asked to join -
 * AddNodeToNetwork 08 (+ Long Range) with the home IDs of its DSK, then steps 2-8 of §4 without
 * the installer: Security 2 is asked for even if its node information does not list it, the keys
 * granted are the entry's (those it asks for), the PIN is the DSK's first group, and its public
 * key must be the DSK - else the exchange fails (ZW_INCL_S2_WRONG_DEVICE). No LISTENING event: the
 * module reports the steps; FOUND's 60 s end it when the device does not come. The caller
 * decided it may run (every partition disarmed, no other operation): ZW_INCL_STARTED or BUSY /
 * NOT_LISTENING. */
zw_incl_result_t zw_incl_smartstart(zw_incl_t *in, uint64_t now, const zw_incl_options_t *opt,
                                    const zw_incl_smartstart_t *ss);
/* "Replace a failed device" (network.md §10), in two steps. zw_incl_replace() takes the
 * operation (STARTING) and sends nothing: the caller checks the old device first, as for removing
 * a failed one (zw_remove_check_failed: pings, IsFailedNode), and hands the outcome to
 * zw_incl_replace_checked(). Then ReplaceFailedNode for `node`; its first report ends the request:
 * "ready" opens the window (LISTENING, `replaced`) - the new device's button is pressed within it
 * - and the rest come as the module's own frames (zw_incl_module_frame): the new device takes the
 * same node ID - JOINED (`replaced`), the SUC return route and the key exchange as for any new
 * device (the installer's grant and PIN), DONE. Refused at once when not allowed or busy;
 * STILL_ANSWERS when the old device answered (a ping, or the radio's report 00); NOT_FAILED when
 * the radio doesn't count it as failed (response 08); BUSY for the radio's "busy" (10), REFUSED
 * for its other refusals. zw_incl_stop() ends the check or the window (STOPPED). */
zw_incl_result_t zw_incl_replace(zw_incl_t *in, uint64_t now, bool allowed,
                                 const zw_incl_options_t *opt, uint16_t node);
/* The old device's check is over: ZW_INCL_STARTED sends ReplaceFailedNode (NOT_LISTENING when it
 * can't go); any other result ends the replacement with it (DONE). Ignored unless a replacement's
 * check runs. */
void zw_incl_replace_checked(zw_incl_t *in, uint64_t now, zw_incl_result_t verdict);
/* "Keep listening": the 60 s again (while listening; not for a replacement - the radio's window
 * is its own). */
void zw_incl_keep_listening(zw_incl_t *in, uint64_t now);
/* The installer stops listening (before a device is found; later the protocol finishes). */
void zw_incl_stop(zw_incl_t *in, uint64_t now);
/* The installer's grant: a non-empty subset of what was requested. False otherwise (nothing
 * changes: grant again or cancel). */
bool zw_incl_grant(zw_incl_t *in, uint64_t now, uint8_t keys);
/* The PIN the installer typed. False when none is awaited. */
bool zw_incl_pin(zw_incl_t *in, uint64_t now, uint16_t pin);
/* The installer cancels the key exchange (KEX Fail "cancel"): added without security. */
void zw_incl_cancel(zw_incl_t *in, uint64_t now);

/* Inputs. */
void zw_incl_module_frame(zw_incl_t *in, uint64_t now, const zw_frame_t *frame);
void zw_incl_s2_command(zw_incl_t *in, uint64_t now, const zw_s2_rx_t *rx);
void zw_incl_s2_event(zw_incl_t *in, uint64_t now, uint16_t node, zw_s2_event_t ev);
void zw_incl_link_down(zw_incl_t *in, uint64_t now);
/* The module restarted - by itself (serial-api.md §2) or by serial-api.md §7's soft reset - and
  * its window closed with it (network.md §4 "The radio restarts"). Before Done (starting,
 * listening, found, adding, finishing; a replacement whose ReplaceFailedNode went): the end at
 * once, ZW_INCL_RADIO_RESTARTED, an AddNode(stop) queued for after the restart - between
 * ProtocolDone and Done too, as Z-Wave JS's performInclusion ends when finishInclusion gets no
 * Done. A device the module took in by then is taken in without keys by the link-up reconcile.
 * Later steps, and a replacement's check that sent nothing yet, carry on. An unexpected restart
 * that ends the AddNode or ReplaceFailedNode in flight (ZW_TX_MODULE_RESTARTED) is the same. */
void zw_incl_radio_restarted(zw_incl_t *in, uint64_t now);

void zw_incl_poll(zw_incl_t *in, uint64_t now);
uint64_t zw_incl_next_deadline(const zw_incl_t *in);
zw_incl_state_t zw_incl_state(const zw_incl_t *in);
/* The node being added (0 before the protocol named it). */
uint16_t zw_incl_node(const zw_incl_t *in);

#ifdef __cplusplus
}
#endif

#endif /* ZW_INCL_H */
