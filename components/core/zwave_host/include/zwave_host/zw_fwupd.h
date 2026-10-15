/*
 * zw_fwupd.h - sending a device its new firmware (Firmware Update Meta Data, 7A; docs/features/36
 * ZW-033). Never the ZAC93's own firmware (that is network.md §14, separate): an end device's.
 *
 * The owner gives it an image already verified (the update's SHA-256, its file read by
 * zw_fwfile.h) and the device's 7A version; it:
 *   1. asks the device's meta data (MD Get): it must be upgradable, of the manufacturer the node
 *      cache knows, and have the target asked for;
 *   2. asks to update (Request Get: the image's CRC, the target, the fragment size, activation
 *      when the device supports it) and reads its answer (Request Report: FF, or why not -
 *      battery low, wrong hardware, not upgradable...);
 *   3. answers each Get with the fragments asked for (the last one flagged), one send at a time;
 *   4. waits for the device's Status Report (checksum or header errors, or OK with or without a
 *      restart, or waiting for activation - then Activation Set, and its report);
 *   5. ends. The owner interviews the device again after its wait time (its versions change).
 * Every wait has its own time-out; a device that stops asking ends it as TIMEOUT - most devices
 * keep running their old firmware then, and the installer may try again. Abort stops sending: the
 * device times out by itself (the CC has no "cancel").
 *
 * Portable C11, caller-owned state, no heap. The owner serializes calls.
 */
#ifndef ZW_FWUPD_H
#define ZW_FWUPD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zwave_proto/zw_cc_fwu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZW_FWUPD_MD_MS       10000u  /* MD Report / Request Report, each asked up to 3 times */
#define ZW_FWUPD_GET_MS      60000u  /* the device's next Get, from its Get or the last fragment
                                      * delivered (a batch in progress never times out) */
#define ZW_FWUPD_STATUS_MS   180000u /* its Status Report after the last fragment */
#define ZW_FWUPD_ACTIVATE_MS 60000u  /* its Activation Report */
#define ZW_FWUPD_TRIES       3u
#define ZW_FWUPD_TAG_BASE    0x46570000u /* 'FW' | sequence */
#define ZW_FWUPD_TAG_MASK    0xFFFF0000u

typedef enum {
    ZW_FWUPD_IDLE = 0,
    ZW_FWUPD_MD,        /* MD Get sent */
    ZW_FWUPD_REQUEST,   /* Request Get sent */
    ZW_FWUPD_TRANSFER,  /* fragments on the device's Gets */
    ZW_FWUPD_STATUS,    /* the last fragment sent: its verdict awaited */
    ZW_FWUPD_ACTIVATE,  /* Activation Set sent */
} zw_fwupd_state_t;

typedef enum {
    ZW_FWUPD_RUNNING = 0,
    ZW_FWUPD_OK_RESTART,      /* it restarts with the new firmware (after `wait_s`) */
    ZW_FWUPD_OK_NO_RESTART,   /* it runs the new firmware already */
    ZW_FWUPD_NOT_UPGRADABLE,  /* its meta data says so */
    ZW_FWUPD_WRONG_DEVICE,    /* another manufacturer than the node cache knows, or no such target */
    ZW_FWUPD_REFUSED,         /* its Request Report said no: `detail` (ZW_FWU_REQ_*) */
    ZW_FWUPD_DEVICE_FAILED,   /* its Status Report said no: `detail` (ZW_FWU_STATUS_*) */
    ZW_FWUPD_ACTIVATION_FAILED,
    ZW_FWUPD_TIMEOUT,         /* it stopped answering: `state` says where */
    ZW_FWUPD_SEND_FAILED,     /* the radio could not send (after its tries) */
    ZW_FWUPD_ABORTED,         /* the installer stopped it */
} zw_fwupd_result_t;

typedef struct {
    void *ctx;
    /* A command to the node (its encapsulation the owner's: the node's highest class). tag comes
     * back through zw_fwupd_send_done. false: it could not even be queued. */
    bool (*send)(void *ctx, uint16_t node, const uint8_t *cmd, size_t len, uint32_t tag);
} zw_fwupd_port_t;

typedef struct {
    zw_fwupd_port_t port;
    zw_fwupd_state_t state;
    zw_fwupd_result_t result;
    uint8_t detail;
    uint16_t wait_s;           /* OK_RESTART: when to interview it again */
    uint16_t node;
    uint8_t cc_version;
    uint8_t target;
    uint16_t expect_manufacturer;
    const uint8_t *image;
    size_t len;
    uint16_t checksum;
    uint16_t fragment;         /* bytes a fragment carries */
    uint16_t total;            /* fragments */
    uint16_t next;             /* the next fragment to send in the running Get (0: none) */
    uint16_t until;            /* ... and the last of that Get */
    uint16_t sent_max;         /* the highest fragment sent: the progress */
    uint16_t sending;          /* the fragment of the send on its way (its end names no number);
                                * 0: that send is no fragment (MD, Request, Activation Get/Set) */
    bool asked;                /* a Get came while it was on its way: that Get says what is next */
    zw_fwu_md_t md;
    uint8_t tries;
    bool out;                  /* a send is on its way */
    uint16_t seq;
    uint32_t wake_wait_ms;     /* zw_fwupd_expect_sleep: how long its first send may wait */
    bool waiting_wake;         /* its MD Get waits for the device's wake-up */
    uint64_t now, deadline;
} zw_fwupd_t;

void zw_fwupd_init(zw_fwupd_t *f, const zw_fwupd_port_t *port);

/* Starts: `max_payload` is the most command bytes one frame to the node carries (its
 * encapsulation taken off); fragments are sized from it and the device's own limit. false: busy,
 * or nothing to send. */
bool zw_fwupd_start(zw_fwupd_t *f, uint64_t now, uint16_t node, uint8_t cc_version,
                    uint16_t expect_manufacturer, uint8_t target, const uint8_t *image,
                    size_t len, size_t max_payload);
/* Before zw_fwupd_start, for a device that sleeps: its first send (MD Get) waits in the device's
 * wake-up queue up to `ms` - never asked again meanwhile - and the usual waits start once it went
 * (the owner keeps the device awake then: zw_net_hold_awake). Not woken in time: TIMEOUT in
 * state MD. For the next start only. */
void zw_fwupd_expect_sleep(zw_fwupd_t *f, uint32_t ms);
/* A command from `node` (only 7A's are taken): true when it was the update's. */
bool zw_fwupd_received(zw_fwupd_t *f, uint64_t now, uint16_t node, const uint8_t *cmd,
                       size_t len);
/* A send's end; true when the tag was the update's. */
bool zw_fwupd_send_done(zw_fwupd_t *f, uint64_t now, uint32_t tag, bool delivered);
void zw_fwupd_poll(zw_fwupd_t *f, uint64_t now);
uint64_t zw_fwupd_next_deadline(const zw_fwupd_t *f);
void zw_fwupd_abort(zw_fwupd_t *f, uint64_t now);
bool zw_fwupd_running(const zw_fwupd_t *f);

#ifdef __cplusplus
}
#endif

#endif /* ZW_FWUPD_H */
