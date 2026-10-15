/*
 * zbz_registry.c - the panel's Zigbee devices and what of them is stored (zbz_registry.h;
 * docs/features/20 devices.md §4; ZB-010, ZB-021). One record per device in a fixed table of
 * ZBZ_DEV_MAX slots; slot i is NVS key "d<i>" (two digits). The record codec is field by field
 * (id u8 | length u8 | value), so a later firmware only adds fields; the decisions about damaged
 * and newer records are in the header.
 */
#include "zigbee_zones/zbz_registry.h"

#include <string.h>

#define EP_FIELD_LEN 11u
#define HA_PROFILE   0x0104u
#define GP_ENDPOINT  242u

/* ---- little-endian helpers -------------------------------------------------------------------- */

static uint64_t rdn(const uint8_t *p, size_t n)
{
    uint64_t v = 0;
    for (size_t i = n; i > 0; i--) {
        v = (v << 8) | (uint64_t)(unsigned)p[i - 1];
    }
    return v;
}

static void wrn(uint8_t *p, uint64_t v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)(v & 0xFFu);
        v >>= 8;
    }
}

static size_t text_len(const char *s, size_t size)
{
    size_t n = 0;
    while (n + 1u < size && s[n] != '\0') {
        n++;
    }
    return n;
}

/* ---- the table -------------------------------------------------------------------------------- */

void zbz_reg_init(zbz_registry_t *r)
{
    memset(r, 0, sizeof(*r));
    r->next_id = 1;
}

static void slot_key(char key[ZBZ_KEY_LEN], size_t i)
{
    memset(key, 0, ZBZ_KEY_LEN);
    key[0] = 'd';
    key[1] = (char)('0' + (int)(i / 10u));
    key[2] = (char)('0' + (int)(i % 10u));
}

/* The slot "dNN" names, or -1. */
static int key_slot(const char *key)
{
    if (key == NULL || key[0] != 'd' || key[1] < '0' || key[1] > '9' || key[2] < '0' ||
        key[2] > '9' || key[3] != '\0') {
        return -1;
    }
    int i = (key[1] - '0') * 10 + (key[2] - '0');
    return (i < (int)ZBZ_DEV_MAX) ? i : -1;
}

static int slot_of(const zbz_registry_t *r, const zbz_device_t *d)
{
    if (d == NULL || d < &r->dev[0] || d >= &r->dev[ZBZ_DEV_MAX]) {
        return -1;
    }
    return (int)(d - &r->dev[0]);
}

static uint64_t bit(size_t i)
{
    return (uint64_t)1u << i;
}

zbz_device_t *zbz_reg_find(zbz_registry_t *r, uint64_t ieee)
{
    if (ieee == 0u) {
        return NULL;
    }
    for (size_t i = 0; i < ZBZ_DEV_MAX; i++) {
        if (r->dev[i].used && r->dev[i].ieee == ieee) {
            return &r->dev[i];
        }
    }
    return NULL;
}

const zbz_device_t *zbz_reg_cget(const zbz_registry_t *r, uint16_t id)
{
    if (id == 0u) {
        return NULL;
    }
    for (size_t i = 0; i < ZBZ_DEV_MAX; i++) {
        if (r->dev[i].used && r->dev[i].id == id) {
            return &r->dev[i];
        }
    }
    return NULL;
}

zbz_device_t *zbz_reg_get(zbz_registry_t *r, uint16_t id)
{
    const zbz_device_t *d = zbz_reg_cget(r, id);
    return (d == NULL) ? NULL : &r->dev[d - &r->dev[0]];
}

size_t zbz_reg_count(const zbz_registry_t *r)
{
    size_t n = 0;
    for (size_t i = 0; i < ZBZ_DEV_MAX; i++) {
        n += r->dev[i].used ? 1u : 0u;
    }
    return n;
}

static uint16_t id_after(uint16_t id)
{
    if ((unsigned)id >= 0xFFFFu) {
        return 1;
    }
    return (uint16_t)((unsigned)id + 1u);
}

