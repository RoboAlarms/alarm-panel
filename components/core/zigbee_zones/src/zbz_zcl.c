/*
 * zbz_zcl.c - ZCL frames as bytes (zbz_zcl.h; docs/features/20 testing.md §3; ZB-003, ZB-012,
 * ZB-022). The ZCL header both frame types share, the global commands' attribute records, and
 * IAS Zone's commands. Every read is bounds-checked against the bytes given; nothing is written
 * past `cap` or ZSRC_ZB_ZCL_PAYLOAD_MAX. Little-endian (ZCL 2.4).
 */
#include "zigbee_zones/zbz_zcl.h"

#include <string.h>

/* ---- little-endian helpers -------------------------------------------------------------------- */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

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

/* ---- data types (ZCL 2.6.2) ------------------------------------------------------------------- */

#define SIZE_STR1    (-1) /* a length byte, then the bytes */
#define SIZE_STR2    (-2) /* a length u16, then the bytes */
#define SIZE_UNKNOWN (-3) /* arrays, structures, sets, bags, reserved ids */

static int type_size(uint8_t t)
{
    if (t == 0x00u || t == 0xFFu) {
        return 0; /* no data, unknown */
    }
    if (t >= 0x08u && t <= 0x0Fu) {
        return (int)t - 0x07; /* data8..data64 */
    }
    if (t == 0x10u) {
        return 1; /* boolean */
    }
    if (t >= 0x18u && t <= 0x1Fu) {
        return (int)t - 0x17; /* map8..map64 */
    }
    if (t >= 0x20u && t <= 0x27u) {
        return (int)t - 0x1F; /* uint8..uint64 */
    }
    if (t >= 0x28u && t <= 0x2Fu) {
        return (int)t - 0x27; /* int8..int64 */
    }
    switch (t) {
    case 0x30u: return 1;  /* enum8 */
    case 0x31u: return 2;  /* enum16 */
    case 0x38u: return 2;  /* semi-precision */
    case 0x39u: return 4;  /* single precision */
    case 0x3Au: return 8;  /* double precision */
    case 0x41u:            /* octet string */
    case 0x42u: return SIZE_STR1; /* character string */
    case 0x43u:            /* long octet string */
    case 0x44u: return SIZE_STR2; /* long character string */
    case 0xE0u:            /* time of day */
    case 0xE1u:            /* date */
    case 0xE2u: return 4;  /* UTC time */
    case 0xE8u:            /* cluster id */
    case 0xE9u: return 2;  /* attribute id */
    case 0xEAu: return 4;  /* BACnet OID */
    case 0xF0u: return 8;  /* IEEE address */
    case 0xF1u: return 16; /* 128-bit security key */
    default: return SIZE_UNKNOWN;
    }
}

/* Analog types carry a reportable change in Configure Reporting (ZCL 2.5.7.1.7). */
static bool type_analog(uint8_t t)
{
    return (t >= 0x20u && t <= 0x2Fu) || (t >= 0x38u && t <= 0x3Au) || (t >= 0xE0u && t <= 0xE2u);
}

/* A value of `type` at bytes[0..avail): its bytes in `*vlen`, where they start in `*skip` (the
 * string length's bytes), the whole length in `*total`. */
static zbz_zcl_result_t value_span(uint8_t type, const uint8_t *bytes, size_t avail, size_t *skip,
                                   size_t *vlen, size_t *total)
{
    int sz = type_size(type);
    if (sz == SIZE_UNKNOWN) {
        return ZBZ_ZCL_ERR_DATA_TYPE;
    }
    if (sz >= 0) {
        if (avail < (size_t)sz) {
            return ZBZ_ZCL_ERR_SHORT;
        }
        *skip = 0;
        *vlen = (size_t)sz;
        *total = (size_t)sz;
        return ZBZ_ZCL_OK;
    }
    size_t lb = (sz == SIZE_STR1) ? 1u : 2u;
    if (avail < lb) {
        return ZBZ_ZCL_ERR_SHORT;
    }
    size_t n = (lb == 1u) ? (size_t)bytes[0] : (size_t)rd16(bytes);
    if ((lb == 1u && n == 0xFFu) || (lb == 2u && n == 0xFFFFu)) {
        n = 0; /* an invalid string: no characters follow */
    }
    if (avail - lb < n) {
        return ZBZ_ZCL_ERR_SHORT;
    }
    *skip = lb;
    *vlen = n;
    *total = lb + n;
    return ZBZ_ZCL_OK;
}

