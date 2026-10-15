/*
 * test_backup.c - the backup file (docs/features/14 CFG-005, CFG-006): its payload records, its
 * encryption, the frozen format-1 fixture, what it holds and the check before a restore.
 */
#include <stdio.h>
#include <string.h>

#include "alarm/alarm_codec.h"
#include "alarm_proto/proto_fwsrv.h"
#include "backup/backup.h"
#include "backup/backup_check.h"
#include "backup_fixture_v1.h"
#include "backup_host_crypto.h"
#include "unity.h"

static backup_host_crypto_t HC;
static backup_crypto_t PORT;
static uint8_t BUF[64 * 1024];
static uint8_t FILEBUF[64 * 1024];
static uint8_t OUT[64 * 1024];
static backup_scratch_t SCRATCH;

void setUp(void)
{
    backup_host_crypto_init(&HC, 42u);
    PORT = backup_host_crypto_port(&HC);
}

void tearDown(void) {}

static void hex(const char *s, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        TEST_ASSERT_EQUAL_INT(1, sscanf(s + 2 * i, "%2x", &v));
        out[i] = (uint8_t)v;
    }
}

static const backup_meta_t META = {.created_s = 1790000000u, .firmware = "0.6.0",
                                   .board = "crowpanel_p4"};

static backup_entry_t ent(const char *ns, const char *key, uint8_t type, const void *v,
                          uint32_t len)
{
    backup_entry_t e;
    memset(&e, 0, sizeof(e));
    snprintf(e.ns, sizeof(e.ns), "%s", ns);
    snprintf(e.key, sizeof(e.key), "%s", key);
    e.type = type;
    e.value = v;
    e.len = len;
    return e;
}

/* ---- the primitives, against published and Python's answers (CFG-005) ----------------------- */

/* CFG-005: PBKDF2-HMAC-SHA256, RFC 7914 §11's two vectors. */
static void test_pbkdf2_known_answers(void)
{
    uint8_t out[64];
    uint8_t want[64];
    bhc_pbkdf2_sha256((const uint8_t *)"passwd", 6, (const uint8_t *)"salt", 4, 1, out, 64);
    hex("55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
        "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783",
        want, 64);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, 64);
    bhc_pbkdf2_sha256((const uint8_t *)"Password", 8, (const uint8_t *)"NaCl", 4, 80000, out, 64);
    hex("4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56"
        "a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d",
        want, 64);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, 64);
}

/* CFG-005: AES-256 (FIPS-197 C.3) and AES-256-GCM (the GCM specification's test cases 13 and
 * 16, confirmed with Python's cryptography package). */
static void test_aes256_gcm_known_answers(void)
{
    uint8_t key[32];
    uint8_t in[16];
    uint8_t out[16];
    uint8_t want[64];
    for (unsigned i = 0; i < 32u; i++) {
        key[i] = (uint8_t)i;
    }
    hex("00112233445566778899aabbccddeeff", in, 16);
    bhc_aes256_encrypt(&HC, key, in, out);
    hex("8ea2b7ca516745bfeafc49904b496089", want, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, 16);

    uint8_t zero_key[32] = {0};
    uint8_t zero_nonce[12] = {0};
    uint8_t tag[16];
    bhc_gcm_encrypt(&HC, zero_key, zero_nonce, NULL, 0, NULL, 0, NULL, tag);
    hex("530f8afbc74536b9a963b4f1c4cb738b", want, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, tag, 16);

    uint8_t k16[32];
    uint8_t iv[12];
    uint8_t pt[60];
    uint8_t aad[20];
    uint8_t ct[60];
    hex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308", k16, 32);
    hex("cafebabefacedbaddecaf888", iv, 12);
    hex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
        "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
        pt, 60);
    hex("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad, 20);
    bhc_gcm_encrypt(&HC, k16, iv, aad, 20, pt, 60, ct, tag);
    hex("522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
        "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662",
        want, 60);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, ct, 60);
    hex("76fc6ece0f4e1768cddf8853bb2d551b", want, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, tag, 16);
}

/* ---- the payload ------------------------------------------------------------------------------ */

/* CFG-005: every NVS type goes in and comes back as it was, in order, with the META record. */
static void test_payload_round_trip(void)
{
    backup_writer_t w;
    backup_writer_init(&w, BUF, sizeof(BUF), &META);
    const uint8_t u8 = 80;
    const uint8_t u16[2] = {0xB3, 0x22};
    const uint8_t u64[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const uint8_t blob[5] = {0, 1, 0, 2, 0};
    backup_entry_t in[5] = {
        ent("ui", "bright", BACKUP_T_U8, &u8, 1),
        ent("mqtt", "port", BACKUP_T_U16, u16, 2),
        ent("zwave", "gen", BACKUP_T_U64, u64, 8),
        ent("net", "ssid", BACKUP_T_STR, "Harbor House", 12),
        ent("zwave", "keys", BACKUP_T_BLOB, blob, 5),
    };
    for (unsigned i = 0; i < 5u; i++) {
        TEST_ASSERT_TRUE(backup_writer_add(&w, &in[i]));
    }
    size_t len = 0;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_writer_finish(&w, &len));

    backup_reader_t r;
    backup_meta_t meta;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_reader_init(&r, BUF, len, &meta));
    TEST_ASSERT_EQUAL_UINT64(1790000000u, meta.created_s);
    TEST_ASSERT_EQUAL_STRING("0.6.0", meta.firmware);
    TEST_ASSERT_EQUAL_STRING("crowpanel_p4", meta.board);
    for (unsigned i = 0; i < 5u; i++) {
        backup_entry_t e;
        bool done = true;
        TEST_ASSERT_EQUAL(BACKUP_OK, backup_reader_next(&r, &e, &done));
        TEST_ASSERT_FALSE(done);
        TEST_ASSERT_EQUAL_STRING(in[i].ns, e.ns);
        TEST_ASSERT_EQUAL_STRING(in[i].key, e.key);
        TEST_ASSERT_EQUAL_UINT8(in[i].type, e.type);
        TEST_ASSERT_EQUAL_UINT32(in[i].len, e.len);
        TEST_ASSERT_EQUAL_MEMORY(in[i].value, e.value, e.len);
    }
    backup_entry_t e;
    bool done = false;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_reader_next(&r, &e, &done));
    TEST_ASSERT_TRUE(done);
}

/* CFG-005: the writer refuses what NVS couldn't take back, and anything that doesn't fit. */
static void test_writer_refuses_malformed_entries(void)
{
    const uint8_t two[2] = {1, 2};
    const char nul_str[3] = {'a', 0, 'b'};
    const backup_entry_t bad[] = {
        ent("", "k", BACKUP_T_U8, two, 1),                 /* empty namespace */
        ent("ui", "a key", BACKUP_T_U8, two, 1),           /* a space in a name */
        ent("ui", "bright", BACKUP_T_U8, two, 2),          /* a u8 two bytes long */
        ent("ui", "bright", BACKUP_T_U32, two, 2),         /* a u32 two bytes long */
        ent("ui", "bright", 0, two, 1),                    /* no type */
        ent("ui", "bright", BACKUP_T_BLOB + 1, two, 1),    /* an unknown type */
        ent("net", "ssid", BACKUP_T_STR, nul_str, 3),      /* a NUL inside a string */
        ent("net", "ssid", BACKUP_T_STR, BUF, BACKUP_STR_MAX + 1u), /* longer than NVS takes */
        ent("zwave", "nodes", BACKUP_T_BLOB, BUF, BACKUP_BLOB_MAX + 1u),
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        backup_writer_t w;
        backup_writer_init(&w, BUF, sizeof(BUF), &META);
        TEST_ASSERT_FALSE(backup_writer_add(&w, &bad[i]));
        size_t len = 99;
        TEST_ASSERT_EQUAL(BACKUP_ERR_ARG, backup_writer_finish(&w, &len));
        TEST_ASSERT_EQUAL(0u, len);
    }
    /* Out of room: the payload is refused, never cut short. */
    backup_writer_t w;
    backup_writer_init(&w, BUF, 40, &META);
    const backup_entry_t big = ent("zwave", "nodes", BACKUP_T_BLOB, BUF + 1000, 100);
    TEST_ASSERT_FALSE(backup_writer_add(&w, &big));
    size_t len = 0;
    TEST_ASSERT_EQUAL(BACKUP_ERR_ARG, backup_writer_finish(&w, &len));
}

static size_t small_payload(uint8_t *buf, size_t cap)
{
    backup_writer_t w;
    backup_writer_init(&w, buf, cap, &META);
    const uint8_t v = 7;
    const backup_entry_t e = ent("ui", "volume", BACKUP_T_U8, &v, 1);
    TEST_ASSERT_TRUE(backup_writer_add(&w, &e));
    size_t len = 0;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_writer_finish(&w, &len));
    return len;
}