zbz_device_t *zbz_reg_add(zbz_registry_t *r, uint64_t ieee, uint16_t nwk)
{
    if (r->read_only || ieee == 0u || zbz_reg_find(r, ieee) != NULL) {
        return NULL;
    }
    size_t slot = ZBZ_DEV_MAX;
    for (size_t i = 0; i < ZBZ_DEV_MAX; i++) {
        if (!r->dev[i].used) {
            slot = i;
            break;
        }
    }
    if (slot == ZBZ_DEV_MAX) {
        return NULL;
    }
    /* At most ZBZ_DEV_MAX ids are in use: one of the next ZBZ_DEV_MAX + 1 is free. */
    uint16_t id = (r->next_id == 0u) ? (uint16_t)1u : r->next_id;
    while (zbz_reg_cget(r, id) != NULL) {
        id = id_after(id);
    }
    zbz_device_t *d = &r->dev[slot];
    memset(d, 0, sizeof(*d));
    d->used = true;
    d->id = id;
    d->ieee = ieee;
    d->nwk = nwk;
    r->next_id = id_after(id);
    r->dirty |= bit(slot);
    r->meta_dirty = true;
    return d;
}

bool zbz_reg_drop(zbz_registry_t *r, uint16_t id)
{
    zbz_device_t *d = zbz_reg_get(r, id);
    if (r->read_only || d == NULL) {
        return false;
    }
    size_t slot = (size_t)(d - &r->dev[0]);
    memset(d, 0, sizeof(*d));
    r->dirty |= bit(slot);
    r->meta_dirty = true;
    return true;
}

void zbz_reg_touch(zbz_registry_t *r, const zbz_device_t *d)
{
    int slot = slot_of(r, d);
    if (r->read_only || slot < 0 || !r->dev[slot].used) {
        return;
    }
    r->dirty |= bit((size_t)slot);
}

/* ---- endpoints -------------------------------------------------------------------------------- */

uint16_t zbz_cluster_bits(const uint16_t *ids, uint8_t count)
{
    static const struct {
        uint16_t id;
        uint16_t bit;
    } map[] = {
        {ZSRC_ZB_ZCL_IAS_ZONE, ZSRC_ZB_CL_IAS_ZONE},   {ZSRC_ZB_ZCL_ON_OFF, ZSRC_ZB_CL_ON_OFF},
        {ZSRC_ZB_ZCL_OCCUPANCY, ZSRC_ZB_CL_OCCUPANCY}, {ZSRC_ZB_ZCL_TEMP_MEAS, ZSRC_ZB_CL_TEMP},
        {ZSRC_ZB_ZCL_POWER_CFG, ZSRC_ZB_CL_POWER_CFG}, {ZSRC_ZB_ZCL_POLL_CTRL, ZSRC_ZB_CL_POLL_CTRL},
        {ZSRC_ZB_ZCL_TUYA, ZSRC_ZB_CL_TUYA},           {ZSRC_ZB_ZCL_IAS_WD, ZSRC_ZB_CL_IAS_WD},
        {ZSRC_ZB_ZCL_IAS_ACE, ZSRC_ZB_CL_IAS_ACE},     {ZSRC_ZB_ZCL_IDENTIFY, ZSRC_ZB_CL_IDENTIFY},
        {ZSRC_ZB_ZCL_BASIC, ZSRC_ZB_CL_BASIC},         {ZSRC_ZB_ZCL_OTA, ZSRC_ZB_CL_OTA},
        {ZSRC_ZB_ZCL_DIAGNOSTICS, ZSRC_ZB_CL_DIAGNOSTICS},
    };
    uint16_t bits = 0;
    if (ids == NULL) {
        return 0;
    }
    for (uint8_t i = 0; i < count; i++) {
        for (size_t k = 0; k < sizeof(map) / sizeof(map[0]); k++) {
            if (ids[i] == map[k].id) {
                bits = (uint16_t)(bits | map[k].bit);
            }
        }
    }
    return bits;
}

/* How much the panel uses an endpoint: 3 zone inputs or an alarm device, 2 only device
 * management clusters, 1 Home Automation with nothing known, 0 Green Power or another profile. */
static int ep_score(const zbz_endpoint_t *e)
{
    const uint16_t inputs = ZSRC_ZB_CL_IAS_ZONE | ZSRC_ZB_CL_ON_OFF | ZSRC_ZB_CL_OCCUPANCY |
                            ZSRC_ZB_CL_TEMP | ZSRC_ZB_CL_TUYA | ZSRC_ZB_CL_IAS_WD |
                            ZSRC_ZB_CL_IAS_ACE;
    uint16_t all = (uint16_t)(e->in_clusters | e->out_clusters);
    if (e->endpoint == GP_ENDPOINT || e->profile != HA_PROFILE) {
        return 0;
    }
    if ((all & inputs) != 0u) {
        return 3;
    }
    return (all != 0u) ? 2 : 1;
}

