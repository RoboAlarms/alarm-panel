/*
 * codec.c - the configuration as a versioned, self-describing blob (see alarm_codec.h).
 */
#include "alarm/alarm_codec.h"

#include <stddef.h>
#include <string.h>

#define HDR_LEN 16u

/* Item ids are (group << 8) | field. Field 0x00 of a group selects the entry (1-based).
 * External contract: never reuse or renumber an id, only add new ones. */
enum {
    GRP_SYSTEM = 0x00,
    GRP_PARTITION = 0x01,
    GRP_ZONE = 0x02,
    GRP_USER = 0x03,
    GRP_DEVICE = 0x04,
};

/* K_NAME: a name as firmware before 2026-09-29 read it, cut on a whole character to
 * ALARM_NAME_LEGACY_LEN - 1 bytes (such firmware refuses a blob with a longer one).
 * K_NAME_LONG: the whole name, written only when it is longer than that, in a field older
 * firmware skips; it comes after the K_NAME field and replaces what that set. */
/* K_UINT_NZ, K_BYTES_NZ: read as K_UINT and K_BYTES, but written only when not zero (a missing
 * one reads as zero), so a configuration that doesn't use them writes the bytes it always did. */
typedef enum { K_UINT, K_BOOL, K_STR, K_BYTES, K_NAME, K_NAME_LONG, K_UINT_NZ, K_BYTES_NZ } kind_t;

typedef struct {
    uint8_t id;     /* field number inside the group */
    uint8_t kind;   /* kind_t */
    uint8_t width;  /* K_UINT: encoded bytes; K_BYTES: exact length */
    uint8_t size;   /* size of the C object */
    uint16_t offset;
} field_t;

#define MEMBER_SIZE(T, m) ((uint8_t)sizeof(((T *)0)->m))
#define F_UINT(T, i, m, w) {(i), K_UINT, (w), MEMBER_SIZE(T, m), (uint16_t)offsetof(T, m)}
#define F_BOOL(T, i, m)    {(i), K_BOOL, 1u, MEMBER_SIZE(T, m), (uint16_t)offsetof(T, m)}
#define F_STR(T, i, m)     {(i), K_STR, 0u, MEMBER_SIZE(T, m), (uint16_t)offsetof(T, m)}
#define F_BYTES(T, i, m)   {(i), K_BYTES, MEMBER_SIZE(T, m), MEMBER_SIZE(T, m), (uint16_t)offsetof(T, m)}
#define F_NAME(T, i, m)    {(i), K_NAME, 0u, MEMBER_SIZE(T, m), (uint16_t)offsetof(T, m)}
#define F_NAME_LONG(T, i, m) {(i), K_NAME_LONG, 0u, MEMBER_SIZE(T, m), (uint16_t)offsetof(T, m)}
#define F_UINT_NZ(T, i, m, w) {(i), K_UINT_NZ, (w), MEMBER_SIZE(T, m), (uint16_t)offsetof(T, m)}
#define F_BYTES_NZ(T, i, m)                                                                    \
    {(i), K_BYTES_NZ, MEMBER_SIZE(T, m), MEMBER_SIZE(T, m), (uint16_t)offsetof(T, m)}

