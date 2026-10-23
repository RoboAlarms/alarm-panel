/* The stored record: encode, decode, the NVS cost (docs/features/41 AUT-008, DESIGN.md section 6
 * "Stored format"). Compat byte 1, then TLV id u8 | len u8 | value, little-endian. Ids below 0x80
 * are critical (unknown: NEWER), 0x80 and above ignorable (kept and written back). Zero fields are
 * not written, except an action's value (absent = -1). The ids are stored: never renumber. */
#include "automation/automation.h"

#include <string.h>

#include "home_control/home_control.h"

#define AUTO_COMPAT 1u

/* Record ids (DESIGN.md section 6). */
enum {
    ID_UID = 0x01,
    ID_FLAGS = 0x02,
    ID_MADE_BY = 0x04,
    ID_TRIG_KIND = 0x10,
    ID_PARTITIONS = 0x11,
    ID_LEVELS = 0x12,
    ID_ZONE = 0x13,
    ID_MINUTE = 0x14,
    ID_DAYS = 0x15,
    ID_WIN_FROM = 0x16,
    ID_WIN_TO = 0x17,
    /* 0x18 reserved: a zone bitmap (OUT-005); unknown here, so NEWER */
    ID_TROUBLE = 0x19, /* u8: a trouble trigger's auto_trouble_t (BL-224); an older firmware
                        * already loads the trigger kind as NEWER */
    ID_ACT1 = 0x20,
    ID_ACT2 = 0x30,
    ID_IGNORABLE = 0x80, /* 0x80 itself reserved: a typed name */
    /* Ignorable fields this firmware knows (an older one keeps them as stored and runs without
     * them, safely): */
    ID_BYPASS = 0x82,     /* u8: bit n = action n+1's "Bypass open zones" (D29); only on an Arm
                           * action. An older firmware arms without bypassing: it fails on an
                           * open zone instead, never arms less protected */
    ID_CHANGED_AT = 0x83, /* u32 epoch seconds, when the sentence last changed (D30). An older
                           * firmware shows no "changed since": words only */
    ID_SAVED_BY = 0x84,   /* u8: who saved it when that isn't made_by (BL-224: the installer, for
                           * an Arm that acts as the system master). An older firmware shows "Set
                           * up by" the system master: words only */
};

/* Action sub-ids: ID_ACT1 / ID_ACT2 + n. */
enum {
    SUB_KIND = 0x0,
    SUB_SOURCE = 0x1,
    SUB_DEV_KIND = 0x2,
    SUB_DEV_ACTION = 0x3,
    SUB_VALUE = 0x4,
    SUB_MODE = 0x5,
    SUB_REF = 0x6,
    SUB_NAME = 0x7,
    SUB_OFF_AFTER = 0x8,
    SUB_CONFIRMED = 0x9,
    SUB_ARM_PARTITION = 0xA,
    SUB_ARM_LEVEL = 0xB,
    SUB_WARN_MIN = 0xC,
    SUB_OUTPUT = 0xD,   /* BL-224: AUTO_ACT_OUTPUT's wired output */
    SUB_OPTION = 0xE,   /* BL-224: a reminder's notification, a chime's on */
    SUB_LAST = SUB_OPTION,
};

#define FLAG_OFF 0x01u
#define VALUE_ABSENT (-1)

/* ---- encode ---- */

typedef struct {
    uint8_t *out;
    size_t cap;
    size_t len;
    bool ok;
} writer_t;

static void put(writer_t *w, uint8_t id, const uint8_t *v, size_t n)
{
    if (!w->ok || n > 255u || w->len + 2u + n > w->cap) {
        w->ok = false;
        return;
    }
    w->out[w->len++] = id;
    w->out[w->len++] = (uint8_t)n;
    if (n > 0u) {
        memcpy(&w->out[w->len], v, n);
    }
    w->len += n;
}

static void put_u8(writer_t *w, uint8_t id, uint8_t v)
{
    if (v != 0u) {
        put(w, id, &v, 1u);
    }
}

