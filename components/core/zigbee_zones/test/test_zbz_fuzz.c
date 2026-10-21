/*
 * test_zbz_fuzz.c - every decoder of zigbee_zones against random and mutated input, with fixed
 * seeds (docs/features/20 testing.md §7 "robustness"; ZB-003, ZB-010, ZB-012, ZB-015, ZB-021,
 * ZB-023, ZB-024): the ZCL frame decoders and the attribute iterator, the IAS Zone, WD and ACE
 * command decoders (zba_decode_request among them), the stored registry, codes, cfg, wd and ace
 * blobs, and the install-code parsers.
 *
 * What each case proves:
 * - no crash, and no read past the input: each input is decoded twice, once with 0x00 and once
 *   with 0xFF bytes after its end (after f->len in a frame's payload too), and both decodes must
 *   say the same - a decoder that peeked past the end would see the difference. The host build's
 *   sanitizers (ALARM_SANITIZE, address and undefined; CI runs them, MinGW has none) watch the
 *   same runs for overruns the poison can't show;
 * - no write past the output: every encoder writes in front of guard bytes that must stay as they
 *   were, and with a `cap` one byte short it writes nothing and answers 0;
 * - whatever decodes encodes back the same: byte for byte where the format keeps every bit (a
 *   ZCL frame), else decode(encode(decode(x))) == decode(x) (the stored blobs drop unknown
 *   fields and zero ones, an invalid string reads as "", a setup in flight reads STOPPED).
 *
 * The random numbers are the test's own (splitmix64 from fixed seeds), so a failure replays.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "zigbee_zones/zbz_ace.h"
#include "zigbee_zones/zbz_codes.h"
#include "zigbee_zones/zbz_panel.h"
#include "zigbee_zones/zbz_registry.h"
#include "zigbee_zones/zbz_wd.h"
#include "zigbee_zones/zbz_zcl.h"

#include "ace_fixture_v1.h"
#include "cfg_fixture_v1.h"
#include "codes_fixture_v1.h"
#include "registry_fixture_v1.h"
#include "wd_fixture_v1.h"
#include "wd_fixture_v2.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- fixed-seed random numbers and mutations -------------------------------------------------------- */

static uint64_t g_state;

static void seed(uint64_t s)
{
    g_state = s;
}

static uint64_t rnd64(void)
{
    uint64_t z = (g_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static uint8_t rnd_byte(void)
{
    return (uint8_t)(rnd64() & 0xFFu);
}

static size_t rnd_below(size_t n)
{
    return n == 0u ? 0u : (size_t)(rnd64() % (uint64_t)n);
}

/* Edge values a length or count byte likes to break on. */
static uint8_t rnd_edge(void)
{
    static const uint8_t edges[] = {0x00, 0x01, 0x02, 0x7F, 0x80, 0xFE, 0xFF, 0x10, 0x20, 0x30};
    return edges[rnd_below(sizeof(edges))];
}

/* One to four mutations of buf[0..len) in place (cap bytes room); the new length. */
static size_t mutate(uint8_t *buf, size_t len, size_t cap)
{
    const size_t n = 1u + rnd_below(4u);
    for (size_t k = 0; k < n; k++) {
        switch (rnd_below(8u)) {
        case 0:
            if (len > 0u) {
                buf[rnd_below(len)] ^= (uint8_t)(1u << rnd_below(8u));
            }
            break;
        case 1:
            if (len > 0u) {
                buf[rnd_below(len)] = rnd_byte();
            }
            break;
        case 2:
            if (len > 0u) {
                buf[rnd_below(len)] = rnd_edge();
            }
            break;
        case 3: len = rnd_below(len + 1u); break; /* truncated */
        case 4:                                   /* longer */
            while (len < cap && rnd_below(3u) != 0u) {
                buf[len++] = rnd_byte();
            }
            break;
        case 5: /* a byte removed */
            if (len > 0u) {
                const size_t at = rnd_below(len);
                memmove(&buf[at], &buf[at + 1u], len - at - 1u);
                len--;
            }
            break;
        case 6: /* a byte inserted */
            if (len < cap) {
                const size_t at = rnd_below(len + 1u);
                memmove(&buf[at + 1u], &buf[at], len - at);
                buf[at] = rnd_edge();
                len++;
            }
            break;
        default: /* a run copied over another place */
            if (len > 2u) {
                const size_t from = rnd_below(len);
                const size_t to = rnd_below(len);
                const size_t run = 1u + rnd_below(len - (from > to ? from : to));
                memmove(&buf[to], &buf[from], run);
            }
            break;
        }
    }
    return len;
}

/* An input: random bytes (1 in 4) or a mutated seed. */
static size_t make_input(uint8_t *buf, size_t cap, const uint8_t *const *seeds,
                         const size_t *seed_len, size_t seed_count)
{
    if (seed_count == 0u || rnd_below(4u) == 0u) {
        const size_t len = rnd_below(cap + 1u);
        for (size_t i = 0; i < len; i++) {
            buf[i] = rnd_byte();
        }
        return len;
    }
    const size_t s = rnd_below(seed_count);
    size_t len = seed_len[s] < cap ? seed_len[s] : cap;
    memcpy(buf, seeds[s], len);
    return mutate(buf, len, cap);
}

/* n bytes the same (Unity refuses to compare 0 bytes). */
#define SAME_MEM(x, y, n)                                                                          \
    do {                                                                                           \
        if ((n) > 0u) {                                                                            \
            TEST_ASSERT_EQUAL_MEMORY((x), (y), (n));                                               \
        }                                                                                          \
    } while (0)
#define SAME_MEM_MSG(x, y, n, m)                                                                   \
    do {                                                                                           \
        if ((n) > 0u) {                                                                            \
            TEST_ASSERT_EQUAL_MEMORY_MESSAGE((x), (y), (n), (m));                                  \
        }                                                                                          \
    } while (0)

/* ---- the poison and the guards ---------------------------------------------------------------------- */

#define TAIL  32u /* poison bytes after an input */
#define GUARD 16u /* guard bytes after an output */

/* `in` twice, each followed by TAIL bytes of its poison. */
typedef struct {
    uint8_t a[1024 + TAIL];
    uint8_t b[1024 + TAIL];
} twin_t;
static twin_t T;

static void twin(const uint8_t *in, size_t len)
{
    TEST_ASSERT_TRUE(len <= 1024u);
    memset(T.a, 0x00, sizeof(T.a));
    memset(T.b, 0xFF, sizeof(T.b));
    if (len > 0u) {
        memcpy(T.a, in, len);
        memcpy(T.b, in, len);
    }
}

/* A frame's payload past f->len poisoned, as a radio's reused buffer would leave it. */
static void poison_payload(zsrc_zb_zcl_t *f, uint8_t v)
{
    if (f->len < ZSRC_ZB_ZCL_PAYLOAD_MAX) {
        memset(&f->payload[f->len], v, ZSRC_ZB_ZCL_PAYLOAD_MAX - f->len);
    }
}

/* An encoder's output with its guard: the length it answers, after checking the guard and that a
 * cap one byte short writes nothing and answers 0. */
typedef size_t (*enc_fn)(const void *obj, uint8_t *out, size_t cap);

static uint8_t g_out[1024 + GUARD];

static size_t encode_guarded(enc_fn enc, const void *obj, size_t cap)
{
    TEST_ASSERT_TRUE(cap <= 1024u);
    memset(g_out, 0xA5, sizeof(g_out));
    const size_t n = enc(obj, g_out, cap);
    TEST_ASSERT_TRUE(n <= cap);
    for (size_t i = cap; i < cap + GUARD; i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xA5, g_out[i], "written past cap");
    }
    if (n > 0u) {
        static uint8_t again[1024 + GUARD];
        memset(again, 0x5A, sizeof(again));
        TEST_ASSERT_EQUAL_UINT(0u, enc(obj, again, n - 1u));
        for (size_t i = n - 1u; i < n - 1u + GUARD; i++) {
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x5A, again[i], "written past a short cap");
        }
    }
    return n;
}

