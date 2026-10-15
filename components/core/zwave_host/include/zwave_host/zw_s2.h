/*
 * zw_s2.h - the S2 security manager (docs/features/36 security.md §1, §6-9; requirements ZW-007,
 * ZW-009): the network keys, what each node was granted, and per peer the SPAN, the sequence
 * numbers and the nonce exchanges; encryption for sending and decryption on receipt; the rule
 * that a node is only believed at its highest granted class (§8); the answers to Commands
 * Supported (§9).
 *
 * The panel runs one manager for its whole network. A simulated device (the host tests, the PC
 * simulator) runs one of its own in device mode (config.panel false), with the panel as its only
 * peer: the SPAN protocol is the same on both sides, and §8's drops and §9's answers are the
 * panel's alone.
 *
 * Traffic goes through the port: `send` hands a frame to the radio (the owner wraps it in
 * SendData and reports the end with zw_s2_sent_tx()), `received` delivers every command that passes
 * the rules - one call per command, with the class it arrived at and what wrapped it - and
 * `send_done` ends each zw_s2_send(). Frames come in through zw_s2_receive() (singlecast) and
 * zw_s2_receive_group() (broadcast, multicast), after the owner has dropped what is not from this
 * network (serial-api.md §6.11).
 *
 * Receiving, the manager unwraps every layer (zwave_proto's zw_cc_unwrap) and applies
 * command-classes.md §1: Security 2 counts only as the outermost layer (a Message Encapsulation or
 * a nonce command inside any other layer drops the frame; other Security 2 commands are commands
 * like any), every other layer appears at most once, Transport Service and S0 encapsulation are
 * dropped (v1, phase 3). Then §8, as Z-Wave JS (Driver.ts isSecurityLevelTooLow): a node whose
 * highest class is S2 sends every command at that class, whatever its NIF lists - except bare
 * Security 2 commands, Security 2 Commands Supported Get and Report at any S2 class, and a
 * singlecast follow-up of a multicast; a node whose highest class is S0 needs S0 only for the CCs
 * its secure list names; a node with no class, or one being bootstrapped (its temporary key
 * installed), sends anything any way. A CRC-16 layer is unwrapped and what it carries judged.
 *
 * One addition to §6's states: the receiver EI the panel sent in a Nonce Report stays good for
 * one SPAN extension until one uses it, whatever the peer's state becomes meanwhile - so when
 * both sides resync at once (two frames that crossed, over 500 ms apart), each side's frame on
 * the other's receiver EI still decrypts instead of both being lost.
 *
 * Callbacks may call zw_s2_send() and the node functions; never zw_s2_receive(). Portable C11,
 * caller-owned state (about 15 KB with the defaults: PSRAM in the firmware), no heap.
 */
#ifndef ZW_S2_H
#define ZW_S2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zwave_host/zw_crypto.h"
#include "zwave_host/zw_s2_cc.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ZW_S2_PEERS
#define ZW_S2_PEERS 64u    /* nodes the manager knows */
#endif
#define ZW_S2_SENDS 4u     /* zw_s2_send()s in progress at once (one per node goes at a time) */
#define ZW_S2_PLAIN_MAX 64u /* the longest command sent or received securely */
#define ZW_S2_FRAME_MAX (ZW_S2_ENCAP_HEADER + ZW_S2_EXT_SPAN_LEN + ZW_S2_PLAIN_MAX + \
                         ZW_S2_TAG_BYTES)
#define ZW_S2_RX_MAX 256u   /* the longest command received (a Serial API frame's payload) */
#define ZW_S2_MAX_FRAME_DEFAULT 46u /* serial-api.md §5 step 8: the module's classic maximum */

/* §6 timing. */
#define ZW_S2_NONCE_ATTEMPTS  5u    /* nonces tried on a frame without a SPAN extension */
#define ZW_S2_CROSSED_MS      500u  /* the panel's last sending nonce stays good this long */
#define ZW_S2_RESEND_MS       500u  /* a Nonce Report this soon after a frame: it was not decoded */
#define ZW_S2_NONCE_WAIT_MS   1000u /* a Nonce Get delivered: its report, plus the round trip */
/* serial-api.md §6.12: transmissions of a frame to a node that listens (the owner's attempts
 * function decides which nodes: see zw_s2_set_attempts) */