/* Group 0x00: system options and the code salt (offsets into alarm_config_t). */
static const field_t SYSTEM_FIELDS[] = {
    F_UINT(alarm_config_t, 0x01, opt.code_min_len, 1),
    F_UINT(alarm_config_t, 0x02, opt.abort_window_s, 1),
    F_UINT(alarm_config_t, 0x03, opt.cancel_window_s, 2),
    F_UINT(alarm_config_t, 0x04, opt.bell_timeout_s, 2),
    F_BOOL(alarm_config_t, 0x05, opt.fire_bell_timeout),
    F_UINT(alarm_config_t, 0x06, opt.swinger_trips, 1),
    F_UINT(alarm_config_t, 0x07, opt.cross_zone_window_s, 2),
    F_UINT(alarm_config_t, 0x08, opt.fire_verify_window_s, 2),
    F_UINT(alarm_config_t, 0x09, opt.smoke_reset_s, 1),
    F_UINT(alarm_config_t, 0x0A, opt.exit_restart_limit, 1),
    F_UINT(alarm_config_t, 0x0B, opt.recent_closing_s, 2),
    F_BOOL(alarm_config_t, 0x0C, opt.auto_stay),
    F_BOOL(alarm_config_t, 0x0D, opt.exit_sounds),
    F_BOOL(alarm_config_t, 0x0E, opt.quick_arm),
    F_BOOL(alarm_config_t, 0x0F, opt.quick_exit),
    F_BOOL(alarm_config_t, 0x10, opt.remote_arm_without_code),
    F_BOOL(alarm_config_t, 0x11, opt.power_up_previous_state),
    F_UINT(alarm_config_t, 0x12, opt.power_up_settle_s, 2),
    F_UINT(alarm_config_t, 0x13, opt.lockout_attempts, 1),
    F_UINT(alarm_config_t, 0x14, opt.lockout_s, 2),
    F_UINT(alarm_config_t, 0x15, opt.walk_test_timeout_s, 2),
    F_UINT(alarm_config_t, 0x16, opt.trouble_beep_mask, 4),
    F_UINT(alarm_config_t, 0x17, opt.panic_keys[0], 1),
    F_UINT(alarm_config_t, 0x18, opt.panic_keys[1], 1),
    F_UINT(alarm_config_t, 0x19, opt.panic_keys[2], 1),
    F_BOOL(alarm_config_t, 0x1A, opt.relaxed_limits),
    F_BOOL(alarm_config_t, 0x1B, opt.power_up_program),
    F_BOOL(alarm_config_t, 0x1C, opt.chime_needs_code),
    F_BOOL(alarm_config_t, 0x1D, opt.auto_less_secure),
    F_UINT_NZ(alarm_config_t, 0x1E, opt.voice_revision_counter, 4), /* CORE-CFG-021 */
    F_BYTES(alarm_config_t, 0x40, code_salt),
};

static const field_t PARTITION_FIELDS[] = {
    F_BOOL(alarm_partition_config_t, 0x01, enabled),
    F_NAME(alarm_partition_config_t, 0x02, name),
    F_UINT(alarm_partition_config_t, 0x03, exit_delay_s, 2),
    F_UINT(alarm_partition_config_t, 0x04, entry_delay1_s, 2),
    F_UINT(alarm_partition_config_t, 0x05, entry_delay2_s, 2),
    F_NAME_LONG(alarm_partition_config_t, 0x06, name),
};

static const field_t ZONE_FIELDS[] = {
    F_UINT(alarm_zone_config_t, 0x01, type, 1),
    F_UINT(alarm_zone_config_t, 0x02, partition, 1),
    F_UINT(alarm_zone_config_t, 0x03, flags, 2),
    F_UINT(alarm_zone_config_t, 0x04, device_class, 1),
    F_UINT(alarm_zone_config_t, 0x05, user, 1),
    F_UINT(alarm_zone_config_t, 0x06, swinger_trips, 1),
    F_UINT(alarm_zone_config_t, 0x07, cross_zone, 2),
    F_UINT(alarm_zone_config_t, 0x08, supervision_s, 2),
    F_NAME(alarm_zone_config_t, 0x09, name),
    F_NAME_LONG(alarm_zone_config_t, 0x0A, name),
    /* The spoken name (CORE-CFG-020): eight little-endian u16 token IDs, exactly 16 bytes. */
    F_BYTES_NZ(alarm_zone_config_t, 0x0B, spoken_words),
    /* Its revisions (CORE-CFG-021). */
    F_UINT_NZ(alarm_zone_config_t, 0x0C, voice_binding_revision, 4),
    F_UINT_NZ(alarm_zone_config_t, 0x0D, voice_approved_revision, 4),
    F_UINT_NZ(alarm_zone_config_t, 0x0E, voice_pack_revision, 4),
};

static const field_t USER_FIELDS[] = {
    F_UINT(alarm_user_t, 0x01, authority, 1),
    F_UINT(alarm_user_t, 0x02, partitions, 1),
    F_BYTES(alarm_user_t, 0x03, code_hash),
    F_NAME(alarm_user_t, 0x04, name),
    F_NAME_LONG(alarm_user_t, 0x05, name),
};

static const field_t DEVICE_FIELDS[] = {
    F_BOOL(alarm_device_config_t, 0x01, enabled),
    F_UINT(alarm_device_config_t, 0x02, partition, 1),
    F_UINT(alarm_device_config_t, 0x03, supervision_s, 2),
    F_NAME(alarm_device_config_t, 0x04, name),
    F_NAME_LONG(alarm_device_config_t, 0x05, name),
};

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