/* ==== ZCL frames ======================================================================================= */

/* How often each decoder said OK: the checks behind it ran (asserted after the run). */
enum {
    HIT_FRAME,
    HIT_GLOBAL,
    HIT_RECORDS,
    HIT_CONFIG,
    HIT_DEFAULT,
    HIT_ENROLL,
    HIT_STATUS,
    HIT_WARNING,
    HIT_ARM,
    HIT_PANEL,
    HIT_REQUEST,
    HIT_COUNT
};
static unsigned g_hit[HIT_COUNT];

static const uint16_t CLUSTERS[] = {ZSRC_ZB_ZCL_BASIC,    ZSRC_ZB_ZCL_POWER_CFG, ZSRC_ZB_ZCL_IDENTIFY,
                                    ZSRC_ZB_ZCL_ON_OFF,   ZSRC_ZB_ZCL_POLL_CTRL, ZSRC_ZB_ZCL_TEMP_MEAS,
                                    ZSRC_ZB_ZCL_OCCUPANCY, ZSRC_ZB_ZCL_IAS_ZONE, ZSRC_ZB_ZCL_IAS_ACE,
                                    ZSRC_ZB_ZCL_IAS_WD,   ZSRC_ZB_ZCL_TUYA};

typedef struct {
    uint16_t attr;
    uint8_t status;
    uint8_t type;
    uint8_t len;
    uint8_t value[256];
} rec_t;

#define RECS_MAX 64u

/* Every record of a global frame, checking each value lies inside the frame's payload. */
static size_t records(const zsrc_zb_zcl_t *f, rec_t *out, zbz_zcl_result_t *err)
{
    zbz_zcl_iter_t it;
    zbz_zcl_iter(&it, f);
    zbz_zcl_attr_t a;
    size_t n = 0u;
    while (zbz_zcl_next_attr(&it, &a)) {
        TEST_ASSERT_TRUE_MESSAGE(n < RECS_MAX, "the iterator never ends");
        if (a.len > 0u) {
            TEST_ASSERT_NOT_NULL(a.value);
            TEST_ASSERT_TRUE(a.value >= f->payload && a.value + a.len <= f->payload + f->len);
        }
        rec_t *r = &out[n++];
        memset(r, 0, sizeof(*r));
        r->attr = a.attr;
        r->status = a.status;
        r->type = a.type;
        r->len = a.len;
        if (a.len > 0u) {
            memcpy(r->value, a.value, a.len);
        }
        (void)zbz_zcl_attr_uint(&a);
        (void)zbz_zcl_attr_int(&a);
    }
    *err = it.err;
    return n;
}

static void same_records(const rec_t *x, size_t nx, const rec_t *y, size_t ny)
{
    TEST_ASSERT_EQUAL_UINT(nx, ny);
    for (size_t i = 0; i < nx; i++) {
        TEST_ASSERT_EQUAL_HEX16(x[i].attr, y[i].attr);
        TEST_ASSERT_EQUAL_HEX8(x[i].status, y[i].status);
        TEST_ASSERT_EQUAL_HEX8(x[i].type, y[i].type);
        TEST_ASSERT_EQUAL_UINT8(x[i].len, y[i].len);
        SAME_MEM(x[i].value, y[i].value, x[i].len);
    }
}

static void same_reporting(const zsrc_zb_reporting_t *x, const zsrc_zb_reporting_t *y, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_HEX16(x[i].cluster, y[i].cluster);
        TEST_ASSERT_EQUAL_HEX16(x[i].attr, y[i].attr);
        TEST_ASSERT_EQUAL_HEX8(x[i].type, y[i].type);
        TEST_ASSERT_EQUAL_UINT16(x[i].min_s, y[i].min_s);
        TEST_ASSERT_EQUAL_UINT16(x[i].max_s, y[i].max_s);
        TEST_ASSERT_EQUAL_UINT32(x[i].change, y[i].change);
    }
}

static size_t enc_zcl(const void *f, uint8_t *out, size_t cap)
{
    return zbz_zcl_encode((const zsrc_zb_zcl_t *)f, 0x42u, out, cap);
}

static size_t enc_zcl_global(const void *f, uint8_t *out, size_t cap)
{
    return zbz_zcl_encode_global((const zsrc_zb_zcl_t *)f, 0x42u, out, cap);
}

/* A global frame decoded twice (fa, fb, payload tails poisoned apart): its records, Configure
 * Reporting and Default Response the same both ways, and rebuilt the same. */
static void check_global(zsrc_zb_zcl_t *fa, zsrc_zb_zcl_t *fb)
{
    static rec_t ra[RECS_MAX], rb[RECS_MAX], rc[RECS_MAX];
    zbz_zcl_result_t ea, eb, ec;
    const size_t na = records(fa, ra, &ea);
    const size_t nb = records(fb, rb, &eb);
    TEST_ASSERT_EQUAL_INT(ea, eb);
    same_records(ra, na, rb, nb);
    if (ea == ZBZ_ZCL_OK && na > 0u) {
        g_hit[HIT_RECORDS]++;
        /* the records rebuilt read back the same (an invalid string's 0xFF length reads "" both
         * times, a Configure Reporting Response's direction is not kept) */
        zsrc_zb_zcl_t g;
        zbz_zcl_global(fa->cluster, fa->command, fa->server_to_client, &g);
        for (size_t i = 0; i < na; i++) {
            const zbz_zcl_attr_t a = {.attr = ra[i].attr,
                                      .status = ra[i].status,
                                      .type = ra[i].type,
                                      .len = ra[i].len,
                                      .value = ra[i].len > 0u ? ra[i].value : NULL};
            TEST_ASSERT_TRUE_MESSAGE(zbz_zcl_put_attr(&g, &a), "a record read could not be written");
        }
        TEST_ASSERT_TRUE(g.len <= fa->len);
        const size_t nc = records(&g, rc, &ec);
        TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, ec);
        same_records(ra, na, rc, nc);
    }

    static zsrc_zb_reporting_t pa[16], pb[16], pc[16];
    uint8_t ca = 0u, cb = 0u, cc = 0u;
    memset(pa, 0, sizeof(pa));
    memset(pb, 0, sizeof(pb));
    const zbz_zcl_result_t qa = zbz_zcl_decode_config_report(fa, pa, 16u, &ca);
    const zbz_zcl_result_t qb = zbz_zcl_decode_config_report(fb, pb, 16u, &cb);
    TEST_ASSERT_EQUAL_INT(qa, qb);
    TEST_ASSERT_EQUAL_UINT8(ca, cb);
    TEST_ASSERT_TRUE(ca <= 16u);
    if (qa == ZBZ_ZCL_OK) {
        g_hit[HIT_CONFIG]++;
        same_reporting(pa, pb, ca);
        zsrc_zb_zcl_t g;
        if (ca > 0u && zbz_zcl_config_report(pa, ca, &g)) {
            TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode_config_report(&g, pc, 16u, &cc));
            TEST_ASSERT_EQUAL_UINT8(ca, cc);
            same_reporting(pa, pc, ca);
        }
    }

    uint8_t cmd_a = 0u, st_a = 0u, cmd_b = 0u, st_b = 0u;
    const zbz_zcl_result_t da = zbz_zcl_decode_default_rsp(fa, &cmd_a, &st_a);
    TEST_ASSERT_EQUAL_INT(da, zbz_zcl_decode_default_rsp(fb, &cmd_b, &st_b));
    if (da == ZBZ_ZCL_OK) {
        g_hit[HIT_DEFAULT]++;
        TEST_ASSERT_EQUAL_HEX8(cmd_a, cmd_b);
        TEST_ASSERT_EQUAL_HEX8(st_a, st_b);
        zsrc_zb_zcl_t g;
        zbz_zcl_default_rsp(fa->cluster, cmd_a, st_a, fa->server_to_client, &g);
        uint8_t c2 = 0u, s2 = 0u;
        TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode_default_rsp(&g, &c2, &s2));
        TEST_ASSERT_EQUAL_HEX8(cmd_a, c2);
        TEST_ASSERT_EQUAL_HEX8(st_a, s2);
    }
}

