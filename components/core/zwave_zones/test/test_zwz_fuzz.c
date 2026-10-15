/*
 * test_zwz_fuzz.c - the Z-Wave panel's stored blobs, its Security 2 receive path and the bridge
 * into the zone source against random and mutated input, with fixed seeds (docs/features/36
 * ZW-023; ZW-009 for the class rule). The same approach as zigbee_zones' test_zbz_fuzz.c.
 *
 * What each case proves:
 * - the stored blobs (zwz_cfg, zwz_keys, zwz_checkins, zwf keypad feedback, zwkc keypad control,
 *   zwo garage, zw_prov SmartStart list, zw_nodes node cache): no crash and no read past the
 *   input - each input is decoded twice, once with 0x00 and once with 0xFF bytes after its end,
 *   and both must say the same; no write past the output - every encoder writes in front of
 *   guard bytes, and with a cap one byte short writes nothing and answers 0; a refused blob
 *   leaves the empty value where the decoder promises it; and whatever decodes encodes back the
 *   same: decode(encode(decode(x))) == decode(x), compared through the encoding. Half of the
 *   mutated inputs of a blob with a check value get it recomputed, so the body's checks run;
 * - the S2 receive path: a panel and a device with fixed keys over a pretend radio establish a
 *   SPAN; the panel's whole state is kept before each frame it accepted, and each mutated copy of
 *   that frame goes into that same state: nothing is delivered encrypted, and nothing a node
 *   supports only securely is delivered in the clear. Random S2 commands (KEX, Nonce Report,
 *   Message Encapsulation) from the device and from a stranger deliver nothing encrypted. The
 *   S2 command parsers (encapsulation, extensions as on decrypted plaintext, KEX, nonces) read
 *   the same with either poison after their input;
 * - the bridge: a door sensor joined at each grant over the fake ZAC93 and bound to a zone gets
 *   random and mutated commands at every class it holds: a frame below its highest granted class
 *   never makes a zone event (ZW-009), and at its own class nothing crashes; random commands and
 *   reports go straight into zwz_received and zsrc_zw_report too. Below its class nothing is a
 *   check-in either; a CC the device lists nowhere is held to its class too
 *   (test_bridge_unlisted_cc_below_class).
 * Inputs handed to the receive paths sit at the very end of their array, so a read past them
 * leaves the object: the host build's sanitizers (ALARM_SANITIZE) see it.
 *
 * The random numbers are the test's own (splitmix64 from fixed seeds), so a failure replays.
 */
#include <stdio.h>
#include <string.h>

#include "fake_zac93.h"
#include "fake_zdev.h"
#include "unity.h"
#include "zone_hub/zone_hub.h"
#include "zone_hub/zsrc_zwave.h"
#include "zw_host_crypto.h"
#include "zwave_host/zw_nodes.h"
#include "zwave_host/zw_prov.h"
#include "zwave_host/zw_s2.h"
#include "zwave_host/zw_s2_cc.h"
#include "zwave_proto/zw_api.h"
#include "zwave_proto/zw_cc.h"
#include "zwave_proto/zw_crc16.h"
#include "zwave_zones/zw_feedback.h"
#include "zwave_zones/zw_keyctl.h"
#include "zwave_zones/zw_outputs.h"
#include "zwave_zones/zwave_zones.h"
#include "zwave_zones/zwz_panel.h"

#include "feedback_fixture_v1.h"
#include "zw_nodes_fixture_v1.h"
#include "zw_nodes_fixture_v2.h"
#include "zw_prov_fixture_v1.h"

/* ---- fixed-seed random numbers and mutations ------------------------------------------------ */

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
    static const uint8_t edges[] = {0x00, 0x01, 0x02, 0x7F, 0x80, 0xFE, 0xFF, 0x10, 0x20, 0x38};
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
#define SAME_MEM_MSG(x, y, n, m)                                                                   \
    do {                                                                                           \
        if ((n) > 0u) {                                                                            \
            TEST_ASSERT_EQUAL_MEMORY_MESSAGE((x), (y), (n), (m));                                  \
        }                                                                                          \
    } while (0)

/* A failure's input, as hex, in its message. */
static char g_msg[1024];

static const char *hex_msg(const char *what, const uint8_t *b, size_t len)
{
    int at = snprintf(g_msg, sizeof(g_msg), "%s; input (%u bytes):", what, (unsigned)len);
    for (size_t i = 0; i < len && at > 0 && (size_t)at + 4u < sizeof(g_msg); i++) {
        at += snprintf(&g_msg[at], sizeof(g_msg) - (size_t)at, " %02X", (unsigned)b[i]);
    }
    return g_msg;
}

/* ==== the stored blobs ======================================================================= */

#define TAIL    32u    /* poison bytes after an input */
#define GUARD   16u    /* guard bytes after an output */
#define IN_MAX  8400u  /* the longest input (a SmartStart list's count x entry size) */
#define OUT_MAX 32768u /* the room an encoder gets */

static uint8_t g_a[IN_MAX + TAIL], g_b[IN_MAX + TAIL];

/* `in` twice, each followed by TAIL bytes of its poison. */
static void twin(const uint8_t *in, size_t len)
{
    TEST_ASSERT_TRUE(len <= IN_MAX);
    memset(g_a, 0x00, sizeof(g_a));
    memset(g_b, 0xFF, sizeof(g_b));
    if (len > 0u) {
        memcpy(g_a, in, len);
        memcpy(g_b, in, len);
    }
}

/* One stored format: decode into its own object (left non-empty first, so a refusal that keeps
 * anything shows), encode that object, and what a refusal must leave. */
typedef struct {
    const char *name;
    bool (*decode)(const uint8_t *in, size_t len);
    size_t (*encode)(uint8_t *out, size_t cap);
    bool (*empty)(void);                    /* NULL: the decoder promises nothing on a refusal */
    void (*reseal)(uint8_t *b, size_t len); /* its check value made right; NULL: none */
    bool short_writes; /* a short cap may write inside it (zw_nodes: payload, never its header) */
    unsigned ok;                            /* decodes that said yes */
} codec_t;

static uint8_t g_out[OUT_MAX + GUARD], g_short[OUT_MAX + GUARD];

/* The encoding into dst, after checking the guard and that a cap one byte short writes nothing at
 * all and answers 0. */
static size_t encode_guarded(const codec_t *c, uint8_t *dst)
{
    memset(g_out, 0xA5, sizeof(g_out));
    const size_t n = c->encode(g_out, OUT_MAX);
    TEST_ASSERT_TRUE(n <= OUT_MAX);
    for (size_t i = n; i < n + GUARD; i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xA5, g_out[i], c->name);
    }
    if (n > 0u) {
        memset(g_short, 0x5A, n + GUARD);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, c->encode(g_short, n - 1u), c->name);
        for (size_t i = c->short_writes ? n - 1u : 0u; i < n - 1u + GUARD; i++) {
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x5A, g_short[i], c->name); /* a short cap wrote */
        }
        memcpy(dst, g_out, n);
    }
    return n;
}

static uint8_t g_ea[OUT_MAX], g_eb[OUT_MAX], g_ec[OUT_MAX];

static void check_blob(codec_t *c, const uint8_t *in, size_t len)
{
    twin(in, len);
    const bool ok = c->decode(g_a, len);
    if (!ok && c->empty != NULL) {
        TEST_ASSERT_TRUE_MESSAGE(c->empty(), hex_msg("a refused blob left a value", in, len));
    }
    const size_t na = encode_guarded(c, g_ea);
    TEST_ASSERT_EQUAL_MESSAGE(ok, c->decode(g_b, len), hex_msg("read past the end", in, len));
    const size_t nb = encode_guarded(c, g_eb);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(na, nb, hex_msg("read past the end", in, len));
    SAME_MEM_MSG(g_ea, g_eb, na, hex_msg("read past the end", in, len));
    if (ok) {
        c->ok++;
        TEST_ASSERT_TRUE_MESSAGE(na > 0u, hex_msg("decoded, not encoded", in, len));
        TEST_ASSERT_TRUE_MESSAGE(c->decode(g_ea, na), hex_msg("its encoding refused", in, len));
        const size_t nc = encode_guarded(c, g_ec);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(na, nc, hex_msg("not the same again", in, len));
        SAME_MEM_MSG(g_ea, g_ec, na, hex_msg("not the same again", in, len));
    }
}

/* A codec's fuzz run: its seeds as they are, then mutated and random inputs of up to `cap`
 * bytes, half the mutated ones resealed. */
static void fuzz_codec(codec_t *c, uint64_t s, unsigned rounds, size_t cap,
                       const uint8_t *const *seeds, const size_t *seed_len, size_t seed_count)
{
    static uint8_t in[IN_MAX];
    TEST_ASSERT_TRUE(cap <= IN_MAX);
    for (size_t k = 0; k < seed_count; k++) {
        const unsigned before = c->ok;
        check_blob(c, seeds[k], seed_len[k]);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(before + 1u, c->ok, c->name); /* every seed decodes */
    }
    seed(s);
    for (unsigned i = 0; i < rounds; i++) {
        size_t len = make_input(in, cap, seeds, seed_len, seed_count);
        if (c->reseal != NULL && rnd_below(2u) == 0u) {
            c->reseal(in, len);
        }
        check_blob(c, in, len);
    }
}

static void le16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void le32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4u; i++) {
        p[i] = (uint8_t)((v >> (8u * i)) & 0xFFu);
    }
}

/* ---- zwz_cfg (NVS zwave/cfg) ---- */

static zwz_cfg_t g_cfg;
static const zwz_cfg_t CFG_FULL = {true, 2u, 3600u, true, -70, true, -15, true, 140};