static backup_result_t read_all(const uint8_t *p, size_t len, uint32_t *entries)
{
    backup_reader_t r;
    backup_meta_t meta;
    backup_result_t res = backup_reader_init(&r, p, len, &meta);
    *entries = 0;
    while (res == BACKUP_OK) {
        backup_entry_t e;
        bool done;
        res = backup_reader_next(&r, &e, &done);
        if (res == BACKUP_OK && done) {
            return BACKUP_OK;
        }
        if (res == BACKUP_OK) {
            (*entries)++;
        }
    }
    return res;
}

/* CFG-006: a damaged payload is refused, however it is damaged; a record type from a later
 * firmware is skipped. */
static void test_reader_refuses_damage_and_skips_new_records(void)
{
    const size_t len = small_payload(BUF, sizeof(BUF));
    uint32_t n;
    TEST_ASSERT_EQUAL(BACKUP_OK, read_all(BUF, len, &n));
    TEST_ASSERT_EQUAL_UINT32(1, n);

    /* Cut anywhere before its end, it is refused. */
    for (size_t cut = 0; cut < len; cut++) {
        TEST_ASSERT_NOT_EQUAL(BACKUP_OK, read_all(BUF, cut, &n));
    }
    /* Something after END. */
    memcpy(OUT, BUF, len);
    OUT[len] = 0;
    TEST_ASSERT_EQUAL(BACKUP_ERR_FORMAT, read_all(OUT, len + 1u, &n));
    /* END counting another number of entries. */
    memcpy(OUT, BUF, len);
    OUT[len - 4u] = 2;
    TEST_ASSERT_EQUAL(BACKUP_ERR_FORMAT, read_all(OUT, len, &n));
    /* Another format of payload. */
    memcpy(OUT, BUF, len);
    OUT[1] = 2;
    TEST_ASSERT_EQUAL(BACKUP_ERR_FORMAT, read_all(OUT, len, &n));
    /* A second META. */
    memcpy(OUT, BUF, len - 5u);
    memcpy(OUT + len - 5u, BUF, len);
    TEST_ASSERT_EQUAL(BACKUP_ERR_FORMAT, read_all(OUT, 2u * len - 5u, &n));

    /* A record type 0x40 (type | len u32 | bytes) before END: skipped. */
    memcpy(OUT, BUF, len - 5u);
    size_t at = len - 5u;
    const uint8_t rec[] = {0x40, 3, 0, 0, 0, 'a', 'b', 'c'};
    memcpy(OUT + at, rec, sizeof(rec));
    at += sizeof(rec);
    memcpy(OUT + at, BUF + len - 5u, 5u);
    at += 5u;
    TEST_ASSERT_EQUAL(BACKUP_OK, read_all(OUT, at, &n));
    TEST_ASSERT_EQUAL_UINT32(1, n);
}

/* ---- the file --------------------------------------------------------------------------------- */

/* CFG-005: a file opens with its password and gives the payload back; each file gets a fresh
 * salt and nonce, so two files of the same panel never share a key stream. */
static void test_seal_and_open(void)
{
    const size_t len = small_payload(BUF, sizeof(BUF));
    size_t flen = 0;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_seal(&PORT, "Harbor House 2026", BACKUP_ITER_MIN, BUF,
                                             len, FILEBUF, sizeof(FILEBUF), &flen));
    TEST_ASSERT_EQUAL(len + BACKUP_OVERHEAD, flen);
    TEST_ASSERT_EQUAL_MEMORY("RABK", FILEBUF, 4);
    uint32_t plen = 0;
    uint32_t iter = 0;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_peek(FILEBUF, flen, &plen, &iter));
    TEST_ASSERT_EQUAL_UINT32(len, plen);
    TEST_ASSERT_EQUAL_UINT32(BACKUP_ITER_MIN, iter);
    /* The payload isn't in the file in the clear. */
    TEST_ASSERT_TRUE(memcmp(FILEBUF + BACKUP_HEADER_LEN, BUF, len) != 0);

    size_t got = 0;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_open(&PORT, "Harbor House 2026", FILEBUF, flen, OUT,
                                             sizeof(OUT), &got));
    TEST_ASSERT_EQUAL(len, got);
    TEST_ASSERT_EQUAL_MEMORY(BUF, OUT, len);

    static uint8_t second[1024];
    size_t flen2 = 0;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_seal(&PORT, "Harbor House 2026", BACKUP_ITER_MIN, BUF,
                                             len, second, sizeof(second), &flen2));
    TEST_ASSERT_TRUE(memcmp(FILEBUF + 12, second + 12, 16) != 0); /* salt */
    TEST_ASSERT_TRUE(memcmp(FILEBUF + 28, second + 28, 12) != 0); /* nonce */
}

/* CFG-005: the default work is BACKUP_ITER_DEFAULT; the password is the installer's choice
 * within limits. */
static void test_default_work_and_password_rules(void)
{
    const size_t len = small_payload(BUF, sizeof(BUF));
    size_t flen = 0;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_seal(&PORT, "Harbor House 2026", 0, BUF, len, FILEBUF,
                                             sizeof(FILEBUF), &flen));
    uint32_t iter = 0;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_peek(FILEBUF, flen, NULL, &iter));
    TEST_ASSERT_EQUAL_UINT32(BACKUP_ITER_DEFAULT, iter);

    TEST_ASSERT_FALSE(backup_password_ok(NULL));
    TEST_ASSERT_FALSE(backup_password_ok("1234567"));  /* 7: too short */
    TEST_ASSERT_TRUE(backup_password_ok("12345678"));
    TEST_ASSERT_FALSE(backup_password_ok("tab\there!"));
    char long_pw[BACKUP_PASSWORD_MAX + 2];
    memset(long_pw, 'x', sizeof(long_pw) - 1u);
    long_pw[sizeof(long_pw) - 1u] = '\0';
    TEST_ASSERT_FALSE(backup_password_ok(long_pw));
    long_pw[BACKUP_PASSWORD_MAX] = '\0';
    TEST_ASSERT_TRUE(backup_password_ok(long_pw));
    TEST_ASSERT_EQUAL(BACKUP_ERR_PASSWORD, backup_seal(&PORT, "short", BACKUP_ITER_MIN, BUF, len,
                                                       FILEBUF, sizeof(FILEBUF), &flen));
    TEST_ASSERT_EQUAL(0u, flen);
}

/* CFG-006: nothing comes out of a file with the wrong password or one changed anywhere: the
 * header is authenticated with the rest. */
static void test_wrong_password_or_changed_file_gives_nothing(void)
{
    const size_t len = small_payload(BUF, sizeof(BUF));
    size_t flen = 0;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_seal(&PORT, "Harbor House 2026", BACKUP_ITER_MIN, BUF,
                                             len, FILEBUF, sizeof(FILEBUF), &flen));
    size_t got = 1;
    memset(OUT, 0x5A, len);
    TEST_ASSERT_EQUAL(BACKUP_ERR_DECRYPT, backup_open(&PORT, "Harbor House 2025", FILEBUF, flen,
                                                      OUT, sizeof(OUT), &got));
    TEST_ASSERT_EQUAL(0u, got);
    for (size_t i = 0; i < len; i++) {
        TEST_ASSERT_EQUAL_HEX8(0, OUT[i]); /* not even a wrong-key plaintext */
    }
    /* Every byte of salt, nonce, ciphertext and tag, flipped in turn. */
    for (size_t i = 12; i < flen; i++) {
        if (i >= 40u && i < 44u) {
            continue; /* the length: backup_peek refuses it first (below) */
        }
        FILEBUF[i] ^= 0x01u;
        TEST_ASSERT_EQUAL(BACKUP_ERR_DECRYPT, backup_open(&PORT, "Harbor House 2026", FILEBUF,
                                                          flen, OUT, sizeof(OUT), &got));
        FILEBUF[i] ^= 0x01u;
    }
    /* The iteration count changed (10000 -> 14096): the key changes and the tag fails. */
    FILEBUF[9] ^= 0x10u;
    TEST_ASSERT_EQUAL(BACKUP_ERR_DECRYPT, backup_open(&PORT, "Harbor House 2026", FILEBUF, flen,
                                                      OUT, sizeof(OUT), &got));
    FILEBUF[9] ^= 0x10u;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_open(&PORT, "Harbor House 2026", FILEBUF, flen, OUT,
                                             sizeof(OUT), &got));
}

