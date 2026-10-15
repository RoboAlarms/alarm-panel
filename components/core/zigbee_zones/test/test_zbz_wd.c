/*
 * test_zbz_wd.c - Zigbee sirens and strobes as the panel's bell (zbz_wd.h; docs/features/20
 * alarm-devices.md §2 and §4, testing.md §4 and §12). Every test proves ZB-023.
 *
 * Where the bytes come from, per vector (cross-checked against the open projects on 2026-10-03,
 * BL-143; the links are permalinks to the commits named):
 * - "spec": mode in the high nibble. Start Warning is Warning Info (mode bits 7-4, strobe bits
 *   3-2 where a set bit 2 is "use strobe", siren level bits 1-0) | duration u16 | strobe duty
 *   cycle u8 | strobe level u8; Squawk is one byte, mode bits 7-4, strobe bit 3, level bits 1-0.
 *   zigpy's tests build exactly these (raw = mode << 4 | strobe << 2 | level; squawk mode << 4 |
 *   strobe << 3 | level), Wireshark's dissector masks them (0xF0 and 0x0C; squawk 0xF0, 0x08,
 *   0x03), and zigpy/zha, deCONZ's SIRZB-111 file, a SmartThings handler (13: burglar, no
 *   strobe, very high) and zigbee-herdsman-converters' `warning` (all models but those named
 *   below) send them so. The ZCL PDF itself (07-5123-08) is over the fetch tool's 10 MB limit and
 *   was not read: this is what its implementers read in it.
 * - "reversed": the same fields read from the low bit up: mode bits 3-0, strobe from bit 4 (bits
 *   5-4 in Warning Info, where 1 is "use strobe"; bit 4 in Squawk with bit 5 reserved), level
 *   bits 7-6, for Warning Info and Squawk alike. NXP's MCUXpresso SDK Zigbee guide (JNUG3132,
 *   IAS WD cluster) documents exactly this for both bytes. zigbee-herdsman-converters packs
 *   `warning` so for SIRZB-110, SRAC-23B-ZBSR, AV2010/29A and AV2010/24A (SIRZB-111 gets the
 *   spec layout, in `warning` and in `warning_simple`), and its `squawk` converter packs
 *   state + (strobe << 4) + (level << 6) for EVERY model.
 * - "herdsman": Koenkk/zigbee-herdsman-converters at 82f3b9115814 (2026-10-03),
 *   https://github.com/Koenkk/zigbee-herdsman-converters/blob/82f3b9115814/src/converters/toZigbee.ts
 * - "zigpy": zigpy/zigpy at fb63170d520e (2026-09-29), tests/test_zcl_clusters.py,
 *   https://github.com/zigpy/zigpy/blob/fb63170d520e/tests/test_zcl_clusters.py
 * - "zha": zigpy/zha at f3eb54c0fa42 (2026-10-01), tests/test_siren.py,
 *   https://github.com/zigpy/zha/blob/f3eb54c0fa42/tests/test_siren.py
 * - "deconz": dresden-elektronik/deconz-rest-plugin at a4c17adfa04a (2026-09-10),
 *   https://github.com/dresden-elektronik/deconz-rest-plugin/blob/a4c17adfa04a/devices/frient/sirzb-111_siren.json
 * - "wireshark": wireshark/wireshark at b9c42e6480e1 (2026-10-02),
 *   https://github.com/wireshark/wireshark/blob/b9c42e6480e1/epan/dissectors/packet-zbee-zcl-sas.c
 * - "nxp": https://mcuxpresso.nxp.com/mcuxsdk/25.09.00/html/middleware/wireless/zigbee/Docs/JNUG3132/IAS_WD_cluster/topics/ias_warning_device_cluster.html
 * The header byte 0x57 is only the sequence number of testing.md's tables. Addresses are
 * fictional and locally administered (0x02AB_CD00_0000_0001).
 */
#include <stdint.h>
#include <string.h>

#include "unity.h"
#include "wd_fixture_v1.h"
#include "zigbee_zones/zbz_models.h"
#include "zigbee_zones/zbz_wd.h"

#define IEEE_1 0x02ABCD0000000001ull
#define IEEE_2 0x02ABCD0000000002ull
#define IEEE_3 0x02ABCD0000000003ull
#define IEEE_4 0x02ABCD0000000004ull
#define IEEE_5 0x02ABCD0000000005ull

/* ---- the fake radio ---------------------------------------------------------------------------- */

#define SENT_MAX 64u

typedef struct {
    uint64_t ieee;
    uint8_t endpoint;
    zsrc_zb_zcl_t cmd;
    uint32_t tag;
} sent_t;

typedef struct {
    sent_t sent[SENT_MAX];
    unsigned count;
    bool refuse;          /* send answers false */
    unsigned trouble_calls;
    bool trouble;
    unsigned checkins;
} fake_t;

static fake_t fake;

static bool fake_send(void *ctx, uint64_t ieee, uint8_t endpoint, const zsrc_zb_zcl_t *cmd,
                      uint32_t tag)
{
    fake_t *f = ctx;
    if (f->refuse) {
        return false;
    }
    TEST_ASSERT_TRUE(f->count < SENT_MAX);
    sent_t *s = &f->sent[f->count++];
    s->ieee = ieee;
    s->endpoint = endpoint;
    s->cmd = *cmd;
    s->tag = tag;
    return true;
}

static void fake_trouble(void *ctx, bool active)
{
    fake_t *f = ctx;
    f->trouble_calls++;
    f->trouble = active;
}

static void fake_checkin(void *ctx, uint8_t device)
{
    (void)device;
    ((fake_t *)ctx)->checkins++;
}

static zbw_t wd;

void setUp(void)
{
    memset(&fake, 0, sizeof(fake));
    zbw_port_t port = {.ctx = &fake,
                       .send = fake_send,
                       .bell_trouble = fake_trouble,
                       .checkin = fake_checkin};
    zbw_init(&wd, &port);
}

void tearDown(void)
{
    TEST_ASSERT_EQUAL_UINT(0, fake.checkins); /* alarm-devices.md §4: never called */
}

static zbw_assign_t siren(uint16_t id, uint64_t ieee, uint8_t partitions, bool burglary,
                          bool fire)
{
    zbw_assign_t a;
    memset(&a, 0, sizeof(a));
    a.id = id;
    a.ieee = ieee;
    a.endpoint = 43u;
    a.partitions = partitions;
    a.burglary = burglary;
    a.fire = fire;
    a.volume = ZBW_LEVEL_HIGH;
    return a;
}

static const sent_t *last(void)
{
    TEST_ASSERT_TRUE(fake.count > 0u);
    return &fake.sent[fake.count - 1u];
}

/* The last frame sent to `ieee`, decoded; NULL when none was. */
static const sent_t *last_to(uint64_t ieee)
{
    for (unsigned i = fake.count; i > 0u; i--) {
        if (fake.sent[i - 1u].ieee == ieee) {
            return &fake.sent[i - 1u];
        }
    }
    return NULL;
}

static unsigned sends_to(uint64_t ieee)
{
    unsigned n = 0;
    for (unsigned i = 0; i < fake.count; i++) {
        n += fake.sent[i].ieee == ieee ? 1u : 0u;
    }
    return n;
}

static zbw_warning_t warning_of(const sent_t *s, uint8_t layout)
{
    zbw_warning_t w;
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbw_decode_start_warning(&s->cmd, layout, &w));
    return w;
}