typedef struct {
    const field_t *fields;
    size_t count;
    size_t entry_size; /* 0 = not an array */
    uint16_t entries;
    uint8_t index_width;
} group_t;

static const group_t GROUPS[] = {
    [GRP_SYSTEM] = {SYSTEM_FIELDS, COUNT_OF(SYSTEM_FIELDS), 0u, 0u, 0u},
    [GRP_PARTITION] = {PARTITION_FIELDS, COUNT_OF(PARTITION_FIELDS),
                       sizeof(alarm_partition_config_t), ALARM_MAX_PARTITIONS, 1u},
    [GRP_ZONE] = {ZONE_FIELDS, COUNT_OF(ZONE_FIELDS), sizeof(alarm_zone_config_t),
                  ALARM_MAX_ZONES, 2u},
    [GRP_USER] = {USER_FIELDS, COUNT_OF(USER_FIELDS), sizeof(alarm_user_t), ALARM_MAX_USERS, 1u},
    [GRP_DEVICE] = {DEVICE_FIELDS, COUNT_OF(DEVICE_FIELDS), sizeof(alarm_device_config_t),
                    ALARM_MAX_DEVICES, 1u},
};

uint32_t alarm_crc32(uint32_t crc, const void *data, size_t len)
{
    static const uint32_t nibble[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
        0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        crc = (crc >> 4) ^ nibble[crc & 15u];
        crc = (crc >> 4) ^ nibble[crc & 15u];
    }
    return ~crc;
}

/* ---- encoding ---------------------------------------------------------------------------- */

static uint8_t str_len(const uint8_t *p, uint8_t size)
{
    uint8_t n = 0;
    while (n + 1u < size && p[n] != '\0') {
        n++;
    }
    return n;
}

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool overflow;
} writer_t;

static void put(writer_t *w, const void *data, size_t n)
{
    if (w->overflow || n > w->cap - w->len) {
        w->overflow = true;
        return;
    }
    memcpy(w->buf + w->len, data, n);
    w->len += n;
}

static void put_item(writer_t *w, uint8_t group, uint8_t field, const void *value, uint8_t len)
{
    const uint8_t head[3] = {field, group, len};
    put(w, head, sizeof(head));
    put(w, value, len);
}

static void put_uint(writer_t *w, uint8_t group, uint8_t field, uint32_t v, uint8_t width)
{
    uint8_t b[4];
    for (unsigned i = 0; i < width; i++) {
        b[i] = (uint8_t)(v >> (8u * i));
    }
    put_item(w, group, field, b, width);
}

static uint32_t load_uint(const uint8_t *p, uint8_t size)
{
    switch (size) {
    case 1: return p[0];
    case 2: {
        uint16_t v;
        memcpy(&v, p, sizeof(v));
        return v;
    }
    default: {
        uint32_t v;
        memcpy(&v, p, sizeof(v));
        return v;
    }
    }
}

static void put_fields(writer_t *w, uint8_t group, const uint8_t *base)
{
    const group_t *g = &GROUPS[group];
    for (size_t i = 0; i < g->count; i++) {
        const field_t *f = &g->fields[i];
        const uint8_t *p = base + f->offset;
        switch ((kind_t)f->kind) {
        case K_UINT: put_uint(w, group, f->id, load_uint(p, f->size), f->width); break;
        case K_BOOL: put_uint(w, group, f->id, *(const bool *)p ? 1u : 0u, 1u); break;
        case K_STR: {
            uint8_t n = 0;
            while (n + 1u < f->size && p[n] != '\0') {
                n++;
            }
            put_item(w, group, f->id, p, n);
            break;
        }
        case K_NAME: {
            uint8_t n = str_len(p, f->size);
            if (n > ALARM_NAME_LEGACY_LEN - 1u) {
                n = ALARM_NAME_LEGACY_LEN - 1u;
                while (n > 0u && ((unsigned)p[n] & 0xC0u) == 0x80u) {
                    n--; /* never inside a character: back to where one starts */
                }
            }
            put_item(w, group, f->id, p, n);
            break;
        }
        case K_NAME_LONG: {
            const uint8_t n = str_len(p, f->size);
            if (n > ALARM_NAME_LEGACY_LEN - 1u) {
                put_item(w, group, f->id, p, n);
            }
            break;
        }
        case K_BYTES: put_item(w, group, f->id, p, f->width); break;
        case K_UINT_NZ: {
            const uint32_t v = load_uint(p, f->size);
            if (v != 0u) {
                put_uint(w, group, f->id, v, f->width);
            }
            break;
        }
        case K_BYTES_NZ: {
            uint8_t any = 0u;
            for (unsigned b = 0; b < f->width; b++) {
                any |= p[b];
            }
            if (any != 0u) {
                put_item(w, group, f->id, p, f->width);
            }
            break;
        }
        }
    }
}