/* ---- the header (ZCL 2.4.1) ------------------------------------------------------------------- */

static zbz_zcl_result_t decode_frame(const uint8_t *bytes, size_t len, uint16_t cluster,
                                     uint8_t want_type, zsrc_zb_zcl_t *out)
{
    if (out == NULL) {
        return ZBZ_ZCL_ERR_VALUE;
    }
    memset(out, 0, sizeof(*out));
    if (bytes == NULL || len < ZBZ_ZCL_HEADER_MIN) {
        return ZBZ_ZCL_ERR_SHORT;
    }
    uint8_t fc = bytes[0];
    if (((unsigned)fc & ZBZ_ZCL_FC_TYPE_MASK) != (unsigned)want_type) {
        return ZBZ_ZCL_ERR_TYPE; /* the other frame type, or a reserved one */
    }
    if (((unsigned)fc & ZBZ_ZCL_FC_RESERVED) != 0u) {
        return ZBZ_ZCL_ERR_VALUE;
    }
    size_t pos = 1;
    if (((unsigned)fc & ZBZ_ZCL_FC_MANUFACTURER) != 0u) {
        if (len < ZBZ_ZCL_HEADER_MAX) {
            return ZBZ_ZCL_ERR_SHORT;
        }
        out->manufacturer = rd16(&bytes[1]);
        if (out->manufacturer == 0u) {
            /* zsrc_zb_zcl_t says "standard" with manufacturer 0: kept, a manufacturer's command
             * would be read as the standard one of the same id (an IAS Zone status, an ACE Arm).
             * Found by test_zbz_fuzz.c (decode, encode, compare). */
            memset(out, 0, sizeof(*out));
            return ZBZ_ZCL_ERR_VALUE;
        }
        pos = 3;
    }
    size_t payload = len - pos - 2u;
    if (payload > ZSRC_ZB_ZCL_PAYLOAD_MAX) {
        memset(out, 0, sizeof(*out));
        return ZBZ_ZCL_ERR_LONG;
    }
    out->cluster = cluster;
    out->server_to_client = ((unsigned)fc & ZBZ_ZCL_FC_SERVER_TO_CLIENT) != 0u;
    out->want_response = ((unsigned)fc & ZBZ_ZCL_FC_NO_DEFAULT_RSP) == 0u;
    out->tsn = bytes[pos];
    out->command = bytes[pos + 1u];
    out->len = (uint8_t)payload;
    if (payload > 0u) {
        memcpy(out->payload, &bytes[pos + 2u], payload);
    }
    return ZBZ_ZCL_OK;
}

static size_t encode_frame(const zsrc_zb_zcl_t *f, uint8_t type, uint8_t tsn, uint8_t *out,
                           size_t cap)
{
    if (f == NULL || out == NULL || f->len > ZSRC_ZB_ZCL_PAYLOAD_MAX) {
        return 0;
    }
    if (f->tsn != ZSRC_ZB_TSN_NEW && f->tsn > 0xFFu) {
        return 0;
    }
    bool mfr = f->manufacturer != 0u;
    size_t need = (mfr ? ZBZ_ZCL_HEADER_MAX : ZBZ_ZCL_HEADER_MIN) + (size_t)f->len;
    if (cap < need) {
        return 0;
    }
    uint8_t fc = type;
    if (mfr) {
        fc |= ZBZ_ZCL_FC_MANUFACTURER;
    }
    if (f->server_to_client) {
        fc |= ZBZ_ZCL_FC_SERVER_TO_CLIENT;
    }
    if (!f->want_response) {
        fc |= ZBZ_ZCL_FC_NO_DEFAULT_RSP;
    }
    size_t pos = 0;
    out[pos++] = fc;
    if (mfr) {
        wr16(&out[pos], f->manufacturer);
        pos += 2;
    }
    out[pos++] = (f->tsn == ZSRC_ZB_TSN_NEW) ? tsn : (uint8_t)f->tsn;
    out[pos++] = f->command;
    if (f->len > 0u) {
        memcpy(&out[pos], f->payload, f->len);
    }
    return need;
}