static bool cfg_decode(const uint8_t *in, size_t len)
{
    g_cfg = CFG_FULL;
    return zwz_cfg_decode(&g_cfg, in, len);
}
static size_t cfg_encode(uint8_t *out, size_t cap)
{
    return zwz_cfg_encode(&g_cfg, out, cap);
}
static bool cfg_empty(void)
{
    return !g_cfg.region_set && g_cfg.region == 0u && g_cfg.check_in_s == 0u &&
           !g_cfg.s0_allowed && g_cfg.jam_dbm == 0 && !g_cfg.power_cap_set &&
           g_cfg.power_cap_dbm10 == 0 && !g_cfg.lr_cap_set && g_cfg.lr_cap_dbm10 == 0;
}

/* ---- zwz_keys (NVS zwave/keys) ---- */

static zwz_keys_t g_keys;
static zwz_keys_t g_keys_full;

static bool keys_decode(const uint8_t *in, size_t len)
{
    g_keys = g_keys_full;
    return zwz_keys_decode(&g_keys, in, len);
}
static size_t keys_encode(uint8_t *out, size_t cap)
{
    return zwz_keys_encode(&g_keys, out, cap);
}
static bool keys_empty(void)
{
    static const zw_s2_keys_t none;
    return !g_keys.bound && g_keys.home_id == 0u &&
           memcmp(&g_keys.keys, &none, sizeof(none)) == 0;
}

/* ---- zwz_checkins (NVS zwave/chk) ---- */

static zwz_checkin_t g_chk[ZWZ_PANEL_CHECKINS];
static size_t g_chk_n;

static bool chk_decode(const uint8_t *in, size_t len)
{
    g_chk_n = 3u;
    return zwz_checkins_decode(g_chk, ZWZ_PANEL_CHECKINS, &g_chk_n, in, len);
}
static size_t chk_encode(uint8_t *out, size_t cap)
{
    return zwz_checkins_encode(g_chk, g_chk_n, out, cap);
}
static bool chk_empty(void)
{
    return g_chk_n == 0u;
}

/* ---- zwf (keypad feedback) ---- */

static zwf_t g_fb;

static bool fb_decode(const uint8_t *in, size_t len)
{
    TEST_ASSERT_TRUE(zwf_decode(&g_fb, FEEDBACK_FIXTURE_V1, sizeof(FEEDBACK_FIXTURE_V1)));
    return zwf_decode(&g_fb, in, len);
}
static size_t fb_encode(uint8_t *out, size_t cap)
{
    return zwf_encode(&g_fb, out, cap);
}
static bool fb_empty(void)
{
    for (size_t i = 0; i < ZWF_TARGETS; i++) {
        const zwf_binding_t *b = &g_fb.target[i].binding;
        if (b->partition != 0u || b->node != 0u || b->home != 0u || b->generation != 0u) {
            return false;
        }
    }
    return true;
}
static void fb_reseal(uint8_t *b, size_t len)
{
    if (len == ZWF_BLOB_SIZE) {
        le16(&b[len - 2u], zw_crc16(ZW_CRC16_INIT, b, len - 2u));
    }
}

/* ---- zwkc (keypad control) ---- */

static bool kc_command(void *ctx, const alarm_cmd_t *cmd, const alarm_source_t *source,
                       zwk_reply_t *reply)
{
    (void)ctx;
    (void)cmd;
    (void)source;
    (void)reply;
    TEST_FAIL_MESSAGE("decoding ran a keypad command");
    return false;
}

static const zwk_port_t KC_PORT = {NULL, kc_command};
static zwkc_t g_kc;
static uint8_t KC_SEED[ZWKC_BLOB_SIZE];

static bool kc_decode(const uint8_t *in, size_t len)
{
    zwkc_init(&g_kc, &KC_PORT);
    TEST_ASSERT_TRUE(zwkc_decode(&g_kc, KC_SEED, sizeof(KC_SEED)));
    return zwkc_decode(&g_kc, in, len);
}
static size_t kc_encode(uint8_t *out, size_t cap)
{
    return zwkc_encode(&g_kc, out, cap);
}
static bool kc_empty(void)
{
    for (size_t i = 0; i < ZWKC_SLOTS; i++) {
        if (g_kc.slot[i].binding.partition != 0u || g_kc.slot[i].binding.node != 0u) {
            return false;
        }
    }
    return true;
}
static void kc_reseal(uint8_t *b, size_t len)
{
    if (len == ZWKC_BLOB_SIZE) {
        le16(&b[len - 2u], zw_crc16(ZW_CRC16_INIT, b, len - 2u));
    }
}

/* A slot of the keypad control blob, by hand from zw_keyctl.h's layout. */
static void kc_slot(uint8_t *p, uint32_t home, unsigned node, unsigned gen, uint8_t ep,
                    uint8_t partition, uint8_t security, uint8_t panic)
{
    le32(&p[0], home);
    le16(&p[4], node);
    le16(&p[6], gen);
    p[8] = ep;
    p[9] = partition;
    p[10] = security;
    p[11] = panic;
}

/* ---- zwo garage ---- */

static bool go_send(void *ctx, uint16_t node, uint8_t ep, const uint8_t *cmd, size_t len,
                    uint32_t tag)
{
    (void)ctx;
    (void)node;
    (void)ep;
    (void)cmd;
    (void)len;
    (void)tag;
    return true;
}
static zwo_position_t go_position(void *ctx, uint16_t zone)
{
    (void)ctx;
    (void)zone;
    return ZWO_POS_UNKNOWN;
}
static void go_result(void *ctx, uint32_t cmd, zwo_result_t r)
{
    (void)ctx;
    (void)cmd;
    (void)r;
}
static void go_changed(void *ctx)
{
    (void)ctx;
}

static zwo_t g_go, g_go_full;
static uint8_t GA_SEED[ZWO_GARAGE_BLOB];

static void go_output(zwo_t *o, size_t i, unsigned node, unsigned gen, uint8_t ep, uint8_t cc)
{
    o->out[i].used = true;
    o->out[i].node = (uint16_t)node;
    o->out[i].generation = (uint16_t)gen;
    o->out[i].endpoint = ep;
    o->out[i].cc = cc;
    o->out[i].kind = (uint8_t)(cc == (uint8_t)ZW_CC_BINARY_SWITCH ? ZWO_RELAY : ZWO_DIMMER);
    o->out[i].level = -1;
}

static bool go_decode(const uint8_t *in, size_t len)
{
    g_go = g_go_full; /* the outputs scanned, both garages assigned */
    return zwo_garage_decode(&g_go, in, len);
}
static size_t go_encode(uint8_t *out, size_t cap)
{
    return zwo_garage_encode(&g_go, out, cap);
}
static void go_reseal(uint8_t *b, size_t len)
{
    if (len >= ZWO_GARAGE_BLOB) {
        le16(&b[ZWO_GARAGE_BLOB - 2u], zw_crc16(ZW_CRC16_INIT, b, ZWO_GARAGE_BLOB - 2u));
    }
}

/* ---- zw_prov (SmartStart list) ---- */

static zw_prov_t g_prov;

static bool prov_decode(const uint8_t *in, size_t len)
{
    TEST_ASSERT_TRUE(zw_prov_decode(&g_prov, ZW_PROV_FIXTURE_V1, sizeof(ZW_PROV_FIXTURE_V1)));
    return zw_prov_decode(&g_prov, in, len);
}
static size_t prov_encode(uint8_t *out, size_t cap)
{
    return zw_prov_encode(&g_prov, out, cap);
}
static bool prov_empty(void)
{
    return g_prov.n == 0u;
}
/* Its CRC-16 (big-endian, zw_prov.c) after the count and entry size the blob states. */
static void prov_reseal(uint8_t *b, size_t len)
{
    if (len < 7u) {
        return;
    }
    const size_t want = 5u + (size_t)b[3] * (size_t)b[4] + 2u;
    if (want <= len) {
        const uint16_t crc = zw_crc16(ZW_CRC16_INIT, b, want - 2u);
        b[want - 2u] = (uint8_t)(crc >> 8);
        b[want - 1u] = (uint8_t)(crc & 0xFFu);
    }
}

/* ---- zw_nodes (the node cache) ---- */

static zw_node_cache_t g_nc, g_nc_full;

static bool nc_decode(const uint8_t *in, size_t len)
{
    memcpy(&g_nc, &g_nc_full, sizeof(g_nc));
    return zw_node_cache_decode(&g_nc, in, len);
}
static size_t nc_encode(uint8_t *out, size_t cap)
{
    return zw_node_cache_encode(&g_nc, out, cap);
}
/* zw_nodes.h: a refused blob leaves the cache as it was (nc_decode starts from the full one) or
 * empty. */
static bool nc_unchanged_or_empty(void)
{
    return memcmp(&g_nc, &g_nc_full, sizeof(g_nc)) == 0 ||
           (g_nc.node_count == 0u && g_nc.unknown_groups_len == 0u);
}
/* Its CRC-32 over the payload the header states. */
static void nc_reseal(uint8_t *b, size_t len)
{
    if (len < 16u) {
        return;
    }
    const size_t hdr = (size_t)b[6] | ((size_t)b[7] << 8);
    const size_t payload = (size_t)b[8] | ((size_t)b[9] << 8) | ((size_t)b[10] << 16) |
                           ((size_t)b[11] << 24);
    if (hdr >= 16u && hdr <= len && payload <= len - hdr) {
        le32(&b[12], zw_nodes_crc32(0u, &b[hdr], payload));
    }
}

/* ZW-023: the installer's settings, the network keys and the check-ins kept - mutated from
 * values the encoders wrote (and the 8-byte settings older firmware wrote) and random. */