/* CFG-006: what isn't a backup file of this format is refused before any work is done. */
static void test_not_a_backup_file(void)
{
    const size_t len = small_payload(BUF, sizeof(BUF));
    size_t flen = 0;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_seal(&PORT, "Harbor House 2026", BACKUP_ITER_MIN, BUF,
                                             len, FILEBUF, sizeof(FILEBUF), &flen));
    size_t got = 0;
    const uint32_t calls = HC.pbkdf2_calls;
    TEST_ASSERT_EQUAL(BACKUP_ERR_FORMAT, backup_open(&PORT, "Harbor House 2026", FILEBUF,
                                                     flen - 1u, OUT, sizeof(OUT), &got));
    const size_t header_bytes[] = {0, 3, 4, 5, 6, 7, 40};
    for (size_t i = 0; i < sizeof(header_bytes) / sizeof(header_bytes[0]); i++) {
        FILEBUF[header_bytes[i]] ^= 0x01u;
        TEST_ASSERT_EQUAL(BACKUP_ERR_FORMAT, backup_open(&PORT, "Harbor House 2026", FILEBUF,
                                                         flen, OUT, sizeof(OUT), &got));
        FILEBUF[header_bytes[i]] ^= 0x01u;
    }
    /* Work beyond what a panel will do, or below what a file may ask. */
    uint8_t saved[4];
    memcpy(saved, FILEBUF + 8, 4);
    const uint8_t too_much[4] = {0x81, 0x84, 0x1E, 0x00}; /* 2000001 */
    memcpy(FILEBUF + 8, too_much, 4);
    TEST_ASSERT_EQUAL(BACKUP_ERR_FORMAT, backup_peek(FILEBUF, flen, NULL, NULL));
    const uint8_t too_little[4] = {0x0F, 0x27, 0, 0}; /* 9999 */
    memcpy(FILEBUF + 8, too_little, 4);
    TEST_ASSERT_EQUAL(BACKUP_ERR_FORMAT, backup_peek(FILEBUF, flen, NULL, NULL));
    memcpy(FILEBUF + 8, saved, 4);
    TEST_ASSERT_EQUAL_UINT32(calls, HC.pbkdf2_calls); /* no key was derived for any of these */
    TEST_ASSERT_EQUAL(BACKUP_ERR_ARG, backup_open(&PORT, "Harbor House 2026", FILEBUF, flen, OUT,
                                                  len - 1u, &got)); /* no room */
    TEST_ASSERT_EQUAL(BACKUP_ERR_FORMAT, backup_open(&PORT, "x", (const uint8_t *)"hello", 5, OUT,
                                                     sizeof(OUT), &got));
}

/* CFG-005: a failing crypto port makes no file (and nothing is half written). */
static void test_crypto_failures_make_no_file(void)
{
    const size_t len = small_payload(BUF, sizeof(BUF));
    const bhc_fail_t fails[] = {BHC_FAIL_RANDOM, BHC_FAIL_PBKDF2, BHC_FAIL_ENCRYPT};
    for (size_t i = 0; i < 3u; i++) {
        HC.fail = fails[i];
        size_t flen = 1;
        TEST_ASSERT_EQUAL(BACKUP_ERR_CRYPTO, backup_seal(&PORT, "Harbor House 2026",
                                                         BACKUP_ITER_MIN, BUF, len, FILEBUF,
                                                         sizeof(FILEBUF), &flen));
        TEST_ASSERT_EQUAL(0u, flen);
    }
    HC.fail = BHC_FAIL_NONE;
    size_t flen = 0;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_seal(&PORT, "Harbor House 2026", BACKUP_ITER_MIN, BUF,
                                             len, FILEBUF, sizeof(FILEBUF), &flen));
    HC.fail = BHC_FAIL_PBKDF2;
    size_t got = 1;
    TEST_ASSERT_EQUAL(BACKUP_ERR_CRYPTO, backup_open(&PORT, "Harbor House 2026", FILEBUF, flen,
                                                     OUT, sizeof(OUT), &got));
    TEST_ASSERT_EQUAL(0u, got);
}

/* CFG-005: format 1 as frozen on 2026-09-29, made by Python's hashlib and cryptography: this
 * firmware opens it, reads its six entries, and seals the same bytes from the same inputs. */
static void test_frozen_fixture_v1(void)
{
    size_t got = 0;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_open(&PORT, BACKUP_FIXTURE_V1_PASSWORD,
                                             BACKUP_FIXTURE_V1_FILE,
                                             sizeof(BACKUP_FIXTURE_V1_FILE), OUT, sizeof(OUT),
                                             &got));
    TEST_ASSERT_EQUAL(sizeof(BACKUP_FIXTURE_V1_PAYLOAD), got);
    TEST_ASSERT_EQUAL_MEMORY(BACKUP_FIXTURE_V1_PAYLOAD, OUT, got);

    backup_reader_t r;
    backup_meta_t meta;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_reader_init(&r, OUT, got, &meta));
    TEST_ASSERT_EQUAL_STRING("crowpanel_p4", meta.board);
    const char *const keys[6] = {"net/ssid", "ui/bright", "mqtt/port",
                                 "zwave/keys", "ota/src", "rf/min_us"};
    for (unsigned i = 0; i < 6u; i++) {
        backup_entry_t e;
        bool done;
        TEST_ASSERT_EQUAL(BACKUP_OK, backup_reader_next(&r, &e, &done));
        TEST_ASSERT_FALSE(done);
        char name[40];
        snprintf(name, sizeof(name), "%s/%s", e.ns, e.key);
        TEST_ASSERT_EQUAL_STRING(keys[i], name);
        if (i == 2u) {
            TEST_ASSERT_EQUAL_UINT8(BACKUP_T_U16, e.type);
            TEST_ASSERT_EQUAL_UINT32(8883u, (uint32_t)e.value[0] | ((uint32_t)e.value[1] << 8));
        }
    }

    uint8_t salt[16];
    uint8_t nonce[12];
    for (unsigned i = 0; i < 16u; i++) {
        salt[i] = (uint8_t)(0xA0u + i);
    }
    for (unsigned i = 0; i < 12u; i++) {
        nonce[i] = (uint8_t)(0xB0u + i);
    }
    size_t flen = 0;
    TEST_ASSERT_EQUAL(BACKUP_OK,
                      backup_seal_with(&PORT, BACKUP_FIXTURE_V1_PASSWORD, 10000u, salt, nonce,
                                       BACKUP_FIXTURE_V1_PAYLOAD,
                                       sizeof(BACKUP_FIXTURE_V1_PAYLOAD), FILEBUF,
                                       sizeof(FILEBUF), &flen));
    TEST_ASSERT_EQUAL(sizeof(BACKUP_FIXTURE_V1_FILE), flen);
    TEST_ASSERT_EQUAL_MEMORY(BACKUP_FIXTURE_V1_FILE, FILEBUF, flen);
}

/* ---- what a backup holds (CFG-005) ------------------------------------------------------------ */