zbz_zcl_result_t zbz_zcl_decode(const uint8_t *bytes, size_t len, uint16_t cluster,
                                zsrc_zb_zcl_t *out)
{
    return decode_frame(bytes, len, cluster, ZBZ_ZCL_FC_CLUSTER, out);
}

size_t zbz_zcl_encode(const zsrc_zb_zcl_t *f, uint8_t tsn, uint8_t *out, size_t cap)
{
    return encode_frame(f, ZBZ_ZCL_FC_CLUSTER, tsn, out, cap);
}

zbz_zcl_result_t zbz_zcl_decode_global(const uint8_t *bytes, size_t len, uint16_t cluster,
                                       zsrc_zb_zcl_t *out)
{
    return decode_frame(bytes, len, cluster, ZBZ_ZCL_FC_GLOBAL, out);
}

size_t zbz_zcl_encode_global(const zsrc_zb_zcl_t *f, uint8_t tsn, uint8_t *out, size_t cap)
{
    return encode_frame(f, ZBZ_ZCL_FC_GLOBAL, tsn, out, cap);
}

bool zbz_zcl_is_global(const uint8_t *bytes, size_t len)
{
    return bytes != NULL && len > 0u && (bytes[0] & ZBZ_ZCL_FC_TYPE_MASK) == ZBZ_ZCL_FC_GLOBAL;
}

void zbz_zcl_global(uint16_t cluster, uint8_t command, bool server_to_client, zsrc_zb_zcl_t *out)
{
    memset(out, 0, sizeof(*out));
    out->cluster = cluster;
    out->command = command;
    out->server_to_client = server_to_client;
    out->want_response = true;
    out->tsn = ZSRC_ZB_TSN_NEW;
}

/* ---- attribute records (ZCL 2.5) -------------------------------------------------------------- */

typedef enum {
    LAYOUT_NONE = 0,
    LAYOUT_READ,       /* attr */
    LAYOUT_READ_RSP,   /* attr | status | (type | value) */
    LAYOUT_WRITE,      /* attr | type | value */
    LAYOUT_STATUS,     /* status | attr, or a lone SUCCESS */
    LAYOUT_STATUS_DIR, /* status | direction | attr, or a lone SUCCESS */
} layout_t;

static layout_t layout_of(uint8_t command)
{
    switch (command) {
    case ZBZ_ZCL_READ_ATTR: return LAYOUT_READ;
    case ZBZ_ZCL_READ_ATTR_RSP: return LAYOUT_READ_RSP;
    case ZBZ_ZCL_WRITE_ATTR:
    case ZBZ_ZCL_WRITE_ATTR_UNDIV:
    case ZBZ_ZCL_WRITE_ATTR_NORSP:
    case ZBZ_ZCL_REPORT_ATTR: return LAYOUT_WRITE;
    case ZBZ_ZCL_WRITE_ATTR_RSP: return LAYOUT_STATUS;
    case ZBZ_ZCL_CONFIG_REPORT_RSP: return LAYOUT_STATUS_DIR;
    default: return LAYOUT_NONE;
    }
}

void zbz_zcl_iter(zbz_zcl_iter_t *it, const zsrc_zb_zcl_t *f)
{
    it->f = f;
    it->pos = 0;
    it->err = ZBZ_ZCL_OK;
}

static bool iter_stop(zbz_zcl_iter_t *it, zbz_zcl_result_t err)
{
    it->err = err;
    it->pos = (it->f != NULL) ? it->f->len : (uint8_t)0u; /* a broken record ends the walk for good */
    return false;
}