#define ZW_S2_TX_ATTEMPTS     3u
/* serial-api.md §6.12: a jammed module (Fail with 0 transmit ticks): the same frame again after
 * this wait, the owner's jam function deciding how often (see zw_s2_set_jammed) */
#define ZW_S2_JAM_WAIT_MS     1000u
#define ZW_S2_JAM_TRIES       5u    /* jammed transmissions before the owner resets the module */

/* The security classes (§1). */
typedef enum {
    ZW_SEC_NONE = 0,  /* unencrypted */
    ZW_SEC_S2_UNAUTH,
    ZW_SEC_S2_AUTH,
    ZW_SEC_S2_ACCESS,
    ZW_SEC_S0,        /* phase 3: counted as a node's highest class, never used here */
    ZW_SEC_TEMP,      /* the temporary key of a key exchange (§5) */
    ZW_SEC_AUTO       /* zw_s2_send: what §8 says - the node's highest class for its secure CCs */
} zw_sec_class_t;

/* A peer's SPAN (§6): nothing agreed; the panel sent its receiver EI; the peer sent its receiver
 * EI (the next frame carries a SPAN extension); a generator running. */
typedef enum {
    ZW_S2_SPAN_NONE = 0,
    ZW_S2_SPAN_LOCAL_EI,
    ZW_S2_SPAN_REMOTE_EI,
    ZW_S2_SPAN_READY
} zw_s2_span_state_t;

/* The six network keys (§1), from the owner (generated and stored by the firmware, Z8). */
typedef struct {
    uint8_t s2_unauth[16];
    uint8_t s2_auth[16];
    uint8_t s2_access[16];
    uint8_t s0[16];
    uint8_t lr_auth[16];   /* Long Range nodes (node ID 256 and up) */
    uint8_t lr_access[16];
} zw_s2_keys_t;

typedef struct {
    bool panel;        /* the panel's rules (§8 drops, §9 answers); false: a simulated device */
    uint8_t max_frame; /* the longest SendData command the module takes; 0: 46 */
} zw_s2_config_t;

/* A command that passed the rules, and how it came. */
typedef struct {
    uint16_t node;
    zw_sec_class_t security;       /* the class that decrypted it; ZW_SEC_NONE unencrypted */
    bool multi_channel;
    uint8_t source_endpoint;
    uint8_t destination_endpoint;
    bool supervised;               /* inside a Supervision Get */
    uint8_t supervision_session;
    bool supervision_updates;
    bool crc16;
    bool multi_command;            /* one of the commands of a Multi Command */
    const uint8_t *command;        /* CC | command | parameters */
    size_t len;
} zw_s2_rx_t;

typedef enum {
    ZW_S2_OK = 0,
    /* zw_s2_send refused it */
    ZW_S2_BUSY,         /* ZW_S2_SENDS in progress already */
    ZW_S2_UNKNOWN_NODE,
    ZW_S2_NO_KEY,       /* the class is not the node's (or it has no temporary key) */
    ZW_S2_TOO_LONG,     /* longer than ZW_S2_PLAIN_MAX, or than the module's frame allows */
    ZW_S2_INVALID,
    /* send_done's outcomes */
    ZW_S2_FAILED,       /* the radio did not deliver it (or its Nonce Get) */
    ZW_S2_NO_NONCE,     /* the node sent no Nonce Report */
    ZW_S2_NOT_DECODED,  /* the node could not decrypt it, twice (§6) */
    ZW_S2_CRYPTO,       /* the crypto port failed */
    ZW_S2_CANCELLED     /* the node was forgotten meanwhile */
} zw_s2_result_t;

/* How a frame handed to port.send ended: the module's SendData callback (serial-api.md §6.12). */
typedef enum {
    ZW_S2_TX_OK = 0,   /* the node acknowledged it */
    ZW_S2_TX_NO_ACK,   /* sent, never acknowledged */
    ZW_S2_TX_FAIL,     /* Fail after time on the air (transmit ticks above 0, or not reported) */
    ZW_S2_TX_JAMMED,   /* Fail with 0 transmit ticks: the module never had the air (network.md
                        * §9) - not a delivery attempt: the owner's jam function decides
                        * (zw_s2_set_jammed); without one it fails the send */
    ZW_S2_TX_NO_ROUTE, /* no route to the node */
    ZW_S2_TX_UNKNOWN   /* no callback, one that did not parse, or another status */
} zw_s2_tx_t;

