/*
 * zw_prov.c - the SmartStart provisioning list (see zw_prov.h).
 */
#include "zwave_host/zw_prov.h"

#include <string.h>

#include "zwave_host/zw_s2.h"
#include "zwave_proto/zw_crc16.h"
#include "zwave_proto/zw_dsk.h"

#define FORMAT 1u
#define HEAD   5u /* 'P' 'V' format count entry-size */

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(((unsigned)p[0] << 8) | (unsigned)p[1]);
}

static void set_name(zw_prov_entry_t *e, const char *name)
{
    memset(e->name, 0, sizeof e->name);
    if (name != NULL) {
        size_t n = strlen(name);
        if (n >= sizeof e->name) {
            n = sizeof e->name - 1u;
            /* never cut a UTF-8 character in two */
            while (n > 0u && ((unsigned char)name[n] & 0xC0u) == 0x80u) {
                n--;
            }
        }
        memcpy(e->name, name, n);
    }
}

void zw_prov_init(zw_prov_t *p)
{
    memset(p, 0, sizeof *p);
}

/* A new entry, or the one with this DSK refreshed (turned on, failures cleared). */
static zw_prov_entry_t *slot(zw_prov_t *p, const uint8_t dsk[16], zw_prov_result_t *r)
{
    const int i = zw_prov_find_dsk(p, dsk);
    if (i >= 0) {
        *r = ZW_PROV_UPDATED;
        zw_prov_entry_t *e = &p->e[i];
        e->active = true;
        e->failures = 0u;
        return e;
    }
    if (p->n >= ZW_PROV_MAX) {
        *r = ZW_PROV_FULL;
        return NULL;
    }
    zw_prov_entry_t *e = &p->e[p->n++];
    memset(e, 0, sizeof *e);
    memcpy(e->dsk, dsk, sizeof e->dsk);
    e->active = true;
    *r = ZW_PROV_ADDED;
    return e;
}

zw_prov_result_t zw_prov_add_qr(zw_prov_t *p, const zw_qr_t *q, const char *name)
{
    if (p == NULL || q == NULL) {
        return ZW_PROV_INVALID;
    }
    if (q->version != ZW_QR_VERSION_SMARTSTART) {
        return ZW_PROV_S2_ONLY;
    }
    const uint8_t keys = (uint8_t)(q->requested_keys & ZW_S2_KEYS_S2);
    if (keys == 0u) {
        return ZW_PROV_INVALID; /* SmartStart is S2 only: a code asking for none is wrong */
    }
    zw_prov_result_t r;
    zw_prov_entry_t *e = slot(p, q->dsk, &r);
    if (e == NULL) {
        return r;
    }
    e->requested = keys;
    e->grant = keys;
    /* Long Range only when the device offers nothing else (phase 3 adds the choice) */
    e->long_range = (q->protocols & ZW_QR_PROTO_CLASSIC) == 0u &&
                    (q->protocols & ZW_QR_PROTO_LONG_RANGE) != 0u;
    e->generic = q->generic;
    e->specific = q->specific;
    e->manufacturer = q->manufacturer;
    e->product_type = q->product_type;
    e->product_id = q->product_id;
    if (name != NULL && name[0] != '\0') {
        set_name(e, name);
    }
    return r;
}

zw_prov_result_t zw_prov_add_dsk(zw_prov_t *p, const uint8_t dsk[16], bool long_range,
                                 const char *name)
{
    if (p == NULL || dsk == NULL) {
        return ZW_PROV_INVALID;
    }
    zw_prov_result_t r;
    zw_prov_entry_t *e = slot(p, dsk, &r);
    if (e == NULL) {
        return r;
    }
    if (r == ZW_PROV_ADDED) {
        e->grant = ZW_S2_KEYS_S2;
        e->long_range = long_range;
    }
    if (name != NULL && name[0] != '\0') {
        set_name(e, name);
    }
    return r;
}