bool zbz_zcl_next_attr(zbz_zcl_iter_t *it, zbz_zcl_attr_t *out)
{
    memset(out, 0, sizeof(*out));
    const zsrc_zb_zcl_t *f = it->f;
    if (f == NULL || f->len > ZSRC_ZB_ZCL_PAYLOAD_MAX) {
        return iter_stop(it, ZBZ_ZCL_ERR_VALUE);
    }
    if (it->err != ZBZ_ZCL_OK) {
        return false;
    }
    layout_t lay = layout_of(f->command);
    if (lay == LAYOUT_NONE) {
        return iter_stop(it, ZBZ_ZCL_ERR_TYPE);
    }
    if (it->pos >= f->len) {
        return false; /* the end: it->err stays OK */
    }
    const uint8_t *p = &f->payload[it->pos];
    size_t avail = (size_t)f->len - (size_t)it->pos;
    size_t used = 0;

    out->status = ZBZ_ZCL_SUCCESS;
    out->type = ZBZ_ZCL_TYPE_NONE;
    switch (lay) {
    case LAYOUT_READ:
        if (avail < 2u) {
            return iter_stop(it, ZBZ_ZCL_ERR_SHORT);
        }
        out->attr = rd16(p);
        used = 2;
        break;
    case LAYOUT_STATUS:
    case LAYOUT_STATUS_DIR: {
        if (it->pos == 0u && f->len == 1u) {
            if (p[0] != ZBZ_ZCL_SUCCESS) {
                return iter_stop(it, ZBZ_ZCL_ERR_SHORT); /* a failure names its attribute */
            }
            out->attr = ZBZ_ZCL_ATTR_ALL;
            used = 1;
            break;
        }
        size_t rec = (lay == LAYOUT_STATUS) ? 3u : 4u;
        if (avail < rec) {
            return iter_stop(it, ZBZ_ZCL_ERR_SHORT);
        }
        out->status = p[0];
        out->attr = rd16(&p[rec - 2u]);
        used = rec;
        break;
    }
    case LAYOUT_READ_RSP:
    case LAYOUT_WRITE: {
        size_t hdr = (lay == LAYOUT_READ_RSP) ? 3u : 2u;
        if (avail < hdr) {
            return iter_stop(it, ZBZ_ZCL_ERR_SHORT);
        }
        out->attr = rd16(p);
        if (lay == LAYOUT_READ_RSP) {
            out->status = p[2];
            if (out->status != ZBZ_ZCL_SUCCESS) {
                used = 3; /* a failed read: no type, no value */
                break;
            }
        }
        if (avail < hdr + 1u) {
            return iter_stop(it, ZBZ_ZCL_ERR_SHORT);
        }
        out->type = p[hdr];
        size_t skip = 0;
        size_t vlen = 0;
        size_t total = 0;
        zbz_zcl_result_t r = value_span(out->type, &p[hdr + 1u], avail - hdr - 1u, &skip, &vlen,
                                        &total);
        if (r != ZBZ_ZCL_OK) {
            return iter_stop(it, r);
        }
        out->len = (uint8_t)vlen;
        out->value = (vlen > 0u) ? &p[hdr + 1u + skip] : NULL;
        used = hdr + 1u + total;
        break;
    }
    case LAYOUT_NONE:
    default:
        return iter_stop(it, ZBZ_ZCL_ERR_TYPE);
    }
    it->pos = (uint8_t)((size_t)it->pos + used);
    return true;
}