/* The owner's choice of transmissions for a frame of the send with `tag` to `node` (1 to
 * ZW_S2_TX_ATTEMPTS; ctx is the port's): ZW_S2_TX_ATTEMPTS for a node that listens, 1 for a
 * sleeping one (its NoAck means asleep) and for a send that counts its own attempts. */
typedef uint8_t (*zw_s2_attempts_fn)(void *ctx, uint16_t node, uint32_t tag);

/* What becomes of a frame the module could not send at all (ZW_S2_TX_JAMMED). */
typedef enum {
    ZW_S2_JAM_FAIL = 0, /* the send fails (ZW_S2_FAILED) */
    ZW_S2_JAM_WAIT,     /* the same frame again after ZW_S2_JAM_WAIT_MS */
    ZW_S2_JAM_NOW       /* the same frame again at once (the owner has begun a reset of the
                         * module: the frame waits behind its gate) */
} zw_s2_jam_t;

/* The owner's choice for the `jams`-th jammed transmission (1 up) of a frame of the send with
 * `tag` to `node` (ctx is the port's). */
typedef zw_s2_jam_t (*zw_s2_jammed_fn)(void *ctx, uint16_t node, uint32_t tag, uint8_t jams);

/* What the manager dropped or could not do, per frame, for the owner's counters and the key
 * exchange's wrong-PIN rule. */
typedef enum {
    ZW_S2_EV_UNKNOWN_NODE = 1,  /* not a node of this network */
    ZW_S2_EV_NO_KEY,            /* S2 from a node with no class and no temporary key */
    ZW_S2_EV_DECRYPT_FAILED,    /* not decrypted (no SPAN, or no nonce authenticated) */
    ZW_S2_EV_DECRYPT_FAILED_SPAN, /* ...a frame with a SPAN extension on the panel's receiver EI:
                                   * the nonce had been exchanged (§5 step 8) */
    ZW_S2_EV_DUPLICATE,         /* the peer's last sequence number again */
    ZW_S2_EV_WRONG_LEVEL,       /* a secure CC unencrypted or below the node's highest class */
    ZW_S2_EV_MALFORMED,         /* against command-classes.md §1, or a frame that does not parse */
    ZW_S2_EV_UNSUPPORTED,       /* Transport Service (never a wrong level), S0 encapsulation */
    ZW_S2_EV_NONCE,             /* not a drop: a Nonce Get or Report taken (it counts as hearing
                                 * from the node, as an accepted frame does: Z-Wave JS's
                                 * markAsAlive, security.md §8) */
    ZW_S2_EV_ANSWERED,          /* not a drop: a Security 2 Commands Supported Get accepted and
                                 * kept by the manager - answered only when bare and S2-encrypted
                                 * (§9); heard from the node, as above */
    ZW_S2_EV_CLASS_LEARNT       /* not a drop: a node whose classes are not all known
                                 * (zw_s2_set_classes_unknown) sent a frame that decrypted at an
                                 * S2 class not granted before - now granted (zw_s2_granted).
                                 * Z-Wave JS stores it at once (Security2CC.ts decryptSinglecast:
                                 * setSecurityClass(secClass, true), a cache-backed map): the
                                 * owner adds it to the node's record (ZW-009, ZW-017) */
} zw_s2_event_t;

typedef struct {
    void *ctx;
    /* A frame for the radio: SendData to `node` with these transmit options. The owner reports
     * its end with zw_s2_sent(tag). False: it could not be queued. */
    bool (*send)(void *ctx, uint16_t node, const uint8_t *frame, size_t len, uint8_t tx_options,
                 uint32_t tag);
    void (*received)(void *ctx, const zw_s2_rx_t *rx);
    void (*send_done)(void *ctx, uint32_t tag, zw_s2_result_t result);
    void (*event)(void *ctx, uint16_t node, zw_s2_event_t ev); /* optional */
} zw_s2_port_t;

