/*
 * zw_prov.h - the SmartStart provisioning list (docs/features/36 network.md §4.2): devices
 * registered by their QR code or DSK before they are powered, so they join by themselves with
 * Security 2 and no button presses.
 *
 * An entry holds the DSK, the keys to grant (what the code requested; every S2 key for a typed
 * DSK - the panel grants only what the device asks for), classic or Long Range, a name, whether
 * it is active and its failures. It is "waiting to join" while active and not joined; after
 * ZW_PROV_FAILURES_MAX failed joins it turns inactive (the Z-Wave page says so) until the
 * installer turns it on again. A device that joined keeps its entry (its node and generation):
 * removing the device without forgetting its code leaves the entry waiting, so the device joins
 * again when it is reset - "Forget its code too" removes the entry.
 *
 * Stored as a blob (NVS zwave/prov): 'P' 'V', format, count, entry size, the entries, CRC-16. A
 * newer firmware may write longer entries: an older reader takes the part it knows. Portable C11,
 * caller-owned state, no heap.
 */
#ifndef ZW_PROV_H
#define ZW_PROV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zwave_proto/zw_qr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZW_PROV_MAX          32u
#define ZW_PROV_FAILURES_MAX 5u
#define ZW_PROV_NAME         24u   /* with its NUL */
#define ZW_PROV_ENTRY_BYTES  56u   /* format 1's entry */
#define ZW_PROV_BLOB_MAX     (7u + ZW_PROV_MAX * ZW_PROV_ENTRY_BYTES)

typedef struct {
    uint8_t dsk[16];
    uint8_t requested;       /* the keys its code asked for (0: a typed DSK - not known) */
    uint8_t grant;           /* the keys it may have (ZW_S2_KEY_* bits) */
    bool long_range;
    bool active;
    uint8_t failures;        /* failed joins since it was added or turned on */
    uint16_t node;           /* joined: its node ID and generation; 0: waiting */
    uint16_t generation;
    uint8_t generic;         /* from its code (0 for a typed DSK) */
    uint8_t specific;
    uint16_t manufacturer;
    uint16_t product_type;
    uint16_t product_id;
    char name[ZW_PROV_NAME];
} zw_prov_entry_t;

typedef struct {
    zw_prov_entry_t e[ZW_PROV_MAX];
    size_t n;
} zw_prov_t;

typedef enum {
    ZW_PROV_ADDED = 0,
    ZW_PROV_UPDATED,      /* its DSK was on the list: its details refreshed, turned on again */
    ZW_PROV_FULL,
    ZW_PROV_S2_ONLY,      /* an S2-only code (version 00): it can't join by itself - add it with
                           * "Add a device" and its PIN */
    ZW_PROV_INVALID
} zw_prov_result_t;

/* Where an entry stands, for its words. */
typedef enum {
    ZW_PROV_WAITING = 0,  /* "Waiting to join" */
    ZW_PROV_JOINED,       /* "Joined" */
    ZW_PROV_STOPPED,      /* "Stopped after 5 tries": inactive after its failures */
    ZW_PROV_OFF           /* turned off by the installer */
} zw_prov_status_t;

void zw_prov_init(zw_prov_t *p);
/* From a parsed QR code (zw_qr_parse). `name` may be NULL or empty. */
zw_prov_result_t zw_prov_add_qr(zw_prov_t *p, const zw_qr_t *q, const char *name);
/* From a DSK typed from a label: every S2 key may be granted. */
zw_prov_result_t zw_prov_add_dsk(zw_prov_t *p, const uint8_t dsk[16], bool long_range,
                                 const char *name);
/* The active, waiting entry whose DSK gives this NWI home ID with this protocol; -1: none. */
int zw_prov_find_nwi(const zw_prov_t *p, const uint8_t nwi[4], bool long_range);
int zw_prov_find_dsk(const zw_prov_t *p, const uint8_t dsk[16]);
/* The entry of a joined device (its node and generation); -1: none. */
int zw_prov_find_node(const zw_prov_t *p, uint16_t node, uint16_t generation);
/* It joined securely: its node, failures cleared. False when its DSK is not listed. */
bool zw_prov_joined(zw_prov_t *p, const uint8_t dsk[16], uint16_t node, uint16_t generation);
/* A join failed: one more failure; true when it has just turned inactive (the fifth). */
bool zw_prov_failed(zw_prov_t *p, const uint8_t dsk[16]);
/* Its device was removed or left: the entry waits again (unless forgotten). */
void zw_prov_node_gone(zw_prov_t *p, uint16_t node, uint16_t generation);
/* The installer turns it on (failures cleared) or off. */
bool zw_prov_set_active(zw_prov_t *p, size_t i, bool active);
bool zw_prov_set_name(zw_prov_t *p, size_t i, const char *name);
bool zw_prov_remove(zw_prov_t *p, size_t i);
/* Every entry dropped (a network reset). */
void zw_prov_clear(zw_prov_t *p);
zw_prov_status_t zw_prov_status(const zw_prov_entry_t *e);
/* Whether any active entry waits to join: the panel listens for SmartStart only then. */
bool zw_prov_waiting(const zw_prov_t *p);

/* The blob. encode: its length (0 when cap is short). decode: false when damaged - the list is
 * then empty. */
size_t zw_prov_encode(const zw_prov_t *p, uint8_t *out, size_t cap);
bool zw_prov_decode(zw_prov_t *p, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZW_PROV_H */