static void test_panel_blobs_fuzz(void)
{
    static codec_t cfg = {"zwz_cfg", cfg_decode, cfg_encode, cfg_empty, NULL, false, 0u};
    uint8_t cfg_new[ZWZ_CFG_BLOB];
    TEST_ASSERT_EQUAL_UINT(ZWZ_CFG_BLOB, zwz_cfg_encode(&CFG_FULL, cfg_new, sizeof(cfg_new)));
    const uint8_t *cs[2] = {cfg_new, cfg_new};
    const size_t cl[2] = {ZWZ_CFG_BLOB, ZWZ_CFG_BLOB_MIN};
    fuzz_codec(&cfg, 0xC0F1u, 8000u, 24u, cs, cl, 2u);

    static codec_t keys = {"zwz_keys", keys_decode, keys_encode, keys_empty, NULL, false, 0u};
    memset(&g_keys_full, 0, sizeof(g_keys_full));
    g_keys_full.bound = true;
    g_keys_full.home_id = 0xC0FFEE01u;
    memset(g_keys_full.keys.s2_unauth, 0x11, 16u);
    memset(g_keys_full.keys.s2_auth, 0x22, 16u);
    memset(g_keys_full.keys.s2_access, 0x33, 16u);
    memset(g_keys_full.keys.s0, 0x44, 16u);
    memset(g_keys_full.keys.lr_auth, 0x55, 16u);
    memset(g_keys_full.keys.lr_access, 0x66, 16u);
    uint8_t keys_blob[ZWZ_KEYS_BLOB];
    TEST_ASSERT_EQUAL_UINT(ZWZ_KEYS_BLOB,
                           zwz_keys_encode(&g_keys_full, keys_blob, sizeof(keys_blob)));
    const uint8_t *ks[1] = {keys_blob};
    const size_t kl[1] = {ZWZ_KEYS_BLOB};
    fuzz_codec(&keys, 0x4B455953u, 5000u, ZWZ_KEYS_BLOB + 16u, ks, kl, 1u);

    static codec_t chk = {"zwz_checkins", chk_decode, chk_encode, chk_empty, NULL, false, 0u};
    const zwz_checkin_t three[3] = {{5u, 1u, 3600u}, {9u, 2u, 43200u}, {232u, 7u, 0u}};
    uint8_t chk_blob[ZWZ_CHECKINS_BLOB];
    const size_t chk_len = zwz_checkins_encode(three, 3u, chk_blob, sizeof(chk_blob));
    TEST_ASSERT_EQUAL_UINT(2u + 3u * 8u, chk_len);
    const uint8_t *hs[1] = {chk_blob};
    const size_t hl[1] = {chk_len};
    fuzz_codec(&chk, 0xC4Bu, 8000u, ZWZ_CHECKINS_BLOB + 16u, hs, hl, 1u);

    TEST_ASSERT_TRUE(cfg.ok > 500u && keys.ok > 200u && chk.ok > 500u);
}

/* ZW-023: what keypads and outputs were given - feedback targets, keypad control slots and
 * garages - mutated from the frozen fixture and hand-made blobs, and random. */
static void test_assignment_blobs_fuzz(void)
{
    static codec_t fb = {"zwf", fb_decode, fb_encode, fb_empty, fb_reseal, false, 0u};
    zwf_init(&g_fb);
    const uint8_t *fs[1] = {FEEDBACK_FIXTURE_V1};
    const size_t fl[1] = {sizeof(FEEDBACK_FIXTURE_V1)};
    fuzz_codec(&fb, 0x4B46u, 8000u, ZWF_BLOB_SIZE + 8u, fs, fl, 1u);

    static codec_t kc = {"zwkc", kc_decode, kc_encode, kc_empty, kc_reseal, false, 0u};
    memset(KC_SEED, 0, sizeof(KC_SEED));
    KC_SEED[0] = 'K';
    KC_SEED[1] = 'C';
    KC_SEED[2] = 1u;
    KC_SEED[3] = (uint8_t)ZWKC_SLOTS;
    kc_slot(&KC_SEED[4], 0xA1B2C3D4u, 5u, 1u, 0u, 1u, (uint8_t)ZW_SEC_S2_AUTH, 0x07u);
    kc_slot(&KC_SEED[4u + 12u * 3u], 0xA1B2C3D4u, 9u, 3u, 1u, 2u, (uint8_t)ZW_SEC_S2_ACCESS, 0u);
    kc_reseal(KC_SEED, sizeof(KC_SEED));
    const uint8_t *ys[1] = {KC_SEED};
    const size_t yl[1] = {sizeof(KC_SEED)};
    fuzz_codec(&kc, 0x4B43u, 8000u, ZWKC_BLOB_SIZE + 8u, ys, yl, 1u);

    static codec_t go = {"zwo_garage", go_decode, go_encode, NULL, go_reseal, false, 0u};
    static const zwo_port_t port = {NULL, go_send, go_position, go_result, go_changed};
    zwo_init(&g_go_full, &port);
    go_output(&g_go_full, 0u, 7u, 2u, 0u, (uint8_t)ZW_CC_BINARY_SWITCH);
    go_output(&g_go_full, 1u, 8u, 4u, 1u, (uint8_t)ZW_CC_BINARY_SWITCH);
    go_output(&g_go_full, 2u, 10u, 1u, 0u, (uint8_t)ZW_CC_MULTILEVEL_SWITCH);
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_garage_assign(&g_go_full, 0u, 0u, 3u, 30u));
    TEST_ASSERT_EQUAL_INT(ZWO_OK, zwo_garage_assign(&g_go_full, 1u, 1u, 12u, 0u));
    TEST_ASSERT_EQUAL_UINT(ZWO_GARAGE_BLOB,
                           zwo_garage_encode(&g_go_full, GA_SEED, sizeof(GA_SEED)));
    const uint8_t *gs[1] = {GA_SEED};
    const size_t gl[1] = {sizeof(GA_SEED)};
    fuzz_codec(&go, 0x4741u, 8000u, ZWO_GARAGE_BLOB + 8u, gs, gl, 1u);

    TEST_ASSERT_TRUE(fb.ok > 200u && kc.ok > 200u && go.ok > 200u);
}

/* ZW-023: the SmartStart list and the node cache, mutated from their frozen fixtures and random.
 * zw_nodes.h's promises: a refused cache is as it was or empty; a short cap writes nothing at or
 * past the cap, and never the header (its first 16 bytes). */
static void test_network_blobs_fuzz(void)
{
    static codec_t prov = {"zw_prov", prov_decode, prov_encode, prov_empty, prov_reseal, false, 0u};
    const uint8_t *ps[1] = {ZW_PROV_FIXTURE_V1};
    const size_t pl[1] = {sizeof(ZW_PROV_FIXTURE_V1)};
    fuzz_codec(&prov, 0x5056u, 5000u, sizeof(ZW_PROV_FIXTURE_V1) + 64u, ps, pl, 1u);

    static codec_t nc = {"zw_nodes", nc_decode, nc_encode, nc_unchanged_or_empty, nc_reseal, true,
                         0u};
    zw_node_cache_init(&g_nc_full);
    TEST_ASSERT_TRUE(
        zw_node_cache_decode(&g_nc_full, ZW_NODES_FIXTURE_V1, sizeof(ZW_NODES_FIXTURE_V1)));
    TEST_ASSERT_TRUE(g_nc_full.node_count > 0u);
    const uint8_t *ns[2] = {ZW_NODES_FIXTURE_V1, ZW_NODES_FIXTURE_V2};
    const size_t nl[2] = {sizeof(ZW_NODES_FIXTURE_V1), sizeof(ZW_NODES_FIXTURE_V2)};
    fuzz_codec(&nc, 0x5A574E44u, 4000u, sizeof(ZW_NODES_FIXTURE_V1) + 64u, ns, nl, 2u);
    /* a short cap, at every length below the encoding: 0, the header untouched, nothing past it */
    memcpy(&g_nc, &g_nc_full, sizeof(g_nc));
    const size_t full = zw_node_cache_encode(&g_nc, g_out, OUT_MAX);
    TEST_ASSERT_TRUE(full > 16u && full + GUARD <= sizeof(g_short));
    for (size_t cap = 0u; cap < full; cap++) {
        memset(g_short, 0x5A, full + GUARD);
        TEST_ASSERT_EQUAL_UINT(0u, zw_node_cache_encode(&g_nc, g_short, cap));
        for (size_t i = 0u; i < full + GUARD; i++) {
            if (i < 16u || i >= cap) {
                TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x5A, g_short[i], "zw_nodes: a short cap wrote");
            }
        }
    }

    TEST_ASSERT_TRUE(prov.ok > 200u && nc.ok > 200u);
}

/* ==== Security 2 ============================================================================= */

#define PANEL    1u
#define DEV      2u
#define STRANGER 9u
#define HOME     0xC0FFEE01u
#define AIR      64u
#define CAPS     8u
#define ALL_S2   (ZW_S2_KEY_UNAUTH | ZW_S2_KEY_AUTH | ZW_S2_KEY_ACCESS)

/* The device's node information as the panel has it: everything else only securely. */
static const uint8_t S_NIF[] = {0x5E, 0x55, 0x9F, 0x6C};

typedef struct {
    uint16_t from, to;
    uint32_t tag;
    size_t len;
    uint8_t b[ZW_S2_FRAME_MAX];
} air_t;

/* A frame the panel accepted, and the panel as it was just before. */
typedef struct {
    zw_s2_t panel;
    zw_host_crypto_t hc;
    uint64_t now;
    zw_sec_class_t sec;
    size_t len;
    uint8_t b[ZW_S2_FRAME_MAX];
} capture_t;