typedef struct {
    uint32_t decrypted;          /* S2 frames decrypted */
    uint32_t decrypt_failures;
    uint32_t duplicates;
    uint32_t wrong_level;        /* "frames at the wrong security level" (§8) */
    uint32_t malformed;
    uint32_t unsupported;
    uint32_t nonce_reports;      /* Nonce Reports sent to it */
    uint32_t resends;            /* frames sent again with a new SPAN after its Nonce Report */
    uint32_t retries;            /* frames sent again after the radio's NoAck (serial-api.md
                                  * §6.12), each on a new sequence number and nonce */
} zw_s2_node_stats_t;

/* ---- State (caller-owned; read it through the functions below) --------------------------- */

typedef struct {
    bool present;
    uint8_t netkey[16];
    uint8_t key_ccm[16];
    uint8_t pstring[32];
} zw_s2_key_t;

typedef struct {
    uint16_t node;            /* 0: a free entry */
    uint8_t granted;          /* ZW_S2_KEY_* bits it holds */
    uint8_t span_state;       /* zw_s2_span_state_t */
    uint8_t span_class;       /* zw_sec_class_t of the SPAN */
    uint8_t own_seq;          /* the panel's sequence number to it */
    uint8_t last_rx_seq;
    bool has_last_rx;
    bool nonce_report_pending;
    uint16_t nonce_report_gen;
    bool crossed_valid;       /* crossed_nonce: the panel's last sending nonce (§6) */
    uint8_t crossed_class;
    uint64_t crossed_until;
    uint8_t crossed_nonce[ZW_S2_NONCE_LEN];
    bool sent_last;           /* the SPAN's last nonce went to the panel's own frame: nothing
                               * from it has authenticated since */
    zw_ctr_drbg_t span;
    uint8_t local_ei[ZW_S2_EI_LEN];  /* the receiver EI the panel sent it... */
    bool local_ei_valid;             /* ...not yet used by a SPAN extension */
    uint8_t remote_ei[ZW_S2_EI_LEN]; /* its receiver EI */
    uint8_t nif[32];          /* CC bitmap: its node information */
    uint8_t secure[32];       /* CC bitmap: its S2 Commands Supported Report */
    bool secure_known;
    bool classes_unknown;     /* its classes are not all known yet (Z-Wave JS: some
                               * hasSecurityClass() undefined): `granted` holds the classes known
                               * granted, `refused` those known refused, the rest are undecided; a
                               * frame is tried with every class not refused whose key the panel
                               * holds, and the class that decrypts it is added to `granted`;
                               * cleared by zw_s2_set_granted */
    uint8_t refused;          /* ZW_S2_KEY_* bits known not granted (RAM only; with
                               * classes_unknown): zw_s2_set_class_result */
    zw_s2_node_stats_t stats;
} zw_s2_peer_t;

typedef struct {
    uint8_t state;            /* zw_s2.c */
    uint8_t cls;              /* zw_sec_class_t, resolved */
    bool retried;
    uint8_t tries;            /* transmissions of the frame out that the radio did not deliver */
    uint8_t jams;             /* ...that the module could not send at all (ZW_S2_TX_JAMMED) */
    uint8_t jam_state;        /* zw_s2.c: what the frame out was, while it waits to go again */
    bool jam_rebuild;         /* an SOS Nonce Report came meanwhile: built again, not resent */
    uint8_t frame_len;        /* the frame out, kept to go again as it was after a jam */
    uint8_t frame[ZW_S2_FRAME_MAX];
    bool internal;            /* the manager's own (a §9 answer): no send_done */
    bool reply;               /* a reply the panel owes (zw_s2_command_is_reply) */
    bool span_only;           /* the frame out is its SPAN alone (the command did not fit with
                               * the extension): the command follows on the next nonce */
    uint16_t node;
    uint16_t gen;
    uint32_t tag;
    uint32_t order;
    uint64_t deadline;
    uint64_t started;
    size_t len;
    uint8_t plain[ZW_S2_PLAIN_MAX];
} zw_s2_send_t;