static void same_warning(const zbw_warning_t *x, const zbw_warning_t *y)
{
    TEST_ASSERT_EQUAL_UINT8(x->mode, y->mode);
    TEST_ASSERT_EQUAL(x->strobe, y->strobe);
    TEST_ASSERT_EQUAL_UINT8(x->level, y->level);
    TEST_ASSERT_EQUAL_UINT16(x->duration_s, y->duration_s);
    TEST_ASSERT_EQUAL_UINT8(x->duty, y->duty);
    TEST_ASSERT_EQUAL_UINT8(x->strobe_level, y->strobe_level);
    TEST_ASSERT_EQUAL_UINT8(x->reserved, y->reserved);
}

static void same_status(const zba_status_t *x, const zba_status_t *y)
{
    TEST_ASSERT_EQUAL_UINT8(x->status, y->status);
    TEST_ASSERT_EQUAL_UINT8(x->seconds, y->seconds);
    TEST_ASSERT_EQUAL_UINT8(x->audible, y->audible);
    TEST_ASSERT_EQUAL_UINT8(x->alarm, y->alarm);
}

/* A cluster frame decoded twice: every command decoder the same both ways, the ones with an
 * encoder rebuilt the same. */
static void check_cluster(zsrc_zb_zcl_t *fa, zsrc_zb_zcl_t *fb)
{
    zbz_ias_enroll_req_t ea, eb;
    memset(&ea, 0, sizeof(ea));
    memset(&eb, 0, sizeof(eb));
    zbz_zcl_result_t r = zbz_ias_decode_enroll_request(fa, &ea);
    TEST_ASSERT_EQUAL_INT(r, zbz_ias_decode_enroll_request(fb, &eb));
    if (r == ZBZ_ZCL_OK) {
        g_hit[HIT_ENROLL]++;
        TEST_ASSERT_EQUAL_HEX16(ea.zone_type, eb.zone_type);
        TEST_ASSERT_EQUAL_HEX16(ea.manufacturer, eb.manufacturer);
    }

    zbz_ias_status_t sa, sb;
    memset(&sa, 0, sizeof(sa));
    memset(&sb, 0, sizeof(sb));
    r = zbz_ias_decode_status_change(fa, &sa);
    TEST_ASSERT_EQUAL_INT(r, zbz_ias_decode_status_change(fb, &sb));
    if (r == ZBZ_ZCL_OK) {
        g_hit[HIT_STATUS]++;
        TEST_ASSERT_EQUAL_HEX16(sa.status, sb.status);
        TEST_ASSERT_EQUAL_HEX8(sa.extended, sb.extended);
        TEST_ASSERT_EQUAL_HEX8(sa.zone_id, sb.zone_id);
        TEST_ASSERT_EQUAL_UINT16(sa.delay_qs, sb.delay_qs);
    }

    for (uint8_t layout = ZBW_LAYOUT_SPEC; layout <= ZBW_LAYOUT_REVERSED; layout++) {
        zbw_warning_t wa, wb, wc;
        memset(&wa, 0, sizeof(wa));
        memset(&wb, 0, sizeof(wb));
        r = zbw_decode_start_warning(fa, layout, &wa);
        TEST_ASSERT_EQUAL_INT(r, zbw_decode_start_warning(fb, layout, &wb));
        if (r == ZBZ_ZCL_OK) {
            g_hit[HIT_WARNING]++;
            same_warning(&wa, &wb);
            zsrc_zb_zcl_t g;
            zbw_start_warning(&wa, layout, &g);
            memset(&wc, 0, sizeof(wc));
            TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbw_decode_start_warning(&g, layout, &wc));
            same_warning(&wa, &wc);
        }
    }

    zba_arm_t aa, ab;
    memset(&aa, 0, sizeof(aa));
    memset(&ab, 0, sizeof(ab));
    r = zba_decode_arm(fa, &aa);
    TEST_ASSERT_EQUAL_INT(r, zba_decode_arm(fb, &ab));
    if (r == ZBZ_ZCL_OK) {
        g_hit[HIT_ARM]++;
        TEST_ASSERT_EQUAL_UINT8(aa.mode, ab.mode);
        TEST_ASSERT_EQUAL(aa.code_bad, ab.code_bad);
        TEST_ASSERT_EQUAL_UINT8(aa.zone_id, ab.zone_id);
        TEST_ASSERT_TRUE(memchr(aa.code, 0, sizeof(aa.code)) != NULL); /* terminated */
        TEST_ASSERT_EQUAL_STRING(aa.code, ab.code);
        if (!aa.code_bad && aa.code[0] != '\0') {
            /* as sent: the bytes after the mode's length byte */
            TEST_ASSERT_TRUE(fa->len >= 2u + strlen(aa.code));
            SAME_MEM(&fa->payload[2], aa.code, strlen(aa.code));
        }
    }

    zba_status_t pa, pb, pc;
    memset(&pa, 0, sizeof(pa));
    memset(&pb, 0, sizeof(pb));
    r = zba_decode_panel_status(fa, &pa);
    TEST_ASSERT_EQUAL_INT(r, zba_decode_panel_status(fb, &pb));
    if (r == ZBZ_ZCL_OK) {
        g_hit[HIT_PANEL]++;
        same_status(&pa, &pb);
        zsrc_zb_zcl_t g;
        zba_panel_status_changed(&pa, &g);
        memset(&pc, 0, sizeof(pc));
        TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zba_decode_panel_status(&g, &pc));
        same_status(&pa, &pc);
    }

    zba_request_t qa, qb;
    memset(&qa, 0, sizeof(qa));
    memset(&qb, 0, sizeof(qb));
    r = zba_decode_request(fa, &qa);
    TEST_ASSERT_EQUAL_INT(r, zba_decode_request(fb, &qb));
    if (r == ZBZ_ZCL_OK) {
        g_hit[HIT_REQUEST]++;
        TEST_ASSERT_EQUAL_UINT8(qa.command, qb.command);
        TEST_ASSERT_TRUE(qa.command != ZBA_CMD_ARM && qa.command <= ZBA_CMD_GET_ZONE_STATUS);
        TEST_ASSERT_EQUAL_UINT8(qa.zone_id, qb.zone_id);
        TEST_ASSERT_EQUAL_UINT8(qa.max_zones, qb.max_zones);
        TEST_ASSERT_EQUAL(qa.mask_flag, qb.mask_flag);
        TEST_ASSERT_EQUAL_HEX16(qa.status_mask, qb.status_mask);
        TEST_ASSERT_EQUAL_UINT8(qa.count, qb.count);
        TEST_ASSERT_TRUE(qa.count <= fa->len);
        SAME_MEM(qa.zones, qb.zones, qa.count);
    }
}