static struct {
    zw_host_crypto_t phc, dhc;
    zw_crypto_port_t pcp, dcp;
    zw_s2_t panel, dev;
    zw_s2_keys_t keys;
    uint64_t now;
    air_t air[AIR];
    size_t head, tail;
    bool capturing;
    capture_t cap[CAPS];
    size_t cap_n;
    unsigned got;            /* commands the panel delivered */
    unsigned got_secure;     /* ...of them encrypted */
    unsigned got_secure_only; /* ...unencrypted, of a CC the device supports only securely */
    zw_sec_class_t last_sec;
} S;

static bool s_on_air(uint16_t from, uint16_t to, const uint8_t *f, size_t len, uint32_t tag)
{
    if (S.tail - S.head >= AIR || len > ZW_S2_FRAME_MAX) {
        return false;
    }
    air_t *a = &S.air[S.tail++ % AIR];
    a->from = from;
    a->to = to;
    a->tag = tag;
    a->len = len;
    memcpy(a->b, f, len);
    return true;
}

static bool s_panel_send(void *ctx, uint16_t node, const uint8_t *f, size_t len, uint8_t opt,
                         uint32_t tag)
{
    (void)ctx;
    (void)opt;
    return s_on_air(PANEL, node, f, len, tag);
}

static bool s_dev_send(void *ctx, uint16_t node, const uint8_t *f, size_t len, uint8_t opt,
                       uint32_t tag)
{
    (void)ctx;
    (void)opt;
    return s_on_air(DEV, node, f, len, tag);
}

/* What the panel may deliver in the clear from the device: its NIF's CCs, and the CCs zw_s2.c's
 * cc_secure() never counts as secure (Security 0, Security 2, Transport Service). */
static bool s_insecure_cc(uint8_t cc)
{
    if (cc == 0x98u || cc == 0x9Fu || cc == 0x55u) {
        return true;
    }
    for (size_t i = 0; i < sizeof(S_NIF); i++) {
        if (S_NIF[i] == cc) {
            return true;
        }
    }
    return false;
}

static void s_received(void *ctx, const zw_s2_rx_t *rx)
{
    if (ctx != (void *)&S.panel) {
        return; /* the device's: the panel's frames to it */
    }
    TEST_ASSERT_NOT_NULL(rx->command);
    TEST_ASSERT_TRUE(rx->len > 0u && rx->len <= ZW_S2_RX_MAX);
    unsigned sum = 0u; /* every byte read: a delivered command past its frame shows (sanitizers) */
    for (size_t i = 0; i < rx->len; i++) {
        sum += rx->command[i];
    }
    (void)sum;
    S.got++;
    S.last_sec = rx->security;
    if (rx->security != ZW_SEC_NONE) {
        S.got_secure++;
    } else if (rx->node == DEV && !s_insecure_cc(rx->command[0])) {
        S.got_secure_only++;
    }
}

static void s_send_done(void *ctx, uint32_t tag, zw_s2_result_t r)
{
    (void)ctx;
    (void)tag;
    (void)r;
}

static zw_s2_t *s_side(uint16_t node)
{
    return node == PANEL ? &S.panel : &S.dev;
}

/* The air settles; while capturing, the panel's state is kept before each encapsulated frame
 * from the device, and the capture stays when the panel delivered it. */
static void s_pump(void)
{
    while (S.head != S.tail) {
        const air_t a = S.air[S.head++ % AIR];
        if (a.to != PANEL && a.to != DEV) {
            continue;
        }
        capture_t *c = NULL;
        if (S.capturing && a.from == DEV && a.to == PANEL && a.len >= 2u && a.b[0] == 0x9Fu &&
            a.b[1] == 0x03u && S.cap_n < CAPS) {
            c = &S.cap[S.cap_n];
            c->panel = S.panel;
            c->hc = S.phc;
            c->now = S.now;
            c->len = a.len;
            memcpy(c->b, a.b, a.len);
        }
        const unsigned before = S.got;
        zw_s2_receive(s_side(a.to), S.now, a.from, a.b, a.len);
        if (c != NULL && S.got > before) {
            c->sec = S.last_sec;
            S.cap_n++;
        }
        zw_s2_sent(s_side(a.from), S.now, a.tag, true);
    }
}

static void s_setup(void)
{
    static const zw_s2_port_t pp = {&S.panel, s_panel_send, s_received, s_send_done, NULL};
    static const zw_s2_port_t dp = {&S.dev, s_dev_send, s_received, s_send_done, NULL};
    const zw_s2_config_t pcfg = {true, 0u};
    const zw_s2_config_t dcfg = {false, 0u};
    memset(&S, 0, sizeof(S));
    zw_host_crypto_init(&S.phc, 0x5041u);
    zw_host_crypto_init(&S.dhc, 0x4445u);
    S.pcp = zw_host_crypto_port(&S.phc);
    S.dcp = zw_host_crypto_port(&S.dhc);
    zw_s2_init(&S.panel, &S.pcp, &pp, &pcfg);
    zw_s2_init(&S.dev, &S.dcp, &dp, &dcfg);
    memset(S.keys.s2_unauth, 0x11, 16u);
    memset(S.keys.s2_auth, 0x22, 16u);
    memset(S.keys.s2_access, 0x33, 16u);
    memset(S.keys.s0, 0x44, 16u);
    memset(S.keys.lr_auth, 0x55, 16u);
    memset(S.keys.lr_access, 0x66, 16u);
    S.now = 1000u;
    TEST_ASSERT_TRUE(zw_s2_start(&S.panel, HOME, PANEL, &S.keys));
    TEST_ASSERT_TRUE(zw_s2_add_node(&S.panel, DEV, ALL_S2, S_NIF, sizeof(S_NIF)));
    TEST_ASSERT_TRUE(zw_s2_start(&S.dev, HOME, DEV, NULL));
    TEST_ASSERT_TRUE(zw_s2_set_key(&S.dev, ZW_S2_KEY_UNAUTH, S.keys.s2_unauth));
    TEST_ASSERT_TRUE(zw_s2_set_key(&S.dev, ZW_S2_KEY_AUTH, S.keys.s2_auth));
    TEST_ASSERT_TRUE(zw_s2_set_key(&S.dev, ZW_S2_KEY_ACCESS, S.keys.s2_access));
    TEST_ASSERT_TRUE(zw_s2_add_node(&S.dev, PANEL, ALL_S2, NULL, 0u));
}

static void s_from_dev(zw_sec_class_t cls, const uint8_t *cmd, size_t len)
{
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, zw_s2_send(&S.dev, S.now, PANEL, cls, cmd, len, 0x100u));
    s_pump();
    for (unsigned i = 0; i < 20u; i++) {
        S.now += 200u;
        zw_s2_poll(&S.panel, S.now);
        zw_s2_poll(&S.dev, S.now);
        s_pump();
    }
}

/* Commands the device sends; secure only, as the panel knows it. */
static const uint8_t DOOR_OPEN[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
static const uint8_t BATTERY_LOW[] = {0x80, 0x03, 0xFF};
static const uint8_t LONG_NOTIF[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x0F,
                                     1,    2,    3,    4,    5,    6,    7,    8,    9,
                                     10,   11,   12,   13,   14,   15};

/* The panel delivers the device's first frames (its SPAN established on the first), each kept
 * with the panel's state before it. */
static void s_capture(void)
{
    s_setup();
    S.capturing = true;
    s_from_dev(ZW_SEC_S2_ACCESS, DOOR_OPEN, sizeof(DOOR_OPEN));
    s_from_dev(ZW_SEC_S2_ACCESS, BATTERY_LOW, sizeof(BATTERY_LOW));
    s_from_dev(ZW_SEC_S2_ACCESS, LONG_NOTIF, sizeof(LONG_NOTIF));
    s_from_dev(ZW_SEC_S2_ACCESS, DOOR_OPEN, sizeof(DOOR_OPEN));
    S.capturing = false;
    TEST_ASSERT_EQUAL_UINT(4u, S.cap_n);
    TEST_ASSERT_EQUAL_UINT(4u, S.got_secure);
    TEST_ASSERT_EQUAL_UINT(0u, S.got_secure_only);
}

/* A frame at the very end of its array: a read past it leaves the object. */
static uint8_t g_edge[ZW_S2_RX_MAX];

static const uint8_t *at_edge(const uint8_t *b, size_t len)
{
    TEST_ASSERT_TRUE(len <= sizeof(g_edge));
    uint8_t *p = &g_edge[sizeof(g_edge) - len];
    if (len > 0u) {
        memmove(p, b, len);
    }
    return p;
}

/* The panel as it was before capture c, the air and the counts clear. */
static void s_restore(const capture_t *c)
{
    S.panel = c->panel;
    S.phc = c->hc;
    S.now = c->now;
    S.head = S.tail = 0u;
    S.got = S.got_secure = S.got_secure_only = 0u;
}

/* ZW-023, ZW-007, ZW-009: each frame the panel accepted, mutated and given to the panel in the
 * state it accepted it in, is never delivered encrypted, nor in the clear for a CC the device
 * supports only securely; the frame itself still is (the control). */
static void test_s2_mutated_frames_never_delivered(void)
{
    s_capture();
    for (size_t k = 0; k < S.cap_n; k++) {
        const capture_t *c = &S.cap[k];
        s_restore(c);
        zw_s2_receive(&S.panel, S.now, DEV, at_edge(c->b, c->len), c->len);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1u, S.got_secure, "the control frame was not delivered");
        TEST_ASSERT_EQUAL_INT(c->sec, S.last_sec);
    }
    static uint8_t buf[ZW_S2_RX_MAX];
    unsigned tried = 0u;
    seed(0x53324D55u);
    for (unsigned i = 0; i < 20000u; i++) {
        const capture_t *c = &S.cap[rnd_below(S.cap_n)];
        memcpy(buf, c->b, c->len);
        const size_t len = mutate(buf, c->len, sizeof(buf));
        if (len == c->len && memcmp(buf, c->b, len) == 0) {
            continue; /* not a mutation */
        }
        s_restore(c);
        zw_s2_receive(&S.panel, S.now, DEV, at_edge(buf, len), len);
        S.now += 10u;
        zw_s2_poll(&S.panel, S.now);
        if (S.got_secure != 0u || S.got_secure_only != 0u) {
            TEST_FAIL_MESSAGE(hex_msg("a mutated S2 frame was delivered", buf, len));
        }
        tried++;
    }
    TEST_ASSERT_TRUE(tried > 15000u);
}