bool zbz_zcl_put_attr(zsrc_zb_zcl_t *f, const zbz_zcl_attr_t *a)
{
    if (f == NULL || a == NULL || f->len > ZSRC_ZB_ZCL_PAYLOAD_MAX) {
        return false;
    }
    layout_t lay = layout_of(f->command);
    uint8_t rec[ZSRC_ZB_ZCL_PAYLOAD_MAX];
    size_t n = 0;
    bool with_value = false;

    switch (lay) {
    case LAYOUT_READ:
        wr16(rec, a->attr);
        n = 2;
        break;
    case LAYOUT_STATUS:
    case LAYOUT_STATUS_DIR:
        if (a->attr == ZBZ_ZCL_ATTR_ALL) {
            if (f->len != 0u || a->status != ZBZ_ZCL_SUCCESS) {
                return false; /* a lone SUCCESS is the whole answer */
            }
            rec[0] = ZBZ_ZCL_SUCCESS;
            n = 1;
            break;
        }
        rec[n++] = a->status;
        if (lay == LAYOUT_STATUS_DIR) {
            rec[n++] = 0u; /* the device's reports */
        }
        wr16(&rec[n], a->attr);
        n += 2u;
        break;
    case LAYOUT_READ_RSP:
        wr16(rec, a->attr);
        rec[2] = a->status;
        n = 3;
        with_value = a->status == ZBZ_ZCL_SUCCESS;
        break;
    case LAYOUT_WRITE:
        wr16(rec, a->attr);
        n = 2;
        with_value = true;
        break;
    case LAYOUT_NONE:
    default:
        return false;
    }

    if (with_value) {
        int sz = type_size(a->type);
        if (sz == SIZE_UNKNOWN || (a->len > 0u && a->value == NULL)) {
            return false;
        }
        rec[n++] = a->type;
        if (sz >= 0) {
            if ((size_t)a->len != (size_t)sz || n + (size_t)sz > sizeof(rec)) {
                return false;
            }
        } else {
            size_t lb = (sz == SIZE_STR1) ? 1u : 2u;
            if ((unsigned)a->len >= 0xFFu || n + lb + (size_t)a->len > sizeof(rec)) {
                return false;
            }
            if (lb == 1u) {
                rec[n] = a->len;
            } else {
                wr16(&rec[n], a->len);
            }
            n += lb;
        }
        if (a->len > 0u) {
            memcpy(&rec[n], a->value, a->len);
            n += (size_t)a->len;
        }
    }
    if ((size_t)f->len + n > ZSRC_ZB_ZCL_PAYLOAD_MAX) {
        return false;
    }
    memcpy(&f->payload[(size_t)f->len], rec, n);
    f->len = (uint8_t)((size_t)f->len + n);
    return true;
}

uint64_t zbz_zcl_attr_uint(const zbz_zcl_attr_t *a)
{
    int sz = type_size(a->type);
    if (sz <= 0 || sz > 8 || a->len != (uint8_t)sz || a->value == NULL) {
        return 0;
    }
    return rdn(a->value, (size_t)sz);
}

int64_t zbz_zcl_attr_int(const zbz_zcl_attr_t *a)
{
    uint64_t v = zbz_zcl_attr_uint(a);
    int sz = type_size(a->type);
    /* Only a value read at its type's length (1..7 bytes) is extended: anything else read as 0,
     * and a length of 0 would shift by -1. */
    if (a->type >= 0x28u && a->type <= 0x2Fu && sz >= 1 && sz <= 7 && a->len == (uint8_t)sz &&
        a->value != NULL) {
        uint64_t sign = (uint64_t)1u << (8u * (unsigned)a->len - 1u);
        if ((v & sign) != 0u) {
            v |= ~((sign << 1) - 1u); /* extend the sign past the value's bytes */
        }
    }
    return (int64_t)v;
}

/* ---- Configure Reporting (ZCL 2.5.7) ---------------------------------------------------------- */

bool zbz_zcl_config_report(const zsrc_zb_reporting_t *r, uint8_t count, zsrc_zb_zcl_t *out)
{
    if (r == NULL || out == NULL || count == 0u) {
        return false;
    }
    zbz_zcl_global(r[0].cluster, ZBZ_ZCL_CONFIG_REPORT, false, out);
    size_t n = 0;
    for (uint8_t i = 0; i < count; i++) {
        const zsrc_zb_reporting_t *e = &r[i];
        int sz = type_size(e->type);
        if (e->cluster != r[0].cluster || sz == SIZE_UNKNOWN) {
            return false;
        }
        bool analog = type_analog(e->type);
        size_t rec = 8u + (analog ? (size_t)sz : 0u);
        if (n + rec > ZSRC_ZB_ZCL_PAYLOAD_MAX) {
            return false;
        }
        uint8_t *p = &out->payload[n];
        p[0] = 0u; /* direction: the device reports */
        wr16(&p[1], e->attr);
        p[3] = e->type;
        wr16(&p[4], e->min_s);
        wr16(&p[6], e->max_s);
        if (analog) {
            wrn(&p[8], (uint64_t)e->change, (size_t)sz);
        }
        n += rec;
    }
    out->len = (uint8_t)n;
    return true;
}