/* CFG-005: everything needed to rebuild the panel, never the panel itself. */
static void test_policy_keeps_the_installation_not_the_identity(void)
{
    /* In: the configuration, the bindings, the household's settings and secrets. */
    TEST_ASSERT_TRUE(backup_policy_keeps("alarm", "config"));
    TEST_ASSERT_TRUE(backup_policy_keeps("zones", "bind"));
    TEST_ASSERT_TRUE(backup_policy_keeps("net", "ssid"));
    TEST_ASSERT_TRUE(backup_policy_keeps("net", "pass"));
    TEST_ASSERT_TRUE(backup_policy_keeps("mqtt", "host"));
    TEST_ASSERT_TRUE(backup_policy_keeps("mqtt", "pass"));
    TEST_ASSERT_TRUE(backup_policy_keeps("mqtt", "ca"));
    TEST_ASSERT_TRUE(backup_policy_keeps("ntfy", "tok"));
    TEST_ASSERT_TRUE(backup_policy_keeps("pushover", "usr"));
    TEST_ASSERT_TRUE(backup_policy_keeps("weblogin", "u1"));
    TEST_ASSERT_TRUE(backup_policy_keeps("zwave", "keys"));
    TEST_ASSERT_TRUE(backup_policy_keeps("zwave", "nodes"));
    TEST_ASSERT_TRUE(backup_policy_keeps("zwave", "prov"));
    /* ZB-020 (docs/features/20 devices.md §13): the Zigbee panel's namespace, whole - the
     * registry's meta and records, the install codes, the channel, sirens and keypads. */
    TEST_ASSERT_TRUE(backup_policy_keeps("zbpanel", "meta"));
    TEST_ASSERT_TRUE(backup_policy_keeps("zbpanel", "d00"));
    TEST_ASSERT_TRUE(backup_policy_keeps("zbpanel", "d63"));
    TEST_ASSERT_TRUE(backup_policy_keeps("zbpanel", "codes"));
    TEST_ASSERT_TRUE(backup_policy_keeps("zbpanel", "cfg"));
    TEST_ASSERT_TRUE(backup_policy_keeps("zbpanel", "wd"));
    TEST_ASSERT_TRUE(backup_policy_keeps("zbpanel", "ace"));
    /* S05: the update server the updater reads (the trust record) and the channel; the old
     * manifest source "src" is no longer read, so no longer kept. */
    TEST_ASSERT_TRUE(backup_policy_keeps("ota", "trust"));
    TEST_ASSERT_TRUE(backup_policy_keeps("ota", "chan"));
    TEST_ASSERT_FALSE(backup_policy_keeps("ota", "src"));
    TEST_ASSERT_TRUE(backup_policy_retired("ota", "src"));
    TEST_ASSERT_FALSE(backup_policy_retired("ota", "trust"));
    TEST_ASSERT_FALSE(backup_policy_retired(NULL, "src"));
    TEST_ASSERT_FALSE(backup_policy_retired("ota", NULL));
    TEST_ASSERT_FALSE(backup_policy_keeps("ota", "try"));
    TEST_ASSERT_FALSE(backup_policy_keeps("ota", "upd"));
    TEST_ASSERT_FALSE(backup_policy_keeps("ota", "rbv"));
    TEST_ASSERT_TRUE(backup_policy_keeps("ui", "bright"));
    TEST_ASSERT_TRUE(backup_policy_keeps("sys", "setup"));
    /* AUT-008: the automations, whole - the store's meta and every record */
    TEST_ASSERT_TRUE(backup_policy_keeps("automations", "meta"));
    TEST_ASSERT_TRUE(backup_policy_keeps("automations", "a01"));
    TEST_ASSERT_TRUE(backup_policy_keeps("automations", "a16"));

    /* Out: the panel's identity, its keys, the developer enrollment. */
    TEST_ASSERT_FALSE(backup_policy_keeps("mqtt", "id"));
    TEST_ASSERT_FALSE(backup_policy_keeps("mqtt", "did"));
    TEST_ASSERT_FALSE(backup_policy_keeps("halink", "key"));
    TEST_ASSERT_FALSE(backup_policy_keeps("halink", "peer"));
    TEST_ASSERT_FALSE(backup_policy_keeps("panelweb", "mode"));
    TEST_ASSERT_FALSE(backup_policy_keeps("devrem", "cert"));
    TEST_ASSERT_FALSE(backup_policy_keeps("devota", "anything"));
    /* Out: this board's counters and what is in flight. */
    TEST_ASSERT_FALSE(backup_policy_keeps("alarm", "rt"));
    TEST_ASSERT_FALSE(backup_policy_keeps("mqtt", "acked"));
    TEST_ASSERT_FALSE(backup_policy_keeps("notify", "ackedn"));
    TEST_ASSERT_FALSE(backup_policy_keeps("notify", "ackedp"));
    TEST_ASSERT_TRUE(backup_policy_keeps("notify", "cats"));
    TEST_ASSERT_FALSE(backup_policy_keeps("log", "boot"));
    TEST_ASSERT_FALSE(backup_policy_keeps("ota", "pending"));
    TEST_ASSERT_FALSE(backup_policy_keeps("ota", "cp_try"));
    TEST_ASSERT_FALSE(backup_policy_keeps("net", "previous"));
    TEST_ASSERT_FALSE(backup_policy_keeps("nettrial", "ssid"));
    TEST_ASSERT_FALSE(backup_policy_keeps("touch", "cal"));
    TEST_ASSERT_FALSE(backup_policy_keeps("nvs.net80211", "sta.ssid"));
    TEST_ASSERT_FALSE(backup_policy_keeps(NULL, "x"));
    /* ZB-020: the Zigbee stack's own namespaces are not in it until BL-036 step 5 names them,
     * and a name only near "zbpanel" is not it. */
    TEST_ASSERT_FALSE(backup_policy_keeps("zb_storage", "nwk"));
    TEST_ASSERT_FALSE(backup_policy_keeps("zbpane", "meta"));
    TEST_ASSERT_FALSE(backup_policy_keeps("zbpanel2", "meta"));
    TEST_ASSERT_FALSE(backup_policy_keeps("zbpanel", NULL));

    /* The namespaces a restore replaces: the table's, each once. */
    size_t n = 0;
    for (const char *const *ns = backup_policy_namespaces(); *ns != NULL; ns++, n++) {
        for (const char *const *other = ns + 1; *other != NULL; other++) {
            TEST_ASSERT_TRUE(strcmp(*ns, *other) != 0);
        }
    }
    TEST_ASSERT_EQUAL(16u, n);
    bool zigbee = false;
    for (const char *const *ns = backup_policy_namespaces(); *ns != NULL; ns++) {
        zigbee = zigbee || strcmp(*ns, "zbpanel") == 0;
        TEST_ASSERT_TRUE(strlen(*ns) <= 15u); /* an NVS namespace's longest name */
    }
    TEST_ASSERT_TRUE(zigbee);
}

/* ---- the check before a restore (CFG-006) ------------------------------------------------------ */

static void fake_hash(void *ctx, const uint8_t salt[ALARM_CODE_SALT_LEN], const char *code,
                      uint8_t out[ALARM_CODE_HASH_LEN])
{
    (void)ctx;
    memset(out, 0, ALARM_CODE_HASH_LEN);
    for (size_t i = 0; code[i] != '\0' && i < ALARM_CODE_HASH_LEN; i++) {
        out[i] = (uint8_t)(code[i] ^ salt[i % ALARM_CODE_SALT_LEN]);
    }
}

static uint8_t CFG_BLOB[ALARM_CONFIG_BLOB_MAX];
static uint8_t BIND_BLOB[ZONE_HUB_BLOB_MAX];

/* A panel's worth: a configuration with two zones and the factory users, empty bindings, the
 * Wi-Fi network and Z-Wave keys. extra: one more entry, or NULL. */
static size_t panel_payload(const char *board, const backup_entry_t *extra, bool with_config)
{
    alarm_config_t *cfg = &SCRATCH.cfg;
    alarm_config_defaults(cfg);
    alarm_config_set_default_users(cfg, fake_hash, NULL);
    alarm_zone_config_defaults(&cfg->zones[0], 1, ALARM_ZT_ENTRY_EXIT_1, 1);
    alarm_zone_config_defaults(&cfg->zones[1], 2, ALARM_ZT_PERIMETER, 1);
    size_t cfg_len = 0;
    TEST_ASSERT_EQUAL(ALARM_OK, alarm_config_encode(cfg, CFG_BLOB, sizeof(CFG_BLOB), &cfg_len));
    zone_hub_init(&SCRATCH.hub, NULL);
    size_t bind_len = 0;
    TEST_ASSERT_EQUAL(ZONE_HUB_OK,
                      zone_hub_encode(&SCRATCH.hub, BIND_BLOB, sizeof(BIND_BLOB), &bind_len));

    backup_meta_t meta = META;
    snprintf(meta.board, sizeof(meta.board), "%s", board);
    backup_writer_t w;
    backup_writer_init(&w, BUF, sizeof(BUF), &meta);
    const uint8_t keys[16] = {1};
    const backup_entry_t list[] = {
        ent("alarm", "config", BACKUP_T_BLOB, CFG_BLOB, (uint32_t)cfg_len),
        ent("zones", "bind", BACKUP_T_BLOB, BIND_BLOB, (uint32_t)bind_len),
        ent("net", "ssid", BACKUP_T_STR, "Harbor House", 12),
        ent("zwave", "keys", BACKUP_T_BLOB, keys, 16),
    };
    for (size_t i = with_config ? 0u : 1u; i < sizeof(list) / sizeof(list[0]); i++) {
        TEST_ASSERT_TRUE(backup_writer_add(&w, &list[i]));
    }
    if (extra != NULL) {
        TEST_ASSERT_TRUE(backup_writer_add(&w, extra));
    }
    size_t len = 0;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_writer_finish(&w, &len));
    return len;
}

/* CFG-006: a whole panel checks out, and the check says what the file would bring back. */
static void test_check_accepts_a_panel(void)
{
    const size_t len = panel_payload("crowpanel_p4", NULL, true);
    backup_summary_t s;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_check(BUF, len, "crowpanel_p4", &SCRATCH, &s));
    TEST_ASSERT_EQUAL_UINT32(4, s.entries);
    TEST_ASSERT_EQUAL_UINT16(2, s.zones);
    TEST_ASSERT_EQUAL_UINT16(2, s.users); /* the installer and the master */
    TEST_ASSERT_TRUE(s.bindings);
    TEST_ASSERT_TRUE(s.network);
    TEST_ASSERT_TRUE(s.zwave);
    TEST_ASSERT_EQUAL_STRING("0.6.0", s.meta.firmware);
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_check(BUF, len, NULL, &SCRATCH, &s)); /* any board */
}

/* ZB-020, CFG-006: a backup with the Zigbee panel's records checks out; one from before Zigbee
 * (the frozen v1 file's entries) is still wholly kept by the policy, so it restores as it did. */