/* ZW-023, ZW-009: random Security 2 commands - KEX, public keys, network keys, nonces, Message
 * Encapsulation - from the device with a SPAN and from a node of no network, alone and mutated
 * from well-formed ones, one after the other into the same panel: nothing is delivered
 * encrypted, and nothing from the stranger at all. The command parsers read each the same with
 * either poison after it. */
static void test_s2_random_commands(void)
{
    s_capture();
    s_restore(&S.cap[S.cap_n - 1u]);
    uint8_t b[64];
    uint8_t key[32];
    for (unsigned i = 0; i < 32u; i++) {
        key[i] = (uint8_t)(0xA0u + i);
    }
    static uint8_t seeds[12][64];
    size_t sl[12];
    const zw_s2_kex_t kex = {0x02u, 0x02u, 0x01u, 0x87u};
    const zw_s2_pubkey_t pk = {false, key};
    const zw_s2_netkey_report_t nk = {ZW_S2_KEY_ACCESS, key};
    const zw_s2_nonce_report_t sos = {0x43u, ZW_S2_NONCE_SOS, key};
    const zw_s2_nonce_report_t mos = {0x44u, ZW_S2_NONCE_MOS, NULL};
    sl[0] = zw_s2_kex_get_build(seeds[0], 64u);
    sl[1] = zw_s2_kex_report_build(seeds[1], 64u, &kex);
    sl[2] = zw_s2_kex_set_build(seeds[2], 64u, &kex);
    sl[3] = zw_s2_kex_fail_build(seeds[3], 64u, ZW_S2_KEX_FAIL_CANCEL);
    sl[4] = zw_s2_pubkey_build(seeds[4], 64u, &pk);
    sl[5] = zw_s2_netkey_get_build(seeds[5], 64u, ZW_S2_KEY_AUTH);
    sl[6] = zw_s2_netkey_report_build(seeds[6], 64u, &nk);
    sl[7] = zw_s2_netkey_verify_build(seeds[7], 64u);
    sl[8] = zw_s2_transfer_end_build(seeds[8], 64u, ZW_S2_TE_VERIFIED);
    sl[9] = zw_s2_nonce_get_build(seeds[9], 64u, 0x42u);
    sl[10] = zw_s2_nonce_report_build(seeds[10], 64u, &sos);
    sl[11] = zw_s2_nonce_report_build(seeds[11], 64u, &mos);
    const uint8_t *sp[13];
    size_t sll[13];
    for (size_t k = 0; k < 12u; k++) {
        TEST_ASSERT_TRUE(sl[k] >= 2u);
        sp[k] = seeds[k];
        sll[k] = sl[k];
    }
    sp[12] = S.cap[1].b; /* an encapsulated frame without its SPAN extension */
    sll[12] = S.cap[1].len;
    unsigned parsed = 0u;
    seed(0x4B4558u);
    for (unsigned i = 0; i < 20000u; i++) {
        size_t len = make_input(b, sizeof(b), sp, sll, 13u);
        if (len >= 2u && rnd_below(2u) == 0u) {
            b[0] = 0x9Fu; /* Security 2, a command of it */
            b[1] = (uint8_t)(1u + rnd_below(0x0Eu));
        }
        /* the parsers, twice */
        twin(b, len);
        zw_s2_encap_t ea, eb;
        memset(&ea, 0, sizeof(ea));
        memset(&eb, 0, sizeof(eb));
        const bool ok = zw_s2_encap_parse(g_a, len, &ea);
        TEST_ASSERT_EQUAL_MESSAGE(ok, zw_s2_encap_parse(g_b, len, &eb), hex_msg("encap", b, len));
        if (ok) {
            parsed++;
            TEST_ASSERT_EQUAL_UINT8(ea.seq, eb.seq);
            TEST_ASSERT_EQUAL_UINT(ea.ext_len, eb.ext_len);
            TEST_ASSERT_EQUAL_UINT(ea.ciphertext_len, eb.ciphertext_len);
            TEST_ASSERT_TRUE(ea.ciphertext >= g_a &&
                             ea.ciphertext + ea.ciphertext_len <= g_a + len);
            TEST_ASSERT_TRUE(ea.tag >= g_a && ea.tag + ZW_S2_TAG_BYTES <= g_a + len);
        }
        for (unsigned enc = 0; enc < 2u; enc++) {
            zw_s2_ext_t xa, xb;
            size_t ua = 0u, ub = 0u;
            const bool xo = zw_s2_ext_parse(g_a, len, enc != 0u, &xa, &ua);
            TEST_ASSERT_EQUAL_MESSAGE(xo, zw_s2_ext_parse(g_b, len, enc != 0u, &xb, &ub),
                                      hex_msg("extensions", b, len));
            if (xo) {
                TEST_ASSERT_EQUAL_UINT(ua, ub);
                TEST_ASSERT_TRUE(ua <= len);
                TEST_ASSERT_EQUAL(xa.span, xb.span);
                TEST_ASSERT_EQUAL(xa.mos, xb.mos);
                TEST_ASSERT_EQUAL(xa.mgrp, xb.mgrp);
            }
        }
        zw_s2_nonce_report_t na, nb;
        const bool no = zw_s2_nonce_report_parse(g_a, len, &na);
        TEST_ASSERT_EQUAL_MESSAGE(no, zw_s2_nonce_report_parse(g_b, len, &nb),
                                  hex_msg("nonce report", b, len));
        if (no && na.receiver_ei != NULL) {
            TEST_ASSERT_TRUE(na.receiver_ei + ZW_S2_EI_LEN <= g_a + len);
        }
        zw_s2_kex_t ka, kb;
        TEST_ASSERT_EQUAL(zw_s2_kex_report_parse(g_a, len, &ka),
                          zw_s2_kex_report_parse(g_b, len, &kb));
        TEST_ASSERT_EQUAL(zw_s2_kex_set_parse(g_a, len, &ka), zw_s2_kex_set_parse(g_b, len, &kb));
        zw_s2_pubkey_t pa, pb;
        const bool po = zw_s2_pubkey_parse(g_a, len, &pa);
        TEST_ASSERT_EQUAL(po, zw_s2_pubkey_parse(g_b, len, &pb));
        if (po) {
            TEST_ASSERT_TRUE(pa.key + ZW_S2_PUBKEY_LEN <= g_a + len);
        }
        zw_s2_netkey_report_t ra, rb;
        const bool ro = zw_s2_netkey_report_parse(g_a, len, &ra);
        TEST_ASSERT_EQUAL(ro, zw_s2_netkey_report_parse(g_b, len, &rb));
        if (ro) {
            TEST_ASSERT_TRUE(ra.netkey + ZW_S2_NETKEY_LEN <= g_a + len);
        }
        /* the manager: from the device, and from a stranger */
        const uint16_t from = rnd_below(5u) == 0u ? (uint16_t)STRANGER : (uint16_t)DEV;
        const unsigned before = S.got;
        S.got_secure = S.got_secure_only = 0u;
        zw_s2_receive(&S.panel, S.now, from, at_edge(b, len), len);
        S.now += 50u;
        zw_s2_poll(&S.panel, S.now);
        S.head = S.tail; /* the panel's answers go nowhere */
        if (S.got_secure != 0u || S.got_secure_only != 0u ||
            (from == STRANGER && S.got != before)) {
            TEST_FAIL_MESSAGE(hex_msg("a random S2 command was delivered", b, len));
        }
    }
    TEST_ASSERT_TRUE(parsed > 500u);
}

/* ==== the bridge ============================================================================= */

#define MIN_MS 60000u

typedef struct {
    zw_net_t net;
    fake_zac93_t zac;
    fake_wire_t wire;
    zw_host_crypto_t hc;
    zw_crypto_port_t crypto;
    zw_net_config_t cfg;
    uint8_t nodes[8192];
    size_t nodes_len;
    uint8_t bind[ZONE_HUB_BLOB_MAX];
    size_t bind_len;
    zw_incl_event_t incl[32];
    size_t incl_n;
    uint16_t pin;
    uint8_t grant;
    zwz_t bridge;
    zsrc_zw_t src;
    zone_hub_t hub;
    unsigned zev;   /* zone events, all told */
    unsigned state; /* ...of them not a check-in or reachable again */
    alarm_zone_event_t last_ev;
} world_t;

static world_t W;

static void o_link_up(void *ctx, const zw_module_facts_t *facts)
{
    (void)ctx;
    zwz_link_up(&W.bridge, facts, W.wire.now);
}

static void o_link_down(void *ctx, const zw_link_down_t *why)
{
    (void)ctx;
    (void)why;
    zwz_link_down(&W.bridge, W.wire.now);
}

static void o_restarted(void *ctx, uint8_t reason)
{
    (void)ctx;
    (void)reason;
    zwz_link_down(&W.bridge, W.wire.now);
}

/* Commands that reached the zones' bridge from `below_node` at a class below `below_top` (ZW-009:
 * each one a frame that must not have been delivered, unless Z-Wave JS accepts it). Counted apart
 * from W.bridge.stats.reports, which the device's own answers at its class also move. */
static uint16_t below_node;
static zw_sec_class_t below_top;
static size_t below_reports;