zbz_zcl_result_t zbz_zcl_decode_config_report(const zsrc_zb_zcl_t *f, zsrc_zb_reporting_t *out,
                                              uint8_t cap, uint8_t *count)
{
    if (count != NULL) {
        *count = 0;
    }
    if (f == NULL || out == NULL || count == NULL) {
        return ZBZ_ZCL_ERR_VALUE;
    }
    if (f->command != ZBZ_ZCL_CONFIG_REPORT || f->server_to_client) {
        return ZBZ_ZCL_ERR_TYPE;
    }
    if (f->len > ZSRC_ZB_ZCL_PAYLOAD_MAX) {
        return ZBZ_ZCL_ERR_LONG;
    }
    size_t pos = 0;
    while (pos < f->len) {
        const uint8_t *p = &f->payload[pos];
        size_t avail = (size_t)f->len - pos;
        if (avail < 1u) {
            return ZBZ_ZCL_ERR_SHORT;
        }
        if (p[0] != 0u) {
            return ZBZ_ZCL_ERR_VALUE; /* direction 1 (a timeout): never the panel's */
        }
        if (avail < 4u) {
            return ZBZ_ZCL_ERR_SHORT;
        }
        int sz = type_size(p[3]);
        if (sz == SIZE_UNKNOWN) {
            return ZBZ_ZCL_ERR_DATA_TYPE;
        }
        bool analog = type_analog(p[3]);
        size_t rec = 8u + (analog ? (size_t)sz : 0u);
        if (avail < rec) {
            return ZBZ_ZCL_ERR_SHORT;
        }
        if ((unsigned)*count >= (unsigned)cap) {
            return ZBZ_ZCL_ERR_LONG;
        }
        uint64_t change = analog ? rdn(&p[8], (size_t)sz) : 0u;
        if (change > 0xFFFFFFFFu) {
            return ZBZ_ZCL_ERR_VALUE;
        }
        zsrc_zb_reporting_t *e = &out[*count];
        memset(e, 0, sizeof(*e));
        e->cluster = f->cluster;
        e->attr = rd16(&p[1]);
        e->type = p[3];
        e->min_s = rd16(&p[4]);
        e->max_s = rd16(&p[6]);
        e->change = (uint32_t)change;
        *count = (uint8_t)((unsigned)*count + 1u);
        pos += rec;
    }
    return ZBZ_ZCL_OK;
}

/* ---- Default Response (ZCL 2.5.12) ------------------------------------------------------------ */

void zbz_zcl_default_rsp(uint16_t cluster, uint8_t command, uint8_t status,
                         bool server_to_client, zsrc_zb_zcl_t *out)
{
    zbz_zcl_global(cluster, ZBZ_ZCL_DEFAULT_RSP, server_to_client, out);
    out->want_response = false; /* never a Default Response to a Default Response */
    out->payload[0] = command;
    out->payload[1] = status;
    out->len = 2;
}

zbz_zcl_result_t zbz_zcl_decode_default_rsp(const zsrc_zb_zcl_t *f, uint8_t *command,
                                            uint8_t *status)
{
    if (f == NULL || command == NULL || status == NULL) {
        return ZBZ_ZCL_ERR_VALUE;
    }
    if (f->command != ZBZ_ZCL_DEFAULT_RSP) {
        return ZBZ_ZCL_ERR_TYPE;
    }
    if (f->len > ZSRC_ZB_ZCL_PAYLOAD_MAX) {
        return ZBZ_ZCL_ERR_LONG;
    }
    if (f->len < 2u) {
        return ZBZ_ZCL_ERR_SHORT;
    }
    *command = f->payload[0];
    *status = f->payload[1];
    return ZBZ_ZCL_OK;
}

/* ---- IAS Zone (ZB-003, ZB-022) ---------------------------------------------------------------- */