static void put_u16_always(writer_t *w, uint8_t id, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)(v & 0xFFu), (uint8_t)((unsigned)v >> 8)};
    put(w, id, b, 2u);
}

static void put_u16(writer_t *w, uint8_t id, uint16_t v)
{
    if (v != 0u) {
        put_u16_always(w, id, v);
    }
}

/* A string field: written when non-empty; an unterminated buffer is refused (encode returns 0). */
static void put_str(writer_t *w, uint8_t id, const char *s, size_t cap)
{
    size_t n = 0;
    while (n < cap && s[n] != '\0') {
        n++;
    }
    if (n == cap) {
        w->ok = false;
        return;
    }
    if (n > 0u) {
        put(w, id, (const uint8_t *)s, n);
    }
}

static void put_action(writer_t *w, uint8_t base, const auto_action_t *a)
{
    put_u8(w, (uint8_t)(base + SUB_KIND), a->kind);
    put_u8(w, (uint8_t)(base + SUB_SOURCE), a->source);
    put_u8(w, (uint8_t)(base + SUB_DEV_KIND), a->dev_kind);
    put_u8(w, (uint8_t)(base + SUB_DEV_ACTION), a->dev_action);
    if (a->value != VALUE_ABSENT) { /* 0 is a value (brightness 0): written */
        put_u16_always(w, (uint8_t)(base + SUB_VALUE), (uint16_t)a->value);
    }
    put_str(w, (uint8_t)(base + SUB_MODE), a->mode, sizeof a->mode);
    put_str(w, (uint8_t)(base + SUB_REF), a->ref, sizeof a->ref);
    put_str(w, (uint8_t)(base + SUB_NAME), a->name, sizeof a->name);
    put_u16(w, (uint8_t)(base + SUB_OFF_AFTER), a->off_after_min);
    put_u8(w, (uint8_t)(base + SUB_CONFIRMED), a->confirmed);
    put_u8(w, (uint8_t)(base + SUB_ARM_PARTITION), a->partition);
    put_u8(w, (uint8_t)(base + SUB_ARM_LEVEL), a->arm_level);
    put_u8(w, (uint8_t)(base + SUB_WARN_MIN), a->warn_min);
    put_u8(w, (uint8_t)(base + SUB_OUTPUT), a->output);
    put_u8(w, (uint8_t)(base + SUB_OPTION), a->option);
}

size_t auto_encode(const auto_rule_t *r, uint8_t *out, size_t cap)
{
    if (r == NULL || out == NULL || cap == 0u || r->n > AUTO_ACTIONS ||
        r->keep_len > AUTO_KEEP_MAX) {
        return 0;
    }
    writer_t w = {out, cap, 0u, true};
    out[w.len++] = (uint8_t)AUTO_COMPAT;
    put_u16(&w, ID_UID, r->uid);
    put_u8(&w, ID_FLAGS, r->off ? (uint8_t)FLAG_OFF : (uint8_t)0u);
    put_u8(&w, ID_MADE_BY, r->made_by);
    put_u8(&w, ID_TRIG_KIND, r->when.kind);
    put_u8(&w, ID_PARTITIONS, r->when.partitions);
    put_u8(&w, ID_LEVELS, r->when.levels);
    put_u16(&w, ID_ZONE, r->when.zone);
    put_u16(&w, ID_MINUTE, r->when.minute);
    put_u8(&w, ID_DAYS, r->when.days);
    put_u16(&w, ID_WIN_FROM, r->when.win_from);
    put_u16(&w, ID_WIN_TO, r->when.win_to);
    put_u8(&w, ID_TROUBLE, r->when.trouble);
    uint8_t bypass = 0u;
    for (unsigned i = 0; i < r->n; i++) {
        put_action(&w, i == 0u ? (uint8_t)ID_ACT1 : (uint8_t)ID_ACT2, &r->then[i]);
        if (r->then[i].bypass_open != 0u) {
            bypass = (uint8_t)(bypass | (1u << i));
        }
    }
    put_u8(&w, ID_BYPASS, bypass);
    if (r->changed_at != 0u) {
        const uint32_t t = r->changed_at;
        const uint8_t b[4] = {(uint8_t)(t & 0xFFu), (uint8_t)((t >> 8) & 0xFFu),
                              (uint8_t)((t >> 16) & 0xFFu), (uint8_t)(t >> 24)};
        put(&w, ID_CHANGED_AT, b, 4u);
    }
    put_u8(&w, ID_SAVED_BY, r->saved_by);
    /* the ignorable fields this firmware doesn't know, as stored (whole TLVs) */
    if (w.ok && r->keep_len > 0u) {
        if (w.len + r->keep_len > cap) {
            w.ok = false;
        } else {
            memcpy(&out[w.len], r->keep, r->keep_len);
            w.len += r->keep_len;
        }
    }
    return w.ok ? w.len : 0u;
}