static void encodes_to(const zsrc_zb_zcl_t *f, const uint8_t *b, size_t n)
{
    uint8_t out[ZBZ_ZCL_FRAME_MAX];
    size_t m = zbz_zcl_encode(f, 0x57u, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(n, m);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(b, out, n);
}

/* ---- the bits (testing.md §4) ------------------------------------------------------------------ */

/* ZB-023: all 256 Warning Info bytes decode and encode back to themselves, in both layouts; the
 * fields sit where each layout puts them. */
static void test_warning_info_every_byte(void)
{
    for (unsigned layout = 0; layout <= 1u; layout++) {
        for (unsigned b = 0; b < 256u; b++) {
            zbw_warning_t w;
            zbw_warning_info_decode((uint8_t)b, (uint8_t)layout, &w);
            TEST_ASSERT_EQUAL_HEX8(b, zbw_warning_info(&w, (uint8_t)layout));
            if (layout == ZBW_LAYOUT_SPEC) {
                TEST_ASSERT_EQUAL_UINT8(b >> 4, w.mode);
                TEST_ASSERT_EQUAL((b & 0x04u) != 0u, w.strobe);
                TEST_ASSERT_EQUAL_UINT8(b & 0x03u, w.level);
            } else {
                TEST_ASSERT_EQUAL_UINT8(b & 0x0Fu, w.mode);
                TEST_ASSERT_EQUAL((b & 0x10u) != 0u, w.strobe);
                TEST_ASSERT_EQUAL_UINT8(b >> 6, w.level);
            }
        }
    }
    /* "herdsman" (`warning` for SIRZB-110 and the other reversed models): mode + (strobe << 4) +
     * (level << 6), so burglar + strobe + high reversed is 1 + 16 + 128 = 0x91; the same warning
     * in the spec layout is 0x16 ("zigpy": raw = mode << 4 | strobe << 2 | level). */
    zbw_warning_t w = {.mode = ZBW_MODE_BURGLAR, .strobe = true, .level = ZBW_LEVEL_HIGH};
    TEST_ASSERT_EQUAL_HEX8(0x16, zbw_warning_info(&w, ZBW_LAYOUT_SPEC));
    TEST_ASSERT_EQUAL_HEX8(0x91, zbw_warning_info(&w, ZBW_LAYOUT_REVERSED));
    /* "zha": a basic siren, burglar, no strobe, high is 18 (tests/test_siren.py asserts
     * warning == 18 for its default turn_on). */
    zbw_warning_t basic = {.mode = ZBW_MODE_BURGLAR, .level = ZBW_LEVEL_HIGH};
    TEST_ASSERT_EQUAL_UINT8(18, zbw_warning_info(&basic, ZBW_LAYOUT_SPEC));
    /* A built warning never sets a reserved bit; a mode past 15 is cut to its nibble. */
    zbw_warning_t big = {.mode = 0x1Fu, .level = 7u};
    TEST_ASSERT_EQUAL_HEX8(0xF3, zbw_warning_info(&big, ZBW_LAYOUT_SPEC));
}

/* ZB-023: all 256 Squawk bytes too; "herdsman"/"spec" goldens of testing.md §4. */
static void test_squawk_every_byte(void)
{
    for (unsigned layout = 0; layout <= 1u; layout++) {
        for (unsigned b = 0; b < 256u; b++) {
            zbw_squawk_t s;
            zbw_squawk_decode((uint8_t)b, (uint8_t)layout, &s);
            TEST_ASSERT_EQUAL_HEX8(b, zbw_squawk_byte(&s, (uint8_t)layout));
        }
    }
    zbw_squawk_t s;
    zbw_squawk_decode(0x1A, ZBW_LAYOUT_SPEC, &s);
    TEST_ASSERT_EQUAL_UINT8(ZBW_SQUAWK_DISARMED, s.mode);
    TEST_ASSERT_TRUE(s.strobe);
    TEST_ASSERT_EQUAL_UINT8(ZBW_LEVEL_HIGH, s.level);
    TEST_ASSERT_EQUAL_HEX8(0, s.reserved);
}

/* ZB-023: Start Warning golden frames ("spec", testing.md §4), both ways. */
static void test_golden_start_warning(void)
{
    zbw_warning_t w = {.mode = ZBW_MODE_BURGLAR,
                       .strobe = true,
                       .level = ZBW_LEVEL_HIGH,
                       .duration_s = 60u,
                       .duty = 50u,
                       .strobe_level = ZBW_LEVEL_HIGH};
    zsrc_zb_zcl_t f;
    zbw_start_warning(&w, ZBW_LAYOUT_SPEC, &f);
    static const uint8_t spec[] = {0x01, 0x57, 0x00, 0x16, 0x3C, 0x00, 0x32, 0x02};
    encodes_to(&f, spec, sizeof(spec));
    TEST_ASSERT_EQUAL_HEX16(ZSRC_ZB_ZCL_IAS_WD, f.cluster);
    TEST_ASSERT_TRUE(f.want_response);

    /* The same warning in the reversed layout ("herdsman"). */
    zbw_start_warning(&w, ZBW_LAYOUT_REVERSED, &f);
    static const uint8_t rev[] = {0x01, 0x57, 0x00, 0x91, 0x3C, 0x00, 0x32, 0x02};
    encodes_to(&f, rev, sizeof(rev));

    /* Decoded back from the bytes. */
    zsrc_zb_zcl_t in;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode(spec, sizeof(spec), ZSRC_ZB_ZCL_IAS_WD, &in));
    zbw_warning_t got;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbw_decode_start_warning(&in, ZBW_LAYOUT_SPEC, &got));
    TEST_ASSERT_EQUAL_MEMORY(&w, &got, sizeof(w));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode(rev, sizeof(rev), ZSRC_ZB_ZCL_IAS_WD, &in));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbw_decode_start_warning(&in, ZBW_LAYOUT_REVERSED, &got));
    TEST_ASSERT_EQUAL_MEMORY(&w, &got, sizeof(w));

    /* Stop: mode 0, level high ("spec", testing.md §4: 02; "zha" asserts warning == 2 for every
     * turn_off). */
    zbw_warning_t stop = {.mode = ZBW_MODE_STOP, .level = ZBW_LEVEL_HIGH};
    zbw_start_warning(&stop, ZBW_LAYOUT_SPEC, &f);
    static const uint8_t stop_b[] = {0x01, 0x57, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00};
    encodes_to(&f, stop_b, sizeof(stop_b));

    /* "deconz": the SIRZB-111 file's payload reads as burglar, strobe, very high, 1 s. verified
     * (bytes): the file's "eval" sends '1701000000' and its note describes byte 1 as the options
     * (0x17 is warning mode 1 burglar, strobe, very high sound), bytes 2-3 the duration, byte 4
     * the strobe duty cycle and byte 5 the strobe level. */
    static const uint8_t dz[] = {0x01, 0x57, 0x00, 0x17, 0x01, 0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbz_zcl_decode(dz, sizeof(dz), ZSRC_ZB_ZCL_IAS_WD, &in));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbw_decode_start_warning(&in, ZBW_LAYOUT_SPEC, &got));
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_BURGLAR, got.mode);
    TEST_ASSERT_TRUE(got.strobe);
    TEST_ASSERT_EQUAL_UINT8(ZBW_LEVEL_VERY_HIGH, got.level);
    TEST_ASSERT_EQUAL_UINT16(1, got.duration_s);
}

/* ZB-023: Squawk golden frames ("spec", "herdsman"; testing.md §4). */
static void test_golden_squawk(void)
{
    zsrc_zb_zcl_t f;
    zbw_squawk_t armed = {.mode = ZBW_SQUAWK_ARMED, .strobe = false, .level = ZBW_LEVEL_VERY_HIGH};
    zbw_squawk(&armed, ZBW_LAYOUT_SPEC, &f);
    static const uint8_t a[] = {0x01, 0x57, 0x01, 0x03};
    encodes_to(&f, a, sizeof(a));
    zbw_squawk_t dis = {.mode = ZBW_SQUAWK_DISARMED, .strobe = true, .level = ZBW_LEVEL_HIGH};
    zbw_squawk(&dis, ZBW_LAYOUT_SPEC, &f);
    static const uint8_t d[] = {0x01, 0x57, 0x01, 0x1A};
    encodes_to(&f, d, sizeof(d));
    /* reversed: mode 1 | strobe bit 4 | high << 6. Sourced (BL-143, 2026-10-03): zigbee-herdsman-
     * converters' `squawk` converter packs state + (strobe << 4) + (level << 6) for every model,
     * so disarmed + strobe + high is 1 + 16 + 128 = 0x91, and NXP's JNUG3132 page documents the
     * Squawk byte as mode bits 0-3, strobe bit 4, reserved bit 5, level bits 6-7. This replaces
     * the earlier note that herdsman packs the Squawk in the spec order: it does not (it is the
     * Warning Info that herdsman packs in the spec order for most models). The bench capture still
     * decides which siren wants which (alarm-devices.md §6, question 1). */
    zbw_squawk(&dis, ZBW_LAYOUT_REVERSED, &f);
    static const uint8_t r[] = {0x01, 0x57, 0x01, 0x91};
    encodes_to(&f, r, sizeof(r));
}

/* ZB-023 (BL-143, cross-checked 2026-10-03): the Warning Info and Squawk bytes the open projects'
 * own tests and converters hold, read through the layout each project uses (the header names the
 * commits). They are facts copied with their source, no code. */
static void test_project_bytes(void)
{
    zbw_warning_t w;
    zbw_squawk_t s;

    /* "zigpy" test_security_iaswd_warning_mode: raw 0x00..0x03 are mode 0 (Stop) and raw
     * 0x30..0x33 mode 3 (Emergency), the mode being the high nibble; its _warning_siren test
     * reads the siren level from the low two bits; test_security_iaswd_squawk_mode has raw
     * 0x00..0x03 Armed and 0x10..0x13 Disarmed.
     * https://github.com/zigpy/zigpy/blob/fb63170d520e/tests/test_zcl_clusters.py */
    for (unsigned lvl = 0; lvl <= 3u; lvl++) {
        zbw_warning_info_decode((uint8_t)lvl, ZBW_LAYOUT_SPEC, &w);
        TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_STOP, w.mode);
        TEST_ASSERT_EQUAL_UINT8(lvl, w.level);
        zbw_warning_info_decode((uint8_t)(0x30u + lvl), ZBW_LAYOUT_SPEC, &w);
        TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_EMERGENCY, w.mode);
        TEST_ASSERT_EQUAL_UINT8(lvl, w.level);
        TEST_ASSERT_FALSE(w.strobe);
        zbw_squawk_decode((uint8_t)lvl, ZBW_LAYOUT_SPEC, &s);
        TEST_ASSERT_EQUAL_UINT8(ZBW_SQUAWK_ARMED, s.mode);
        TEST_ASSERT_EQUAL_UINT8(lvl, s.level);
        zbw_squawk_decode((uint8_t)(0x10u + lvl), ZBW_LAYOUT_SPEC, &s);
        TEST_ASSERT_EQUAL_UINT8(ZBW_SQUAWK_DISARMED, s.mode);
        TEST_ASSERT_EQUAL_UINT8(lvl, s.level);
    }
    /* Its _warning_strobe test sets the strobe as `strobe << 2`, _squawk_strobe as `strobe << 3`.
     */
    zbw_warning_info_decode(0x04, ZBW_LAYOUT_SPEC, &w);
    TEST_ASSERT_TRUE(w.strobe);
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_STOP, w.mode);
    TEST_ASSERT_EQUAL_UINT8(ZBW_LEVEL_LOW, w.level);
    zbw_squawk_decode(0x08, ZBW_LAYOUT_SPEC, &s);
    TEST_ASSERT_TRUE(s.strobe);
    TEST_ASSERT_EQUAL_UINT8(ZBW_SQUAWK_ARMED, s.mode);

    /* "zha" tests/test_siren.py: the Warning Info of a default turn_on is 50 (emergency, no
     * strobe, high), with volume_level 3 and tone 3 it is 51 (very high), a basic siren's is 18
     * (burglar, high) and every turn_off sends 2 (stop, high); strobe duty cycle 0, strobe level 2.
     * https://github.com/zigpy/zha/blob/f3eb54c0fa42/tests/test_siren.py */
    static const struct {
        uint8_t info, mode, level;
    } zha[] = {
        {50, ZBW_MODE_EMERGENCY, ZBW_LEVEL_HIGH}, {51, ZBW_MODE_EMERGENCY, ZBW_LEVEL_VERY_HIGH},
        {18, ZBW_MODE_BURGLAR, ZBW_LEVEL_HIGH},   {2, ZBW_MODE_STOP, ZBW_LEVEL_HIGH},
    };
    for (size_t i = 0; i < sizeof(zha) / sizeof(zha[0]); i++) {
        zbw_warning_info_decode(zha[i].info, ZBW_LAYOUT_SPEC, &w);
        TEST_ASSERT_EQUAL_UINT8(zha[i].mode, w.mode);
        TEST_ASSERT_EQUAL_UINT8(zha[i].level, w.level);
        TEST_ASSERT_FALSE(w.strobe);
        const zbw_warning_t built = {.mode = zha[i].mode, .level = zha[i].level};
        TEST_ASSERT_EQUAL_UINT8(zha[i].info, zbw_warning_info(&built, ZBW_LAYOUT_SPEC));
    }

    /* A SmartThings handler's Start Warning "13": burglar (0b0001), strobe off (0b00), siren very
     * high (0b11) in the spec layout, followed by the duration and two zero bytes.
     * https://community.smartthings.com/t/zigbee-siren-payload/151576 */
    zbw_warning_info_decode(0x13, ZBW_LAYOUT_SPEC, &w);
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_BURGLAR, w.mode);
    TEST_ASSERT_FALSE(w.strobe);
    TEST_ASSERT_EQUAL_UINT8(ZBW_LEVEL_VERY_HIGH, w.level);

    /* "herdsman" `warning_simple` for the Frient/Develco sirens: SIRZB-111 sends
     * (3 << 4) + (1 << 2) + 3 = 0x37 (emergency, strobe, very high: the spec layout), SIRZB-110
     * sends 3 + (1 << 4) + (3 << 6) = 0xD3 (the same warning in the reversed layout), and both
     * send 0 to stop.
     * https://github.com/Koenkk/zigbee-herdsman-converters/blob/82f3b9115814/src/converters/toZigbee.ts */
    zbw_warning_info_decode(0x37, ZBW_LAYOUT_SPEC, &w);
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_EMERGENCY, w.mode);
    TEST_ASSERT_TRUE(w.strobe);
    TEST_ASSERT_EQUAL_UINT8(ZBW_LEVEL_VERY_HIGH, w.level);
    TEST_ASSERT_EQUAL_HEX8(0x37, zbw_warning_info(&w, ZBW_LAYOUT_SPEC));
    zbw_warning_info_decode(0xD3, ZBW_LAYOUT_REVERSED, &w);
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_EMERGENCY, w.mode);
    TEST_ASSERT_TRUE(w.strobe);
    TEST_ASSERT_EQUAL_UINT8(ZBW_LEVEL_VERY_HIGH, w.level);
    TEST_ASSERT_EQUAL_HEX8(0xD3, zbw_warning_info(&w, ZBW_LAYOUT_REVERSED));
    zbw_warning_info_decode(0x00, ZBW_LAYOUT_SPEC, &w);
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_STOP, w.mode);
    zbw_warning_info_decode(0x00, ZBW_LAYOUT_REVERSED, &w);
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_STOP, w.mode);

    /* "herdsman" `squawk`, for every model: state + (strobe << 4) + (level << 6), state 0 armed
     * and 1 disarmed - the reversed layout, which the reversed decoder reads back field for field.
     * The same file's `warning` packs the reversed layout as mode + (strobe << 4) + (level << 6)
     * for SIRZB-110, SRAC-23B-ZBSR, AV2010/29A and AV2010/24A, and the spec layout otherwise.
     * https://github.com/Koenkk/zigbee-herdsman-converters/blob/82f3b9115814/src/converters/toZigbee.ts */
    for (unsigned state = 0; state <= 1u; state++) {
        for (unsigned strobe = 0; strobe <= 1u; strobe++) {
            for (unsigned lvl = 0; lvl <= 3u; lvl++) {
                const uint8_t byte = (uint8_t)(state + (strobe << 4) + (lvl << 6));
                zbw_squawk_decode(byte, ZBW_LAYOUT_REVERSED, &s);
                TEST_ASSERT_EQUAL_UINT8(state, s.mode);
                TEST_ASSERT_EQUAL((strobe != 0u), s.strobe);
                TEST_ASSERT_EQUAL_UINT8(lvl, s.level);
                TEST_ASSERT_EQUAL_UINT8(0, s.reserved);
                TEST_ASSERT_EQUAL_HEX8(byte, zbw_squawk_byte(&s, ZBW_LAYOUT_REVERSED));
            }
        }
    }
}