static void o_received(void *ctx, const zw_s2_rx_t *rx)
{
    (void)ctx;
    if (below_node != 0u && rx->node == below_node && (int)rx->security < (int)below_top) {
        below_reports++;
    }
    zwz_received(&W.bridge, rx, W.wire.now);
}

static void o_send_done(void *ctx, uint32_t tag, zw_s2_result_t r)
{
    (void)ctx;
    (void)zwz_send_done(&W.bridge, tag, r);
}

static void o_incl(void *ctx, const zw_incl_event_t *ev)
{
    (void)ctx;
    if (W.incl_n < sizeof(W.incl) / sizeof(W.incl[0])) {
        W.incl[W.incl_n] = *ev;
        W.incl[W.incl_n].nif = NULL;
        W.incl_n++;
    }
    zw_incl_t *in = zw_net_incl(&W.net);
    if (ev->kind == ZW_INCL_EV_GRANT) {
        TEST_ASSERT_TRUE(zw_incl_grant(in, W.wire.now, W.grant != 0u ? W.grant : ev->requested));
    } else if (ev->kind == ZW_INCL_EV_PIN) {
        TEST_ASSERT_TRUE(zw_incl_pin(in, W.wire.now, W.pin));
    }
}

static void o_event(void *ctx, const zw_net_event_t *ev)
{
    (void)ctx;
    zwz_event(&W.bridge, ev, W.wire.now);
}

static void o_store_nodes(void *ctx, const zw_node_cache_t *nodes, uint16_t generation)
{
    (void)ctx;
    W.nodes_len = zw_node_cache_encode(nodes, W.nodes, sizeof(W.nodes));
    TEST_ASSERT_NOT_EQUAL(0u, W.nodes_len);
    W.cfg.generation = generation;
}

static void o_store_keys(void *ctx, uint32_t home_id, const zw_s2_keys_t *keys)
{
    (void)ctx;
    W.cfg.keys = *keys;
    W.cfg.keys_bound = true;
    W.cfg.keys_home_id = home_id;
}

static bool o_quirk(void *ctx, const zw_node_record_t *rec, zw_iv_quirk_t *out)
{
    (void)ctx;
    return zwz_quirk(&W.bridge, rec, out);
}

static bool o_write(void *ctx, const uint8_t *bytes, size_t len)
{
    (void)ctx;
    return fake_wire_write(&W.wire, bytes, len);
}

static void hub_event(void *ctx, uint16_t zone, alarm_zone_event_t ev)
{
    (void)ctx;
    (void)zone;
    W.last_ev = ev;
    W.zev++;
    if (ev != ALARM_ZEV_CHECKIN && ev != ALARM_ZEV_REACHABLE) {
        W.state++;
    }
}

static void hub_changed(void *ctx)
{
    (void)ctx;
    const zone_hub_result_t r = zone_hub_encode(&W.hub, W.bind, sizeof(W.bind), &W.bind_len);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, r);
}

static void hub_trouble(void *ctx, uint8_t kind, bool on)
{
    (void)ctx;
    (void)kind;
    (void)on;
}

static void sink_input(void *ctx, const zsrc_address_t *a, const char *state, int32_t v, uint64_t t)
{
    (void)ctx;
    zone_hub_input(&W.hub, a, state, v, t);
}

static void sink_event(void *ctx, const zsrc_address_t *a, alarm_zone_event_t ev, uint64_t t)
{
    (void)ctx;
    zone_hub_event(&W.hub, a, ev, t);
}

static void sink_reachable(void *ctx, const zsrc_address_t *a, bool ok, uint64_t t)
{
    (void)ctx;
    zone_hub_reachable(&W.hub, a, ok, t);
}

static void sink_link(void *ctx, uint8_t kind, bool up, uint64_t t)
{
    (void)ctx;
    zone_hub_link(&W.hub, kind, up, t);
}

static void n_control(void *ctx, uint64_t now, uint8_t b)
{
    (void)ctx;
    zw_net_on_control(&W.net, now, b);
}

static void n_frame(void *ctx, uint64_t now, const zw_frame_t *f)
{
    (void)ctx;
    zw_net_on_frame(&W.net, now, f);
}

static void n_text(void *ctx, uint64_t now, const uint8_t *b, size_t n)
{
    (void)ctx;
    zw_net_on_text(&W.net, now, b, n);
}

static void n_poll(void *ctx, uint64_t now)
{
    (void)ctx;
    zw_net_poll(&W.net, now);
    zwz_poll(&W.bridge, now);
    zone_hub_tick(&W.hub, now);
}

static uint64_t n_deadline(void *ctx)
{
    (void)ctx;
    uint64_t t = zw_net_next_deadline(&W.net);
    const uint64_t b = zwz_next_deadline(&W.bridge);
    const uint64_t h = zone_hub_next_deadline(&W.hub, W.wire.now);
    t = b < t ? b : t;
    return h < t ? h : t;
}

static void run(uint64_t t)
{
    fake_wire_run_until(&W.wire, t);
    TEST_ASSERT_FALSE_MESSAGE(W.wire.stuck, "the wire did not settle");
}

static void run_for(uint64_t ms)
{
    run(W.wire.now + ms);
}

/* The panel's Z-Wave, the bridge, the zone source and the hub afresh, as test_zwave_zones.c's. */
static void world_up(void)
{
    static const zw_net_port_t port = {
        .write = o_write,
        .link_up = o_link_up,
        .link_down = o_link_down,
        .restarted = o_restarted,
        .received = o_received,
        .send_done = o_send_done,
        .incl_event = o_incl,
        .event = o_event,
        .store_nodes = o_store_nodes,
        .store_keys = o_store_keys,
        .quirk = o_quirk,
    };
    static const fake_host_t fh = {NULL, n_control, n_frame, n_text, n_poll, n_deadline};
    static const zone_hub_port_t hport = {NULL, hub_event, hub_changed, hub_trouble};
    static const zsrc_sink_t sink = {NULL, sink_input, sink_event, sink_reachable, sink_link, NULL};
    static const zw_cc_panel_identity_t id = {0u, 0u, 1u, 5u, 0xFFFFu, 0xFFFFu, 0xFFFFu,
                                              0x00u, 0x00u, 0x0500u, 0x0500u, false,
                                              0u, 0u, 0u, 0u, 0u, 0u};
    memset(&W, 0, sizeof(W));
    W.cfg.interview = true;
    zw_host_crypto_init(&W.hc, 0x7A6F6E65u);
    W.crypto = zw_host_crypto_port(&W.hc);
    fake_zac93_init(&W.zac);
    const uint64_t t = W.wire.now;
    zw_net_init(&W.net, &port, &W.cfg, &W.crypto);
    zsrc_zw_init(&W.src, &sink);
    zone_hub_init(&W.hub, &hport);
    TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_register(&W.hub, zsrc_zw_ops(), &W.src, t));
    zone_hub_start(&W.hub, t);
    zwz_init(&W.bridge, &W.net, &W.src, &id);
    fake_wire_init(&W.wire, &W.zac, &fh, t);
    zw_net_start(&W.net, t);
    run(t + 1000u);
    TEST_ASSERT_EQUAL_INT(ZW_LINK_UP, zw_host_link(zw_net_host(&W.net)));
}

/* "Add a device": the node it got. */
static uint16_t include(fake_zdev_t *d, uint8_t grant)
{
    TEST_ASSERT_TRUE(fake_zac93_add_device(&W.zac, d));
    W.pin = d->pin;
    W.grant = grant;
    const size_t before = W.incl_n;
    TEST_ASSERT_EQUAL_INT(ZW_INCL_STARTED, zw_net_add_device(&W.net, W.wire.now, true, 2u, 0u));
    run_for(100u);
    TEST_ASSERT_TRUE(fake_zac93_press(&W.zac, W.wire.now, d));
    for (unsigned i = 0; i < 400u; i++) {
        run_for(100u);
        for (size_t k = before; k < W.incl_n; k++) {
            if (W.incl[k].kind == ZW_INCL_EV_DONE) {
                TEST_ASSERT_EQUAL_INT(ZW_INCL_ADDED, W.incl[k].result);
                return W.incl[k].node;
            }
        }
    }
    TEST_FAIL_MESSAGE("the inclusion did not end");
    return 0u;
}

static const zsrc_zw_device_t *until_ready(uint16_t node)
{
    for (unsigned i = 0; i < 600u; i++) {
        const zsrc_zw_device_t *d = zsrc_zw_find(&W.src, node);
        if (d != NULL && d->ready) {
            return d;
        }
        run_for(100u);
    }
    TEST_FAIL_MESSAGE("the device never offered its inputs");
    return NULL;
}

/* The node's door contact bound to zone 1. */
static void bind_contact(uint16_t node)
{
    static zsrc_candidate_t c[128];
    const size_t n = zone_hub_candidates(&W.hub, ZSRC_ZWAVE, NULL, 0u, c, 128u);
    for (size_t i = 0; i < n; i++) {
        uint32_t home = 0u;
        uint16_t gen = 0u;
        uint16_t nd = 0u;
        uint8_t e = 0u;
        uint8_t s = 0u;
        if (zsrc_zw_address_parse(&c[i].addr, &home, &gen, &nd, &e, &s) && nd == node &&
            e == 0u && s == ZSRC_ZW_SLOT_CONTACT) {
            zone_hub_binding_t b;
            memset(&b, 0, sizeof(b));
            b.in[ZSRC_ROLE_MAIN].addr = c[i].addr;
            zsrc_mapping_defaults(&b.in[ZSRC_ROLE_MAIN].map,
                                  (alarm_device_class_t)c[i].device_class);
            TEST_ASSERT_EQUAL_INT(ZONE_HUB_OK, zone_hub_bind(&W.hub, 1u, &b, W.wire.now));
            return;
        }
    }
    TEST_FAIL_MESSAGE("no door contact offered");
}