/* ---- decode ---- */

/* Outcomes rank BAD > NEWER > BROKEN > OK: the worst one found wins. */
static void worsen(auto_rec_t *rec, auto_rec_t to)
{
    static const uint8_t rank[] = {
        [AUTO_REC_OK] = 0, [AUTO_REC_BROKEN] = 1, [AUTO_REC_NEWER] = 2, [AUTO_REC_BAD] = 3};
    if (rank[to] > rank[*rec]) {
        *rec = to;
    }
}

static uint16_t get_u16(const uint8_t *v)
{
    return (uint16_t)((unsigned)v[0] | ((unsigned)v[1] << 8));
}

/* A string field: 1..max bytes, no NUL inside. */
static bool get_str(char *dst, size_t cap, size_t max, const uint8_t *v, size_t n)
{
    if (n == 0u || n > max || n >= cap || memchr(v, 0, n) != NULL) {
        return false;
    }
    memcpy(dst, v, n);
    dst[n] = '\0';
    return true;
}

/* One action field; returns the outcome it causes (OK, NEWER or BAD). */
static auto_rec_t get_action(auto_action_t *a, unsigned sub, const uint8_t *v, size_t n)
{
    switch (sub) {
    case SUB_MODE:
        return get_str(a->mode, sizeof a->mode, DEV_MODE_LEN - 1u, v, n) ? AUTO_REC_OK
                                                                        : AUTO_REC_BAD;
    case SUB_REF:
        return get_str(a->ref, sizeof a->ref, DEV_REF_LEN - 1u, v, n) ? AUTO_REC_OK
                                                                     : AUTO_REC_BAD;
    case SUB_NAME:
        return get_str(a->name, sizeof a->name, DEV_NAME_LEN - 1u, v, n) ? AUTO_REC_OK
                                                                        : AUTO_REC_BAD;
    case SUB_VALUE:
    case SUB_OFF_AFTER:
        if (n != 2u) {
            return AUTO_REC_BAD;
        }
        if (sub == SUB_VALUE) {
            uint16_t u = get_u16(v);
            a->value = u >= 0x8000u ? (int16_t)((int32_t)u - 0x10000) : (int16_t)u;
        } else {
            a->off_after_min = get_u16(v);
        }
        return AUTO_REC_OK;
    default:
        break;
    }
    if (n != 1u) {
        return AUTO_REC_BAD;
    }
    uint8_t b = v[0];
    switch (sub) {
    case SUB_KIND:
        a->kind = b;
        return (b == AUTO_ACT_DEVICE || b == AUTO_ACT_ARM || b == AUTO_ACT_DISARM ||
                b == AUTO_ACT_OUTPUT || b == AUTO_ACT_REMIND || b == AUTO_ACT_CHECK_ARMED ||
                b == AUTO_ACT_CHIME || b == AUTO_ACT_NOTIFY)
                   ? AUTO_REC_OK
                   : AUTO_REC_NEWER;
    case SUB_SOURCE:
        a->source = b;
        return b <= AUTO_SOURCE_LAST ? AUTO_REC_OK : AUTO_REC_NEWER;
    case SUB_DEV_KIND:
        a->dev_kind = b;
        return b < DEV_KIND_COUNT ? AUTO_REC_OK : AUTO_REC_NEWER;
    case SUB_DEV_ACTION:
        a->dev_action = b;
        return b < ACT_COUNT ? AUTO_REC_OK : AUTO_REC_NEWER;
    case SUB_CONFIRMED:
        a->confirmed = b;
        return AUTO_REC_OK;
    case SUB_ARM_PARTITION:
        a->partition = b;
        return AUTO_REC_OK;
    case SUB_ARM_LEVEL:
        a->arm_level = b;
        return AUTO_REC_OK;
    case SUB_WARN_MIN:
        a->warn_min = b;
        return AUTO_REC_OK;
    case SUB_OUTPUT:
        a->output = b;
        return AUTO_REC_OK;
    case SUB_OPTION:
        a->option = b;
        return AUTO_REC_OK;
    default:
        return AUTO_REC_NEWER; /* a sub-id this firmware doesn't know */
    }
}