static void put_entry(writer_t *w, uint8_t group, unsigned index, const void *entry)
{
    put_uint(w, group, 0x00, index, GROUPS[group].index_width);
    put_fields(w, group, (const uint8_t *)entry);
}

static bool device_blank(const alarm_device_config_t *d)
{
    return !d->enabled && d->partition == 0u && d->supervision_s == 0u && d->name[0] == '\0';
}

static void wr_le16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr_le32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4u; i++) {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

alarm_result_t alarm_config_encode(const alarm_config_t *cfg, uint8_t *buf, size_t cap,
                                   size_t *len_out)
{
    if (cfg == NULL || buf == NULL || cap < HDR_LEN) {
        return ALARM_ERR_BAD_ARG;
    }
    writer_t w = {buf, cap, HDR_LEN, false};
    put_fields(&w, GRP_SYSTEM, (const uint8_t *)cfg);
    for (unsigned i = 0; i < ALARM_MAX_PARTITIONS; i++) {
        put_entry(&w, GRP_PARTITION, i + 1u, &cfg->partitions[i]);
    }
    for (unsigned i = 0; i < ALARM_MAX_ZONES; i++) {
        if (cfg->zones[i].type != ALARM_ZT_NOT_USED) {
            put_entry(&w, GRP_ZONE, i + 1u, &cfg->zones[i]);
        }
    }
    for (unsigned i = 0; i < ALARM_MAX_USERS; i++) {
        if (cfg->users[i].authority != ALARM_AUTH_NONE) {
            put_entry(&w, GRP_USER, i + 1u, &cfg->users[i]);
        }
    }
    for (unsigned i = 0; i < ALARM_MAX_DEVICES; i++) {
        if (!device_blank(&cfg->devices[i])) {
            put_entry(&w, GRP_DEVICE, i + 1u, &cfg->devices[i]);
        }
    }
    if (w.overflow) {
        return ALARM_ERR_BAD_ARG;
    }
    const size_t payload = w.len - HDR_LEN;
    buf[0] = 'A';
    buf[1] = 'C';
    buf[2] = 'F';
    buf[3] = 'G';
    buf[4] = ALARM_CONFIG_BLOB_FORMAT;
    buf[5] = 1u; /* readers of format 1 understand everything written here */
    wr_le16(buf + 6, HDR_LEN);
    wr_le32(buf + 8, (uint32_t)payload);
    wr_le32(buf + 12, alarm_crc32(0u, buf + HDR_LEN, payload));
    if (len_out != NULL) {
        *len_out = w.len;
    }
    return ALARM_OK;
}

/* ---- decoding ---------------------------------------------------------------------------- */

static uint32_t rd_le(const uint8_t *p, unsigned n)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++) {
        v |= (uint32_t)p[i] << (8u * i);
    }
    return v;
}

static bool store_uint(uint8_t *p, uint8_t size, uint32_t v)
{
    switch (size) {
    case 1:
        if (v > 0xFFu) {
            return false;
        }
        *p = (uint8_t)v;
        return true;
    case 2: {
        if (v > 0xFFFFu) {
            return false;
        }
        const uint16_t x = (uint16_t)v;
        memcpy(p, &x, sizeof(x));
        return true;
    }
    case 4:
        memcpy(p, &v, sizeof(v));
        return true;
    default: return false;
    }
}