/* ZB-023: a Start Warning that isn't one, or is cut, or out of range. */
static void test_decode_refusals(void)
{
    zbw_warning_t w = {.mode = ZBW_MODE_FIRE, .duration_s = 5u};
    zsrc_zb_zcl_t f;
    zbw_warning_t got;
    zbw_start_warning(&w, ZBW_LAYOUT_SPEC, &f);
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbw_decode_start_warning(&f, ZBW_LAYOUT_SPEC, &got));

    zsrc_zb_zcl_t bad = f;
    bad.cluster = ZSRC_ZB_ZCL_IAS_ZONE;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE, zbw_decode_start_warning(&bad, 0, &got));
    bad = f;
    bad.command = ZBW_CMD_SQUAWK;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE, zbw_decode_start_warning(&bad, 0, &got));
    bad = f;
    bad.server_to_client = true;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE, zbw_decode_start_warning(&bad, 0, &got));
    bad = f;
    bad.manufacturer = 0x1015u;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_TYPE, zbw_decode_start_warning(&bad, 0, &got));
    bad = f;
    bad.len = 2u;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, zbw_decode_start_warning(&bad, 0, &got));
    bad.len = ZSRC_ZB_ZCL_PAYLOAD_MAX + 1u;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_LONG, zbw_decode_start_warning(&bad, 0, &got));
    /* An older sender's three bytes: no duty cycle or strobe level. */
    bad = f;
    bad.len = 3u;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_OK, zbw_decode_start_warning(&bad, 0, &got));
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_FIRE, got.mode);
    TEST_ASSERT_EQUAL_UINT16(5, got.duration_s);
    TEST_ASSERT_EQUAL_UINT8(0, got.duty);
    bad = f;
    bad.payload[3] = 101u;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_VALUE, zbw_decode_start_warning(&bad, 0, &got));
    bad = f;
    bad.payload[4] = 4u;
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_VALUE, zbw_decode_start_warning(&bad, 0, &got));
    TEST_ASSERT_EQUAL_INT(ZBZ_ZCL_ERR_SHORT, zbw_decode_start_warning(NULL, 0, &got));
    /* A duty cycle past 100 is written as 100. */
    w.duty = 200u;
    zbw_start_warning(&w, ZBW_LAYOUT_SPEC, &f);
    TEST_ASSERT_EQUAL_UINT8(100, f.payload[3]);
}

/* ZB-023 (testing.md §12): the cluster's numbers against an embedded table of Espressif's ezbee
 * ias_wd.h and zigpy's security.py - command ids, warning modes, levels, squawk modes, the
 * MaxDuration attribute and its default 0x00F0. */
static void test_constants(void)
{
    static const struct {
        unsigned ours, theirs;
    } table[] = {
        {ZSRC_ZB_ZCL_IAS_WD, 0x0502u},  {ZBW_CMD_START_WARNING, 0x00u}, {ZBW_CMD_SQUAWK, 0x01u},
        {ZBW_ATTR_MAX_DURATION, 0x0000u}, {ZBW_MAX_DURATION_DEF, 0x00F0u},
        {ZBW_MODE_STOP, 0u},           {ZBW_MODE_BURGLAR, 1u},          {ZBW_MODE_FIRE, 2u},
        {ZBW_MODE_EMERGENCY, 3u},      {ZBW_MODE_POLICE_PANIC, 4u},     {ZBW_MODE_FIRE_PANIC, 5u},
        {ZBW_MODE_EMERGENCY_PANIC, 6u}, {ZBW_LEVEL_LOW, 0u},            {ZBW_LEVEL_MEDIUM, 1u},
        {ZBW_LEVEL_HIGH, 2u},          {ZBW_LEVEL_VERY_HIGH, 3u},       {ZBW_SQUAWK_ARMED, 0u},
        {ZBW_SQUAWK_DISARMED, 1u},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        TEST_ASSERT_EQUAL_UINT(table[i].theirs, table[i].ours);
    }
}

/* ZB-023: the warning mode for every engine pattern, and on a one-tone model. */
static void test_mode_for(void)
{
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_STOP, zbw_mode_for(ALARM_PAT_OFF, 0));
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_BURGLAR, zbw_mode_for(ALARM_PAT_STEADY, 0));
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_FIRE, zbw_mode_for(ALARM_PAT_TEMPORAL3, 0));
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_EMERGENCY, zbw_mode_for(ALARM_PAT_TEMPORAL4, 0));
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_BURGLAR, zbw_mode_for(ALARM_PAT_TEST, 0));
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_BURGLAR, zbw_mode_for(ALARM_PAT_ON, 0));
    const uint16_t one = ZBZ_MODEL_WD_ONE_TONE;
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_STOP, zbw_mode_for(ALARM_PAT_OFF, one));
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_EMERGENCY, zbw_mode_for(ALARM_PAT_STEADY, one));
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_EMERGENCY, zbw_mode_for(ALARM_PAT_TEMPORAL3, one));
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_EMERGENCY, zbw_mode_for(ALARM_PAT_TEMPORAL4, one));
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_EMERGENCY, zbw_mode_for(ALARM_PAT_TEST, one));
}

/* ---- the policy -------------------------------------------------------------------------------- */

/* Four sirens: 1 burglary on partition 1, 2 fire on partition 1, 3 both on partition 2, 4 both on
 * partitions 1 and 2 and one-tone. The radio is up from time 0. */
static void four_sirens(void)
{
    zbw_assign_t a = siren(1, IEEE_1, 0x01, true, false);
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
    a = siren(2, IEEE_2, 0x01, false, true);
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
    a = siren(3, IEEE_3, 0x02, true, true);
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
    a = siren(4, IEEE_4, 0x03, true, true);
    a.model_flags = ZBZ_MODEL_WD_ONE_TONE;
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
    zbw_radio(&wd, 0, true);
    TEST_ASSERT_EQUAL_UINT(0, fake.count); /* quiet: nothing to say */
}

/* The mode a siren gives now, 0xFF quiet. */
static unsigned mode_now(uint16_t id)
{
    zbw_warning_t w;
    return zbw_wanted(&wd, zbw_find(&wd, id), &w) ? w.mode : 0xFFu;
}

/* ZB-023: the policy table - each alarm kind on each partition sounds the sirens of that
 * partition that sound for it, with its pattern; a one-tone siren sounds fire too. */
static void test_policy_table(void)
{
    four_sirens();
    static const struct {
        uint8_t partition;
        alarm_pattern_t pat;
        unsigned m1, m2, m3, m4;
    } rows[] = {
        {1, ALARM_PAT_STEADY, ZBW_MODE_BURGLAR, 0xFF, 0xFF, ZBW_MODE_EMERGENCY},
        {1, ALARM_PAT_TEMPORAL3, 0xFF, ZBW_MODE_FIRE, 0xFF, ZBW_MODE_EMERGENCY},
        {1, ALARM_PAT_TEMPORAL4, 0xFF, ZBW_MODE_EMERGENCY, 0xFF, ZBW_MODE_EMERGENCY},
        {2, ALARM_PAT_STEADY, 0xFF, 0xFF, ZBW_MODE_BURGLAR, ZBW_MODE_EMERGENCY},
        {2, ALARM_PAT_TEMPORAL3, 0xFF, 0xFF, ZBW_MODE_FIRE, ZBW_MODE_EMERGENCY},
        {2, ALARM_PAT_TEMPORAL4, 0xFF, 0xFF, ZBW_MODE_EMERGENCY, ZBW_MODE_EMERGENCY},
        {3, ALARM_PAT_STEADY, 0xFF, 0xFF, 0xFF, 0xFF}, /* nobody's partition */
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        fake.count = 0;
        zbw_output(&wd, 1000, rows[i].partition, ALARM_OUT_BELL, rows[i].pat);
        TEST_ASSERT_EQUAL_UINT(rows[i].m1, mode_now(1));
        TEST_ASSERT_EQUAL_UINT(rows[i].m2, mode_now(2));
        TEST_ASSERT_EQUAL_UINT(rows[i].m3, mode_now(3));
        TEST_ASSERT_EQUAL_UINT(rows[i].m4, mode_now(4));
        /* What was sent is what it should give: a Start Warning to each sounding siren. */
        const uint64_t ieees[] = {IEEE_1, IEEE_2, IEEE_3, IEEE_4};
        const unsigned want[] = {rows[i].m1, rows[i].m2, rows[i].m3, rows[i].m4};
        for (unsigned k = 0; k < 4u; k++) {
            if (want[k] == 0xFFu) {
                TEST_ASSERT_EQUAL_UINT(0, sends_to(ieees[k]));
            } else {
                zbw_warning_t w = warning_of(last_to(ieees[k]), ZBW_LAYOUT_SPEC);
                TEST_ASSERT_EQUAL_UINT(want[k], w.mode);
                TEST_ASSERT_EQUAL_UINT16(ZBW_MAX_DURATION_DEF, w.duration_s);
                TEST_ASSERT_EQUAL_UINT8(ZBW_LEVEL_HIGH, w.level);
                TEST_ASSERT_FALSE(w.strobe);
                TEST_ASSERT_EQUAL_UINT8(ZBW_CMD_START_WARNING, last_to(ieees[k])->cmd.command);
                TEST_ASSERT_EQUAL_UINT8(43, last_to(ieees[k])->endpoint);
            }
        }
        /* Off: Stop to each that sounded, and quiet. */
        fake.count = 0;
        zbw_output(&wd, 2000, rows[i].partition, ALARM_OUT_BELL, ALARM_PAT_OFF);
        for (unsigned k = 0; k < 4u; k++) {
            if (want[k] == 0xFFu) {
                TEST_ASSERT_EQUAL_UINT(0, sends_to(ieees[k]));
            } else {
                TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_STOP,
                                        warning_of(last_to(ieees[k]), ZBW_LAYOUT_SPEC).mode);
            }
            TEST_ASSERT_EQUAL_UINT(0xFF, mode_now((uint16_t)(k + 1u)));
        }
        /* Every send confirmed, so the next row starts clean. */
        for (unsigned k = 0; k < fake.count; k++) {
            TEST_ASSERT_TRUE(zbw_send_done(&wd, 2000, fake.sent[k].tag, ZSRC_ZB_DONE_OK));
        }
    }
    TEST_ASSERT_EQUAL_UINT(0, fake.trouble_calls);
}