/* Comes before: more used, then the lower endpoint number. */
static bool ep_before(const zbz_endpoint_t *a, const zbz_endpoint_t *b)
{
    int sa = ep_score(a);
    int sb = ep_score(b);
    return (sa != sb) ? (sa > sb) : (a->endpoint < b->endpoint);
}

static void ep_sort(zbz_device_t *d)
{
    for (uint8_t i = 1; i < d->ep_count; i++) {
        zbz_endpoint_t e = d->ep[i];
        uint8_t j = i;
        while (j > 0u && ep_before(&e, &d->ep[(unsigned)j - 1u])) {
            d->ep[j] = d->ep[(unsigned)j - 1u];
            j--;
        }
        d->ep[j] = e;
    }
}

bool zbz_dev_set_endpoint(zbz_device_t *d, const zsrc_zb_simple_desc_t *s)
{
    if (d == NULL || s == NULL || s->endpoint == 0u ||
        (s->endpoint > 240u && s->endpoint != GP_ENDPOINT)) {
        return false;
    }
    uint8_t in_n = (s->in_count > ZSRC_ZB_EP_CLUSTERS_MAX) ? (uint8_t)ZSRC_ZB_EP_CLUSTERS_MAX
                                                          : s->in_count;
    uint8_t out_n = (s->out_count > ZSRC_ZB_EP_CLUSTERS_MAX) ? (uint8_t)ZSRC_ZB_EP_CLUSTERS_MAX
                                                            : s->out_count;
    zbz_endpoint_t e = {
        .endpoint = s->endpoint,
        .profile = s->profile,
        .device_id = s->device_id,
        .in_clusters = zbz_cluster_bits(s->in, in_n),
        .out_clusters = zbz_cluster_bits(s->out, out_n),
        .zone_type = ZSRC_ZB_ZONE_TYPE_NONE,
    };
    for (uint8_t i = 0; i < d->ep_count; i++) {
        if (d->ep[i].endpoint == e.endpoint) {
            if ((e.in_clusters & ZSRC_ZB_CL_IAS_ZONE) != 0u) {
                e.zone_type = d->ep[i].zone_type; /* read from IAS Zone, not the descriptor */
            }
            d->ep[i] = e;
            ep_sort(d);
            return true;
        }
    }
    if (d->ep_count < ZBZ_EP_MAX) {
        d->ep[d->ep_count++] = e;
        ep_sort(d);
        return true;
    }
    /* Full: the last is the least used; the new one takes its place only if it is used more. */
    zbz_endpoint_t *last = &d->ep[ZBZ_EP_MAX - 1u];
    if (ep_score(&e) <= ep_score(last)) {
        return false;
    }
    *last = e;
    ep_sort(d);
    return true;
}

bool zbz_dev_desc(const zbz_device_t *d, uint8_t endpoint, zsrc_zb_desc_t *out)
{
    if (d == NULL || out == NULL || endpoint == 0u) {
        return false;
    }
    for (uint8_t i = 0; i < d->ep_count && i < ZBZ_EP_MAX; i++) {
        const zbz_endpoint_t *e = &d->ep[i];
        if (e->endpoint != endpoint) {
            continue;
        }
        memset(out, 0, sizeof(*out));
        out->ieee = d->ieee;
        out->nwk = d->nwk;
        out->endpoint = endpoint;
        out->clusters = e->in_clusters; /* the inputs a zone can watch */
        out->zone_type = e->zone_type;
        out->router = zbz_dev_router(d);
        _Static_assert(sizeof(out->manufacturer) >= ZBZ_TEXT_LEN, "manufacturer fits");
        _Static_assert(sizeof(out->model) >= ZBZ_TEXT_LEN, "model fits");
        memcpy(out->manufacturer, d->manufacturer, text_len(d->manufacturer, ZBZ_TEXT_LEN));
        memcpy(out->model, d->model, text_len(d->model, ZBZ_TEXT_LEN));
        out->join = d->join;
        return true;
    }
    return false;
}

/* Basic's power source (low 7 bits): mains single or three phase, DC, emergency mains. */
static bool powered(uint8_t power_source)
{
    uint8_t p = (uint8_t)(power_source & 0x7Fu);
    return p == 0x01u || p == 0x02u || p == 0x04u || p == 0x05u || p == 0x06u;
}