/* One input, every way a frame is read. */
static void check_frame(const uint8_t *in, size_t len, uint16_t cluster)
{
    twin(in, len);
    static zsrc_zb_zcl_t fa, fb;
    zbz_zcl_result_t ra = zbz_zcl_decode(T.a, len, cluster, &fa);
    zbz_zcl_result_t rb = zbz_zcl_decode(T.b, len, cluster, &fb);
    TEST_ASSERT_EQUAL_INT(ra, rb);
    SAME_MEM(&fa, &fb, sizeof(fa));
    if (ra == ZBZ_ZCL_OK) {
        g_hit[HIT_FRAME]++;
        TEST_ASSERT_TRUE(fa.len <= ZSRC_ZB_ZCL_PAYLOAD_MAX);
        TEST_ASSERT_EQUAL_UINT16(cluster, fa.cluster);
        const size_t n = encode_guarded(enc_zcl, &fa, 64u);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(len, n, "a decoded frame encodes to another length");
        SAME_MEM_MSG(in, g_out, len, "a decoded frame encodes otherwise");
        poison_payload(&fa, 0x00u);
        poison_payload(&fb, 0xFFu);
        check_cluster(&fa, &fb);
    } else {
        TEST_ASSERT_EQUAL_UINT8(0u, fa.len);
    }

    ra = zbz_zcl_decode_global(T.a, len, cluster, &fa);
    rb = zbz_zcl_decode_global(T.b, len, cluster, &fb);
    TEST_ASSERT_EQUAL_INT(ra, rb);
    SAME_MEM(&fa, &fb, sizeof(fa));
    TEST_ASSERT_EQUAL(zbz_zcl_is_global(T.a, len), zbz_zcl_is_global(T.b, len));
    if (ra == ZBZ_ZCL_OK) {
        g_hit[HIT_GLOBAL]++;
        const size_t n = encode_guarded(enc_zcl_global, &fa, 64u);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(len, n, "a decoded global frame encodes to another length");
        SAME_MEM_MSG(in, g_out, len, "a decoded global frame encodes otherwise");
        poison_payload(&fa, 0x00u);
        poison_payload(&fb, 0xFFu);
        check_global(&fa, &fb);
    }
}

/* The seeds: golden frames of every kind the panel reads, as their encoders build them. */
static uint8_t g_seed_buf[24][64];
static const uint8_t *g_seeds[24];
static size_t g_seed_len[24];
static size_t g_nseeds;

static void add_seed(const zsrc_zb_zcl_t *f, bool global)
{
    TEST_ASSERT_TRUE(g_nseeds < 24u);
    const size_t n = global ? zbz_zcl_encode_global(f, 0x57u, g_seed_buf[g_nseeds], 64u)
                            : zbz_zcl_encode(f, 0x57u, g_seed_buf[g_nseeds], 64u);
    TEST_ASSERT_TRUE(n > 0u);
    g_seeds[g_nseeds] = g_seed_buf[g_nseeds];
    g_seed_len[g_nseeds] = n;
    g_nseeds++;
}

static void frame_seeds(void)
{
    g_nseeds = 0u;
    zsrc_zb_zcl_t f;
    /* IAS Zone: a status change (6 bytes), an enroll request, the CIE write, test mode */
    memset(&f, 0, sizeof(f));
    f.cluster = ZSRC_ZB_ZCL_IAS_ZONE;
    f.command = 0x00u;
    f.server_to_client = true;
    f.tsn = ZSRC_ZB_TSN_NEW;
    static const uint8_t status[] = {0x01, 0x00, 0x00, 0x05, 0x02, 0x00};
    memcpy(f.payload, status, sizeof(status));
    f.len = (uint8_t)sizeof(status);
    add_seed(&f, false);
    f.command = 0x01u;
    static const uint8_t enroll[] = {0x15, 0x00, 0x15, 0x10};
    memcpy(f.payload, enroll, sizeof(enroll));
    f.len = (uint8_t)sizeof(enroll);
    add_seed(&f, false);
    zbz_ias_cie_write(0x02ABCD0000000001ull, &f);
    add_seed(&f, true);
    zbz_ias_test_mode(60u, 0u, &f);
    add_seed(&f, false);
    /* a manufacturer's command */
    f.manufacturer = 0x115Fu;
    add_seed(&f, false);

    /* global: Read Attributes Response with a string, an invalid string and a refusal; a
     * report; Configure Reporting; Default Response; Write Attributes Response */
    zbz_zcl_global(ZSRC_ZB_ZCL_BASIC, ZBZ_ZCL_READ_ATTR_RSP, true, &f);
    static const uint8_t model[] = {'S', 'N', 'Z', 'B'};
    const zbz_zcl_attr_t m = {.attr = 0x0005u, .type = ZBZ_ZCL_TYPE_STRING, .len = 4, .value = model};
    const zbz_zcl_attr_t x = {.attr = 0x0004u, .status = ZBZ_ZCL_UNSUP_ATTRIBUTE};
    const zbz_zcl_attr_t p = {.attr = 0x0007u, .type = ZBZ_ZCL_TYPE_ENUM8, .len = 1, .value = model};
    TEST_ASSERT_TRUE(zbz_zcl_put_attr(&f, &m));
    TEST_ASSERT_TRUE(zbz_zcl_put_attr(&f, &x));
    TEST_ASSERT_TRUE(zbz_zcl_put_attr(&f, &p));
    add_seed(&f, true);
    static const uint8_t invalid_string[] = {0x04, 0x00, 0x00, 0x42, 0xFF};
    zbz_zcl_global(ZSRC_ZB_ZCL_BASIC, ZBZ_ZCL_READ_ATTR_RSP, true, &f);
    memcpy(f.payload, invalid_string, sizeof(invalid_string));
    f.len = (uint8_t)sizeof(invalid_string);
    add_seed(&f, true);
    zbz_zcl_global(ZSRC_ZB_ZCL_TEMP_MEAS, ZBZ_ZCL_REPORT_ATTR, true, &f);
    static const uint8_t t[] = {0x34, 0x08};
    const zbz_zcl_attr_t temp = {.attr = 0x0000u, .type = 0x29u, .len = 2, .value = t};
    TEST_ASSERT_TRUE(zbz_zcl_put_attr(&f, &temp));
    add_seed(&f, true);
    const zsrc_zb_reporting_t rep[2] = {
        {.cluster = ZSRC_ZB_ZCL_POWER_CFG, .attr = 0x0021u, .type = 0x20u, .min_s = 3600u,
         .max_s = 21600u, .change = 2u},
        {.cluster = ZSRC_ZB_ZCL_POWER_CFG, .attr = 0x0035u, .type = 0x18u, .min_s = 0u,
         .max_s = 3600u, .change = 0u}};
    TEST_ASSERT_TRUE(zbz_zcl_config_report(rep, 2u, &f));
    add_seed(&f, true);
    zbz_zcl_default_rsp(ZSRC_ZB_ZCL_IAS_WD, 0x00u, 0x81u, false, &f);
    add_seed(&f, true);
    zbz_zcl_global(ZSRC_ZB_ZCL_IAS_ZONE, ZBZ_ZCL_CONFIG_REPORT_RSP, true, &f);
    const zbz_zcl_attr_t all = {.attr = ZBZ_ZCL_ATTR_ALL};
    TEST_ASSERT_TRUE(zbz_zcl_put_attr(&f, &all));
    add_seed(&f, true);
    zbz_zcl_global(ZSRC_ZB_ZCL_POLL_CTRL, ZBZ_ZCL_READ_ATTR, false, &f);
    const zbz_zcl_attr_t rd = {.attr = 0x0000u};
    TEST_ASSERT_TRUE(zbz_zcl_put_attr(&f, &rd));
    add_seed(&f, true);

    /* IAS WD: a Start Warning (5 bytes), a Squawk */
    const zbw_warning_t w = {.mode = 1u, .strobe = true, .level = 2u, .duration_s = 240u,
                             .duty = 50u, .strobe_level = 1u};
    zbw_start_warning(&w, ZBW_LAYOUT_SPEC, &f);
    add_seed(&f, false);
    const zbw_squawk_t sq = {.mode = 0u, .strobe = true, .level = 1u};
    zbw_squawk(&sq, ZBW_LAYOUT_SPEC, &f);
    add_seed(&f, false);

    /* IAS ACE: an Arm with a code, a Bypass, Get Zone Status, a Panel Status Changed */
    memset(&f, 0, sizeof(f));
    f.cluster = ZSRC_ZB_ZCL_IAS_ACE;
    f.command = ZBA_CMD_ARM;
    f.tsn = ZSRC_ZB_TSN_NEW;
    static const uint8_t arm[] = {0x03, 0x04, '1', '1', '1', '1', 0x00};
    memcpy(f.payload, arm, sizeof(arm));
    f.len = (uint8_t)sizeof(arm);
    add_seed(&f, false);
    f.command = ZBA_CMD_BYPASS;
    static const uint8_t bypass[] = {0x02, 0x03, 0x05, 0x04, '1', '1', '1', '1'};
    memcpy(f.payload, bypass, sizeof(bypass));
    f.len = (uint8_t)sizeof(bypass);
    add_seed(&f, false);
    f.command = ZBA_CMD_GET_ZONE_STATUS;
    static const uint8_t zs[] = {0x01, 0x05, 0x01, 0x01, 0x00};
    memcpy(f.payload, zs, sizeof(zs));
    f.len = (uint8_t)sizeof(zs);
    add_seed(&f, false);
    const zba_status_t ps = {.status = 0x05u, .seconds = 30u, .audible = 1u, .alarm = 0u};
    zba_panel_status_changed(&ps, &f);
    add_seed(&f, false);
}