/* ZB-023: a siren of two partitions sounds the most urgent of their bells (fire over CO over
 * burglary), and goes back to the other when the urgent one stops. */
static void test_policy_most_urgent(void)
{
    four_sirens();
    zbw_output(&wd, 0, 2, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    TEST_ASSERT_EQUAL_UINT(ZBW_MODE_BURGLAR, mode_now(3));
    zbw_output(&wd, 0, 2, ALARM_OUT_BELL, ALARM_PAT_TEMPORAL3);
    TEST_ASSERT_EQUAL_UINT(ZBW_MODE_FIRE, mode_now(3));
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_FIRE, warning_of(last_to(IEEE_3), ZBW_LAYOUT_SPEC).mode);

    /* Siren 5 in siren 1's place: burglary and fire on partitions 1 and 2. */
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_clear(&wd, 1));
    zbw_assign_t a = siren(5, IEEE_5, 0x03, true, true);
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
    TEST_ASSERT_EQUAL_UINT64(0, zbw_next_deadline(&wd)); /* a new siren: poll at once */
    zbw_output(&wd, 0, 1, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    TEST_ASSERT_EQUAL_UINT(ZBW_MODE_FIRE, mode_now(5));
    zbw_output(&wd, 0, 2, ALARM_OUT_BELL, ALARM_PAT_TEMPORAL4);
    TEST_ASSERT_EQUAL_UINT(ZBW_MODE_EMERGENCY, mode_now(5));
    zbw_output(&wd, 0, 2, ALARM_OUT_BELL, ALARM_PAT_OFF);
    TEST_ASSERT_EQUAL_UINT(ZBW_MODE_BURGLAR, mode_now(5));
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_BURGLAR, warning_of(last_to(IEEE_5), ZBW_LAYOUT_SPEC).mode);
    /* A smoke reset or a partition out of range is not a siren's. */
    unsigned n = fake.count;
    zbw_output(&wd, 0, 0, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    zbw_output(&wd, 0, ALARM_MAX_PARTITIONS + 1u, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    zbw_output(&wd, 0, 1, ALARM_OUT_SMOKE_RESET, ALARM_PAT_ON);
    TEST_ASSERT_EQUAL_UINT(n, fake.count);
}

/* ZB-023: the strobe with the sound when chosen; strobe-only (Stop with the strobe bit) while the
 * strobe output stays on after the bell; Stop when it goes off. The reversed layout on its
 * model. */
static void test_policy_strobe(void)
{
    zbw_assign_t a = siren(1, IEEE_1, 0x01, true, false);
    a.strobe = true;
    a.model_flags = ZBZ_MODEL_WD_REVERSED;
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
    zbw_radio(&wd, 0, true);
    zbw_output(&wd, 0, 1, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    zbw_warning_t w = warning_of(last(), ZBW_LAYOUT_REVERSED);
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_BURGLAR, w.mode);
    TEST_ASSERT_TRUE(w.strobe);
    TEST_ASSERT_EQUAL_UINT8(ZBW_STROBE_DUTY, w.duty);
    TEST_ASSERT_EQUAL_UINT8(ZBW_LEVEL_HIGH, w.strobe_level);
    TEST_ASSERT_EQUAL_HEX8(0x91, last()->cmd.payload[0]);
    unsigned n = fake.count;
    zbw_output(&wd, 0, 1, ALARM_OUT_STROBE, ALARM_PAT_ON); /* already strobing: nothing new */
    TEST_ASSERT_EQUAL_UINT(n, fake.count);

    zbw_output(&wd, 1000, 1, ALARM_OUT_BELL, ALARM_PAT_OFF);
    w = warning_of(last(), ZBW_LAYOUT_REVERSED);
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_STOP, w.mode);
    TEST_ASSERT_TRUE(w.strobe);
    TEST_ASSERT_EQUAL_UINT16(ZBW_MAX_DURATION_DEF, w.duration_s);
    TEST_ASSERT_TRUE(zbw_find(&wd, 1)->strobe_only);

    zbw_output(&wd, 2000, 1, ALARM_OUT_STROBE, ALARM_PAT_OFF);
    w = warning_of(last(), ZBW_LAYOUT_REVERSED);
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_STOP, w.mode);
    TEST_ASSERT_FALSE(w.strobe);
    TEST_ASSERT_FALSE(zbw_find(&wd, 1)->strobe_only);

    /* A siren without the strobe chosen ignores the strobe output. */
    a = siren(2, IEEE_2, 0x01, true, false);
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
    zbw_poll(&wd, 3000);
    n = sends_to(IEEE_2);
    zbw_output(&wd, 3000, 1, ALARM_OUT_STROBE, ALARM_PAT_ON);
    TEST_ASSERT_EQUAL_UINT(n, sends_to(IEEE_2));
}

/* ZB-023: MaxDuration respected (its own, else 240, at most ZBW_WARN_MAX_S) and asked again
 * ZBW_REFRESH_LEAD_MS before it runs out while the bell is on - not a millisecond earlier. */
static void test_refresh_timing(void)
{
    /* The lead is the whole retry budget: three tries unanswered for 15 s each, 2 s apart. */
    TEST_ASSERT_EQUAL_UINT(51000u, ZBW_REFRESH_LEAD_MS);
    TEST_ASSERT_TRUE(ZBW_REFRESH_LEAD_MS >= ZBW_REFRESH_S * 1000u);
    static const struct {
        uint16_t max_duration;
        uint16_t asked;
        uint64_t lead_ms;
    } rows[] = {
        {0, ZBW_MAX_DURATION_DEF, 51000u},
        {900, ZBW_WARN_MAX_S, 51000u},
        {120, 120, 51000u},
        {60, 60, 30000u}, /* a short one is asked again at its half */
        {6, 6, 3000u},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        setUp();
        zbw_assign_t a = siren(1, IEEE_1, 0x01, true, false);
        a.max_duration_s = rows[i].max_duration;
        TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
        zbw_radio(&wd, 0, true);
        const uint64_t t0 = 5000u;
        zbw_output(&wd, t0, 1, ALARM_OUT_BELL, ALARM_PAT_STEADY);
        TEST_ASSERT_EQUAL_UINT16(rows[i].asked, warning_of(last(), 0).duration_s);
        TEST_ASSERT_TRUE(zbw_send_done(&wd, t0 + 100u, last()->tag, ZSRC_ZB_DONE_OK));
        const uint64_t due = t0 + (uint64_t)rows[i].asked * 1000u - rows[i].lead_ms;
        TEST_ASSERT_EQUAL_UINT64(due, zbw_next_deadline(&wd));
        zbw_poll(&wd, due - 1u);
        TEST_ASSERT_EQUAL_UINT(1, fake.count);
        zbw_poll(&wd, due);
        TEST_ASSERT_EQUAL_UINT(2, fake.count);
        TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_BURGLAR, warning_of(last(), 0).mode);
        TEST_ASSERT_EQUAL_UINT16(rows[i].asked, warning_of(last(), 0).duration_s);
        TEST_ASSERT_TRUE(zbw_send_done(&wd, due, last()->tag, ZSRC_ZB_DONE_OK));
        /* Off: Stop, and no more refreshes. */
        zbw_output(&wd, due + 1000u, 1, ALARM_OUT_BELL, ALARM_PAT_OFF);
        TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_STOP, warning_of(last(), 0).mode);
        TEST_ASSERT_TRUE(zbw_send_done(&wd, due + 1000u, last()->tag, ZSRC_ZB_DONE_OK));
        unsigned n = fake.count;
        zbw_poll(&wd, due + 3600000u);
        TEST_ASSERT_EQUAL_UINT(n, fake.count);
    }
}

/* MaxDuration from an IAS WD Read Attributes Response's bytes (decoded as the radio hands it on);
 * false when the frame doesn't decode or holds no valid value. */
static bool max_from_bytes(const uint8_t *b, size_t n, uint16_t cluster, uint16_t *out)
{
    zsrc_zb_zcl_t f;
    if (zbz_zcl_decode_global(b, n, cluster, &f) != ZBZ_ZCL_OK) {
        return false;
    }
    return zbw_max_duration_of(&f, out);
}

/* ZB-023 (ZB03): the MaxDuration the setup reads, byte for byte (frame control 0x18: global,
 * server to client, no Default Response; record attr u16 | status | type | value, ZCL 2.5.2):
 * only attribute 0x0000 answered SUCCESS as a uint16 0..0xFFFE counts - a failed read, another
 * type, a record cut short, 0xFFFF (ZCL's invalid value), another attribute, command, cluster,
 * direction or a manufacturer's frame is "not read". derived (ZCL 07-5123 3.11.2.2.1; NXP
 * JNUG3132 IAS WD attributes: 0..0xFFFE, default 240). */
