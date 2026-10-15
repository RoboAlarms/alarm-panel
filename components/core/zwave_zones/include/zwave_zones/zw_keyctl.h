/*
 * zw_keyctl.h - which Z-Wave keypads may arm and disarm the panel (docs/features/36 ZW-027,
 * ZW-031): the installer's explicit assignments, stored, and the commands of an assigned keypad
 * handed to zw_keypad.h's adapter - and through it to the alarm engine's own authority (the
 * user's code, its partitions, arm-only, duress, lockout). Separate from zw_feedback.h: a keypad
 * showing the panel's state is not allowed to control it, and control is never implied by it.
 *
 * An assignment names the keypad by home ID, node, generation and end point, and the Security 2
 * class it holds when assigned (Authenticated or Access Control): a command counts only from that
 * very device, at that very class. A removed device, a node ID given to another, a new network or
 * a re-pairing at another class leaves the assignment unusable until the installer assigns again.
 * Emergency buttons (fire, police, medical) count only when allowed explicitly (`panic_mask`,
 * bits 1 << alarm_panic_t).
 *
 * The owner copies an assigned keypad's Entry Control Notification out of the network task's
 * lock (zwkc_claims) and runs it later with zwkc_run, from where it may wait for the alarm task.
 * No PIN is kept here or anywhere after the command ran.
 *
 * NVS `zwave/keypad_ctl`: 'K' 'C' | format 1 | slots 8 | 8 x (home u32, node u16, generation u16,
 * endpoint u8, partition u8, security u8 (zw_sec_class_t: 2 Authenticated, 3 Access Control),
 * panic u8) | CRC-16 (little-endian). The security numbers are stored: never renumber them.
 */
#ifndef ZW_KEYCTL_H
#define ZW_KEYCTL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_types.h"
#include "zwave_host/zw_net.h"
#include "zwave_zones/zw_keypad.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZWKC_SLOTS     8u
#define ZWKC_BLOB_SIZE (4u + ZWKC_SLOTS * 12u + 2u)
/* The alarm source numbers of the slots' keypads (ALARM_SRC_KEYPAD): the top ZWKC_SLOTS device
 * numbers, so their events and lockouts are each keypad's own. */
#define ZWKC_DEVICE(slot) ((uint16_t)(ALARM_MAX_DEVICES - ZWKC_SLOTS + 1u + (slot)))

typedef struct {
    uint32_t home;
    uint16_t node, generation;
    uint8_t endpoint;
    uint8_t partition;   /* 0: the slot is off */
    uint8_t security;    /* zw_sec_class_t */
    uint8_t panic_mask;  /* 1 << ALARM_PANIC_*: the emergency buttons it may use */
} zwkc_binding_t;

typedef struct {
    zwkc_binding_t binding;
    zwk_t adapter; /* zw_keypad.h: the last commands' results for retries, never a PIN */
} zwkc_slot_t;

typedef struct {
    zwkc_slot_t slot[ZWKC_SLOTS];
    zwk_port_t port; /* the alarm engine's command, as the owner runs it */
} zwkc_t;

/* Why a device may or may not be assigned (the device page's words). */
typedef enum {
    ZWKC_FIT_NONE = 0,     /* no Entry Control: not a keypad */
    ZWKC_FIT_OK,
    ZWKC_FIT_NOT_READY,    /* its interview has not finished */
    ZWKC_FIT_NOT_SECURE    /* not S2 Authenticated or Access Control, or Entry Control not secure */
} zwkc_fit_t;

void zwkc_init(zwkc_t *k, const zwk_port_t *port);
zwkc_fit_t zwkc_fit(const zw_net_t *net, uint16_t node);
/* The slot assigned to `node` (any generation), or ZWKC_SLOTS. */
size_t zwkc_slot_of(const zwkc_t *k, uint16_t node);
/* Its own slot, else the first free one; ZWKC_SLOTS: full. */
size_t zwkc_slot_for(const zwkc_t *k, uint16_t node);
/* Assign `node` (its current generation and highest class) to `partition` with `panic_mask`,
 * or turn the slot off (partition 0). False: not fit, another slot has it, bad values. */
bool zwkc_assign(zwkc_t *k, size_t slot, const zw_net_t *net, uint16_t node, uint8_t partition,
                 uint8_t panic_mask);
/* Whether the slot's keypad is the device it was assigned to, as the network knows it now. */
bool zwkc_live(const zwkc_t *k, size_t slot, const zw_net_t *net);

/* Under the network owner's lock: whether this received command is an assigned keypad's Entry
 * Control Notification (the slot in *slot) - the owner copies it and answers its Supervision as
 * received; everything else is not the keypad table's. */
bool zwkc_claims(const zwkc_t *k, const zw_net_t *net, const zw_s2_rx_t *rx, size_t *slot);
/* Later, without the lock: run a claimed command. `home` and `generation` are the network's
 * facts at claim time. The reply (the engine's result) goes back to the caller; a PIN in `rx` is
 * the caller's to erase. */
zwk_result_t zwkc_run(zwkc_t *k, size_t slot, uint32_t home, uint16_t generation,
                      const zw_s2_rx_t *rx, uint64_t now_ms, zwk_reply_t *reply);

size_t zwkc_encode(const zwkc_t *k, uint8_t *out, size_t cap);
/* False (and every slot off) when the stored value is damaged or of another format. */
bool zwkc_decode(zwkc_t *k, const uint8_t *blob, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZW_KEYCTL_H */