typedef struct {
    zw_crypto_port_t crypto;
    zw_s2_port_t port;
    zw_s2_config_t cfg;
    zw_s2_attempts_fn attempts;      /* NULL: one transmission each */
    zw_s2_jammed_fn jammed;          /* NULL: a jammed frame fails its send */
    bool started;
    uint32_t home_id;
    uint16_t own_node;
    uint64_t now;
    zw_ctr_drbg_t rng;               /* the panel's own: entropy inputs and sequence numbers */
    zw_s2_key_t keys[6];             /* S2 unauth, auth, access, S0, LR auth, LR access */
    uint16_t temp_node;              /* the node with a temporary key; 0: none */
    zw_sec_class_t exchange_class;   /* the class being exchanged with it (Network Key Verify) */
    uint8_t temp_key_ccm[16];
    uint8_t temp_pstring[32];
    zw_s2_peer_t peers[ZW_S2_PEERS];
    zw_s2_send_t sends[ZW_S2_SENDS];
    uint32_t next_order;
    uint16_t tx_gen;                 /* stamps every frame handed to port.send (its tag) */
    uint32_t dropped_unknown;        /* frames from nodes it does not know */
    uint8_t plain[ZW_S2_RX_MAX];     /* a received frame's plaintext */
    uint8_t aad[ZW_S2_AAD_FIXED + ZW_S2_RX_MAX];
    uint8_t tx[ZW_S2_FRAME_MAX];     /* a frame being built */
} zw_s2_t;

/* ---- Functions ---------------------------------------------------------------------------- */

/* Ready, no keys, no nodes; nothing is sent or accepted before zw_s2_start(). */
void zw_s2_init(zw_s2_t *s2, const zw_crypto_port_t *crypto, const zw_s2_port_t *port,
                const zw_s2_config_t *cfg);

/* serial-api.md §6.12: a frame the radio did not deliver (NoAck, Fail after time on the air,
 * NoRoute) goes again, up to `attempts`' count, each time as a new frame - a new sequence number
 * and, encrypted, the SPAN's next nonce (a Nonce Get is sent again the same way). A Nonce Report
 * never goes again (§6: one attempt). Without it (NULL, as after zw_s2_init) each frame goes
 * once. Set it after zw_s2_init. */
void zw_s2_set_attempts(zw_s2_t *s2, zw_s2_attempts_fn attempts);

/* serial-api.md §6.12: a frame the module could not send at all (Fail with 0 transmit ticks: a
 * jammed module) is not a delivery attempt. `jammed` decides: the same bytes again after
 * ZW_S2_JAM_WAIT_MS or at once - the same sequence number and nonce, as the node never heard
 * them; built again only if an SOS Nonce Report came meanwhile - or the send fails. Without it
 * (NULL, as after zw_s2_init) the send fails. Set it after zw_s2_init. */
void zw_s2_set_jammed(zw_s2_t *s2, zw_s2_jammed_fn jammed);

/* The network it belongs to (home ID, its own node ID) and its keys: each present key is
 * expanded once (§3) and kept in RAM; keys NULL - a simulated device before its key exchange -
 * starts with none. Seeds the manager's own CTR_DRBG from the port's random. Nodes, SPANs and
 * sends are forgotten (a restart of the panel: every peer is "none" again, §6). False when the
 * crypto port failed. */
ZW_CRYPTO_MUST_CHECK
bool zw_s2_start(zw_s2_t *s2, uint32_t home_id, uint16_t own_node, const zw_s2_keys_t *keys);

/* Stops it: keys, nodes, SPANs and sends forgotten and wiped, no network (home and node ID 0) -
 * nothing is sent or accepted until zw_s2_start() again (a first start that could not get its
 * keys, ZW01). */
void zw_s2_stop(zw_s2_t *s2);

/* One network key (ZW_S2_KEY_* bit), expanded: how a simulated device keeps the keys its key
 * exchange delivers. */
ZW_CRYPTO_MUST_CHECK
bool zw_s2_set_key(zw_s2_t *s2, uint8_t key_bit, const uint8_t key[16]);

/* The network key of a class for a node (§5 step 10's Network Key Report): NULL when absent. */
const uint8_t *zw_s2_network_key(const zw_s2_t *s2, uint16_t node, uint8_t key_bit);

/* Nodes. add_node registers (or updates) a node with its granted classes and the CC list of its
 * node information; false when the table is full. */
bool zw_s2_add_node(zw_s2_t *s2, uint16_t node, uint8_t granted, const uint8_t *nif_ccs,
                    size_t nif_len);