static void test_check_accepts_zigbee_and_old_files(void)
{
    const uint8_t record[12] = {0x01, 0x00, 0x01, 0x00, 0x02, 0x01, 0x00, 0x00,
                                0x00, 0x00, 0xAB, 0x02}; /* any bytes: the check reads no further */
    const backup_entry_t zb = ent("zbpanel", "d00", BACKUP_T_BLOB, record, sizeof(record));
    const size_t len = panel_payload("crowpanel_p4", &zb, true);
    backup_summary_t s;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_check(BUF, len, "crowpanel_p4", &SCRATCH, &s));
    TEST_ASSERT_EQUAL_UINT32(5, s.entries);

    /* The stack's own namespace is not in the policy yet (BL-036 step 5): refused, not guessed. */
    const backup_entry_t stack = ent("zb_storage", "nwk", BACKUP_T_BLOB, record, sizeof(record));
    const size_t len2 = panel_payload("crowpanel_p4", &stack, true);
    TEST_ASSERT_EQUAL(BACKUP_ERR_CONTENT, backup_check(BUF, len2, "crowpanel_p4", &SCRATCH, &s));
    TEST_ASSERT_EQUAL_STRING("zb_storage", s.bad_ns);

    /* The v1 file's entries are kept, or retired (S05: "ota"/"src", accepted and left out). */
    static const char *const V1[][2] = {{"net", "ssid"},   {"ui", "bright"}, {"mqtt", "port"},
                                        {"zwave", "keys"}, {"ota", "src"},   {"rf", "min_us"}};
    for (size_t i = 0; i < sizeof(V1) / sizeof(V1[0]); i++) {
        TEST_ASSERT_TRUE(backup_policy_keeps(V1[i][0], V1[i][1]) ||
                         backup_policy_retired(V1[i][0], V1[i][1]));
    }

    /* S05, CFG-006: a file made before the trust record still checks out with its "src"... */
    const char *url = "https://updates.example/releases.json";
    const backup_entry_t src = ent("ota", "src", BACKUP_T_STR, url, (uint32_t)strlen(url));
    const size_t len3 = panel_payload("crowpanel_p4", &src, true);
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_check(BUF, len3, "crowpanel_p4", &SCRATCH, &s));
    TEST_ASSERT_EQUAL_UINT32(5, s.entries);
    /* ...and the restore leaves it out: the updater doesn't read it. */
    backup_entry_t out;
    bool write = true;
    uint8_t buf[PROTO_FWSRV_TRUST_BLOB_MAX];
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_restore_entry(&src, buf, sizeof(buf), &out, &write));
    TEST_ASSERT_FALSE(write);
}

/* CFG-006: made on another kind of panel, or carrying an identity, a repeated entry, a
 * configuration the engine wouldn't take or no configuration at all: refused as a whole. */
static void test_check_refuses_the_wrong_file(void)
{
    backup_summary_t s;
    size_t len = panel_payload("e32r40t", NULL, true);
    TEST_ASSERT_EQUAL(BACKUP_ERR_BOARD, backup_check(BUF, len, "crowpanel_p4", &SCRATCH, &s));

    const uint8_t key[8] = {9};
    const backup_entry_t identity = ent("halink", "key", BACKUP_T_BLOB, key, 8);
    len = panel_payload("crowpanel_p4", &identity, true);
    TEST_ASSERT_EQUAL(BACKUP_ERR_CONTENT, backup_check(BUF, len, "crowpanel_p4", &SCRATCH, &s));
    TEST_ASSERT_EQUAL_STRING("halink", s.bad_ns);
    TEST_ASSERT_EQUAL_STRING("key", s.bad_key);

    const backup_entry_t panel_id = ent("mqtt", "id", BACKUP_T_STR, "RA-0000", 7);
    len = panel_payload("crowpanel_p4", &panel_id, true);
    TEST_ASSERT_EQUAL(BACKUP_ERR_CONTENT, backup_check(BUF, len, "crowpanel_p4", &SCRATCH, &s));

    const backup_entry_t again = ent("net", "ssid", BACKUP_T_STR, "Other", 5);
    len = panel_payload("crowpanel_p4", &again, true);
    TEST_ASSERT_EQUAL(BACKUP_ERR_CONTENT, backup_check(BUF, len, "crowpanel_p4", &SCRATCH, &s));
    TEST_ASSERT_EQUAL_STRING("ssid", s.bad_key);

    len = panel_payload("crowpanel_p4", NULL, false);
    TEST_ASSERT_EQUAL(BACKUP_ERR_CONTENT, backup_check(BUF, len, "crowpanel_p4", &SCRATCH, &s));

    /* A configuration blob damaged inside (its CRC no longer matches). */
    len = panel_payload("crowpanel_p4", NULL, true);
    const size_t cfg_at = 1u + 1u + 8u + 1u + 5u + 1u + 12u /* META */ + 1u + 1u + 5u + 1u + 6u +
                          1u + 4u /* the entry's head */;
    BUF[cfg_at + 20u] = (uint8_t)~(unsigned)BUF[cfg_at + 20u];
    TEST_ASSERT_EQUAL(BACKUP_ERR_CONTENT, backup_check(BUF, len, "crowpanel_p4", &SCRATCH, &s));
    TEST_ASSERT_EQUAL_STRING("config", s.bad_key);

    /* The configuration as a string, not a blob. */
    const backup_entry_t cfg_str = ent("alarm", "config", BACKUP_T_STR, "ACFG", 4);
    len = panel_payload("crowpanel_p4", NULL, false);
    backup_writer_t w;
    backup_writer_init(&w, BUF, sizeof(BUF), &META);
    TEST_ASSERT_TRUE(backup_writer_add(&w, &cfg_str));
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_writer_finish(&w, &len));
    TEST_ASSERT_EQUAL(BACKUP_ERR_CONTENT, backup_check(BUF, len, "crowpanel_p4", &SCRATCH, &s));
}

/* CFG-005 + CFG-006 end to end: a panel sealed into a file, opened and checked. */
static void test_panel_through_a_file(void)
{
    const size_t len = panel_payload("crowpanel_p4", NULL, true);
    size_t flen = 0;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_seal(&PORT, "Harbor House 2026", BACKUP_ITER_MIN, BUF,
                                             len, FILEBUF, sizeof(FILEBUF), &flen));
    size_t got = 0;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_open(&PORT, "Harbor House 2026", FILEBUF, flen, OUT,
                                             sizeof(OUT), &got));
    backup_summary_t s;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_check(OUT, got, "crowpanel_p4", &SCRATCH, &s));
    TEST_ASSERT_EQUAL_UINT16(2, s.zones);
}

/* CORE-CFG-021, CFG-006: the configuration a restore stores is the file's with no approval and
 * fresh binding revisions above the running panel's and the file's; what it can't make is
 * refused before anything is written. */