/* One critical field outside the actions; returns the outcome it causes. */
static auto_rec_t get_field(auto_rule_t *r, unsigned id, const uint8_t *v, size_t n)
{
    switch (id) {
    case ID_UID:
    case ID_ZONE:
    case ID_MINUTE:
    case ID_WIN_FROM:
    case ID_WIN_TO: {
        if (n != 2u) {
            return AUTO_REC_BAD;
        }
        uint16_t u = get_u16(v);
        if (id == ID_UID) {
            r->uid = u;
        } else if (id == ID_ZONE) {
            r->when.zone = u;
        } else if (id == ID_MINUTE) {
            r->when.minute = u;
        } else if (id == ID_WIN_FROM) {
            r->when.win_from = u;
        } else {
            r->when.win_to = u;
        }
        return AUTO_REC_OK;
    }
    case ID_FLAGS:
    case ID_MADE_BY:
    case ID_TRIG_KIND:
    case ID_PARTITIONS:
    case ID_LEVELS:
    case ID_DAYS:
    case ID_TROUBLE:
        break;
    default:
        return AUTO_REC_NEWER; /* a critical id this firmware doesn't know */
    }
    if (n != 1u) {
        return AUTO_REC_BAD;
    }
    uint8_t b = v[0];
    switch (id) {
    case ID_FLAGS:
        r->off = (b & FLAG_OFF) != 0u;
        return (b & (uint8_t)~FLAG_OFF) == 0u ? AUTO_REC_OK : AUTO_REC_NEWER;
    case ID_MADE_BY:
        r->made_by = b;
        return AUTO_REC_OK;
    case ID_TRIG_KIND:
        r->when.kind = b;
        return b < AUTO_TRIG_COUNT ? AUTO_REC_OK : AUTO_REC_NEWER;
    case ID_PARTITIONS:
        r->when.partitions = b;
        return AUTO_REC_OK;
    case ID_LEVELS:
        r->when.levels = b;
        return AUTO_REC_OK;
    case ID_TROUBLE:
        r->when.trouble = b;
        return b <= AUTO_TROUBLE_LAST ? AUTO_REC_OK : AUTO_REC_NEWER;
    default: /* ID_DAYS */
        r->when.days = b;
        return AUTO_REC_OK;
    }
}