/* What a door sensor says, for the mutations. */
static const uint8_t B_OPEN[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
static const uint8_t B_CLOSED[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x17, 0x00};
static const uint8_t B_TAMPER[] = {0x71, 0x05, 0x00, 0x00, 0x00, 0xFF, 0x07, 0x03, 0x00};
static const uint8_t B_V1[] = {0x71, 0x05, 0x01, 0xFF};
static const uint8_t B_BATTERY[] = {0x80, 0x03, 0xFF};
static const uint8_t B_BASIC[] = {0x20, 0x01, 0xFF};
static const uint8_t B_BINARY[] = {0x30, 0x03, 0xFF, 0x0A};
static const uint8_t B_ALARM_SENSOR[] = {0x9C, 0x02, 0x00, 0x0A, 0xFF, 0x00, 0x00};
static const uint8_t B_SCENE[] = {0x5B, 0x03, 0x01, 0x00, 0x01};
static const uint8_t B_WAKE[] = {0x84, 0x07};
static const uint8_t B_MULTI_CHANNEL[] = {0x60, 0x0D, 0x01, 0x00, 0x71, 0x05, 0x00, 0x00,
                                          0x00, 0xFF, 0x06, 0x16, 0x00};
static const uint8_t B_SUPERVISED[] = {0x6C, 0x01, 0x05, 0x09, 0x71, 0x05, 0x00, 0x00,
                                       0x00, 0xFF, 0x06, 0x16, 0x00};
static const uint8_t B_MULTI_CMD[] = {0x8F, 0x01, 0x02, 0x03, 0x80, 0x03, 0xFF, 0x09, 0x71,
                                      0x05, 0x00, 0x00, 0x00, 0xFF, 0x06, 0x16, 0x00};
static const uint8_t *const B_SEEDS[] = {B_OPEN,   B_CLOSED,       B_TAMPER,        B_V1,
                                         B_BATTERY, B_BASIC,       B_BINARY,        B_ALARM_SENSOR,
                                         B_SCENE,  B_WAKE,         B_MULTI_CHANNEL, B_SUPERVISED,
                                         B_MULTI_CMD};
static const size_t B_LEN[] = {sizeof(B_OPEN),        sizeof(B_CLOSED),  sizeof(B_TAMPER),
                               sizeof(B_V1),          sizeof(B_BATTERY), sizeof(B_BASIC),
                               sizeof(B_BINARY),      sizeof(B_ALARM_SENSOR),
                               sizeof(B_SCENE),       sizeof(B_WAKE),    sizeof(B_MULTI_CHANNEL),
                               sizeof(B_SUPERVISED),  sizeof(B_MULTI_CMD)};
#define B_SEED_N (sizeof(B_SEEDS) / sizeof(B_SEEDS[0]))

/* The CCs the fake door sensor supports (fake_zdev.c's door_model), and those of the one
 * without Security 2 below: the class rule is asserted for these. A CC on none of its lists is
 * test_bridge_unlisted_cc_below_class's. */
static const uint8_t DOOR_CCS[] = {0x5E, 0x55, 0x9F, 0x6C, 0x86, 0x72, 0x5A,
                                   0x85, 0x8E, 0x59, 0x71, 0x80, 0x84, 0x70};
static const uint8_t PLAIN_NIF[] = {0x5E, 0x86, 0x72, 0x5A, 0x85, 0x59, 0x71, 0x80, 0x84};

static bool door_lists(uint8_t cc)
{
    for (size_t i = 0; i < sizeof(DOOR_CCS); i++) {
        if (DOOR_CCS[i] == cc) {
            return true;
        }
    }
    return false;
}

static const char *class_name(zw_sec_class_t c)
{
    switch (c) {
    case ZW_SEC_NONE: return "a frame in the clear";
    case ZW_SEC_S2_UNAUTH: return "an S2 Unauthenticated frame";
    case ZW_SEC_S2_AUTH: return "an S2 Authenticated frame";
    default: return "an S2 frame";
    }
}

/* ZW-009 (Z-Wave JS isCCConsideredSecure): what an S2 node may send in the clear - a bare
 * Security 2 command, and Security 0's Nonce Get, Nonce Report, Scheme Get and Scheme Report. */
static bool accepted_in_clear(const uint8_t *cmd, size_t len)
{
    return len >= 1u && (cmd[0] == 0x9Fu ||
                         (len >= 2u && cmd[0] == 0x98u &&
                          (cmd[1] == 0x04u || cmd[1] == 0x05u || cmd[1] == 0x40u ||
                           cmd[1] == 0x80u)));
}

/* ZW-009 (Z-Wave JS isCCConsideredSecure): what an S2 node may send encrypted below its highest
 * class - Security 2 Commands Supported Get and Report. (A singlecast follow-up of a multicast is
 * accepted too, but the fake device sends none; a node whose classes are known learns nothing by
 * decrypting, so security.md §8's cache-lost case does not arise here.) */
static bool accepted_below_class(const uint8_t *cmd, size_t len)
{
    return len >= 2u && cmd[0] == 0x9Fu && (cmd[1] == 0x0Du || cmd[1] == 0x0Eu);
}

/* A door sensor joined with `grant` (0: a device without Security 2), bound to zone 1: at each
 * class below its highest, `rounds` random and mutated commands make no zone event (ZW-009); its
 * own open still does; at its own class, the same commands crash nothing. */
static void bridge_case(uint8_t grant, zw_sec_class_t top, uint64_t s, unsigned rounds)
{
    static fake_zdev_t door;
    world_up();
    fake_zdev_init(&door, s);
    door.m.answers = true;
    door.m.sleeps = true;
    if (grant == 0u) {
        fake_zdev_no_s2(&door);
        door.m.secure_n = 0u;
        memcpy(door.nif, PLAIN_NIF, sizeof(PLAIN_NIF));
        door.nif_len = sizeof(PLAIN_NIF);
    }
    const uint16_t node = include(&door, grant);
    TEST_ASSERT_NOT_EQUAL(0u, node);
    (void)until_ready(node);
    bind_contact(node);
    /* the control: its own open and close reach the zone */
    unsigned before = W.zev;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&door, W.wire.now, ZW_SEC_AUTO, B_OPEN,
                                                     sizeof(B_OPEN)));
    run_for(1000u);
    TEST_ASSERT_TRUE_MESSAGE(W.zev > before, "the control open made no zone event");
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&door, W.wire.now, ZW_SEC_AUTO, B_CLOSED,
                                                     sizeof(B_CLOSED)));
    run_for(1000u);
    static uint8_t cmd[ZW_S2_PLAIN_MAX];
    seed(s);
    /* below its class: nothing reaches the zones - not a report (exactly none at a class below
     * its own: below_reports), not a state, not a check-in (ZW-009): a frame Security 2 drops never
     * counts. The exceptions, all accepted by Z-Wave JS by design: in the clear a bare Security 2
     * command and Security 0's nonce and scheme commands (accepted_in_clear); encrypted below its
     * class Security 2 Commands Supported Get and Report (accepted_below_class); and an encrypted
     * frame's own Nonce Get, which the panel answers (security.md §6: a sender changing class
     * asks for a nonce first) - at most one check-in per Nonce Report the panel sent. */
    unsigned sent = 0u;
    unsigned heartbeats = 0u;
    below_node = node;
    below_top = top;
    for (int c = (int)ZW_SEC_NONE; c < (int)top; c++) {
        for (unsigned i = 0; i < rounds; i++) {
            const size_t len = make_input(cmd, 40u, B_SEEDS, B_LEN, B_SEED_N);
            if (len == 0u || !door_lists(cmd[0])) {
                continue;
            }
            before = W.state;
            const unsigned beats = W.zev - W.state;
            const size_t reports = below_reports;
            const uint32_t nonces = zw_s2_node_stats(zw_net_s2(&W.net), node)->nonce_reports;
            if (fake_zdev_report(&door, W.wire.now, (zw_sec_class_t)c, cmd, len) != ZW_S2_OK) {
                continue; /* a class it holds no key for, or too long for it */
            }
            sent++;
            run_for(1000u);
            const bool accepted = c == (int)ZW_SEC_NONE ? accepted_in_clear(cmd, len)
                                                        : accepted_below_class(cmd, len);
            /* check-ins the panel's own Nonce Reports explain (0 in the clear) */
            const unsigned answered =
                c == (int)ZW_SEC_NONE
                    ? 0u
                    : (unsigned)(zw_s2_node_stats(zw_net_s2(&W.net), node)->nonce_reports - nonces);
            if (!accepted && below_reports != reports) {
                char what[96];
                snprintf(what, sizeof(what), "%s reached the zones",
                         class_name((zw_sec_class_t)c));
                TEST_FAIL_MESSAGE(hex_msg(what, cmd, len));
            }
            if (!accepted && W.zev - W.state - beats > answered) {
                heartbeats++;
                char what[96];
                snprintf(what, sizeof(what), "%s made a check-in", class_name((zw_sec_class_t)c));
                TEST_FAIL_MESSAGE(hex_msg(what, cmd, len));
            }
            if (W.state != before) {
                char what[96];
                snprintf(what, sizeof(what), "%s made zone event %d",
                         class_name((zw_sec_class_t)c), (int)W.last_ev);
                TEST_FAIL_MESSAGE(hex_msg(what, cmd, len));
            }
        }
    }
    below_node = 0u;
    if (top != ZW_SEC_NONE) {
        TEST_ASSERT_TRUE(sent > rounds / 4u);
        TEST_ASSERT_EQUAL_UINT(0u, heartbeats);
    }
    /* its own class: anything at all */
    for (unsigned i = 0; i < rounds; i++) {
        const size_t len = make_input(cmd, 40u, B_SEEDS, B_LEN, B_SEED_N);
        if (len > 0u) {
            (void)fake_zdev_report(&door, W.wire.now, top, cmd, len);
            run_for(1000u);
        }
    }
    run_for(MIN_MS);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, W.zac.overflow, "the fake module ran out of room");
}