/* ZB-003, ZB-012, ZB-023, ZB-024: every ZCL decoder, 40 000 inputs from fixed seeds. */
static void test_zcl_frames_fuzz(void)
{
    frame_seeds();
    memset(g_hit, 0, sizeof(g_hit));
    static const uint64_t seeds[] = {0x5EED0001u, 0xC0FFEEu, 0x2B992DDFA23249D6ull, 7u};
    uint8_t in[64];
    for (size_t s = 0; s < sizeof(seeds) / sizeof(seeds[0]); s++) {
        seed(seeds[s]);
        for (unsigned i = 0; i < 10000u; i++) {
            const size_t len = make_input(in, sizeof(in), g_seeds, g_seed_len, g_nseeds);
            check_frame(in, len, CLUSTERS[rnd_below(sizeof(CLUSTERS) / sizeof(CLUSTERS[0]))]);
        }
    }
    /* every seed itself, on its own cluster and the others */
    for (size_t k = 0; k < g_nseeds; k++) {
        for (size_t c = 0; c < sizeof(CLUSTERS) / sizeof(CLUSTERS[0]); c++) {
            check_frame(g_seeds[k], g_seed_len[k], CLUSTERS[c]);
        }
    }
    /* every decoder got far enough often enough for its checks to mean something */
    for (unsigned h = 0; h < (unsigned)HIT_COUNT; h++) {
        char what[32];
        snprintf(what, sizeof(what), "decoder %u rarely decoded", h);
        TEST_ASSERT_TRUE_MESSAGE(g_hit[h] >= 20u, what);
    }
}

/* ZB-003, ZB-024 (found by the round trip above): a manufacturer-specific frame whose manufacturer
 * code is 0 is refused - zsrc_zb_zcl_t carries manufacturer 0 as a standard command, so a
 * manufacturer's command 0x00 of IAS Zone would have read as a Zone Status Change Notification,
 * and its 0x00 of IAS ACE as an Arm. */
static void test_manufacturer_frame_with_code_zero_refused(void)
{
    static const uint8_t status[] = {0x0D, 0x00, 0x00, 0x57, 0x00, 0x01, 0x00, 0x00, 0x05};
    zsrc_zb_zcl_t f;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_VALUE,
                          zbz_zcl_decode(status, sizeof(status), ZSRC_ZB_ZCL_IAS_ZONE, &f));
    TEST_ASSERT_EQUAL_UINT16(0u, f.manufacturer);
    TEST_ASSERT_EQUAL_UINT8(0u, f.len);
    static const uint8_t arm[] = {0x05, 0x00, 0x00, 0x57, 0x00, 0x03, 0x00, 0x00};
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_VALUE,
                          zbz_zcl_decode(arm, sizeof(arm), ZSRC_ZB_ZCL_IAS_ACE, &f));
    static const uint8_t read[] = {0x04, 0x00, 0x00, 0x57, 0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_VALUE,
                          zbz_zcl_decode_global(read, sizeof(read), ZSRC_ZB_ZCL_BASIC, &f));
    /* with a code it reads, keeps it, and never passes for the standard command */
    static const uint8_t ok[] = {0x0D, 0x5F, 0x11, 0x57, 0x00, 0x01, 0x00, 0x00, 0x05};
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode(ok, sizeof(ok), ZSRC_ZB_ZCL_IAS_ZONE, &f));
    TEST_ASSERT_EQUAL_HEX16(0x115Fu, f.manufacturer);
    zbz_ias_status_t s;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE, zbz_ias_decode_status_change(&f, &s));
}

/* ==== the stored blobs ==================================================================================== */

static zbz_device_t g_dev_a, g_dev_b;
static zbz_registry_t g_reg_a, g_reg_b;

static size_t enc_dev(const void *d, uint8_t *out, size_t cap)
{
    return zbz_dev_encode((const zbz_device_t *)d, out, cap);
}
static size_t enc_meta(const void *r, uint8_t *out, size_t cap)
{
    return zbz_meta_encode((const zbz_registry_t *)r, out, cap);
}