auto_rec_t auto_decode(const uint8_t *in, size_t len, auto_rule_t *out)
{
    if (out == NULL) {
        return AUTO_REC_BAD;
    }
    memset(out, 0, sizeof *out);
    for (unsigned i = 0; i < AUTO_ACTIONS; i++) {
        out->then[i].value = VALUE_ABSENT;
    }
    auto_rec_t rec = AUTO_REC_OK;
    /* 0xFF is erased flash; 0 was never written */
    if (in == NULL || len == 0u || in[0] == 0xFFu || in[0] == 0u) {
        out->rec = (uint8_t)AUTO_REC_BAD;
        return AUTO_REC_BAD;
    }
    if (in[0] > AUTO_COMPAT) {
        out->rec = (uint8_t)AUTO_REC_NEWER;
        return AUTO_REC_NEWER;
    }
    uint64_t seen = 0; /* critical ids 0x00..0x3F: a second copy is BAD */
    uint8_t bypass = 0u;
    bool keep_full = false;
    size_t pos = 1;
    while (pos < len && rec != AUTO_REC_BAD) {
        if (len - pos < 2u) {
            rec = AUTO_REC_BAD; /* an id without its length */
            break;
        }
        unsigned id = in[pos];
        size_t n = in[pos + 1u];
        const uint8_t *v = &in[pos + 2u];
        if (len - pos - 2u < n) {
            rec = AUTO_REC_BAD; /* a value cut short */
            break;
        }
        pos += 2u + n;

        if (id == ID_BYPASS && n == 1u) {
            bypass = v[0];
            continue;
        }
        if (id == ID_SAVED_BY && n == 1u) {
            out->saved_by = v[0];
            continue;
        }
        if (id == ID_CHANGED_AT && n == 4u) {
            out->changed_at = (uint32_t)v[0] | ((uint32_t)v[1] << 8) | ((uint32_t)v[2] << 16) |
                              ((uint32_t)v[3] << 24);
            continue;
        }
        if (id >= ID_IGNORABLE) {
            /* (a known ignorable id of another length is kept as stored, like an unknown one) */
            /* kept in stored order while they fit; past AUTO_KEEP_MAX the record is read-only */
            if (!keep_full && out->keep_len + 2u + n <= AUTO_KEEP_MAX) {
                memcpy(&out->keep[out->keep_len], &in[pos - 2u - n], 2u + n);
                out->keep_len = (uint8_t)(out->keep_len + 2u + n);
            } else {
                keep_full = true;
                worsen(&rec, AUTO_REC_NEWER);
            }
            continue;
        }
        if (id < 64u) {
            uint64_t bit = (uint64_t)1u << id;
            if ((seen & bit) != 0u) {
                worsen(&rec, AUTO_REC_BAD);
                continue;
            }
            seen |= bit;
        }
        if (id >= ID_ACT1 && id <= ID_ACT2 + SUB_LAST && (id & 0x0Fu) <= SUB_LAST) {
            unsigned a = id >= ID_ACT2 ? 1u : 0u;
            if (out->n < a + 1u) {
                out->n = (uint8_t)(a + 1u);
            }
            worsen(&rec, get_action(&out->then[a], id & 0x0Fu, v, n));
        } else {
            worsen(&rec, get_field(out, id, v, n));
        }
    }
    /* D29: "Bypass open zones" belongs to an Arm action. A bit for any other action (an older
     * firmware kept the field through an edit that changed the action) means nothing: dropped. */
    for (unsigned i = 0; i < out->n; i++) {
        out->then[i].bypass_open =
            ((unsigned)bypass >> i & 1u) != 0u && out->then[i].kind == AUTO_ACT_ARM ? 1u : 0u;
    }
    if (rec == AUTO_REC_OK) {
        /* AUT-002 layer 3: a stored pair no automation may do is kept and shown, never run. The
         * installer option never makes a record BROKEN (AUT-015): judged as if it were on. */
        for (unsigned i = 0; i < out->n; i++) {
            const auto_action_t *a = &out->then[i];
            if (a->kind == AUTO_ACT_DEVICE &&
                !hc_auto_allowed((dev_kind_t)a->dev_kind, (dev_action_t)a->dev_action, a->value,
                                 a->mode, true)) {
                rec = AUTO_REC_BROKEN;
            }
        }
    }
    out->rec = (uint8_t)rec;
    return rec;
}

size_t auto_nvs_entries(size_t record_bytes)
{
    /* zbz_panel's count: the index entry, one header per 4000-byte chunk, 32-byte data entries */
    return 1u + (record_bytes + 3999u) / 4000u + (record_bytes + 31u) / 32u;
}