static void test_restore_config_never_brings_approval_back(void)
{
    static alarm_config_t current, stored, again;
    static uint8_t blob[ALARM_CONFIG_BLOB_MAX];
    /* The file: an older panel whose zone 1 name was approved under binding 3. */
    alarm_config_t *cfg = &SCRATCH.cfg;
    alarm_config_defaults(cfg);
    alarm_config_set_default_users(cfg, fake_hash, NULL);
    alarm_zone_config_defaults(&cfg->zones[0], 1, ALARM_ZT_ENTRY_EXIT_1, 1);
    alarm_zone_config_defaults(&cfg->zones[1], 2, ALARM_ZT_PERIMETER, 1);
    alarm_zone_set_word(&cfg->zones[0], 0, 12u);
    cfg->zones[0].voice_binding_revision = 3u;
    cfg->zones[0].voice_approved_revision = 3u;
    cfg->zones[0].voice_pack_revision = 7u;
    cfg->zones[1].voice_binding_revision = 2u;
    cfg->opt.voice_revision_counter = 3u;
    TEST_ASSERT_TRUE(alarm_zone_voice_approved(&cfg->zones[0]));
    size_t cfg_len = 0;
    TEST_ASSERT_EQUAL(ALARM_OK, alarm_config_encode(cfg, CFG_BLOB, sizeof(CFG_BLOB), &cfg_len));
    backup_writer_t w;
    backup_writer_init(&w, BUF, sizeof(BUF), &META);
    const backup_entry_t e = ent("alarm", "config", BACKUP_T_BLOB, CFG_BLOB, (uint32_t)cfg_len);
    TEST_ASSERT_TRUE(backup_writer_add(&w, &e));
    size_t len = 0;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_writer_finish(&w, &len));
    backup_summary_t s;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_check(BUF, len, NULL, &SCRATCH, &s));

    /* The running panel has handed out up to 20. */
    alarm_config_defaults(&current);
    current.opt.voice_revision_counter = 20u;

    size_t n = 0;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_restore_config(BUF, len, &current, &SCRATCH, blob,
                                                       sizeof(blob), &n));
    TEST_ASSERT_EQUAL(ALARM_OK, alarm_config_decode(blob, n, &stored));
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(&stored.zones[0]));
    TEST_ASSERT_EQUAL_UINT32(0u, stored.zones[0].voice_approved_revision);
    TEST_ASSERT_EQUAL_UINT32(0u, stored.zones[0].voice_pack_revision);
    TEST_ASSERT_EQUAL_UINT32(21u, stored.zones[0].voice_binding_revision);
    TEST_ASSERT_EQUAL_UINT32(22u, stored.zones[1].voice_binding_revision);
    TEST_ASSERT_EQUAL_UINT32(22u, stored.opt.voice_revision_counter);
    TEST_ASSERT_EQUAL_UINT16(12u, alarm_zone_word(&stored.zones[0], 0)); /* the words stay */
    TEST_ASSERT_EQUAL_MEMORY(&cfg->users, &stored.users, sizeof(stored.users)); /* the rest too */

    /* Cut short after the configuration was stored, and restored again: higher still. */
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_restore_config(BUF, len, &stored, &SCRATCH, blob,
                                                       sizeof(blob), &n));
    TEST_ASSERT_EQUAL(ALARM_OK, alarm_config_decode(blob, n, &again));
    TEST_ASSERT_EQUAL_UINT32(23u, again.zones[0].voice_binding_revision);
    TEST_ASSERT_EQUAL_UINT32(24u, again.opt.voice_revision_counter);
    TEST_ASSERT_FALSE(alarm_zone_voice_approved(&again.zones[0]));

    /* Refused, with nothing to store: arguments, a buffer too small, no configuration, one
     * that doesn't decode, a payload that isn't one. */
    TEST_ASSERT_EQUAL(BACKUP_ERR_ARG, backup_restore_config(NULL, len, &current, &SCRATCH, blob,
                                                            sizeof(blob), &n));
    TEST_ASSERT_EQUAL(BACKUP_ERR_ARG, backup_restore_config(BUF, len, NULL, &SCRATCH, blob,
                                                            sizeof(blob), &n));
    TEST_ASSERT_EQUAL(BACKUP_ERR_ARG, backup_restore_config(BUF, len, &current, NULL, blob,
                                                            sizeof(blob), &n));
    TEST_ASSERT_EQUAL(BACKUP_ERR_ARG, backup_restore_config(BUF, len, &current, &SCRATCH, NULL,
                                                            sizeof(blob), &n));
    TEST_ASSERT_EQUAL(BACKUP_ERR_ARG, backup_restore_config(BUF, len, &current, &SCRATCH, blob,
                                                            sizeof(blob), NULL));
    n = 99u;
    TEST_ASSERT_EQUAL(BACKUP_ERR_ARG,
                      backup_restore_config(BUF, len, &current, &SCRATCH, blob, 16u, &n));
    TEST_ASSERT_EQUAL_UINT(0u, n);
    TEST_ASSERT_EQUAL(BACKUP_ERR_FORMAT, backup_restore_config(BUF, 3u, &current, &SCRATCH, blob,
                                                               sizeof(blob), &n));
    len = panel_payload("crowpanel_p4", NULL, false);
    TEST_ASSERT_EQUAL(BACKUP_ERR_CONTENT, backup_restore_config(BUF, len, &current, &SCRATCH,
                                                                blob, sizeof(blob), &n));
    const backup_entry_t bad = ent("alarm", "config", BACKUP_T_BLOB, "ACFG", 4);
    backup_writer_init(&w, BUF, sizeof(BUF), &META);
    TEST_ASSERT_TRUE(backup_writer_add(&w, &bad));
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_writer_finish(&w, &len));
    TEST_ASSERT_EQUAL(BACKUP_ERR_CONTENT, backup_restore_config(BUF, len, &current, &SCRATCH,
                                                                blob, sizeof(blob), &n));
    const backup_entry_t str = ent("alarm", "config", BACKUP_T_STR, "ACFG", 4);
    backup_writer_init(&w, BUF, sizeof(BUF), &META);
    TEST_ASSERT_TRUE(backup_writer_add(&w, &str));
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_writer_finish(&w, &len));
    TEST_ASSERT_EQUAL(BACKUP_ERR_CONTENT, backup_restore_config(BUF, len, &current, &SCRATCH,
                                                                blob, sizeof(blob), &n));
}

/* ---- the update server in a restore (S05, CFG-006) ------------------------------------------ */

static size_t trust_blob(const char *server, bool key, uint8_t *out, size_t cap)
{
    proto_fwsrv_trust_t t;
    memset(&t, 0, sizeof(t));
    snprintf(t.server, sizeof(t.server), "%s", server);
    if (key) {
        snprintf(t.key_name, sizeof(t.key_name), "%s", "Harbor House releases");
        memset(t.key_sha256, 0x5A, sizeof(t.key_sha256));
        t.has_key = true;
    }
    const size_t n = proto_fwsrv_trust_encode(&t, out, cap);
    TEST_ASSERT_TRUE(n > 0u);
    return n;
}

/* S05, CFG-006, OTA-020: the server and the channel come back; a signing key accepted for that
 * server does not - it is confirmed again with its fingerprint (the updater has no "restored,
 * not yet confirmed" state, so the safe form is none). */
static void test_restore_brings_the_update_server_back_without_its_key(void)
{
    uint8_t stored[PROTO_FWSRV_TRUST_BLOB_MAX];
    uint8_t buf[PROTO_FWSRV_TRUST_BLOB_MAX];
    backup_entry_t out;
    bool write = false;

    /* A custom server with its accepted key: written without the key. */
    size_t n = trust_blob("updates.example.net", true, stored, sizeof(stored));
    const backup_entry_t trust = ent("ota", "trust", BACKUP_T_BLOB, stored, (uint32_t)n);
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_restore_entry(&trust, buf, sizeof(buf), &out, &write));
    TEST_ASSERT_TRUE(write);
    TEST_ASSERT_EQUAL_STRING("ota", out.ns);
    TEST_ASSERT_EQUAL_STRING("trust", out.key);
    TEST_ASSERT_EQUAL_UINT8(BACKUP_T_BLOB, out.type);
    TEST_ASSERT_TRUE(out.value == buf);
    proto_fwsrv_trust_t t;
    TEST_ASSERT_TRUE(proto_fwsrv_trust_decode(out.value, out.len, &t));
    TEST_ASSERT_EQUAL_STRING("updates.example.net", t.server);
    TEST_ASSERT_FALSE(t.has_key);
    TEST_ASSERT_EQUAL_STRING("", t.key_name);
    const uint8_t zero[32] = {0};
    TEST_ASSERT_EQUAL_MEMORY(zero, t.key_sha256, sizeof(zero));

    /* A server with no extra key: written as it is. */
    n = trust_blob("updates.example.net", false, stored, sizeof(stored));
    const backup_entry_t plain = ent("ota", "trust", BACKUP_T_BLOB, stored, (uint32_t)n);
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_restore_entry(&plain, buf, sizeof(buf), &out, &write));
    TEST_ASSERT_TRUE(write);
    TEST_ASSERT_TRUE(out.value == stored);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)n, out.len);

    /* The channel, and any other kept entry: as they are. */
    const backup_entry_t chan = ent("ota", "chan", BACKUP_T_STR, "beta", 4);
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_restore_entry(&chan, buf, sizeof(buf), &out, &write));
    TEST_ASSERT_TRUE(write);
    TEST_ASSERT_EQUAL_MEMORY("beta", out.value, 4);
    const backup_entry_t ssid = ent("net", "ssid", BACKUP_T_STR, "Harbor House", 12);
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_restore_entry(&ssid, NULL, 0u, &out, &write));
    TEST_ASSERT_TRUE(write);

    /* A damaged record, or one that isn't a blob: left out (the panel uses its own server). */
    n = trust_blob("updates.example.net", true, stored, sizeof(stored));
    stored[5] = (uint8_t)(stored[5] ^ 0x01u);
    const backup_entry_t damaged = ent("ota", "trust", BACKUP_T_BLOB, stored, (uint32_t)n);
    write = true;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_restore_entry(&damaged, buf, sizeof(buf), &out, &write));
    TEST_ASSERT_FALSE(write);
    const backup_entry_t as_str = ent("ota", "trust", BACKUP_T_STR, "updates.example.net", 19);
    write = true;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_restore_entry(&as_str, buf, sizeof(buf), &out, &write));
    TEST_ASSERT_FALSE(write);

    /* Refused: no room for the record without its key, or no arguments. */
    n = trust_blob("updates.example.net", true, stored, sizeof(stored));
    const backup_entry_t again = ent("ota", "trust", BACKUP_T_BLOB, stored, (uint32_t)n);
    TEST_ASSERT_EQUAL(BACKUP_ERR_ARG, backup_restore_entry(&again, buf, 8u, &out, &write));
    TEST_ASSERT_EQUAL(BACKUP_ERR_ARG, backup_restore_entry(&again, NULL, 0u, &out, &write));
    TEST_ASSERT_EQUAL(BACKUP_ERR_ARG, backup_restore_entry(NULL, buf, sizeof(buf), &out, &write));
    TEST_ASSERT_EQUAL(BACKUP_ERR_ARG, backup_restore_entry(&again, buf, sizeof(buf), NULL, &write));
    TEST_ASSERT_EQUAL(BACKUP_ERR_ARG, backup_restore_entry(&again, buf, sizeof(buf), &out, NULL));

    /* The whole file checks out with the trust record and the channel in it. */
    const size_t len = panel_payload("crowpanel_p4", &again, true);
    backup_summary_t s;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_check(BUF, len, "crowpanel_p4", &SCRATCH, &s));
}