bool zbz_dev_router(const zbz_device_t *d)
{
    if (d->node_type == ZBZ_NODE_ROUTER) {
        return true;
    }
    if (d->node_type == ZBZ_NODE_END_DEVICE) {
        return false;
    }
    if (d->mac_caps != 0u) {
        return (d->mac_caps & ZSRC_ZB_MAC_FFD) != 0u;
    }
    return powered(d->power_source);
}

bool zbz_dev_sleepy(const zbz_device_t *d)
{
    if (d->node_type == ZBZ_NODE_ROUTER) {
        return false;
    }
    if (d->node_type == ZBZ_NODE_END_DEVICE) {
        return (d->mac_caps & ZSRC_ZB_MAC_RX_ON_IDLE) == 0u;
    }
    if (d->mac_caps != 0u) {
        return (d->mac_caps & (ZSRC_ZB_MAC_FFD | ZSRC_ZB_MAC_RX_ON_IDLE)) == 0u;
    }
    return (d->power_source & 0x7Fu) == 0x03u; /* battery */
}

/* ---- the record codec (devices.md §4) --------------------------------------------------------- */

typedef struct {
    uint8_t *out;
    size_t cap;
    size_t pos;
    bool ok;
} writer_t;

static void put_field(writer_t *w, uint8_t id, const uint8_t *value, size_t len)
{
    if (!w->ok || len > 0xFFu || w->pos + 2u + len > w->cap) {
        w->ok = false;
        return;
    }
    w->out[w->pos++] = id;
    w->out[w->pos++] = (uint8_t)len;
    memcpy(&w->out[w->pos], value, len);
    w->pos += len;
}

/* A number field, left out when zero (unless required). */
static void put_num(writer_t *w, uint8_t id, uint64_t v, size_t len, bool required)
{
    if (v == 0u && !required) {
        return;
    }
    uint8_t b[8];
    wrn(b, v, len);
    put_field(w, id, b, len);
}

static void put_text(writer_t *w, uint8_t id, const char *s, size_t size)
{
    size_t n = text_len(s, size);
    if (n > 0u) {
        put_field(w, id, (const uint8_t *)s, n);
    }
}

size_t zbz_dev_encode(const zbz_device_t *d, uint8_t *out, size_t cap)
{
    if (d == NULL || out == NULL || !d->used || d->id == 0u || d->ieee == 0u || cap < 1u) {
        return 0;
    }
    writer_t w = {.out = out, .cap = cap, .pos = 0, .ok = true};
    out[w.pos++] = ZBZ_COMPAT;
    put_num(&w, ZBZ_F_ID, d->id, 2, true);
    put_num(&w, ZBZ_F_IEEE, d->ieee, 8, true);
    put_num(&w, ZBZ_F_NWK, d->nwk, 2, false);
    put_text(&w, ZBZ_F_NAME, d->name, sizeof(d->name));
    put_text(&w, ZBZ_F_MANUFACTURER, d->manufacturer, sizeof(d->manufacturer));
    put_text(&w, ZBZ_F_MODEL, d->model, sizeof(d->model));
    put_text(&w, ZBZ_F_SW_BUILD, d->sw_build, sizeof(d->sw_build));
    put_num(&w, ZBZ_F_APP_VERSION, d->app_version, 1, false);
    put_num(&w, ZBZ_F_HW_VERSION, d->hw_version, 1, false);
    put_num(&w, ZBZ_F_MFR_CODE, d->manufacturer_code, 2, false);
    put_num(&w, ZBZ_F_NODE_TYPE, d->node_type, 1, false);
    put_num(&w, ZBZ_F_MAC_CAPS, d->mac_caps, 1, false);
    put_num(&w, ZBZ_F_POWER_SOURCE, d->power_source, 1, false);
    put_num(&w, ZBZ_F_JOIN, d->join, 1, false);
    put_num(&w, ZBZ_F_JOINED_AT, d->joined_at, 4, false);
    put_num(&w, ZBZ_F_ROLE, d->role, 1, false);
    put_num(&w, ZBZ_F_SETUP, d->setup, 1, false);
    put_num(&w, ZBZ_F_SETUP_STEP, d->setup_step, 1, false);
    put_num(&w, ZBZ_F_CHECK_IN, d->check_in_s, 4, false);
    for (uint8_t i = 0; i < d->ep_count && i < ZBZ_EP_MAX; i++) {
        const zbz_endpoint_t *e = &d->ep[i];
        uint8_t b[EP_FIELD_LEN];
        b[0] = e->endpoint;
        wrn(&b[1], e->profile, 2);
        wrn(&b[3], e->device_id, 2);
        wrn(&b[5], e->in_clusters, 2);
        wrn(&b[7], e->out_clusters, 2);
        wrn(&b[9], e->zone_type, 2);
        put_field(&w, ZBZ_F_ENDPOINT, b, sizeof(b));
    }
    if (d->wd_max_read && d->wd_max_s < 0xFFFFu) {
        put_num(&w, ZBZ_F_WD_MAX_DURATION, (uint64_t)d->wd_max_s + 1u, 2, false);
    }
    return w.ok ? w.pos : 0u;
}