/* A device record: the same both ways; read, it encodes to a record that reads to the same. */
static void check_dev(const uint8_t *in, size_t len)
{
    twin(in, len);
    memset(&g_dev_a, 0, sizeof(g_dev_a));
    memset(&g_dev_b, 0, sizeof(g_dev_b));
    const zbz_reg_load_t ra = zbz_dev_decode(&g_dev_a, T.a, len);
    TEST_ASSERT_EQUAL_INT(ra, zbz_dev_decode(&g_dev_b, T.b, len));
    if (ra != ZBZ_REG_OK && ra != ZBZ_REG_FUTURE) {
        return;
    }
    static uint8_t e1[ZBZ_RECORD_MAX], e2[ZBZ_RECORD_MAX];
    const size_t n1 = encode_guarded(enc_dev, &g_dev_a, ZBZ_RECORD_MAX);
    memcpy(e1, g_out, n1);
    TEST_ASSERT_EQUAL_UINT(n1, encode_guarded(enc_dev, &g_dev_b, ZBZ_RECORD_MAX));
    SAME_MEM(e1, g_out, n1);
    if (n1 == 0u) {
        return;
    }
    zbz_device_t *again = &g_dev_b;
    memset(again, 0, sizeof(*again));
    TEST_ASSERT_EQUAL_INT(ZBZ_REG_OK, zbz_dev_decode(again, e1, n1));
    const size_t n2 = encode_guarded(enc_dev, again, ZBZ_RECORD_MAX);
    memcpy(e2, g_out, n2);
    TEST_ASSERT_EQUAL_UINT(n1, n2);
    SAME_MEM(e1, e2, n1);
    TEST_ASSERT_EQUAL_UINT16(g_dev_a.id, again->id);
    TEST_ASSERT_EQUAL_UINT64(g_dev_a.ieee, again->ieee);
}

static void check_meta(const uint8_t *in, size_t len)
{
    twin(in, len);
    zbz_reg_init(&g_reg_a);
    zbz_reg_init(&g_reg_b);
    const zbz_reg_load_t ra = zbz_meta_decode(&g_reg_a, T.a, len);
    TEST_ASSERT_EQUAL_INT(ra, zbz_meta_decode(&g_reg_b, T.b, len));
    if (ra == ZBZ_REG_OK) {
        const size_t n = encode_guarded(enc_meta, &g_reg_a, ZBZ_META_LEN);
        static uint8_t e[ZBZ_META_LEN];
        memcpy(e, g_out, n);
        TEST_ASSERT_EQUAL_UINT(n, encode_guarded(enc_meta, &g_reg_b, ZBZ_META_LEN));
        SAME_MEM(e, g_out, n);
    }
}

/* ZB-010, ZB-021: the registry's records and meta, mutated from the frozen v1 fixture and random:
 * read the same with any bytes after them, and what reads writes back to what reads the same. */
static void test_registry_records_fuzz(void)
{
    const uint8_t *seeds[3] = {ZBZ_REG_FIXTURE_V1_D00, ZBZ_REG_FIXTURE_V1_D01,
                               ZBZ_REG_FIXTURE_V1_TABLE[0]};
    const size_t lens[3] = {sizeof(ZBZ_REG_FIXTURE_V1_D00), sizeof(ZBZ_REG_FIXTURE_V1_D01),
                            sizeof(ZBZ_REG_FIXTURE_V1_TABLE[0])};
    /* the fixture itself first: every record reads, and the full one writes back byte for byte */
    check_dev(ZBZ_REG_FIXTURE_V1_D00, sizeof(ZBZ_REG_FIXTURE_V1_D00));
    TEST_ASSERT_EQUAL_UINT(sizeof(ZBZ_REG_FIXTURE_V1_D00),
                           encode_guarded(enc_dev, &g_dev_a, ZBZ_RECORD_MAX));
    SAME_MEM(ZBZ_REG_FIXTURE_V1_D00, g_out, sizeof(ZBZ_REG_FIXTURE_V1_D00));
    check_dev(ZBZ_REG_FIXTURE_V1_D01, sizeof(ZBZ_REG_FIXTURE_V1_D01));
    for (size_t i = 0; i < sizeof(ZBZ_REG_FIXTURE_V1_TABLE) / sizeof(ZBZ_REG_FIXTURE_V1_TABLE[0]); i++) {
        check_dev(ZBZ_REG_FIXTURE_V1_TABLE[i], sizeof(ZBZ_REG_FIXTURE_V1_TABLE[i]));
        TEST_ASSERT_EQUAL_UINT16((uint16_t)(i + 3u), g_dev_a.id);
    }
    check_meta(ZBZ_REG_FIXTURE_V1_META, sizeof(ZBZ_REG_FIXTURE_V1_META));
    TEST_ASSERT_EQUAL_UINT16(65u, g_reg_a.next_id);

    static uint8_t in[ZBZ_RECORD_MAX + 16u];
    seed(0xD15EA5Eu);
    for (unsigned i = 0; i < 30000u; i++) {
        const size_t len = make_input(in, sizeof(in), seeds, lens, 3u);
        check_dev(in, len);
    }
    const uint8_t *meta_seed[1] = {ZBZ_REG_FIXTURE_V1_META};
    const size_t meta_len[1] = {sizeof(ZBZ_REG_FIXTURE_V1_META)};
    for (unsigned i = 0; i < 5000u; i++) {
        const size_t len = make_input(in, 12u, meta_seed, meta_len, 1u);
        check_meta(in, len);
    }
}

static zbz_codes_t g_codes_a, g_codes_b;

static size_t enc_codes(const void *c, uint8_t *out, size_t cap)
{
    return zbz_codes_encode((const zbz_codes_t *)c, out, cap);
}

static void check_codes(const uint8_t *in, size_t len)
{
    twin(in, len);
    zbz_codes_init(&g_codes_a);
    zbz_codes_init(&g_codes_b);
    const bool ok = zbz_codes_decode(&g_codes_a, T.a, len);
    TEST_ASSERT_EQUAL(ok, zbz_codes_decode(&g_codes_b, T.b, len));
    TEST_ASSERT_EQUAL_UINT(g_codes_a.count, g_codes_b.count);
    TEST_ASSERT_TRUE(g_codes_a.count <= ZBZ_CODES_MAX);
    if (!ok) {
        TEST_ASSERT_EQUAL_UINT(0u, g_codes_a.count); /* false: empty */
        return;
    }
    static uint8_t e1[ZBZ_CODES_BLOB_MAX];
    const size_t n1 = encode_guarded(enc_codes, &g_codes_a, ZBZ_CODES_BLOB_MAX);
    memcpy(e1, g_out, n1);
    TEST_ASSERT_EQUAL_UINT(n1, encode_guarded(enc_codes, &g_codes_b, ZBZ_CODES_BLOB_MAX));
    SAME_MEM(e1, g_out, n1);
    zbz_codes_init(&g_codes_b);
    TEST_ASSERT_TRUE(zbz_codes_decode(&g_codes_b, e1, n1));
    TEST_ASSERT_EQUAL_UINT(g_codes_a.count, g_codes_b.count);
    TEST_ASSERT_EQUAL_UINT(n1, encode_guarded(enc_codes, &g_codes_b, ZBZ_CODES_BLOB_MAX));
    SAME_MEM(e1, g_out, n1);
    for (size_t k = 0; k < g_codes_a.count; k++) {
        TEST_ASSERT_TRUE(g_codes_a.codes[k].len <= ZBZ_CODE_MAX);
    }
}