int zw_prov_find_nwi(const zw_prov_t *p, const uint8_t nwi[4], bool long_range)
{
    for (size_t i = 0; i < p->n; i++) {
        const zw_prov_entry_t *e = &p->e[i];
        uint8_t n[4], a[4];
        zw_dsk_home_ids(e->dsk, n, a);
        if (e->active && e->node == 0u && e->long_range == long_range &&
            memcmp(n, nwi, sizeof n) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int zw_prov_find_dsk(const zw_prov_t *p, const uint8_t dsk[16])
{
    for (size_t i = 0; i < p->n; i++) {
        if (memcmp(p->e[i].dsk, dsk, sizeof p->e[i].dsk) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int zw_prov_find_node(const zw_prov_t *p, uint16_t node, uint16_t generation)
{
    for (size_t i = 0; node != 0u && i < p->n; i++) {
        if (p->e[i].node == node && p->e[i].generation == generation) {
            return (int)i;
        }
    }
    return -1;
}

bool zw_prov_joined(zw_prov_t *p, const uint8_t dsk[16], uint16_t node, uint16_t generation)
{
    const int i = zw_prov_find_dsk(p, dsk);
    if (i < 0) {
        return false;
    }
    p->e[i].node = node;
    p->e[i].generation = generation;
    p->e[i].failures = 0u;
    return true;
}

bool zw_prov_failed(zw_prov_t *p, const uint8_t dsk[16])
{
    const int i = zw_prov_find_dsk(p, dsk);
    if (i < 0 || !p->e[i].active) {
        return false;
    }
    zw_prov_entry_t *e = &p->e[i];
    if (e->failures < 0xFFu) {
        e->failures++;
    }
    if (e->failures >= ZW_PROV_FAILURES_MAX) {
        e->active = false;
        return true;
    }
    return false;
}

void zw_prov_node_gone(zw_prov_t *p, uint16_t node, uint16_t generation)
{
    const int i = zw_prov_find_node(p, node, generation);
    if (i >= 0) {
        p->e[i].node = 0u;
        p->e[i].generation = 0u;
    }
}

bool zw_prov_set_active(zw_prov_t *p, size_t i, bool active)
{
    if (i >= p->n) {
        return false;
    }
    p->e[i].active = active;
    if (active) {
        p->e[i].failures = 0u;
    }
    return true;
}

bool zw_prov_set_name(zw_prov_t *p, size_t i, const char *name)
{
    if (i >= p->n) {
        return false;
    }
    set_name(&p->e[i], name);
    return true;
}

bool zw_prov_remove(zw_prov_t *p, size_t i)
{
    if (i >= p->n) {
        return false;
    }
    memmove(&p->e[i], &p->e[i + 1u], (p->n - i - 1u) * sizeof p->e[0]);
    p->n--;
    memset(&p->e[p->n], 0, sizeof p->e[0]);
    return true;
}

void zw_prov_clear(zw_prov_t *p)
{
    zw_prov_init(p);
}

zw_prov_status_t zw_prov_status(const zw_prov_entry_t *e)
{
    if (e->node != 0u) {
        return ZW_PROV_JOINED;
    }
    if (!e->active) {
        return e->failures >= ZW_PROV_FAILURES_MAX ? ZW_PROV_STOPPED : ZW_PROV_OFF;
    }
    return ZW_PROV_WAITING;
}

bool zw_prov_waiting(const zw_prov_t *p)
{
    for (size_t i = 0; i < p->n; i++) {
        if (zw_prov_status(&p->e[i]) == ZW_PROV_WAITING) {
            return true;
        }
    }
    return false;
}

/* ---- The blob --------------------------------------------------------------------------------- */

static void put_entry(uint8_t *b, const zw_prov_entry_t *e)
{
    memset(b, 0, ZW_PROV_ENTRY_BYTES);
    memcpy(b, e->dsk, 16u);
    b[16] = e->requested;
    b[17] = e->grant;
    b[18] = (uint8_t)((e->active ? 0x01u : 0u) | (e->long_range ? 0x02u : 0u));
    b[19] = e->failures;
    put16(&b[20], e->node);
    put16(&b[22], e->generation);
    b[24] = e->generic;
    b[25] = e->specific;
    put16(&b[26], e->manufacturer);
    put16(&b[28], e->product_type);
    put16(&b[30], e->product_id);
    memcpy(&b[32], e->name, ZW_PROV_NAME);
    b[32u + ZW_PROV_NAME - 1u] = 0u;
}

static void get_entry(const uint8_t *b, zw_prov_entry_t *e)
{
    memset(e, 0, sizeof *e);
    memcpy(e->dsk, b, 16u);
    e->requested = b[16];
    e->grant = (uint8_t)(b[17] & ZW_S2_KEYS_S2);
    e->active = (b[18] & 0x01u) != 0u;
    e->long_range = (b[18] & 0x02u) != 0u;
    e->failures = b[19];
    e->node = get16(&b[20]);
    e->generation = get16(&b[22]);
    e->generic = b[24];
    e->specific = b[25];
    e->manufacturer = get16(&b[26]);
    e->product_type = get16(&b[28]);
    e->product_id = get16(&b[30]);
    memcpy(e->name, &b[32], ZW_PROV_NAME);
    e->name[ZW_PROV_NAME - 1u] = '\0';
}

size_t zw_prov_encode(const zw_prov_t *p, uint8_t *out, size_t cap)
{
    const size_t len = HEAD + p->n * ZW_PROV_ENTRY_BYTES + 2u;
    if (out == NULL || cap < len) {
        return 0u;
    }
    out[0] = 'P';
    out[1] = 'V';
    out[2] = FORMAT;
    out[3] = (uint8_t)p->n;
    out[4] = ZW_PROV_ENTRY_BYTES;
    for (size_t i = 0; i < p->n; i++) {
        put_entry(&out[HEAD + i * ZW_PROV_ENTRY_BYTES], &p->e[i]);
    }
    put16(&out[len - 2u], zw_crc16(ZW_CRC16_INIT, out, len - 2u));
    return len;
}

bool zw_prov_decode(zw_prov_t *p, const uint8_t *buf, size_t len)
{
    zw_prov_init(p);
    if (buf == NULL || len < HEAD + 2u || buf[0] != 'P' || buf[1] != 'V' || buf[2] < FORMAT ||
        buf[3] > ZW_PROV_MAX || buf[4] < ZW_PROV_ENTRY_BYTES) {
        return false;
    }
    const size_t n = (unsigned)buf[3];
    const size_t size = (unsigned)buf[4];
    const size_t want = HEAD + n * size + 2u;
    if (len < want || get16(&buf[want - 2u]) != zw_crc16(ZW_CRC16_INIT, buf, want - 2u)) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        get_entry(&buf[HEAD + i * size], &p->e[i]);
    }
    p->n = n;
    return true;
}