/* The node information frame the interview takes at its node-information step (ZW-009): its
 * supported CCs are ADDED to what the manager knows, never removed. Z-Wave JS Node.ts
 * updateNodeInfo adds a NIF's CCs only while interviewStage < NodeInfo; after that a NIF changes no
 * CC, so the owner calls this only for the interview's NIF (zw_net_app_update). False: not a known
 * node. */
bool zw_s2_add_nif_ccs(zw_s2_t *s2, uint16_t node, const uint8_t *nif_ccs, size_t nif_len);
/* A re-read's node information at its READY (BL-152): the CCs of its NIF REPLACE what the manager
 * knows, as Z-Wave JS's refreshInfo forgets the node's CCs and updateNodeInfo adds the new NIF's.
 * Its granted and refused classes and its classes-unknown mark stay as they are. False: not a
 * known node. */
bool zw_s2_set_nif_ccs(zw_s2_t *s2, uint16_t node, const uint8_t *nif_ccs, size_t nif_len);
/* A node whose classes are not known yet (its cache lost, before the interview's probe): Security
 * 2 from it is answered with a Nonce Report and tried, highest first, with every S2 key the panel
 * holds for a class not refused; the class that decrypts it is marked granted and the others stay
 * undecided (Z-Wave JS Security2CC.ts decryptSinglecast), so its highest class rises when a higher
 * one decrypts later. Clears the refused classes. zw_s2_set_granted ends it. False: not a known
 * node. */
bool zw_s2_set_classes_unknown(zw_s2_t *s2, uint16_t node, bool unknown);
/* The interview's probe result for one S2 class of a node whose classes are not all known (Z-Wave
 * JS Security2CC.interview: setSecurityClass(secClass, granted)): granted, or refused - a refused
 * class is no longer tried. False: not a known node or not an S2 class bit. */
bool zw_s2_set_class_result(zw_s2_t *s2, uint16_t node, uint8_t key_bit, bool granted);
/* The S2 class bits known refused (zw_s2_set_class_result), while its classes are not all known. */
uint8_t zw_s2_refused(const zw_s2_t *s2, uint16_t node);
/* Whether the node's classes are not all known (zw_s2_set_classes_unknown, until set_granted). */
bool zw_s2_classes_unknown(const zw_s2_t *s2, uint16_t node);
/* ZW-042: whether a command is Get-type as Z-Wave JS decides it (its class name ends in "Get"):
 * discarded when received broadcast or multicast, and - other than Security's and Security 2's
 * Nonce Get - a sign that a sleeping node is awake (Node.ts). */
bool zw_s2_command_is_get(const uint8_t *cmd, size_t len);
void zw_s2_remove_node(zw_s2_t *s2, uint16_t node);
bool zw_s2_has_node(const zw_s2_t *s2, uint16_t node);
bool zw_s2_set_granted(zw_s2_t *s2, uint16_t node, uint8_t granted);
uint8_t zw_s2_granted(const zw_s2_t *s2, uint16_t node);
/* The node's highest class: Access Control, Authenticated, Unauthenticated, S0, else none. */
zw_sec_class_t zw_s2_highest(const zw_s2_t *s2, uint16_t node);
/* §8's class for a command of CC `cc` to the node (what ZW_SEC_AUTO resolves to), as Z-Wave JS
 * Driver.ts isCCSecure: an S2 node's highest class for Basic and every CC it supports (its NIF or
 * its Commands Supported Report), none for a CC it does not support (a NOP ping); an S0 node's for
 * Basic and its secure-listed CCs; none for a node without keys and for Security 2's, Security 0's
 * and Transport Service's own commands. For a command whose outer layer (Multi Channel,
 * Supervision) hides the CC that decides. */
zw_sec_class_t zw_s2_class_for(const zw_s2_t *s2, uint16_t node, uint8_t cc);
/* ...for a command of CC `cc` to end point `endpoint` of the node (0: the node itself): an end
 * point of an S2 node is reached inside the node's security, whatever its own CC list says, so
 * its commands go at the node's highest class - the class of the Multi Channel layer that carries
 * them (Multi Channel is always one of an S2 node's secure CCs, both ways). */
zw_sec_class_t zw_s2_class_for_endpoint(const zw_s2_t *s2, uint16_t node, uint8_t endpoint,
                                        uint8_t cc);
/* Its secure CCs (its S2 Commands Supported Report; the manager also records one that arrives
 * at its highest class from endpoint 0). */