/* ---- the panel's entries through a port (S04, CFG-005, CFG-006) ----------------------------- */

typedef struct {
    const char *ns;
    const char *key;
    uint8_t type;
    const char *value; /* a string's bytes; other types: its first byte */
} fake_entry_t;

typedef struct {
    const fake_entry_t *entries;
    size_t count;
    /* what goes wrong (NULL / 0: nothing) */
    const char *fail_open;    /* this namespace can't be opened (not: isn't there) */
    const char *fail_first;   /* its listing can't start */
    const char *fail_next_ns; /* its listing fails after fail_next_after entries */
    size_t fail_next_after;
    const char *fail_read;    /* this key can't be read */
    /* the walk */
    const char *ns;
    size_t pos;
    size_t listed;
    int opens, closes, lists, ends;
} fake_store_t;

static bool fake_has_ns(const fake_store_t *f, const char *ns)
{
    for (size_t i = 0; i < f->count; i++) {
        if (strcmp(f->entries[i].ns, ns) == 0) {
            return true;
        }
    }
    return false;
}

static backup_store_status_t fake_open(void *ctx, const char *ns)
{
    fake_store_t *f = ctx;
    if (f->fail_open != NULL && strcmp(f->fail_open, ns) == 0) {
        return BACKUP_STORE_FAILED;
    }
    if (!fake_has_ns(f, ns)) {
        return BACKUP_STORE_ABSENT;
    }
    f->opens++;
    return BACKUP_STORE_OK;
}

static backup_store_status_t fake_scan(fake_store_t *f, char *key, uint8_t *type)
{
    if (f->fail_next_ns != NULL && strcmp(f->fail_next_ns, f->ns) == 0 &&
        f->listed >= f->fail_next_after) {
        return BACKUP_STORE_FAILED;
    }
    for (; f->pos < f->count; f->pos++) {
        if (strcmp(f->entries[f->pos].ns, f->ns) == 0) {
            snprintf(key, BACKUP_NAME_MAX + 1u, "%s", f->entries[f->pos].key);
            *type = f->entries[f->pos].type;
            f->pos++;
            f->listed++;
            return BACKUP_STORE_OK;
        }
    }
    return BACKUP_STORE_ABSENT;
}

static backup_store_status_t fake_first(void *ctx, const char *ns, char *key, uint8_t *type)
{
    fake_store_t *f = ctx;
    f->lists++;
    f->ns = ns;
    f->pos = 0u;
    f->listed = 0u;
    if (f->fail_first != NULL && strcmp(f->fail_first, ns) == 0) {
        return BACKUP_STORE_FAILED;
    }
    return fake_scan(f, key, type);
}

static backup_store_status_t fake_next(void *ctx, char *key, uint8_t *type)
{
    return fake_scan(ctx, key, type);
}

static void fake_end(void *ctx)
{
    ((fake_store_t *)ctx)->ends++;
}

static backup_store_status_t fake_read(void *ctx, const char *key, uint8_t type, uint8_t *buf,
                                       size_t cap, uint32_t *len)
{
    fake_store_t *f = ctx;
    if (f->fail_read != NULL && strcmp(f->fail_read, key) == 0) {
        return BACKUP_STORE_FAILED;
    }
    for (size_t i = 0; i < f->count; i++) {
        const fake_entry_t *e = &f->entries[i];
        if (strcmp(e->ns, f->ns) == 0 && strcmp(e->key, key) == 0 && e->type == type) {
            const size_t n = type == BACKUP_T_STR || type == BACKUP_T_BLOB ? strlen(e->value) : 1u;
            TEST_ASSERT_TRUE(n <= cap);
            memcpy(buf, e->value, n);
            *len = (uint32_t)n;
            return BACKUP_STORE_OK;
        }
    }
    return BACKUP_STORE_FAILED;
}

static void fake_close(void *ctx)
{
    ((fake_store_t *)ctx)->closes++;
}

static backup_store_t fake_port(fake_store_t *f)
{
    const backup_store_t s = {.ctx = f, .open = fake_open, .first = fake_first,
                              .next = fake_next, .end = fake_end, .read = fake_read,
                              .close = fake_close};
    return s;
}

/* A panel: kept entries in several namespaces, some the policy leaves with the board, and a
 * kind of entry a backup doesn't hold (type 0). */
static const fake_entry_t PANEL[] = {
    {"zones", "bind", BACKUP_T_BLOB, "bindings"},
    {"ntfy", "url", BACKUP_T_STR, "https://ntfy.example"},
    {"ntfy", "topic", BACKUP_T_STR, "harbor"},
    {"ntfy", "tok", BACKUP_T_STR, "secret-token"},
    {"mqtt", "id", BACKUP_T_STR, "RA-0000"},
    {"mqtt", "host", BACKUP_T_STR, "192.0.2.10"},
    {"zwave", "keys", BACKUP_T_BLOB, "k"},
    {"zwave", "nodes", BACKUP_T_BLOB, "n"},
    {"zwave", "prov", BACKUP_T_BLOB, "p"},
    {"zwave", "radio", BACKUP_T_U8, "r"},
    {"zwave", "odd", 0u, "?"},
    {"ota", "src", BACKUP_T_STR, "https://updates.example/releases.json"},
    {"ota", "chan", BACKUP_T_STR, "beta"},
    {"ota", "try", BACKUP_T_BLOB, "attempt"},
    {"halink", "key", BACKUP_T_BLOB, "identity"},
};

/* The service's loop: every policy namespace; any failure means no file. */
static backup_store_status_t collect_all(fake_store_t *f, size_t *len, bool *fits)
{
    backup_writer_t w;
    backup_writer_init(&w, BUF, sizeof(BUF), &META);
    static uint8_t value[256];
    const backup_store_t port = fake_port(f);
    *fits = true;
    for (const char *const *ns = backup_policy_namespaces(); *ns != NULL; ns++) {
        const backup_store_status_t st = backup_collect(&w, &port, *ns, value, sizeof(value), fits);
        if (st != BACKUP_STORE_OK || !*fits) {
            return st;
        }
    }
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_writer_finish(&w, len));
    return BACKUP_STORE_OK;
}

/* CFG-005, S04: every entry the policy keeps, from every namespace there is; a namespace that
 * isn't on this panel is no failure; nothing the policy leaves with the board. */
static void test_collect_reads_what_the_policy_keeps(void)
{
    fake_store_t f = {.entries = PANEL, .count = sizeof(PANEL) / sizeof(PANEL[0])};
    size_t len = 0;
    bool fits = false;
    TEST_ASSERT_EQUAL(BACKUP_STORE_OK, collect_all(&f, &len, &fits));
    TEST_ASSERT_TRUE(fits);
    TEST_ASSERT_EQUAL_INT(f.opens, f.closes);
    TEST_ASSERT_EQUAL_INT(f.lists, f.ends);

    /* in the policy's order of namespaces */
    static const char *const WANT[] = {"zones/bind", "mqtt/host",   "ntfy/url",
                                       "ntfy/topic", "ntfy/tok",    "zwave/keys",
                                       "zwave/nodes", "zwave/prov", "zwave/radio",
                                       "ota/chan"};
    backup_reader_t r;
    backup_meta_t meta;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_reader_init(&r, BUF, len, &meta));
    for (size_t i = 0; i < sizeof(WANT) / sizeof(WANT[0]); i++) {
        backup_entry_t e;
        bool done = true;
        TEST_ASSERT_EQUAL(BACKUP_OK, backup_reader_next(&r, &e, &done));
        TEST_ASSERT_FALSE(done);
        char name[40];
        snprintf(name, sizeof(name), "%s/%s", e.ns, e.key);
        TEST_ASSERT_EQUAL_STRING(WANT[i], name);
        if (i == 4u) {
            TEST_ASSERT_EQUAL_UINT32(12u, e.len);
            TEST_ASSERT_EQUAL_MEMORY("secret-token", e.value, 12u);
        }
    }
    backup_entry_t e;
    bool done = false;
    TEST_ASSERT_EQUAL(BACKUP_OK, backup_reader_next(&r, &e, &done));
    TEST_ASSERT_TRUE(done);

    /* A panel with nothing stored at all: an empty backup, not a failure (the service's own
     * check then refuses a file without the configuration). */
    fake_store_t empty = {.entries = PANEL, .count = 0u};
    TEST_ASSERT_EQUAL(BACKUP_STORE_OK, collect_all(&empty, &len, &fits));
}

/* S04: a namespace that can't be opened, a listing that can't start or fails part way, an entry
 * that can't be read: the backup fails as a whole, never leaves the namespace out. */