static void test_max_duration_bytes(void)
{
    static const struct {
        uint8_t b[16];
        uint8_t n;
        bool ok;
        uint16_t value;
    } rows[] = {
        {{0x18, 0x57, 0x01, 0x00, 0x00, 0x00, 0x21, 0xF0, 0x00}, 9, true, 240},
        {{0x18, 0x57, 0x01, 0x00, 0x00, 0x00, 0x21, 0x3C, 0x00}, 9, true, 60},
        {{0x18, 0x57, 0x01, 0x00, 0x00, 0x00, 0x21, 0x00, 0x00}, 9, true, 0}, /* a valid 0 */
        {{0x18, 0x57, 0x01, 0x00, 0x00, 0x00, 0x21, 0xFE, 0xFF}, 9, true, 0xFFFE},
        {{0x18, 0x57, 0x01, 0x00, 0x00, 0x00, 0x21, 0xFF, 0xFF}, 9, false, 0}, /* invalid */
        {{0x18, 0x57, 0x01, 0x00, 0x00, 0x86}, 6, false, 0},       /* unsupported attribute */
        {{0x18, 0x57, 0x01, 0x00, 0x00, 0x00, 0x20, 0x3C}, 8, false, 0}, /* a uint8 */
        {{0x18, 0x57, 0x01, 0x00, 0x00, 0x00, 0x21, 0x3C}, 8, false, 0}, /* value cut short */
        {{0x18, 0x57, 0x01, 0x00, 0x00, 0x00}, 6, false, 0},       /* no type */
        {{0x18, 0x57, 0x01, 0x00, 0x00}, 5, false, 0},             /* no status */
        {{0x18, 0x57, 0x01}, 3, false, 0},                         /* no record */
        {{0x18, 0x57, 0x01, 0x01, 0x00, 0x00, 0x21, 0x3C, 0x00}, 9, false, 0}, /* attr 0x0001 */
        {{0x18, 0x57, 0x01, 0x01, 0x00, 0x86, 0x00, 0x00, 0x00, 0x21, 0x3C, 0x00}, 12, true, 60},
        {{0x18, 0x57, 0x0A, 0x00, 0x00, 0x21, 0x3C, 0x00}, 8, false, 0}, /* a report */
        {{0x10, 0x57, 0x01, 0x00, 0x00, 0x00, 0x21, 0x3C, 0x00}, 9, false, 0}, /* to the server */
        {{0x1C, 0x34, 0x12, 0x57, 0x01, 0x00, 0x00, 0x00, 0x21, 0x3C, 0x00}, 11, false, 0},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        uint16_t v = 0xABCDu;
        const bool ok = max_from_bytes(rows[i].b, rows[i].n, ZSRC_ZB_ZCL_IAS_WD, &v);
        TEST_ASSERT_EQUAL_MESSAGE(rows[i].ok, ok, "row");
        if (ok) {
            TEST_ASSERT_EQUAL_UINT16(rows[i].value, v);
        } else {
            TEST_ASSERT_EQUAL_UINT16(0xABCDu, v); /* untouched */
        }
    }
    /* The same answer from another cluster is not a siren's. */
    uint16_t v = 0u;
    TEST_ASSERT_FALSE(max_from_bytes(rows[1].b, rows[1].n, ZSRC_ZB_ZCL_IAS_ZONE, &v));
    TEST_ASSERT_FALSE(zbw_max_duration_of(NULL, &v));
}

/* ZB-023 (ZB03): the warning asked for and its refresh follow the MaxDuration read at setup - the
 * shorter of it and the model's cap; a read 0 is ZBW_MAX_DURATION_ZERO_S (never a 0 s warning);
 * not read, the model's cap or 240 - each refreshed ZBW_REFRESH_LEAD_MS early, at most half way. */
static void test_refresh_follows_the_read_limit(void)
{
    TEST_ASSERT_EQUAL_UINT(20u, ZBW_MAX_DURATION_ZERO_S);
    static const struct {
        bool read;
        uint16_t read_s;
        uint16_t cap;
        uint16_t asked;
        uint64_t lead_ms;
    } rows[] = {
        {false, 0, 0, ZBW_MAX_DURATION_DEF, 51000u}, /* not read: the default */
        {false, 0, 90, 90, 45000u},                   /* not read: the model's */
        {true, 240, 0, 240, 51000u},                  /* the default, read */
        {true, 60, 0, 60, 30000u},                    /* shorter: refreshed at its half */
        {true, 60, 30, 30, 15000u},                   /* the model's cap is shorter */
        {true, 30, 90, 30, 15000u},                   /* the read one is shorter */
        {true, 0, 0, ZBW_MAX_DURATION_ZERO_S, 10000u}, /* a valid 0 */
        {true, 0, 6, 6, 3000u},
        {true, 0xFFFE, 0, ZBW_WARN_MAX_S, 51000u},
        {true, 5, 0, ZBW_MAX_DURATION_ZERO_S, 10000u}, /* under the floor: as a read 0 */
        {true, 19, 0, ZBW_MAX_DURATION_ZERO_S, 10000u},
        {true, 20, 0, 20, 10000u},
        {true, 1, 6, 6, 3000u},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        setUp();
        zbw_assign_t a = siren(1, IEEE_1, 0x01, true, false);
        a.max_read = rows[i].read;
        a.max_read_s = rows[i].read_s;
        a.max_duration_s = rows[i].cap;
        TEST_ASSERT_EQUAL_UINT16(rows[i].asked, zbw_warn_seconds(&a));
        TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
        zbw_radio(&wd, 0, true);
        const uint64_t t0 = 5000u;
        zbw_output(&wd, t0, 1, ALARM_OUT_BELL, ALARM_PAT_STEADY);
        TEST_ASSERT_EQUAL_UINT16(rows[i].asked, warning_of(last(), 0).duration_s);
        TEST_ASSERT_TRUE(zbw_send_done(&wd, t0 + 100u, last()->tag, ZSRC_ZB_DONE_OK));
        const uint64_t due = t0 + (uint64_t)rows[i].asked * 1000u - rows[i].lead_ms;
        TEST_ASSERT_EQUAL_UINT64(due, zbw_next_deadline(&wd));
        zbw_poll(&wd, due - 1u);
        TEST_ASSERT_EQUAL_UINT(1, fake.count);
        zbw_poll(&wd, due);
        TEST_ASSERT_EQUAL_UINT(2, fake.count);
        TEST_ASSERT_EQUAL_UINT16(rows[i].asked, warning_of(last(), 0).duration_s);
        TEST_ASSERT_TRUE(zbw_send_done(&wd, due, last()->tag, ZSRC_ZB_DONE_OK));
        zbw_output(&wd, due + 100u, 1, ALARM_OUT_BELL, ALARM_PAT_OFF);
        TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_STOP, warning_of(last(), 0).mode);
    }
    TEST_ASSERT_EQUAL_UINT16(0u, zbw_warn_seconds(NULL));
}

/* ZB-023: a refresh that fails is retried, and every try of it - and the bell trouble when all
 * fail - comes before the warning already running in the siren ends, whether the radio never
 * tells the end of a send or tells a timeout; a retry that is confirmed sets the next refresh
 * from its own send. */
static void test_refresh_retries_fit(void)
{
    for (unsigned told = 0; told < 2u; told++) {
        setUp();
        zbw_assign_t a = siren(1, IEEE_1, 0x01, true, false);
        TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
        zbw_radio(&wd, 0, true);
        const uint64_t t0 = 5000u;
        zbw_output(&wd, t0, 1, ALARM_OUT_BELL, ALARM_PAT_STEADY);
        TEST_ASSERT_TRUE(zbw_send_done(&wd, t0 + 100u, last()->tag, ZSRC_ZB_DONE_OK));
        const uint64_t runs_out = t0 + ZBW_MAX_DURATION_DEF * 1000u; /* the siren goes quiet */
        uint64_t t = runs_out - 51000u;
        zbw_poll(&wd, t);
        TEST_ASSERT_EQUAL_UINT(2, fake.count); /* the refresh, try 1 */
        for (unsigned try_no = 2; try_no <= ZBW_SEND_TRIES; try_no++) {
            if (told != 0u) {
                t += 3000u; /* the radio tells a timeout */
                TEST_ASSERT_TRUE(zbw_send_done(&wd, t, last()->tag, ZSRC_ZB_DONE_TIMEOUT));
            } else {
                zbw_poll(&wd, t + ZBW_CONFIRM_MS - 1u);
                TEST_ASSERT_EQUAL_UINT(try_no - 1u, fake.count - 1u);
                t += ZBW_CONFIRM_MS; /* its end never comes */
                zbw_poll(&wd, t);
            }
            TEST_ASSERT_FALSE(fake.trouble);
            t += ZBW_RETRY_MS;
            TEST_ASSERT_EQUAL_UINT64(t, zbw_next_deadline(&wd));
            zbw_poll(&wd, t);
            TEST_ASSERT_EQUAL_UINT(try_no, fake.count - 1u); /* the next try went out ... */
            TEST_ASSERT_TRUE(t < runs_out);                  /* ... while it still sounds */
            TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_BURGLAR, warning_of(last(), 0).mode);
        }
        if (told != 0u) {
            /* the last try confirmed: no trouble, the next refresh counts from it */
            TEST_ASSERT_TRUE(zbw_send_done(&wd, t + 500u, last()->tag, ZSRC_ZB_DONE_OK));
            TEST_ASSERT_FALSE(fake.trouble);
            TEST_ASSERT_EQUAL_UINT64(t + ZBW_MAX_DURATION_DEF * 1000u - 51000u,
                                     zbw_next_deadline(&wd));
        } else {
            /* the last try unanswered too: the bell trouble, still before it runs out */
            t += ZBW_CONFIRM_MS;
            zbw_poll(&wd, t);
            TEST_ASSERT_TRUE(fake.trouble);
            TEST_ASSERT_TRUE(t < runs_out);
        }
    }
}

/* ZB-023: an unconfirmed warning is sent again ZBW_RETRY_MS apart and raises the bell trouble
 * after ZBW_SEND_TRIES; then it is tried slowly while still wanted; a confirmation clears it.
 * A send the stack refuses and one whose end never comes count the same. */
