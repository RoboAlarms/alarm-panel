/*
 * net_harness.h - a panel for the network tests (docs/features/36 network.md §2-§12): zw_net with
 * interviews on, against the fake ZAC93 and simulated devices on a fake clock, and an owner that
 * keeps what the panel keeps in NVS (the node cache, the keys and the home ID they belong to)
 * across a restart of the panel, grants what devices ask for, types their PIN (or leaves it to the
 * test), and records every event - handing what it hears on to a test's own other half (h_extra_t:
 * the bridge to the zones) when one is set. Shared by test_zw_interview.c and test_zw_house.c.
 */
#ifndef NET_HARNESS_H
#define NET_HARNESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fake_zac93.h"
#include "fake_zdev.h"
#include "zw_host_crypto.h"
#include "zwave_host/zw_net.h"
#include "zwave_host/zw_prov.h"

#define H_EVENTS 256u
#define H_RX     256u

typedef struct {
    uint16_t node;
    uint8_t ep;
    zw_sec_class_t security;
    uint64_t at;
    size_t len;
    uint8_t cmd[48];
} h_rx_t;

/* The owner's other half, fed what the owner hears - a test's bridge to the zones (zwave_zones);
 * every member optional. */
typedef struct {
    void *ctx;
    void (*boot)(void *ctx);  /* the panel (re)starts: zw_net is fresh, before its start-up */
    void (*link_up)(void *ctx, const zw_module_facts_t *facts);
    void (*link_down)(void *ctx); /* the link went down, or the module restarted */
    void (*received)(void *ctx, const zw_s2_rx_t *rx);
    bool (*send_done)(void *ctx, uint32_t tag, zw_s2_result_t r); /* true: its own tag */
    void (*event)(void *ctx, const zw_net_event_t *ev);
    void (*poll)(void *ctx, uint64_t now);
    uint64_t (*deadline)(void *ctx);
} h_extra_t;

typedef struct {
    zw_net_t net;
    fake_zac93_t zac;
    fake_wire_t wire;
    zw_host_crypto_t hc;
    zw_crypto_port_t crypto;
    zw_net_config_t cfg;
    /* what the owner keeps */
    uint8_t blob[8192];
    size_t blob_len;
    unsigned stores, key_stores;
    /* what it heard */
    zw_net_event_t ev[H_EVENTS];
    uint64_t ev_at[H_EVENTS];
    size_t ev_n;
    zw_incl_event_t incl[64];
    size_t incl_n;
    h_rx_t rx[H_RX];
    size_t rx_n;
    uint32_t done_tag[32];
    zw_s2_result_t done_r[32];
    size_t done_n;
    unsigned ups;
    struct {                /* module frames nobody else took (zw_net_port_t.frame) */
        uint8_t type, func;
        size_t len;
        uint8_t payload[16];
    } frames[16];
    size_t frames_n;
    /* how it answers */
    bool allowed;           /* installer mode, every partition disarmed */
    zw_iv_quirk_t quirk;    /* the quirks table's row for every model */
    bool has_quirk;
    uint16_t pin;           /* the PIN it types when one is asked */
    bool wrong_pin;         /* h_include types the label's PIN with two digits wrong */
    uint8_t grant;          /* what it grants (0: what was asked) */
    bool answer_supervision; /* a supervised command is answered at once with its Supervision
                              * Report through zw_net_send, as zwave_zones does */
    bool hold_pin;          /* the PIN is not typed when asked: the test types it (zw_incl_pin) */
    bool cancel_at_grant;   /* "Add it without encryption" (zw_incl_cancel) instead of a grant */
    void (*on_incl)(const zw_incl_event_t *ev); /* a test's look at the inclusion's news */
    /* SmartStart (network.md §4.2): the owner's provisioning list, kept as the panel keeps it -
     * a join or a failure noted, a removed device's entry waiting again - and, while `smart`,
     * zw_net_smartstart told whether one waits after each change (h_smart) */
    zw_prov_t prov;
    bool smart;
    h_extra_t extra;        /* set after h_init, then h_boot */
} harness_t;

/* The owner's tag on the Supervision Reports it sends (answer_supervision). */
#define H_SUPERVISION_TAG 0x53555056u

extern harness_t H;

/* A fresh world: a factory ZAC93 with no devices, a panel with nothing stored, booted. */
void h_init(void);
/* The panel (re)starts: a fresh zw_net over the same module, with what the owner stored. */
void h_boot(void);
void h_run(uint64_t t);
void h_run_for(uint64_t ms);

/* The last event of a kind for a node (0: any node) since h_clear_events, or NULL. */
const zw_net_event_t *h_find(zw_net_event_kind_t kind, uint16_t node);
size_t h_count(zw_net_event_kind_t kind, uint16_t node);
void h_clear_events(void);
/* Commands the owner received from a node with this CC and command. */
size_t h_rx_count(uint16_t node, uint8_t cc, uint8_t cmd);

/* Adds a device (on the fake radio first): "Add a device", its button, the key exchange with
 * the harness's answers; the node it got, 0 when it did not join. */
uint16_t h_include(fake_zdev_t *d);
/* Runs until the node's interview is done (READY) or `ms` passed; whether it is. */
bool h_until_ready(uint16_t node, uint64_t ms);

/* SmartStart: the owner tells zw_net whether an entry waits (while H.smart). */
void h_smart(void);

/* The node's record in the panel's cache. */
const zw_node_record_t *h_rec(uint16_t node);

/* A device model the interview can use: it answers, sleeps for real when it is a sleeper. */
void h_device(fake_zdev_t *d, uint64_t seed, bool sleeps);

#endif /* NET_HARNESS_H */