/* ZW-023, ZW-009: a door sensor without Security 2 (crash only: its class is the clear). */
static void test_bridge_plain_device(void)
{
    bridge_case(0u, ZW_SEC_NONE, 0xB001u, 300u);
}

/* ZW-023, ZW-009: granted S2 Unauthenticated - in the clear, nothing reaches its zone. */
static void test_bridge_s2_unauth(void)
{
    bridge_case(ZW_S2_KEY_UNAUTH, ZW_SEC_S2_UNAUTH, 0xB002u, 250u);
}

/* ZW-023, ZW-009: granted S2 Authenticated and Unauthenticated - in the clear or at
 * Unauthenticated, nothing reaches its zone. */
static void test_bridge_s2_auth(void)
{
    bridge_case(ZW_S2_KEY_AUTH | ZW_S2_KEY_UNAUTH, ZW_SEC_S2_AUTH, 0xB003u, 200u);
}

/* ZW-023, ZW-009: granted every S2 class - below Access Control, nothing reaches its zone. */
static void test_bridge_s2_access(void)
{
    bridge_case(ALL_S2, ZW_SEC_S2_ACCESS, 0xB004u, 150u);
}

/* ZW-009, found by this suite, aligned with Z-Wave JS (Driver.ts isSecurityLevelTooLow): from
 * an S2 door sensor, a Binary Sensor Report or a Supervision Get around a Notification Report, in
 * the clear or below its class, reaches no zone and is no check-in (beyond the Nonce Get its
 * encrypted frame needs). A bare Security 2 Commands Supported Report in the clear is accepted:
 * a check-in, but no state. At its class the Binary Sensor Report is delivered (the control). */
static void test_bridge_unlisted_cc_below_class(void)
{
    static fake_zdev_t door;
    world_up();
    fake_zdev_init(&door, 0xB006u);
    door.m.answers = true;
    door.m.sleeps = true;
    const uint16_t node = include(&door, ZW_S2_KEY_AUTH | ZW_S2_KEY_UNAUTH);
    (void)until_ready(node);
    bind_contact(node);
    run_for(MIN_MS); /* the device's answers to the bind's state reads: before the window */
    static const uint8_t BINARY_DOOR_OPEN[] = {0x30, 0x03, 0xFF, 0x0A};
    static const uint8_t SUPERVISED_OPEN[] = {0x6C, 0x01, 0x05, 0x0A, 0x71, 0x05, 0x00, 0x00,
                                              0x00, 0xFF, 0x06, 0x00};
    static const uint8_t *const FRAMES[2] = {BINARY_DOOR_OPEN, SUPERVISED_OPEN};
    static const size_t LENS[2] = {sizeof(BINARY_DOOR_OPEN), sizeof(SUPERVISED_OPEN)};
    const zw_sec_class_t below[2] = {ZW_SEC_NONE, ZW_SEC_S2_UNAUTH};
    below_node = node;
    below_top = ZW_SEC_S2_AUTH;
    for (size_t f = 0; f < 2u; f++) {
        for (size_t k = 0; k < 2u; k++) {
            const unsigned before = W.state;
            const unsigned beats = W.zev - W.state;
            const size_t reports = below_reports;
            const uint32_t nonces = zw_s2_node_stats(zw_net_s2(&W.net), node)->nonce_reports;
            TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&door, W.wire.now, below[k],
                                                             FRAMES[f], LENS[f]));
            run_for(1000u);
            /* nothing reaches the zones, and no check-in - beyond one per Nonce Get of an
             * encrypted frame that the panel answered */
            const unsigned answered =
                below[k] == ZW_SEC_NONE
                    ? 0u
                    : (unsigned)(zw_s2_node_stats(zw_net_s2(&W.net), node)->nonce_reports - nonces);
            if (W.state != before || below_reports != reports ||
                W.zev - W.state - beats > answered) {
                char what[128];
                snprintf(what, sizeof(what), "ZW-009: %s below its class made zone event %d",
                         class_name(below[k]), (int)W.last_ev);
                TEST_FAIL_MESSAGE(hex_msg(what, FRAMES[f], LENS[f]));
            }
        }
    }
    below_node = 0u;
    /* a bare Security 2 command in the clear: accepted - a check-in, no state */
    run_for(MIN_MS);
    static const uint8_t BARE_CSR[] = {0x9F, 0x0E, 0x71};
    unsigned before = W.state;
    unsigned beats = W.zev - W.state;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&door, W.wire.now, ZW_SEC_NONE, BARE_CSR,
                                                     sizeof(BARE_CSR)));
    run_for(1000u);
    TEST_ASSERT_EQUAL_UINT(before, W.state);
    TEST_ASSERT_TRUE_MESSAGE(W.zev - W.state > beats, "ZW-009: the accepted frame made no check-in");
    /* at its class the same frame is the device's own: delivered (it may or may not map to the
     * contact input; it must at least be a check-in) */
    const unsigned zev = W.zev;
    TEST_ASSERT_EQUAL_INT(ZW_S2_OK, fake_zdev_report(&door, W.wire.now, ZW_SEC_S2_AUTH,
                                                     BINARY_DOOR_OPEN, sizeof(BINARY_DOOR_OPEN)));
    run_for(1000u);
    TEST_ASSERT_TRUE_MESSAGE(W.zev > zev, "at its class the unlisted CC made no zone event");
}

/* ZW-023: random commands straight into zwz_received - any node, class, end point and
 * Supervision - and random reports straight into zsrc_zw_report, around a joined door sensor
 * bound to a zone: nothing crashes or reads past them. */
static void test_bridge_direct_random(void)
{
    static fake_zdev_t door;
    world_up();
    fake_zdev_init(&door, 0xB005u);
    door.m.answers = true;
    door.m.sleeps = true;
    const uint16_t node = include(&door, ZW_S2_KEY_AUTH);
    (void)until_ready(node);
    bind_contact(node);
    static uint8_t cmd[ZW_S2_RX_MAX];
    seed(0xD12EC7u);
    for (unsigned i = 0; i < 6000u; i++) {
        const size_t len = make_input(cmd, 120u, B_SEEDS, B_LEN, B_SEED_N);
        zw_s2_rx_t rx;
        memset(&rx, 0, sizeof(rx));
        rx.node = rnd_below(4u) != 0u ? node : (uint16_t)(rnd64() & 0x0FFFu);
        rx.security = (zw_sec_class_t)rnd_below(4u);
        rx.multi_channel = rnd_below(4u) == 0u;
        rx.source_endpoint = rnd_below(2u) == 0u ? rnd_byte() : (uint8_t)rnd_below(3u);
        rx.destination_endpoint = (uint8_t)rnd_below(2u);
        rx.supervised = rnd_below(4u) == 0u;
        rx.supervision_session = rnd_byte();
        rx.supervision_updates = rnd_below(2u) == 0u;
        rx.multi_command = rnd_below(8u) == 0u;
        rx.command = at_edge(cmd, len);
        rx.len = len;
        zwz_received(&W.bridge, &rx, W.wire.now);

        zsrc_zw_report_t r;
        memset(&r, 0, sizeof(r));
        r.kind = (uint8_t)rnd_below(10u);
        r.endpoint = rnd_below(2u) == 0u ? rnd_byte() : 0u;
        r.notif_v1 = rnd_below(2u) == 0u;
        r.notif_status = rnd_edge();
        r.notif_type = (uint8_t)rnd_below(16u);
        r.notif_event = rnd_byte();
        r.notif_param_len = rnd_edge();
        for (size_t k = 0; k < sizeof(r.notif_params); k++) {
            r.notif_params[k] = rnd_byte();
        }
        r.notif_v1_type = rnd_byte();
        r.notif_v1_level = rnd_byte();
        r.binary_has_type = rnd_below(2u) == 0u;
        r.binary_type = rnd_byte();
        r.binary_on = rnd_below(2u) == 0u;
        r.battery_level = rnd_edge();
        r.battery_replace = rnd_edge();
        r.battery_disconnected = rnd_below(2u) == 0u;
        r.ml_type = rnd_byte();
        r.ml_scale = rnd_byte();
        r.ml_value_x100 = (int32_t)(uint32_t)rnd64();
        r.alarm_type = rnd_byte();
        r.alarm_state = rnd_byte();
        r.basic_value = rnd_byte();
        r.scene_seq = rnd_byte();
        r.scene_attr = rnd_byte();
        r.scene_number = rnd_byte();
        zsrc_zw_report(&W.src, rnd_below(3u) != 0u ? node : (uint16_t)rnd_below(300u), &r,
                       W.wire.now);
        if (i % 16u == 0u) {
            run_for(500u); /* the bridge's answers go out */
        }
    }
    run_for(MIN_MS);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, W.zac.overflow, "the fake module ran out of room");
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_panel_blobs_fuzz);
    RUN_TEST(test_assignment_blobs_fuzz);
    RUN_TEST(test_network_blobs_fuzz);
    RUN_TEST(test_s2_mutated_frames_never_delivered);
    RUN_TEST(test_s2_random_commands);
    RUN_TEST(test_bridge_plain_device);
    RUN_TEST(test_bridge_s2_unauth);
    RUN_TEST(test_bridge_s2_auth);
    RUN_TEST(test_bridge_s2_access);
    RUN_TEST(test_bridge_unlisted_cc_below_class);
    RUN_TEST(test_bridge_direct_random);
    return UNITY_END();
}