static size_t enc_cfg(const void *c, uint8_t *out, size_t cap)
{
    return zbz_cfg_encode((const zbz_cfg_t *)c, out, cap);
}

static void check_cfg(const uint8_t *in, size_t len)
{
    twin(in, len);
    zbz_cfg_t a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    const bool ok = zbz_cfg_decode(&a, T.a, len);
    TEST_ASSERT_EQUAL(ok, zbz_cfg_decode(&b, T.b, len));
    TEST_ASSERT_EQUAL(a.channel_set, b.channel_set);
    TEST_ASSERT_EQUAL_UINT8(a.channel, b.channel);
    if (ok) {
        const size_t n = encode_guarded(enc_cfg, &a, ZBZ_CFG_BLOB);
        TEST_ASSERT_EQUAL_UINT(ZBZ_CFG_BLOB, n);
        zbz_cfg_t c;
        memset(&c, 0, sizeof(c));
        TEST_ASSERT_TRUE(zbz_cfg_decode(&c, g_out, n));
        TEST_ASSERT_EQUAL(a.channel_set, c.channel_set);
        TEST_ASSERT_EQUAL_UINT8(a.channel, c.channel);
    }
}

/* ZB-010, ZB-015, ZB-016: the codes waiting and the installer's settings, mutated from their frozen
 * fixtures and random. */
static void test_codes_and_cfg_blobs_fuzz(void)
{
    check_codes(CODES_FIXTURE_V1, sizeof(CODES_FIXTURE_V1));
    TEST_ASSERT_TRUE(g_codes_a.count > 0u);
    check_cfg(CFG_FIXTURE_V1_CHANNEL_20, sizeof(CFG_FIXTURE_V1_CHANNEL_20));
    check_cfg(CFG_FIXTURE_V1_DEFAULT, sizeof(CFG_FIXTURE_V1_DEFAULT));
    static uint8_t in[ZBZ_CODES_BLOB_MAX + 16u];
    const uint8_t *cs[1] = {CODES_FIXTURE_V1};
    const size_t cl[1] = {sizeof(CODES_FIXTURE_V1)};
    seed(0xC0DE5u);
    for (unsigned i = 0; i < 20000u; i++) {
        check_codes(in, make_input(in, sizeof(in), cs, cl, 1u));
    }
    const uint8_t *fs[2] = {CFG_FIXTURE_V1_CHANNEL_20, CFG_FIXTURE_V1_DEFAULT};
    const size_t fl[2] = {sizeof(CFG_FIXTURE_V1_CHANNEL_20), sizeof(CFG_FIXTURE_V1_DEFAULT)};
    for (unsigned i = 0; i < 5000u; i++) {
        check_cfg(in, make_input(in, 12u, fs, fl, 2u));
    }
}

static zbw_t g_wd_a, g_wd_b;
static zba_t g_ace_a, g_ace_b;

static size_t enc_wd(const void *w, uint8_t *out, size_t cap)
{
    return zbw_encode((const zbw_t *)w, out, cap);
}
static size_t enc_ace(const void *a, uint8_t *out, size_t cap)
{
    return zba_encode((const zba_t *)a, out, cap);
}

static void check_wd(const uint8_t *in, size_t len)
{
    static const zbw_port_t port = {0};
    twin(in, len);
    zbw_init(&g_wd_a, &port);
    zbw_init(&g_wd_b, &port);
    const bool ok = zbw_decode(&g_wd_a, T.a, len);
    TEST_ASSERT_EQUAL(ok, zbw_decode(&g_wd_b, T.b, len));
    static uint8_t e1[ZBW_BLOB_MAX];
    const size_t n1 = encode_guarded(enc_wd, &g_wd_a, ZBW_BLOB_MAX);
    memcpy(e1, g_out, n1);
    TEST_ASSERT_EQUAL_UINT(n1, encode_guarded(enc_wd, &g_wd_b, ZBW_BLOB_MAX));
    SAME_MEM(e1, g_out, n1);
    if (!ok) {
        return;
    }
    zbw_init(&g_wd_b, &port);
    TEST_ASSERT_TRUE(zbw_decode(&g_wd_b, e1, n1));
    TEST_ASSERT_EQUAL_UINT(n1, encode_guarded(enc_wd, &g_wd_b, ZBW_BLOB_MAX));
    SAME_MEM(e1, g_out, n1);
}

static void check_ace(const uint8_t *in, size_t len)
{
    static const zba_port_t port = {0};
    twin(in, len);
    zba_init(&g_ace_a, &port);
    zba_init(&g_ace_b, &port);
    const bool ok = zba_decode(&g_ace_a, T.a, len);
    TEST_ASSERT_EQUAL(ok, zba_decode(&g_ace_b, T.b, len));
    static uint8_t e1[ZBA_BLOB_MAX];
    const size_t n1 = encode_guarded(enc_ace, &g_ace_a, ZBA_BLOB_MAX);
    memcpy(e1, g_out, n1);
    TEST_ASSERT_EQUAL_UINT(n1, encode_guarded(enc_ace, &g_ace_b, ZBA_BLOB_MAX));
    SAME_MEM(e1, g_out, n1);
    if (!ok) {
        return;
    }
    zba_init(&g_ace_b, &port);
    TEST_ASSERT_TRUE(zba_decode(&g_ace_b, e1, n1));
    TEST_ASSERT_EQUAL_UINT(n1, encode_guarded(enc_ace, &g_ace_b, ZBA_BLOB_MAX));
    SAME_MEM(e1, g_out, n1);
}

/* ZB-023, ZB-024: the sirens' and keypads' stored assignments, mutated from their frozen fixtures
 * and random: all or nothing, the same with any bytes after them, written back the same. */
static void test_wd_and_ace_blobs_fuzz(void)
{
    check_wd(WD_FIXTURE_V1, sizeof(WD_FIXTURE_V1));
    check_wd(WD_FIXTURE_V2, sizeof(WD_FIXTURE_V2));
    TEST_ASSERT_TRUE(zbw_decode(&g_wd_a, WD_FIXTURE_V2, sizeof(WD_FIXTURE_V2)));
    TEST_ASSERT_EQUAL_UINT(sizeof(WD_FIXTURE_V2), encode_guarded(enc_wd, &g_wd_a, ZBW_BLOB_MAX));
    SAME_MEM(WD_FIXTURE_V2, g_out, sizeof(WD_FIXTURE_V2));
    check_ace(ACE_FIXTURE_V1, sizeof(ACE_FIXTURE_V1));
    TEST_ASSERT_TRUE(zba_decode(&g_ace_a, ACE_FIXTURE_V1, sizeof(ACE_FIXTURE_V1)));
    static uint8_t in[ZBA_BLOB_MAX + ZBW_BLOB_MAX + 16u];
    const uint8_t *ws[1] = {WD_FIXTURE_V1};
    const size_t wl[1] = {sizeof(WD_FIXTURE_V1)};
    const uint8_t *as[1] = {ACE_FIXTURE_V1};
    const size_t al[1] = {sizeof(ACE_FIXTURE_V1)};
    seed(0x51BE11u);
    for (unsigned i = 0; i < 20000u; i++) {
        check_wd(in, make_input(in, ZBW_BLOB_MAX + 16u, ws, wl, 1u));
        check_ace(in, make_input(in, ZBA_BLOB_MAX + 16u, as, al, 1u));
    }
}