static void test_unconfirmed_is_bell_trouble(void)
{
    zbw_assign_t a = siren(1, IEEE_1, 0x01, true, false);
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
    zbw_radio(&wd, 0, true);
    zbw_output(&wd, 0, 1, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    TEST_ASSERT_EQUAL_UINT(1, fake.count);
    TEST_ASSERT_TRUE(last()->cmd.want_response);
    /* 1: the radio says it timed out */
    TEST_ASSERT_TRUE(zbw_send_done(&wd, 1000, last()->tag, ZSRC_ZB_DONE_TIMEOUT));
    TEST_ASSERT_EQUAL_UINT64(1000u + ZBW_RETRY_MS, zbw_next_deadline(&wd));
    zbw_poll(&wd, 1000u + ZBW_RETRY_MS - 1u);
    TEST_ASSERT_EQUAL_UINT(1, fake.count);
    zbw_poll(&wd, 1000u + ZBW_RETRY_MS);
    TEST_ASSERT_EQUAL_UINT(2, fake.count);
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_BURGLAR, warning_of(last(), 0).mode);
    /* 2: the stack refuses the next try */
    uint64_t t = 1000u + ZBW_RETRY_MS;
    TEST_ASSERT_TRUE(zbw_send_done(&wd, t, last()->tag, ZSRC_ZB_DONE_SEND_FAILED));
    TEST_ASSERT_FALSE(zbw_find(&wd, 1)->trouble);
    fake.refuse = true;
    t += ZBW_RETRY_MS;
    zbw_poll(&wd, t); /* 3: refused - the trouble */
    TEST_ASSERT_TRUE(fake.trouble);
    TEST_ASSERT_EQUAL_UINT(1, fake.trouble_calls);
    TEST_ASSERT_TRUE(zbw_find(&wd, 1)->unconfirmed);
    TEST_ASSERT_EQUAL_UINT64(t + ZBW_RETRY_SLOW_MS, zbw_next_deadline(&wd));
    fake.refuse = false;
    zbw_poll(&wd, t + ZBW_RETRY_SLOW_MS);
    TEST_ASSERT_EQUAL_UINT(3, fake.count);
    /* its end never comes: counted after ZBW_CONFIRM_MS, still in trouble */
    t += ZBW_RETRY_SLOW_MS;
    zbw_poll(&wd, t + ZBW_CONFIRM_MS);
    TEST_ASSERT_TRUE(fake.trouble);
    zbw_poll(&wd, t + ZBW_CONFIRM_MS + ZBW_RETRY_SLOW_MS);
    TEST_ASSERT_EQUAL_UINT(4, fake.count);
    /* a stale tag is ours but changes nothing; the current one confirmed clears the trouble */
    TEST_ASSERT_TRUE(zbw_send_done(&wd, t, fake.sent[2].tag, ZSRC_ZB_DONE_OK));
    TEST_ASSERT_TRUE(fake.trouble);
    TEST_ASSERT_TRUE(zbw_send_done(&wd, t + ZBW_CONFIRM_MS + ZBW_RETRY_SLOW_MS, last()->tag,
                                   ZSRC_ZB_DONE_OK));
    TEST_ASSERT_FALSE(fake.trouble);
    TEST_ASSERT_EQUAL_UINT(2, fake.trouble_calls);
    /* someone else's tag */
    TEST_ASSERT_FALSE(zbw_send_done(&wd, t, 0x5A000001u, ZSRC_ZB_DONE_OK));
}

/* ZB-023: a model that never sends Default Responses isn't asked for one. Sends while the radio
 * is down don't count; coming up, the command goes again at once. */
static void test_no_default_rsp_and_radio_down(void)
{
    zbw_assign_t a = siren(1, IEEE_1, 0x01, true, true);
    a.model_flags = ZBZ_MODEL_NO_DEFAULT_RSP;
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
    zbw_output(&wd, 0, 1, ALARM_OUT_BELL, ALARM_PAT_TEMPORAL3); /* radio not up yet */
    TEST_ASSERT_FALSE(last()->cmd.want_response);
    for (unsigned i = 0; i < 5u; i++) {
        TEST_ASSERT_TRUE(zbw_send_done(&wd, 1000u * i, last()->tag, ZSRC_ZB_DONE_NO_RADIO));
        zbw_poll(&wd, zbw_next_deadline(&wd));
    }
    TEST_ASSERT_FALSE(fake.trouble);
    TEST_ASSERT_FALSE(zbw_find(&wd, 1)->unconfirmed);
    unsigned n = fake.count;
    zbw_radio(&wd, 600000, true);
    TEST_ASSERT_EQUAL_UINT(n + 1u, fake.count);
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_FIRE, warning_of(last(), 0).mode);
}

/* ZB-023: a chirp from each siren of the partition that wants one, on arm and disarm, with its
 * strobe and volume; none from one giving a warning. */
static void test_squawk_policy(void)
{
    zbw_assign_t a = siren(1, IEEE_1, 0x01, true, false);
    a.squawk = true;
    a.strobe = true;
    a.volume = ZBW_LEVEL_MEDIUM;
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
    a = siren(2, IEEE_2, 0x01, true, false); /* no squawk */
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
    a = siren(3, IEEE_3, 0x02, true, false);
    a.squawk = true;
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
    zbw_radio(&wd, 0, true);

    zbw_armed(&wd, 100, 1, true);
    TEST_ASSERT_EQUAL_UINT(1, fake.count);
    TEST_ASSERT_TRUE(last()->ieee == IEEE_1);
    TEST_ASSERT_EQUAL_UINT8(ZBW_CMD_SQUAWK, last()->cmd.command);
    TEST_ASSERT_FALSE(last()->cmd.want_response);
    zbw_squawk_t s;
    zbw_squawk_decode(last()->cmd.payload[0], ZBW_LAYOUT_SPEC, &s);
    TEST_ASSERT_EQUAL_UINT8(ZBW_SQUAWK_ARMED, s.mode);
    TEST_ASSERT_TRUE(s.strobe);
    TEST_ASSERT_EQUAL_UINT8(ZBW_LEVEL_MEDIUM, s.level);
    /* a squawk's failure isn't held against it */
    TEST_ASSERT_TRUE(zbw_send_done(&wd, 200, last()->tag, ZSRC_ZB_DONE_TIMEOUT));
    TEST_ASSERT_FALSE(zbw_find(&wd, 1)->trouble);

    zbw_armed(&wd, 300, 1, false);
    TEST_ASSERT_EQUAL_UINT(2, fake.count);
    zbw_squawk_decode(last()->cmd.payload[0], ZBW_LAYOUT_SPEC, &s);
    TEST_ASSERT_EQUAL_UINT8(ZBW_SQUAWK_DISARMED, s.mode);

    /* sounding: no chirp */
    zbw_output(&wd, 400, 1, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    unsigned n = fake.count;
    zbw_armed(&wd, 500, 1, false);
    TEST_ASSERT_EQUAL_UINT(n, fake.count);
    /* the partition's armed state without a chirp */
    zbw_set_armed(&wd, 2, true);
    TEST_ASSERT_TRUE(wd.armed[1]);
    TEST_ASSERT_EQUAL_UINT(n, fake.count);
}

/* ZB-023: "Test it" - ZBW_TEST_S of its burglar sound; refused while any of its partitions is
 * armed, while it is sounding, for an unknown siren, and when the radio can't take it. The
 * engine's bell test pulse sounds each idle siren of the partition the same way, once. */
static void test_test_sound(void)
{
    zbw_assign_t a = siren(1, IEEE_1, 0x03, false, true);
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
    zbw_radio(&wd, 0, true);
    TEST_ASSERT_EQUAL_INT(ZBW_ERR_UNKNOWN, zbw_test(&wd, 0, 9));
    zbw_armed(&wd, 0, 2, true);
    TEST_ASSERT_EQUAL_INT(ZBW_ERR_ARMED, zbw_test(&wd, 0, 1));
    zbw_armed(&wd, 0, 2, false);
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_test(&wd, 0, 1));
    zbw_warning_t w = warning_of(last(), 0);
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_BURGLAR, w.mode);
    TEST_ASSERT_EQUAL_UINT16(ZBW_TEST_S, w.duration_s);
    TEST_ASSERT_TRUE(last()->cmd.want_response);
    TEST_ASSERT_TRUE(zbw_send_done(&wd, 100, last()->tag, ZSRC_ZB_DONE_OK));
    /* no Stop and no refresh follow a test */
    unsigned n = fake.count;
    zbw_poll(&wd, 3600000);
    TEST_ASSERT_EQUAL_UINT(n, fake.count);
    fake.refuse = true;
    TEST_ASSERT_EQUAL_INT(ZBW_ERR_SEND, zbw_test(&wd, 3600000, 1));
    fake.refuse = false;
    zbw_output(&wd, 3600000, 1, ALARM_OUT_BELL, ALARM_PAT_TEMPORAL3);
    TEST_ASSERT_EQUAL_INT(ZBW_ERR_BUSY, zbw_test(&wd, 3600000, 1));
    zbw_output(&wd, 3600000, 1, ALARM_OUT_BELL, ALARM_PAT_OFF);
    TEST_ASSERT_TRUE(zbw_send_done(&wd, 3600000, last()->tag, ZSRC_ZB_DONE_OK));

    /* the engine's walk-test pulse: a short sound, the bell otherwise untouched */
    n = fake.count;
    zbw_output(&wd, 3700000, 2, ALARM_OUT_BELL, ALARM_PAT_TEST);
    TEST_ASSERT_EQUAL_UINT(n + 1u, fake.count);
    w = warning_of(last(), 0);
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_BURGLAR, w.mode);
    TEST_ASSERT_EQUAL_UINT16(ZBW_TEST_S, w.duration_s);
    TEST_ASSERT_EQUAL_UINT8(ALARM_PAT_OFF, wd.bell[1]);
    zbw_poll(&wd, 3800000);
    TEST_ASSERT_EQUAL_UINT(n + 1u, fake.count);
}

/* ZB-023 (alarm-devices.md §4): a siren silent past its limit while the radio works is the bell
 * trouble - a mains siren ZBW_SILENT_ROUTER_S, a sleepy one ZBW_SILENT_SLEEPY_S, else the
 * installer's supervision_min - one second earlier isn't; its next frame clears it; nothing
 * counts while the radio is down, and coming up the silence counts from then. */
static void test_supervision_silence(void)
{
    zbw_assign_t a = siren(1, IEEE_1, 0x01, true, false);
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
    a = siren(2, IEEE_2, 0x01, true, false);
    a.sleepy = true;
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
    a = siren(3, IEEE_3, 0x01, true, false);
    a.supervision_min = 120u;
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
    const uint64_t t0 = 10000u;
    zbw_radio(&wd, t0, true);
    const uint64_t router = t0 + ZBW_SILENT_ROUTER_S * 1000ull;
    TEST_ASSERT_EQUAL_UINT64(router, zbw_next_deadline(&wd));
    zbw_poll(&wd, router - 1000u);
    TEST_ASSERT_FALSE(fake.trouble);
    zbw_poll(&wd, router);
    TEST_ASSERT_TRUE(fake.trouble);
    TEST_ASSERT_TRUE(zbw_find(&wd, 1)->silent);
    TEST_ASSERT_FALSE(zbw_find(&wd, 2)->silent);
    TEST_ASSERT_FALSE(zbw_find(&wd, 3)->silent);
    zbw_heard(&wd, router + 1u, IEEE_1);
    TEST_ASSERT_FALSE(fake.trouble);

    const uint64_t mins = t0 + 120u * 60000ull;
    zbw_poll(&wd, mins - 1000u);
    TEST_ASSERT_FALSE(fake.trouble);
    zbw_poll(&wd, mins);
    TEST_ASSERT_TRUE(zbw_find(&wd, 3)->silent);
    TEST_ASSERT_TRUE(fake.trouble);
    zbw_heard(&wd, mins, IEEE_3);

    const uint64_t sleepy = t0 + ZBW_SILENT_SLEEPY_S * 1000ull;
    zbw_heard(&wd, sleepy - 1000u, IEEE_1);
    zbw_heard(&wd, sleepy - 1000u, IEEE_3);
    zbw_poll(&wd, sleepy - 1000u);
    TEST_ASSERT_FALSE(fake.trouble);
    zbw_poll(&wd, sleepy);
    TEST_ASSERT_TRUE(zbw_find(&wd, 2)->silent);
    TEST_ASSERT_TRUE(fake.trouble);

    /* The radio down: nobody is silent; up again, the silence counts from then. */
    zbw_radio(&wd, sleepy + 1u, false);
    TEST_ASSERT_FALSE(fake.trouble);
    zbw_poll(&wd, sleepy + 100000000u);
    TEST_ASSERT_FALSE(fake.trouble);
    const uint64_t up = sleepy + 100000000u;
    zbw_radio(&wd, up, true);
    zbw_poll(&wd, up + ZBW_SILENT_ROUTER_S * 1000ull - 1000u);
    TEST_ASSERT_FALSE(fake.trouble);
    zbw_poll(&wd, up + ZBW_SILENT_ROUTER_S * 1000ull);
    TEST_ASSERT_TRUE(fake.trouble);
}