bool zw_s2_set_secure_ccs(zw_s2_t *s2, uint16_t node, const uint8_t *ccs, size_t n);
const zw_s2_node_stats_t *zw_s2_node_stats(const zw_s2_t *s2, uint16_t node);
zw_s2_span_state_t zw_s2_span_state(const zw_s2_t *s2, uint16_t node);

/* The key exchange (§5): the temporary key of the node being bootstrapped (one at a time; any
 * SPAN with it is forgotten), the class being exchanged (a SPAN extension from it is tried with
 * the temporary key first, then this class - its Network Key Verify), the temporary key deleted,
 * a SPAN forgotten. */
bool zw_s2_set_temp_key(zw_s2_t *s2, uint16_t node, const uint8_t key_ccm[16],
                        const uint8_t pstring[32]);
void zw_s2_set_exchange_class(zw_s2_t *s2, zw_sec_class_t cls);
void zw_s2_clear_temp_key(zw_s2_t *s2);
void zw_s2_forget_span(zw_s2_t *s2, uint16_t node);

/* Sends `cmd` to `node` at `cls` (ZW_SEC_AUTO: §8's choice), getting a nonce first when needed
 * (§6); send_done follows with `tag`. A secure frame's outcome waits ZW_S2_RESEND_MS after its
 * delivery for a Nonce Report (then it goes once more with a new SPAN). When a new SPAN's
 * extension and the command do not fit one frame, an S2 frame of the extension alone goes first
 * (§8 accepts those whatever the level) and the command follows on the next nonce. Sends to one
 * node go one at a time, in order. */
zw_s2_result_t zw_s2_send(zw_s2_t *s2, uint64_t now, uint16_t node, zw_sec_class_t cls,
                          const uint8_t *cmd, size_t len, uint32_t tag);

/* ZW-042: a broadcast or multicast frame from `node` (Z-Wave JS Driver.ts: the same security
 * check as a singlecast one, isSecurityLevelTooLow, then shouldDiscardCC drops a Get-type
 * command). False when it is not taken at all - not a node of the manager's, a Security 2 frame
 * (S2 multicast is not received, v1), or a Get; a drop for its level is counted as one (true). */
bool zw_s2_receive_group(zw_s2_t *s2, uint64_t now, uint16_t node, const uint8_t *cmd,
                         size_t len);

/* The end of a frame port.send took, as the module's callback said: delivered, or sent again
 * (zw_s2_set_attempts), or the send fails with ZW_S2_FAILED. */
void zw_s2_sent_tx(zw_s2_t *s2, uint64_t now, uint32_t tag, zw_s2_tx_t status);
/* The same with only delivered (ZW_S2_TX_OK) or not (ZW_S2_TX_NO_ACK): a simulated device's
 * radio. */
void zw_s2_sent(zw_s2_t *s2, uint64_t now, uint32_t tag, bool delivered);

/* Whether a command is a reply the panel owes a device - a Supervision Report, or one of
 * command-classes.md §5's answers (Version, Manufacturer Specific, Z-Wave Plus Info, Association,
 * Association Group Information, Multi Channel Association, Time and Date Reports), on its own or
 * inside Multi Channel: the device that sent the Get listens for it now, so it goes at once (never
 * into a sleeping device's wake-up queue, where a Time Report would be an hour old) and ahead of
 * the owner's commands. */
bool zw_s2_command_is_reply(const uint8_t *cmd, size_t len);
/* Whether the frame port.send is handed with this tag is a reply - a Nonce Report, or a frame of
 * a send whose command is a reply (its Nonce Get too): the owner queues it in its reply class
 * (serial-api.md's scheduler: nonce and Supervision replies first). Ask it within port.send. */
bool zw_s2_frame_is_reply(const zw_s2_t *s2, uint32_t tag);

/* A singlecast command from a node of this network (ApplicationCommandHandler's). */
void zw_s2_receive(zw_s2_t *s2, uint64_t now, uint16_t node, const uint8_t *cmd, size_t len);

/* Timers: nonce waits, the resend window. */
void zw_s2_poll(zw_s2_t *s2, uint64_t now);
uint64_t zw_s2_next_deadline(const zw_s2_t *s2);

#ifdef __cplusplus
}
#endif

#endif /* ZW_S2_H */