void zbz_ias_cie_write(uint64_t panel_ieee, zsrc_zb_zcl_t *out)
{
    zbz_zcl_global(ZSRC_ZB_ZCL_IAS_ZONE, ZBZ_ZCL_WRITE_ATTR, false, out);
    uint8_t addr[8];
    wrn(addr, panel_ieee, sizeof(addr));
    const zbz_zcl_attr_t a = {
        .attr = ZBZ_IAS_ATTR_CIE_ADDRESS,
        .type = ZBZ_ZCL_TYPE_EUI64,
        .len = 8,
        .value = addr,
    };
    (void)zbz_zcl_put_attr(out, &a); /* 11 bytes: always fits */
}

/* A frame of IAS Zone's from the sensor (server to client), standard command `command`. */
static zbz_zcl_result_t ias_from_sensor(const zsrc_zb_zcl_t *f, uint8_t command)
{
    if (f->cluster != ZSRC_ZB_ZCL_IAS_ZONE || !f->server_to_client || f->manufacturer != 0u ||
        f->command != command) {
        return ZBZ_ZCL_ERR_TYPE;
    }
    if (f->len > ZSRC_ZB_ZCL_PAYLOAD_MAX) {
        return ZBZ_ZCL_ERR_LONG;
    }
    return ZBZ_ZCL_OK;
}

zbz_zcl_result_t zbz_ias_decode_enroll_request(const zsrc_zb_zcl_t *f, zbz_ias_enroll_req_t *out)
{
    if (f == NULL || out == NULL) {
        return ZBZ_ZCL_ERR_VALUE;
    }
    memset(out, 0, sizeof(*out));
    zbz_zcl_result_t r = ias_from_sensor(f, ZBZ_IAS_ENROLL_REQUEST);
    if (r != ZBZ_ZCL_OK) {
        return r;
    }
    if (f->len < 4u) {
        return ZBZ_ZCL_ERR_SHORT;
    }
    out->zone_type = rd16(&f->payload[0]);
    out->manufacturer = rd16(&f->payload[2]);
    return ZBZ_ZCL_OK;
}

void zbz_ias_enroll_response(uint8_t code, uint8_t zone_id, zsrc_zb_zcl_t *out)
{
    memset(out, 0, sizeof(*out));
    out->cluster = ZSRC_ZB_ZCL_IAS_ZONE;
    out->command = ZBZ_IAS_ENROLL_RESPONSE;
    out->want_response = true;
    out->tsn = ZSRC_ZB_TSN_NEW;
    out->payload[0] = code;
    out->payload[1] = zone_id;
    out->len = 2;
}

zbz_zcl_result_t zbz_ias_decode_status_change(const zsrc_zb_zcl_t *f, zbz_ias_status_t *out)
{
    if (f == NULL || out == NULL) {
        return ZBZ_ZCL_ERR_VALUE;
    }
    memset(out, 0, sizeof(*out));
    out->zone_id = 0xFFu;
    zbz_zcl_result_t r = ias_from_sensor(f, ZBZ_IAS_STATUS_CHANGE);
    if (r != ZBZ_ZCL_OK) {
        return r;
    }
    if (f->len < 2u || f->len == 5u) {
        return ZBZ_ZCL_ERR_SHORT;
    }
    out->status = rd16(&f->payload[0]);
    if (f->len >= 3u) {
        out->extended = f->payload[2];
    }
    if (f->len >= 4u) {
        out->zone_id = f->payload[3];
    }
    if (f->len >= 6u) {
        out->delay_qs = rd16(&f->payload[4]);
    }
    return ZBZ_ZCL_OK;
}

void zbz_ias_test_mode(uint8_t seconds, uint8_t sensitivity, zsrc_zb_zcl_t *out)
{
    memset(out, 0, sizeof(*out));
    out->cluster = ZSRC_ZB_ZCL_IAS_ZONE;
    out->want_response = true;
    out->tsn = ZSRC_ZB_TSN_NEW;
    if (seconds == 0u) {
        out->command = ZBZ_IAS_NORMAL_MODE;
        return;
    }
    out->command = ZBZ_IAS_TEST_MODE;
    out->payload[0] = seconds;
    out->payload[1] = sensitivity;
    out->len = 2;
}