/* ZB-023: its IAS Zone trouble, AC mains or battery-defect bit is the bell trouble; tamper and a
 * low battery are not; a clean status clears it. */
static void test_zone_status_bits(void)
{
    zbw_assign_t a = siren(1, IEEE_1, 0x01, true, false);
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
    zbw_radio(&wd, 0, true);
    static const uint16_t bad[] = {ZSRC_ZB_IAS_TROUBLE, ZSRC_ZB_IAS_AC_MAINS,
                                   ZSRC_ZB_IAS_BATT_DEFECT};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        zbw_zone_status(&wd, 10, IEEE_1, bad[i]);
        TEST_ASSERT_TRUE(fake.trouble);
        zbw_zone_status(&wd, 10, IEEE_1, 0);
        TEST_ASSERT_FALSE(fake.trouble);
    }
    zbw_zone_status(&wd, 10, IEEE_1, ZSRC_ZB_IAS_TAMPER | ZSRC_ZB_IAS_BATTERY);
    TEST_ASSERT_FALSE(fake.trouble);
    zbw_zone_status(&wd, 10, IEEE_2, ZSRC_ZB_IAS_TROUBLE); /* not a siren */
    TEST_ASSERT_FALSE(fake.trouble);
}

/* ZB-023: the assignments - what is refused, FULL, an update keeps the live state, clearing a
 * sounding siren sends it Stop and drops its trouble. */
static void test_assign_and_clear(void)
{
    zbw_assign_t a = siren(1, IEEE_1, 0x01, true, false);
    zbw_assign_t b = a;
    b.id = 0;
    TEST_ASSERT_EQUAL_INT(ZBW_ERR_ARG, zbw_assign(&wd, &b));
    b = a;
    b.ieee = 0;
    TEST_ASSERT_EQUAL_INT(ZBW_ERR_ARG, zbw_assign(&wd, &b));
    b = a;
    b.endpoint = 0;
    TEST_ASSERT_EQUAL_INT(ZBW_ERR_ARG, zbw_assign(&wd, &b));
    b = a;
    b.endpoint = 241;
    TEST_ASSERT_EQUAL_INT(ZBW_ERR_ARG, zbw_assign(&wd, &b));
    b = a;
    b.partitions = 0;
    TEST_ASSERT_EQUAL_INT(ZBW_ERR_ARG, zbw_assign(&wd, &b));
    b = a;
    b.partitions = (uint8_t)(1u << ALARM_MAX_PARTITIONS);
    TEST_ASSERT_EQUAL_INT(ZBW_ERR_ARG, zbw_assign(&wd, &b));
    b = a;
    b.volume = 4;
    TEST_ASSERT_EQUAL_INT(ZBW_ERR_ARG, zbw_assign(&wd, &b));
    b = a;
    b.supervision_min = ZBW_SUPERVISION_MIN_MIN - 1u;
    TEST_ASSERT_EQUAL_INT(ZBW_ERR_ARG, zbw_assign(&wd, &b));
    TEST_ASSERT_EQUAL_INT(ZBW_ERR_ARG, zbw_assign(&wd, NULL));
    TEST_ASSERT_FALSE(wd.dirty);

    a.device = 5; /* ignored: a siren takes no device number */
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
    TEST_ASSERT_TRUE(wd.dirty);
    TEST_ASSERT_EQUAL_UINT8(0, zbw_find(&wd, 1)->a.device);
    b = siren(2, IEEE_1, 0x01, true, false); /* the same siren under another id */
    TEST_ASSERT_EQUAL_INT(ZBW_ERR_ARG, zbw_assign(&wd, &b));
    b.endpoint = 44; /* another endpoint of it */
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &b));
    b = siren(3, IEEE_3, 0x01, true, false);
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &b));
    b = siren(4, IEEE_4, 0x01, true, false);
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &b));
    b = siren(5, IEEE_5, 0x01, true, false);
    TEST_ASSERT_EQUAL_INT(ZBW_ERR_FULL, zbw_assign(&wd, &b));

    zbw_radio(&wd, 0, true);
    zbw_output(&wd, 0, 1, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    TEST_ASSERT_EQUAL_UINT(ZBW_MODE_BURGLAR, zbw_find(&wd, 1)->mode);
    a.volume = ZBW_LEVEL_LOW; /* an update: still sounding, at the new volume */
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
    TEST_ASSERT_EQUAL_UINT(ZBW_MODE_BURGLAR, zbw_find(&wd, 1)->mode);
    zbw_poll(&wd, 10);
    TEST_ASSERT_EQUAL_UINT8(ZBW_LEVEL_LOW, warning_of(last_to(IEEE_1), 0).level);

    zbw_zone_status(&wd, 20, IEEE_3, ZSRC_ZB_IAS_TROUBLE);
    TEST_ASSERT_TRUE(fake.trouble);
    unsigned n = sends_to(IEEE_3);
    wd.dirty = false;
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_clear(&wd, 3));
    TEST_ASSERT_TRUE(wd.dirty);
    TEST_ASSERT_FALSE(fake.trouble);
    TEST_ASSERT_EQUAL_UINT(n + 1u, sends_to(IEEE_3));
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_STOP, warning_of(last_to(IEEE_3), 0).mode);
    TEST_ASSERT_NULL(zbw_find(&wd, 3));
    TEST_ASSERT_EQUAL_INT(ZBW_ERR_UNKNOWN, zbw_clear(&wd, 3));
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &b)); /* its slot is free again */
    TEST_ASSERT_EQUAL_UINT64(0, zbw_next_deadline(&wd)); /* the new one: poll at once */
}

/* ZB-023: a siren cleared while its Stop isn't confirmed yet gets Stop again; one moved to
 * another address or endpoint while it sounds stops at the old one and sounds at the new one;
 * a quiet, confirmed siren cleared or moved gets nothing at its old address. */
static void test_clear_and_move_stop(void)
{
    zbw_assign_t a = siren(1, IEEE_1, 0x01, true, false);
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
    zbw_radio(&wd, 0, true);
    zbw_output(&wd, 0, 1, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    TEST_ASSERT_TRUE(zbw_send_done(&wd, 10, last()->tag, ZSRC_ZB_DONE_OK));
    zbw_output(&wd, 1000, 1, ALARM_OUT_BELL, ALARM_PAT_OFF); /* Stop, not confirmed */
    TEST_ASSERT_TRUE(zbw_find(&wd, 1)->pending);
    unsigned n = sends_to(IEEE_1);
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_clear(&wd, 1));
    TEST_ASSERT_EQUAL_UINT(n + 1u, sends_to(IEEE_1));
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_STOP, warning_of(last_to(IEEE_1), 0).mode);

    /* Moved while it sounds: Stop to the old address, the warning to the new one. */
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
    zbw_output(&wd, 2000, 1, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    TEST_ASSERT_TRUE(zbw_send_done(&wd, 2010, last()->tag, ZSRC_ZB_DONE_OK));
    n = sends_to(IEEE_1);
    a.ieee = IEEE_2;
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
    TEST_ASSERT_EQUAL_UINT(n + 1u, sends_to(IEEE_1));
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_STOP, warning_of(last_to(IEEE_1), 0).mode);
    TEST_ASSERT_EQUAL_UINT64(0, zbw_next_deadline(&wd)); /* the new address: poll at once */
    zbw_poll(&wd, 2100);
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_BURGLAR, warning_of(last_to(IEEE_2), 0).mode);
    TEST_ASSERT_EQUAL_UINT(n + 1u, sends_to(IEEE_1));
    TEST_ASSERT_TRUE(zbw_send_done(&wd, 2110, last()->tag, ZSRC_ZB_DONE_OK));
    /* Another endpoint of the same device: the same, by endpoint. */
    a.endpoint = 44u;
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
    TEST_ASSERT_EQUAL_UINT8(43, last_to(IEEE_2)->endpoint);
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_STOP, warning_of(last_to(IEEE_2), 0).mode);
    zbw_poll(&wd, 2200);
    TEST_ASSERT_EQUAL_UINT8(44, last_to(IEEE_2)->endpoint);
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_BURGLAR, warning_of(last_to(IEEE_2), 0).mode);
    TEST_ASSERT_TRUE(zbw_send_done(&wd, 2210, last()->tag, ZSRC_ZB_DONE_OK));

    /* Quiet and confirmed: moving or clearing it sends nothing to the old address. */
    zbw_output(&wd, 3000, 1, ALARM_OUT_BELL, ALARM_PAT_OFF);
    TEST_ASSERT_TRUE(zbw_send_done(&wd, 3010, last()->tag, ZSRC_ZB_DONE_OK));
    n = fake.count;
    a.ieee = IEEE_3;
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
    zbw_poll(&wd, 3100);
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_clear(&wd, 1));
    TEST_ASSERT_EQUAL_UINT(n, fake.count);
    TEST_ASSERT_FALSE(fake.trouble);
}

/* ---- the store --------------------------------------------------------------------------------- */

static void fill_fixture_content(void)
{
    zbw_assign_t a = siren(3, IEEE_1, 0x01, true, true);
    a.strobe = true;
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
    a = siren(7, IEEE_2, 0x03, false, true);
    a.endpoint = 1;
    a.squawk = true;
    a.volume = ZBW_LEVEL_VERY_HIGH;
    a.supervision_min = 120;
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
}