/* ==== the install-code parsers ============================================================================= */

static void hex(char *out, const uint8_t *b, size_t n)
{
    static const char digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        out[2u * i] = digits[b[i] >> 4];
        out[2u * i + 1u] = digits[b[i] & 0x0Fu];
    }
    out[2u * n] = '\0';
}

/* Random text from the characters the parsers care about (and a few they don't). */
static size_t random_text(char *out, size_t cap)
{
    static const char alphabet[] = "0123456789abcdefABCDEF -:$%Z:I:0xXg\t";
    const size_t len = rnd_below(cap);
    for (size_t i = 0; i < len; i++) {
        out[i] = alphabet[rnd_below(sizeof(alphabet) - 1u)];
    }
    out[len] = '\0';
    return len;
}

/* A valid code of `n` bytes (6, 8, 12 or 16) with its CRC, as text in the QR form or bare. */
static size_t good_text(char *out, size_t cap, bool qr)
{
    static const uint8_t sizes[] = {6u, 8u, 12u, 16u};
    uint8_t code[ZBZ_CODE_MAX + 2u];
    const uint8_t n = sizes[rnd_below(sizeof(sizes))];
    for (uint8_t i = 0; i < n; i++) {
        code[i] = rnd_byte();
    }
    const uint16_t crc = zbz_code_crc(code, n);
    code[n] = (uint8_t)(crc & 0xFFu);
    code[n + 1u] = (uint8_t)(crc >> 8);
    char h[2u * (ZBZ_CODE_MAX + 2u) + 1u];
    hex(h, code, (size_t)n + 2u);
    const int w = qr ? snprintf(out, cap, "Z:02ABCD00%08X$I:%s", (unsigned)rnd64(), h)
                     : snprintf(out, cap, "%s", h);
    TEST_ASSERT_TRUE(w > 0 && (size_t)w < cap);
    return (size_t)w;
}

/* The text parsed with different bytes after its end; a parse that reads writes back the same. */
static void check_text(char *text, size_t len)
{
    static char a[160 + TAIL], b[160 + TAIL];
    memset(a, 0x00, sizeof(a));
    memset(b, 'F', sizeof(b));
    memcpy(a, text, len + 1u);
    memcpy(b, text, len + 1u);

    uint8_t ca[ZBZ_CODE_MAX + 4u], cb[ZBZ_CODE_MAX + 4u];
    uint8_t la = 0u, lb = 0u;
    memset(ca, 0xA5, sizeof(ca));
    memset(cb, 0xA5, sizeof(cb));
    zbz_code_result_t r = zbz_code_parse(a, ca, &la);
    TEST_ASSERT_EQUAL_INT(r, zbz_code_parse(b, cb, &lb));
    for (size_t i = ZBZ_CODE_MAX; i < sizeof(ca); i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xA5, ca[i], "written past ZBZ_CODE_MAX");
    }
    if (r == ZBZ_CODE_OK) {
        TEST_ASSERT_EQUAL_UINT8(la, lb);
        TEST_ASSERT_TRUE(la <= ZBZ_CODE_MAX);
        SAME_MEM(ca, cb, la);
        char h[2u * ZBZ_CODE_MAX + 1u];
        hex(h, ca, la);
        uint8_t cc[ZBZ_CODE_MAX];
        uint8_t lc = 0u;
        TEST_ASSERT_EQUAL_INT(ZBZ_CODE_OK, zbz_code_parse(h, cc, &lc));
        TEST_ASSERT_EQUAL_UINT8(la, lc);
        SAME_MEM(ca, cc, la);
    }

    uint64_t ia = 0u, ib = 0u;
    r = zbz_ieee_parse(a, &ia);
    TEST_ASSERT_EQUAL_INT(r, zbz_ieee_parse(b, &ib));
    if (r == ZBZ_CODE_OK) {
        TEST_ASSERT_EQUAL_UINT64(ia, ib);
        char h[24];
        snprintf(h, sizeof(h), "%08X%08X", (unsigned)(ia >> 32), (unsigned)(ia & 0xFFFFFFFFu));
        uint64_t ic = 0u;
        TEST_ASSERT_EQUAL_INT(ZBZ_CODE_OK, zbz_ieee_parse(h, &ic));
        TEST_ASSERT_EQUAL_UINT64(ia, ic);
    }

    memset(ca, 0xA5, sizeof(ca));
    memset(cb, 0xA5, sizeof(cb));
    r = zbz_code_parse_qr(a, &ia, ca, &la);
    TEST_ASSERT_EQUAL_INT(r, zbz_code_parse_qr(b, &ib, cb, &lb));
    for (size_t i = ZBZ_CODE_MAX; i < sizeof(ca); i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xA5, ca[i], "written past ZBZ_CODE_MAX");
    }
    if (r == ZBZ_CODE_OK) {
        TEST_ASSERT_EQUAL_UINT64(ia, ib);
        TEST_ASSERT_EQUAL_UINT8(la, lb);
        SAME_MEM(ca, cb, la);
        char h[2u * ZBZ_CODE_MAX + 1u];
        hex(h, ca, la);
        char q[96];
        snprintf(q, sizeof(q), "Z:%08X%08X$I:%s", (unsigned)(ia >> 32),
                 (unsigned)(ia & 0xFFFFFFFFu), h);
        uint64_t ic = 0u;
        uint8_t cc[ZBZ_CODE_MAX];
        uint8_t lc = 0u;
        TEST_ASSERT_EQUAL_INT(ZBZ_CODE_OK, zbz_code_parse_qr(q, &ic, cc, &lc));
        TEST_ASSERT_EQUAL_UINT64(ia, ic);
        TEST_ASSERT_EQUAL_UINT8(la, lc);
        SAME_MEM(ca, cc, la);
    }
}

/* ZB-015: the install-code, address and QR parsers against random text and against good codes
 * mutated character by character. */
static void test_code_parsers_fuzz(void)
{
    static char text[160];
    seed(0x1C0DE5u);
    unsigned good = 0u;
    for (unsigned i = 0; i < 30000u; i++) {
        size_t len;
        switch (rnd_below(3u)) {
        case 0: len = random_text(text, sizeof(text)); break;
        case 1: len = good_text(text, sizeof(text), rnd_below(2u) == 0u); break;
        default:
            len = good_text(text, sizeof(text), rnd_below(2u) == 0u);
            len = mutate((uint8_t *)text, len, sizeof(text) - 1u);
            for (size_t k = 0; k < len; k++) {
                text[k] = text[k] == '\0' ? '0' : text[k];
            }
            text[len] = '\0';
            break;
        }
        check_text(text, len);
        uint8_t c[ZBZ_CODE_MAX];
        uint8_t l = 0u;
        good += zbz_code_parse(text, c, &l) == ZBZ_CODE_OK ? 1u : 0u;
    }
    TEST_ASSERT_TRUE(good > 1000u); /* the round trips were exercised */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_zcl_frames_fuzz);
    RUN_TEST(test_manufacturer_frame_with_code_zero_refused);
    RUN_TEST(test_registry_records_fuzz);
    RUN_TEST(test_codes_and_cfg_blobs_fuzz);
    RUN_TEST(test_wd_and_ace_blobs_fuzz);
    RUN_TEST(test_code_parsers_fuzz);
    return UNITY_END();
}