/* Text into a buffer of `size`, cut at a UTF-8 character boundary when it is longer. */
static void get_text(char *dst, size_t size, const uint8_t *src, size_t len)
{
    size_t n = 0;
    while (n < len && src[n] != 0u) {
        n++;
    }
    if (n > size - 1u) {
        n = size - 1u;
        while (n > 0u && ((unsigned)src[n] & 0xC0u) == 0x80u) {
            n--; /* src[n] continues a character: leave the whole character out */
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* A number field's value, false when its length isn't `want`. */
static bool get_num(const uint8_t *v, size_t len, size_t want, uint64_t *out)
{
    if (len != want) {
        return false;
    }
    *out = rdn(v, len);
    return true;
}

zbz_reg_load_t zbz_dev_decode(zbz_device_t *d, const uint8_t *buf, size_t len)
{
    if (d == NULL) {
        return ZBZ_REG_DAMAGED;
    }
    memset(d, 0, sizeof(*d));
    if (buf == NULL || len == 0u || buf[0] == 0u || buf[0] == ZBZ_COMPAT_ERASED) {
        return ZBZ_REG_DAMAGED; /* never a compat (zbz_registry.h) */
    }
    if (buf[0] > ZBZ_COMPAT) {
        return ZBZ_REG_FUTURE;
    }
    size_t pos = 1;
    bool ok = true;
    while (ok && pos < len) {
        if (len - pos < 2u) {
            ok = false; /* a field cut in its header */
            break;
        }
        uint8_t id = buf[pos];
        size_t flen = buf[pos + 1u];
        const uint8_t *v = &buf[pos + 2u];
        if (flen > len - pos - 2u) {
            ok = false; /* a length past the end */
            break;
        }
        uint64_t n = 0;
        switch (id) {
        case ZBZ_F_ID:
            ok = get_num(v, flen, 2, &n);
            d->id = (uint16_t)n;
            break;
        case ZBZ_F_IEEE:
            ok = get_num(v, flen, 8, &n);
            d->ieee = n;
            break;
        case ZBZ_F_NWK:
            ok = get_num(v, flen, 2, &n);
            d->nwk = (uint16_t)n;
            break;
        case ZBZ_F_NAME: get_text(d->name, sizeof(d->name), v, flen); break;
        case ZBZ_F_MANUFACTURER: get_text(d->manufacturer, sizeof(d->manufacturer), v, flen); break;
        case ZBZ_F_MODEL: get_text(d->model, sizeof(d->model), v, flen); break;
        case ZBZ_F_SW_BUILD: get_text(d->sw_build, sizeof(d->sw_build), v, flen); break;
        case ZBZ_F_APP_VERSION:
            ok = get_num(v, flen, 1, &n);
            d->app_version = (uint8_t)n;
            break;
        case ZBZ_F_HW_VERSION:
            ok = get_num(v, flen, 1, &n);
            d->hw_version = (uint8_t)n;
            break;
        case ZBZ_F_MFR_CODE:
            ok = get_num(v, flen, 2, &n);
            d->manufacturer_code = (uint16_t)n;
            break;
        case ZBZ_F_NODE_TYPE:
            ok = get_num(v, flen, 1, &n);
            d->node_type = (uint8_t)n;
            break;
        case ZBZ_F_MAC_CAPS:
            ok = get_num(v, flen, 1, &n);
            d->mac_caps = (uint8_t)n;
            break;
        case ZBZ_F_POWER_SOURCE:
            ok = get_num(v, flen, 1, &n);
            d->power_source = (uint8_t)n;
            break;
        case ZBZ_F_JOIN:
            ok = get_num(v, flen, 1, &n);
            d->join = (uint8_t)n;
            break;
        case ZBZ_F_JOINED_AT:
            ok = get_num(v, flen, 4, &n);
            d->joined_at = (uint32_t)n;
            break;
        case ZBZ_F_ROLE:
            ok = get_num(v, flen, 1, &n);
            d->role = (uint8_t)n;
            break;
        case ZBZ_F_SETUP:
            ok = get_num(v, flen, 1, &n);
            d->setup = (uint8_t)n;
            break;
        case ZBZ_F_SETUP_STEP:
            ok = get_num(v, flen, 1, &n);
            d->setup_step = (uint8_t)n;
            break;
        case ZBZ_F_CHECK_IN:
            ok = get_num(v, flen, 4, &n);
            d->check_in_s = (uint32_t)n;
            break;
        case ZBZ_F_ENDPOINT:
            if (flen < EP_FIELD_LEN) {
                ok = false;
            } else if (v[0] != 0u && d->ep_count < ZBZ_EP_MAX) {
                zbz_endpoint_t *e = &d->ep[d->ep_count++];
                e->endpoint = v[0];
                e->profile = (uint16_t)rdn(&v[1], 2);
                e->device_id = (uint16_t)rdn(&v[3], 2);
                e->in_clusters = (uint16_t)rdn(&v[5], 2);
                e->out_clusters = (uint16_t)rdn(&v[7], 2);
                e->zone_type = (uint16_t)rdn(&v[9], 2);
            }
            break;
        case ZBZ_F_WD_MAX_DURATION:
            ok = get_num(v, flen, 2, &n);
            d->wd_max_read = n != 0u; /* 0 is never written: read as not read */
            d->wd_max_s = n != 0u ? (uint16_t)(n - 1u) : 0u;
            break;
        default:
            break; /* a later firmware's field: skipped */
        }
        pos += 2u + flen;
    }
    if (!ok || d->id == 0u || d->ieee == 0u) {
        memset(d, 0, sizeof(*d));
        return ZBZ_REG_DAMAGED;
    }
    if (d->setup == ZBZ_SETUP_RUNNING || d->setup == ZBZ_SETUP_WAKE) {
        d->setup = ZBZ_SETUP_STOPPED; /* the panel restarted during its setup */
    }
    d->used = true;
    return ZBZ_REG_OK;
}

size_t zbz_meta_encode(const zbz_registry_t *r, uint8_t *out, size_t cap)
{
    if (r == NULL || out == NULL || cap < ZBZ_META_LEN) {
        return 0;
    }
    out[0] = ZBZ_COMPAT;
    wrn(&out[1], r->next_id, 2);
    out[3] = (uint8_t)zbz_reg_count(r);
    return ZBZ_META_LEN;
}

zbz_reg_load_t zbz_meta_decode(zbz_registry_t *r, const uint8_t *buf, size_t len)
{
    if (r == NULL) {
        return ZBZ_REG_DAMAGED;
    }
    if (buf == NULL || len == 0u) {
        return ZBZ_REG_EMPTY;
    }
    if (buf[0] == 0u || buf[0] == ZBZ_COMPAT_ERASED) {
        return ZBZ_REG_DAMAGED; /* never a compat (zbz_registry.h) */
    }
    if (buf[0] > ZBZ_COMPAT) {
        return ZBZ_REG_FUTURE;
    }
    if (len < ZBZ_META_LEN) {
        return ZBZ_REG_DAMAGED;
    }
    uint16_t next = (uint16_t)rdn(&buf[1], 2);
    r->next_id = (next == 0u) ? (uint16_t)1u : next; /* bytes past ZBZ_META_LEN: a later firmware's */
    return ZBZ_REG_OK;
}

zbz_reg_load_t zbz_reg_load(zbz_registry_t *r, const zbz_store_port_t *store)
{
    zbz_reg_init(r);
    if (store == NULL || store->read == NULL) {
        return ZBZ_REG_EMPTY;
    }
    uint8_t buf[ZBZ_RECORD_MAX];
    bool any = false;
    bool damaged = false;
    bool future = false;
    bool meta_ok = false;
    size_t meta_count = 0;

    size_t n = store->read(store->ctx, ZBZ_KEY_META, buf, sizeof(buf));
    if (n > sizeof(buf)) {
        any = true;
        damaged = true;
    } else if (n > 0u) {
        any = true;
        zbz_reg_load_t m = zbz_meta_decode(r, buf, n);
        if (m == ZBZ_REG_OK) {
            meta_ok = true;
            meta_count = buf[3];
        } else if (m == ZBZ_REG_FUTURE) {
            future = true;
        } else {
            damaged = true;
        }
    }

    uint16_t max_id = 0;
    for (size_t i = 0; i < ZBZ_DEV_MAX; i++) {
        char key[ZBZ_KEY_LEN];
        slot_key(key, i);
        n = store->read(store->ctx, key, buf, sizeof(buf));
        if (n == 0u) {
            continue;
        }
        any = true;
        zbz_device_t *d = &r->dev[i];
        zbz_reg_load_t res = (n > sizeof(buf)) ? ZBZ_REG_DAMAGED : zbz_dev_decode(d, buf, n);
        if (res == ZBZ_REG_OK &&
            (zbz_reg_find(r, d->ieee) != d || zbz_reg_cget(r, d->id) != d)) {
            memset(d, 0, sizeof(*d)); /* a second record with one address or one id */
            res = ZBZ_REG_DAMAGED;
        }
        if (res == ZBZ_REG_FUTURE) {
            future = true;
        } else if (res != ZBZ_REG_OK) {
            damaged = true;
            r->dirty |= bit(i); /* erased at the next write-back */
        } else if (d->id > max_id) {
            max_id = d->id;
        }
    }

    size_t found = zbz_reg_count(r);
    if (!meta_ok) {
        r->next_id = id_after(max_id);
        r->meta_dirty = found > 0u || damaged;
    } else {
        if (found != meta_count) {
            r->meta_dirty = true;
            if (found < meta_count) {
                damaged = true; /* records were lost */
            }
        }
        /* Meta older than the records (a write-back cut between them): the next id goes past the
         * highest one read, so an id dropped since isn't given again. After a wrap past 65535
         * this moves the next id forward once (ids in use are still skipped); a next id already
         * at id_after(max) stays, so meta isn't rewritten at every start. */
        if (max_id >= r->next_id && id_after(max_id) != r->next_id) {
            r->next_id = id_after(max_id);
            r->meta_dirty = true;
        }
    }
    if (future) {
        r->read_only = true;
        r->dirty = 0;
        r->meta_dirty = false;
        return ZBZ_REG_FUTURE;
    }
    if (damaged) {
        r->meta_dirty = true;
        return ZBZ_REG_DAMAGED;
    }
    return any ? ZBZ_REG_OK : ZBZ_REG_EMPTY;
}

/* ---- write-back ------------------------------------------------------------------------------- */

zbz_take_t zbz_reg_take(zbz_registry_t *r, char key[ZBZ_KEY_LEN], uint8_t *buf, size_t cap,
                        size_t *len)
{
    if (len != NULL) {
        *len = 0;
    }
    if (r->read_only || key == NULL || buf == NULL || len == NULL || cap < ZBZ_RECORD_MAX) {
        return ZBZ_TAKE_NONE;
    }
    for (size_t i = 0; i < ZBZ_DEV_MAX; i++) {
        if ((r->dirty & bit(i)) == 0u) {
            continue;
        }
        r->dirty &= ~bit(i);
        slot_key(key, i);
        if (r->dev[i].used) {
            size_t n = zbz_dev_encode(&r->dev[i], buf, cap);
            if (n > 0u) {
                *len = n;
                return ZBZ_TAKE_WRITE;
            }
            /* A record without id or address can't be stored: its old one goes. */
        }
        return ZBZ_TAKE_ERASE;
    }
    if (r->meta_dirty) {
        r->meta_dirty = false;
        memset(key, 0, ZBZ_KEY_LEN);
        memcpy(key, ZBZ_KEY_META, sizeof(ZBZ_KEY_META));
        *len = zbz_meta_encode(r, buf, cap);
        return ZBZ_TAKE_WRITE;
    }
    return ZBZ_TAKE_NONE;
}

void zbz_reg_redo(zbz_registry_t *r, const char *key)
{
    if (r->read_only || key == NULL) {
        return;
    }
    if (strcmp(key, ZBZ_KEY_META) == 0) {
        r->meta_dirty = true;
        return;
    }
    int slot = key_slot(key);
    if (slot >= 0) {
        r->dirty |= bit((size_t)slot);
    }
}