/* ZB-023: the frozen v1 blob reads, and the same assignments encode to its bytes. */
static void test_wd_frozen_fixture(void)
{
    TEST_ASSERT_TRUE(zbw_decode(&wd, WD_FIXTURE_V1, sizeof(WD_FIXTURE_V1)));
    TEST_ASSERT_FALSE(wd.dirty);
    const zbw_siren_t *s = zbw_find(&wd, 3);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(s->a.ieee == IEEE_1);
    TEST_ASSERT_EQUAL_UINT8(43, s->a.endpoint);
    TEST_ASSERT_EQUAL_HEX8(0x01, s->a.partitions);
    TEST_ASSERT_TRUE(s->a.burglary && s->a.fire && s->a.strobe && !s->a.squawk);
    TEST_ASSERT_EQUAL_UINT8(ZBW_LEVEL_HIGH, s->a.volume);
    TEST_ASSERT_EQUAL_UINT16(0, s->a.supervision_min);
    TEST_ASSERT_EQUAL_UINT8(ZBW_MODE_STOP, s->mode);
    s = zbw_find(&wd, 7);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(s->a.ieee == IEEE_2);
    TEST_ASSERT_EQUAL_UINT8(1, s->a.endpoint);
    TEST_ASSERT_EQUAL_HEX8(0x03, s->a.partitions);
    TEST_ASSERT_TRUE(!s->a.burglary && s->a.fire && !s->a.strobe && s->a.squawk);
    TEST_ASSERT_EQUAL_UINT8(ZBW_LEVEL_VERY_HIGH, s->a.volume);
    TEST_ASSERT_EQUAL_UINT16(120, s->a.supervision_min);

    setUp();
    fill_fixture_content();
    uint8_t buf[ZBW_BLOB_MAX];
    size_t n = zbw_encode(&wd, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(sizeof(WD_FIXTURE_V1), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(WD_FIXTURE_V1, buf, n);
}

/* ZB-023: four sirens fill ZBW_BLOB_MAX exactly and read back; too small a buffer writes nothing;
 * none is the two-byte header; the live state and the port survive a read. */
static void test_wd_round_trip(void)
{
    for (uint16_t i = 1; i <= ZBW_SIRENS_MAX; i++) {
        zbw_assign_t a = siren(i, IEEE_1 + i, (uint8_t)i, (i & 1u) != 0u, (i & 2u) != 0u);
        a.supervision_min = (uint16_t)(60u * i);
        a.volume = (uint8_t)(i - 1u);
        TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &a));
    }
    uint8_t buf[ZBW_BLOB_MAX + 4u];
    TEST_ASSERT_EQUAL_size_t(0, zbw_encode(&wd, buf, ZBW_BLOB_MAX - 1u));
    size_t n = zbw_encode(&wd, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(ZBW_BLOB_MAX, n);
    zbw_t back;
    zbw_init(&back, &wd.port);
    TEST_ASSERT_TRUE(zbw_decode(&back, buf, n));
    for (uint16_t i = 1; i <= ZBW_SIRENS_MAX; i++) {
        const zbw_siren_t *x = zbw_find(&wd, i);
        const zbw_siren_t *y = zbw_find(&back, i);
        TEST_ASSERT_NOT_NULL(y);
        TEST_ASSERT_TRUE(x->a.ieee == y->a.ieee);
        TEST_ASSERT_EQUAL_UINT8(x->a.partitions, y->a.partitions);
        TEST_ASSERT_EQUAL(x->a.burglary, y->a.burglary);
        TEST_ASSERT_EQUAL(x->a.fire, y->a.fire);
        TEST_ASSERT_EQUAL_UINT8(x->a.volume, y->a.volume);
        TEST_ASSERT_EQUAL_UINT16(x->a.supervision_min, y->a.supervision_min);
    }
    zbw_init(&back, NULL);
    n = zbw_encode(&back, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_TRUE(zbw_decode(&back, buf, n));
    TEST_ASSERT_NULL(zbw_find(&back, 1));
    TEST_ASSERT_EQUAL_size_t(0, zbw_encode(&back, NULL, 10));
}

/* ZB-023: a damaged blob is refused whole and leaves the sirens as they were (none at start-up):
 * every cut, a newer compat, too many entries, a trailing byte, an entry past the end, the same
 * siren twice, values out of range. */
static void test_wd_damaged(void)
{
    /* Live sirens survive a damaged read (a restore at run time): still there, still sounding,
     * nothing sent, nothing to store. */
    zbw_assign_t live = siren(9, IEEE_5, 0x01, true, false);
    TEST_ASSERT_EQUAL_INT(ZBW_OK, zbw_assign(&wd, &live));
    zbw_radio(&wd, 0, true);
    zbw_output(&wd, 0, 1, ALARM_OUT_BELL, ALARM_PAT_STEADY);
    wd.dirty = true;
    const unsigned sent = fake.count;
    TEST_ASSERT_FALSE(zbw_decode(&wd, WD_FIXTURE_V1, sizeof(WD_FIXTURE_V1) - 1u));
    TEST_ASSERT_NOT_NULL(zbw_find(&wd, 9));
    TEST_ASSERT_EQUAL_UINT(ZBW_MODE_BURGLAR, zbw_find(&wd, 9)->mode);
    TEST_ASSERT_TRUE(wd.dirty);
    TEST_ASSERT_EQUAL_UINT(sent, fake.count);
    setUp();

    uint8_t b[sizeof(WD_FIXTURE_V1) + 1u];
    for (size_t cut = 0; cut < sizeof(WD_FIXTURE_V1); cut++) {
        TEST_ASSERT_FALSE(zbw_decode(&wd, WD_FIXTURE_V1, cut));
        TEST_ASSERT_NULL(zbw_find(&wd, 3));
    }
    TEST_ASSERT_FALSE(zbw_decode(&wd, NULL, 4));

    static const struct {
        size_t at;
        uint8_t value;
    } edits[] = {
        {0, 0x00},  /* compat 0 */
        {0, 0x02},  /* a newer compat this reader can't read */
        {1, 0x05},  /* more sirens than there are */
        {1, 0x03},  /* a third entry that isn't there */
        {1, 0x01},  /* one entry and the other's bytes left over */
        {2, 0x12},  /* an entry longer than what is left */
        {3, 0x07},  /* id 7 twice */
        {3, 0x00},  /* id 0 ... */
        {4, 0x00},  /* ... still 3 (high byte): fine, but combined below */
        {13, 0x00}, /* endpoint 0 */
        {14, 0x00}, /* no partition */
        {14, 0x10}, /* a partition this build hasn't */
        {16, 0x04}, /* volume past very high */
        {18, 0x1E}, /* supervision 30 min */
    };
    for (size_t i = 0; i < sizeof(edits) / sizeof(edits[0]); i++) {
        setUp(); /* start-up: no sirens yet */
        memcpy(b, WD_FIXTURE_V1, sizeof(WD_FIXTURE_V1));
        b[edits[i].at] = edits[i].value;
        const bool ok = zbw_decode(&wd, b, sizeof(WD_FIXTURE_V1));
        if (edits[i].at == 4u) {
            TEST_ASSERT_TRUE(ok); /* id 3's high byte 0: unchanged */
            continue;
        }
        TEST_ASSERT_FALSE_MESSAGE(ok, "a damaged blob read");
        TEST_ASSERT_NULL(zbw_find(&wd, 3));
        TEST_ASSERT_NULL(zbw_find(&wd, 7));
    }
    /* the same address and endpoint twice */
    memcpy(b, WD_FIXTURE_V1, sizeof(WD_FIXTURE_V1));
    b[20 + 3] = 0x01; /* the second entry's ieee low byte: IEEE_1 */
    b[20 + 11] = 43;  /* and its endpoint */
    TEST_ASSERT_FALSE(zbw_decode(&wd, b, sizeof(WD_FIXTURE_V1)));
    /* a trailing byte */
    memcpy(b, WD_FIXTURE_V1, sizeof(WD_FIXTURE_V1));
    b[sizeof(WD_FIXTURE_V1)] = 0;
    TEST_ASSERT_FALSE(zbw_decode(&wd, b, sizeof(b)));
}

/* ZB-023: a later format's longer entry reads (its new fields skipped); an older, shorter entry
 * reads its missing fields as 0; the device byte is ignored when read. */
static void test_wd_entry_lengths(void)
{
    /* entry of 12: id 9, IEEE_5, endpoint 2, partition 1 - sounds for nothing, volume low */
    static const uint8_t shorter[] = {0x01, 0x01, 0x0C, 0x09, 0x00, 0x05, 0x00, 0x00, 0x00,
                                      0x00, 0xCD, 0xAB, 0x02, 0x02, 0x01};
    TEST_ASSERT_TRUE(zbw_decode(&wd, shorter, sizeof(shorter)));
    const zbw_siren_t *s = zbw_find(&wd, 9);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(s->a.ieee == IEEE_5);
    TEST_ASSERT_FALSE(s->a.burglary || s->a.fire || s->a.strobe || s->a.squawk);
    TEST_ASSERT_EQUAL_UINT8(0, s->a.volume);
    TEST_ASSERT_EQUAL_UINT16(0, s->a.supervision_min);

    uint8_t longer[sizeof(WD_FIXTURE_V1) + 4u];
    /* the first entry grown by 2 bytes, the second by 2, flags' high bits set (future) */
    size_t at = 0;
    longer[at++] = 0x01;
    longer[at++] = 0x02;
    longer[at++] = 0x13;
    memcpy(&longer[at], &WD_FIXTURE_V1[3], 17);
    longer[at + 12] |= 0xF0u;
    longer[at + 14] = 0x05; /* the device byte: ignored */
    at += 17;
    longer[at++] = 0xEE;
    longer[at++] = 0xEE;
    longer[at++] = 0x13;
    memcpy(&longer[at], &WD_FIXTURE_V1[21], 17);
    at += 17;
    longer[at++] = 0xEE;
    longer[at++] = 0xEE;
    TEST_ASSERT_EQUAL_size_t(sizeof(longer), at);
    TEST_ASSERT_TRUE(zbw_decode(&wd, longer, sizeof(longer)));
    s = zbw_find(&wd, 3);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(s->a.burglary && s->a.fire && s->a.strobe && !s->a.squawk);
    TEST_ASSERT_EQUAL_UINT8(0, s->a.device);
    TEST_ASSERT_EQUAL_UINT16(120, zbw_find(&wd, 7)->a.supervision_min);
    uint8_t buf[ZBW_BLOB_MAX];
    size_t n = zbw_encode(&wd, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(WD_FIXTURE_V1, buf, n); /* written back in v1 */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_warning_info_every_byte);
    RUN_TEST(test_squawk_every_byte);
    RUN_TEST(test_golden_start_warning);
    RUN_TEST(test_golden_squawk);
    RUN_TEST(test_project_bytes);
    RUN_TEST(test_decode_refusals);
    RUN_TEST(test_constants);
    RUN_TEST(test_mode_for);
    RUN_TEST(test_policy_table);
    RUN_TEST(test_policy_most_urgent);
    RUN_TEST(test_policy_strobe);
    RUN_TEST(test_refresh_timing);
    RUN_TEST(test_max_duration_bytes);
    RUN_TEST(test_refresh_follows_the_read_limit);
    RUN_TEST(test_refresh_retries_fit);
    RUN_TEST(test_unconfirmed_is_bell_trouble);
    RUN_TEST(test_no_default_rsp_and_radio_down);
    RUN_TEST(test_squawk_policy);
    RUN_TEST(test_test_sound);
    RUN_TEST(test_supervision_silence);
    RUN_TEST(test_zone_status_bits);
    RUN_TEST(test_assign_and_clear);
    RUN_TEST(test_clear_and_move_stop);
    RUN_TEST(test_wd_frozen_fixture);
    RUN_TEST(test_wd_round_trip);
    RUN_TEST(test_wd_damaged);
    RUN_TEST(test_wd_entry_lengths);
    return UNITY_END();
}