static bool apply_field(const field_t *f, uint8_t *base, const uint8_t *val, uint8_t len)
{
    uint8_t *p = base + f->offset;
    switch ((kind_t)f->kind) {
    case K_UINT:
    case K_UINT_NZ:
        return len >= 1u && len <= 4u && store_uint(p, f->size, rd_le(val, len));
    case K_BOOL:
        if (len != 1u || val[0] > 1u) {
            return false;
        }
        *(bool *)p = val[0] != 0u;
        return true;
    case K_STR:
    case K_NAME:
    case K_NAME_LONG:
        if (len >= f->size) {
            return false;
        }
        memset(p, 0, f->size);
        memcpy(p, val, len);
        return true;
    case K_BYTES:
    case K_BYTES_NZ:
        if (len != f->width) {
            return false;
        }
        memcpy(p, val, len);
        return true;
    }
    return false;
}

static const field_t *find_field(const group_t *g, uint8_t id)
{
    for (size_t i = 0; i < g->count; i++) {
        if (g->fields[i].id == id) {
            return &g->fields[i];
        }
    }
    return NULL;
}

static uint8_t *entry_base(alarm_config_t *cfg, uint8_t group, unsigned index)
{
    switch (group) {
    case GRP_PARTITION: return (uint8_t *)&cfg->partitions[index - 1u];
    case GRP_ZONE: return (uint8_t *)&cfg->zones[index - 1u];
    case GRP_USER: return (uint8_t *)&cfg->users[index - 1u];
    case GRP_DEVICE: return (uint8_t *)&cfg->devices[index - 1u];
    default: return (uint8_t *)cfg;
    }
}

/* lenient (CORE-CFG-022): an index beyond this build's capacities skips its entry's fields until
 * the group's next selector, and the result isn't validated. */
static alarm_result_t decode(const uint8_t *buf, size_t len, alarm_config_t *out, bool lenient)
{
    if (buf == NULL || out == NULL || len < HDR_LEN || memcmp(buf, "ACFG", 4) != 0 ||
        buf[5] > ALARM_CONFIG_BLOB_COMPAT) {
        return ALARM_ERR_BAD_ARG;
    }
    const uint32_t hdr = rd_le(buf + 6, 2);
    const uint32_t payload = rd_le(buf + 8, 4);
    if (hdr < HDR_LEN || hdr > len || payload > len - hdr ||
        alarm_crc32(0u, buf + hdr, payload) != rd_le(buf + 12, 4)) {
        return ALARM_ERR_BAD_ARG;
    }

    alarm_config_defaults(out);
    uint8_t *entry[COUNT_OF(GROUPS)] = {(uint8_t *)out, NULL, NULL, NULL, NULL};
    bool skip[COUNT_OF(GROUPS)] = {false, false, false, false, false};
    const uint8_t *p = buf + hdr;
    const uint8_t *end = p + payload;
    while (p < end) {
        if (end - p < 3) {
            return ALARM_ERR_BAD_ARG;
        }
        const uint8_t field = p[0];
        const uint8_t group = p[1];
        const uint8_t vlen = p[2];
        const uint8_t *val = p + 3;
        if ((size_t)(end - val) < vlen) {
            return ALARM_ERR_BAD_ARG;
        }
        p = val + vlen;
        if (group >= COUNT_OF(GROUPS)) {
            continue; /* a group from a newer format */
        }
        const group_t *g = &GROUPS[group];
        if (field == 0x00u && g->entries != 0u) {
            const uint32_t index = (vlen >= 1u && vlen <= 4u) ? rd_le(val, vlen) : 0u;
            if (index < 1u || (index > g->entries && !lenient)) {
                return ALARM_ERR_BAD_ARG; /* doesn't fit this build's capacities */
            }
            skip[group] = index > g->entries; /* lenient: a bigger master's entry */
            if (skip[group]) {
                continue;
            }
            entry[group] = entry_base(out, group, index);
            if (group != GRP_PARTITION) {
                memset(entry[group], 0, g->entry_size); /* zones, users, devices start blank */
            }
            continue;
        }
        const field_t *f = find_field(g, field);
        if (f == NULL || skip[group]) {
            continue; /* a field from a newer format, or of an entry skipped above */
        }
        if (entry[group] == NULL || !apply_field(f, entry[group], val, vlen)) {
            return ALARM_ERR_BAD_ARG;
        }
    }
    return lenient ? ALARM_OK : alarm_config_validate(out, NULL, 0u);
}

alarm_result_t alarm_config_decode(const uint8_t *buf, size_t len, alarm_config_t *out)
{
    return decode(buf, len, out, false);
}

alarm_result_t alarm_config_decode_lenient(const uint8_t *buf, size_t len, alarm_config_t *out)
{
    return decode(buf, len, out, true);
}