static void test_collect_fails_on_what_it_cannot_read(void)
{
    const size_t count = sizeof(PANEL) / sizeof(PANEL[0]);
    size_t len = 0;
    bool fits = true;

    fake_store_t f = {.entries = PANEL, .count = count, .fail_open = "ntfy"};
    TEST_ASSERT_EQUAL(BACKUP_STORE_FAILED, collect_all(&f, &len, &fits));
    TEST_ASSERT_EQUAL_INT(f.opens, f.closes);

    /* Even for a namespace this panel doesn't have: "can't open" is not "not there". */
    fake_store_t g = {.entries = PANEL, .count = count, .fail_open = "pushover"};
    TEST_ASSERT_EQUAL(BACKUP_STORE_FAILED, collect_all(&g, &len, &fits));

    fake_store_t h = {.entries = PANEL, .count = count, .fail_first = "zones"};
    TEST_ASSERT_EQUAL(BACKUP_STORE_FAILED, collect_all(&h, &len, &fits));
    TEST_ASSERT_EQUAL_INT(h.opens, h.closes);
    TEST_ASSERT_EQUAL_INT(h.lists, h.ends);

    /* Z-Wave's listing fails after two of its four entries. */
    fake_store_t k = {.entries = PANEL, .count = count, .fail_next_ns = "zwave",
                      .fail_next_after = 2u};
    TEST_ASSERT_EQUAL(BACKUP_STORE_FAILED, collect_all(&k, &len, &fits));
    TEST_ASSERT_EQUAL_INT(k.opens, k.closes);
    TEST_ASSERT_EQUAL_INT(k.lists, k.ends);

    fake_store_t m = {.entries = PANEL, .count = count, .fail_read = "tok"};
    TEST_ASSERT_EQUAL(BACKUP_STORE_FAILED, collect_all(&m, &len, &fits));
    TEST_ASSERT_EQUAL_INT(m.opens, m.closes);

    /* An entry that isn't kept is never read, so its failure costs nothing. */
    fake_store_t n = {.entries = PANEL, .count = count, .fail_read = "id"};
    TEST_ASSERT_EQUAL(BACKUP_STORE_OK, collect_all(&n, &len, &fits));

    /* A writer with no room: fits says so (the service refuses the file). */
    backup_writer_t w;
    backup_writer_init(&w, BUF, 32u, &META);
    static uint8_t value[256];
    fake_store_t p = {.entries = PANEL, .count = count};
    const backup_store_t port = fake_port(&p);
    fits = true;
    TEST_ASSERT_EQUAL(BACKUP_STORE_OK,
                      backup_collect(&w, &port, "ntfy", value, sizeof(value), &fits));
    TEST_ASSERT_FALSE(fits);
    TEST_ASSERT_EQUAL_INT(p.opens, p.closes);
    TEST_ASSERT_EQUAL_INT(p.lists, p.ends);

    /* Arguments. */
    fits = true;
    TEST_ASSERT_EQUAL(BACKUP_STORE_FAILED,
                      backup_collect(NULL, &port, "ntfy", value, sizeof(value), &fits));
    TEST_ASSERT_EQUAL(BACKUP_STORE_FAILED,
                      backup_collect(&w, NULL, "ntfy", value, sizeof(value), &fits));
    TEST_ASSERT_EQUAL(BACKUP_STORE_FAILED,
                      backup_collect(&w, &port, NULL, value, sizeof(value), &fits));
    TEST_ASSERT_EQUAL(BACKUP_STORE_FAILED,
                      backup_collect(&w, &port, "ntfy", NULL, sizeof(value), &fits));
    TEST_ASSERT_EQUAL(BACKUP_STORE_FAILED,
                      backup_collect(&w, &port, "ntfy", value, sizeof(value), NULL));
}

/* CFG-006, S04: what a restore erases first - the policy's keys, never alarm/config or what it
 * leaves with the board - in rounds of max; a listing that fails is a failure with nothing
 * taken as cleared. */
static void test_erasable_lists_the_policy_keys_or_fails(void)
{
    static const fake_entry_t STORE[] = {
        {"alarm", "config", BACKUP_T_BLOB, "c"},
        {"alarm", "rt", BACKUP_T_BLOB, "r"},
        {"mqtt", "id", BACKUP_T_STR, "RA-0000"},
        {"mqtt", "host", BACKUP_T_STR, "192.0.2.10"},
        {"mqtt", "pass", BACKUP_T_STR, "pw"},
        {"ota", "src", BACKUP_T_STR, "u"},
        {"ota", "trust", BACKUP_T_BLOB, "t"},
    };
    const size_t count = sizeof(STORE) / sizeof(STORE[0]);
    char keys[4][BACKUP_NAME_MAX + 1];
    size_t n = 99u;

    fake_store_t f = {.entries = STORE, .count = count};
    backup_store_t port = fake_port(&f);
    TEST_ASSERT_EQUAL(BACKUP_STORE_OK, backup_erasable(&port, "alarm", keys, 4u, &n));
    TEST_ASSERT_EQUAL_UINT(0u, n);
    TEST_ASSERT_EQUAL(BACKUP_STORE_OK, backup_erasable(&port, "mqtt", keys, 4u, &n));
    TEST_ASSERT_EQUAL_UINT(2u, n);
    TEST_ASSERT_EQUAL_STRING("host", keys[0]);
    TEST_ASSERT_EQUAL_STRING("pass", keys[1]);
    TEST_ASSERT_EQUAL(BACKUP_STORE_OK, backup_erasable(&port, "mqtt", keys, 1u, &n));
    TEST_ASSERT_EQUAL_UINT(1u, n); /* full: the caller erases and asks again */
    TEST_ASSERT_EQUAL(BACKUP_STORE_OK, backup_erasable(&port, "ota", keys, 4u, &n));
    TEST_ASSERT_EQUAL_UINT(1u, n);
    TEST_ASSERT_EQUAL_STRING("trust", keys[0]); /* "src" is the board's now: left alone */
    TEST_ASSERT_EQUAL(BACKUP_STORE_OK, backup_erasable(&port, "zwave", keys, 4u, &n));
    TEST_ASSERT_EQUAL_UINT(0u, n); /* nothing there */
    TEST_ASSERT_EQUAL_INT(f.lists, f.ends);

    fake_store_t g = {.entries = STORE, .count = count, .fail_next_ns = "mqtt",
                      .fail_next_after = 2u};
    port = fake_port(&g);
    n = 99u;
    TEST_ASSERT_EQUAL(BACKUP_STORE_FAILED, backup_erasable(&port, "mqtt", keys, 4u, &n));
    TEST_ASSERT_EQUAL_UINT(0u, n);
    TEST_ASSERT_EQUAL_INT(g.lists, g.ends);

    fake_store_t h = {.entries = STORE, .count = count, .fail_first = "mqtt"};
    port = fake_port(&h);
    TEST_ASSERT_EQUAL(BACKUP_STORE_FAILED, backup_erasable(&port, "mqtt", keys, 4u, &n));
    TEST_ASSERT_EQUAL_UINT(0u, n);
    TEST_ASSERT_EQUAL_INT(h.lists, h.ends);

    TEST_ASSERT_EQUAL(BACKUP_STORE_FAILED, backup_erasable(NULL, "mqtt", keys, 4u, &n));
    TEST_ASSERT_EQUAL(BACKUP_STORE_FAILED, backup_erasable(&port, NULL, keys, 4u, &n));
    TEST_ASSERT_EQUAL(BACKUP_STORE_FAILED, backup_erasable(&port, "mqtt", NULL, 4u, &n));
    TEST_ASSERT_EQUAL(BACKUP_STORE_FAILED, backup_erasable(&port, "mqtt", keys, 0u, &n));
    TEST_ASSERT_EQUAL(BACKUP_STORE_FAILED, backup_erasable(&port, "mqtt", keys, 4u, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_pbkdf2_known_answers);
    RUN_TEST(test_aes256_gcm_known_answers);
    RUN_TEST(test_payload_round_trip);
    RUN_TEST(test_writer_refuses_malformed_entries);
    RUN_TEST(test_reader_refuses_damage_and_skips_new_records);
    RUN_TEST(test_seal_and_open);
    RUN_TEST(test_default_work_and_password_rules);
    RUN_TEST(test_wrong_password_or_changed_file_gives_nothing);
    RUN_TEST(test_not_a_backup_file);
    RUN_TEST(test_crypto_failures_make_no_file);
    RUN_TEST(test_frozen_fixture_v1);
    RUN_TEST(test_policy_keeps_the_installation_not_the_identity);
    RUN_TEST(test_check_accepts_a_panel);
    RUN_TEST(test_check_accepts_zigbee_and_old_files);
    RUN_TEST(test_check_refuses_the_wrong_file);
    RUN_TEST(test_panel_through_a_file);
    RUN_TEST(test_restore_config_never_brings_approval_back);
    RUN_TEST(test_restore_brings_the_update_server_back_without_its_key);
    RUN_TEST(test_collect_reads_what_the_policy_keeps);
    RUN_TEST(test_collect_fails_on_what_it_cannot_read);
    RUN_TEST(test_erasable_lists_the_policy_keys_or_fails);
    return UNITY_END();
}
