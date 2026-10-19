/*
 * test_plink_home.c - Smart Home on keypads, the pieces both sides share (plink_home.h;
 * docs/features/11 "The device list", "Device state"): the device list blob built from the
 * registry's rows - the rows whose ref the wire can't carry left out of the blob and the records
 * alike, at most 64, every string at its longest within PLINK_DEV_BLOB_MAX - its list_crc (never
 * 0), the keypad's lenient reading of a blob (a newer master's appended fields and bytes skipped,
 * a row it can't use keeping its index, a newer compat refused), the records with the registry's
 * facts in their flags, states_crc over index and section in index order, and a row's record
 * carried over by ref at a new list's install.
 * Requirements: LINK-044, LINK-045, LINK-047 (docs/features/11)
 */
#include <string.h>

#include "alarm/alarm_codec.h"
#include "panel_link/plink_home.h"
#include "unity.h"

static hc_device_t ROWS[80];
static uint8_t BLOB[PLINK_DEV_BLOB_MAX];
static plink_dev_recs_t RECS, OLD, OUT;

void setUp(void)
{
    memset(ROWS, 0, sizeof(ROWS));
    memset(BLOB, 0, sizeof(BLOB));
    memset(&RECS, 0, sizeof(RECS));
    memset(&OLD, 0, sizeof(OLD));
    memset(&OUT, 0, sizeof(OUT));
}

void tearDown(void) {}

static hc_device_t *row(size_t i, uint8_t kind, uint8_t source, const char *ref, const char *name,
                        const char *area, const char *state)
{
    hc_device_t *d = &ROWS[i];
    memset(d, 0, sizeof(*d));
    d->info.kind = kind;
    d->info.source = source;
    strncpy(d->info.ref, ref, sizeof(d->info.ref) - 1u);
    strncpy(d->info.name, name, sizeof(d->info.name) - 1u);
    strncpy(d->info.area, area, sizeof(d->info.area) - 1u);
    strncpy(d->info.state, state, sizeof(d->info.state) - 1u);
    d->info.online = true;
    return d;
}

/* Three rows: a light, a lock, a Z-Wave switch. */
static void three_rows(void)
{
    row(0, DEV_LIGHT, DSRC_HA, "light.hall", "Hall light", "Hall", "on");
    row(1, DEV_LOCK, DSRC_HA, "lock.front", "Front door", "Hall", "locked");
    row(2, DEV_SWITCH, DSRC_ZWAVE, "zw_1_5_0_0", "Porch", "", "off");
}

static size_t build(size_t n, uint32_t *crc)
{
    size_t len = 0u;
    TEST_ASSERT_TRUE(plink_home_list_build(ROWS, n, BLOB, sizeof(BLOB), &len, crc));
    plink_home_recs_build(ROWS, n, &RECS);
    return len;
}

/* LINK-044: the blob is compat 1, the count, then each row's catalog record in the registry's
 * order (Z-Wave rows too); list_crc is its CRC-32; a keypad reads every field back and finds a
 * row by its ref, byte for byte. */
static void test_list_build_and_read(void)
{
    three_rows();
    uint32_t crc = 0u;
    const size_t len = build(3u, &crc);
    TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_CAT_COMPAT, BLOB[0]);
    TEST_ASSERT_EQUAL_UINT8(3u, BLOB[1]);
    TEST_ASSERT_EQUAL_HEX32(alarm_crc32(0u, BLOB, len), crc);
    TEST_ASSERT_EQUAL_HEX32(crc, plink_home_list_crc(BLOB, len));
    uint8_t count = 0u;
    TEST_ASSERT_TRUE(plink_home_list_check(BLOB, len, &count));
    TEST_ASSERT_EQUAL_UINT8(3u, count);
    plink_home_walk_t w = {0u, 0u};
    plink_dev_cat_t cat;
    uint8_t index = 0xFFu;
    bool usable = false;
    for (uint8_t i = 0; i < 3u; i++) {
        TEST_ASSERT_TRUE(plink_home_list_next(BLOB, len, &w, &index, &cat, &usable));
        TEST_ASSERT_EQUAL_UINT8(i, index);
        TEST_ASSERT_TRUE(usable);
        TEST_ASSERT_EQUAL_UINT8(ROWS[i].info.kind, cat.kind);
        TEST_ASSERT_EQUAL_UINT8(ROWS[i].info.source, cat.source);
        TEST_ASSERT_EQUAL_STRING(ROWS[i].info.ref, cat.ref);
        TEST_ASSERT_EQUAL_STRING(ROWS[i].info.name, cat.name);
        TEST_ASSERT_EQUAL_STRING(ROWS[i].info.area, cat.area);
        uint8_t found = 0xFFu;
        TEST_ASSERT_TRUE(plink_home_list_find(BLOB, len, ROWS[i].info.ref, &found));
        TEST_ASSERT_EQUAL_UINT8(i, found);
    }
    TEST_ASSERT_FALSE(plink_home_list_next(BLOB, len, &w, &index, &cat, &usable));
    TEST_ASSERT_FALSE(plink_home_list_find(BLOB, len, "light.hal", NULL));
    TEST_ASSERT_FALSE(plink_home_list_find(BLOB, len, "light.hall2", NULL));
    TEST_ASSERT_FALSE(plink_home_list_find(BLOB, len, "", NULL));
    /* Records index for index with the rows. */
    TEST_ASSERT_EQUAL_UINT8(3u, RECS.count);
    plink_dev_rec_t r;
    TEST_ASSERT_TRUE(plink_home_rec(&RECS, 1u, &r));
    TEST_ASSERT_EQUAL_STRING("locked", r.word);
    TEST_ASSERT_FALSE(plink_home_rec(&RECS, 3u, &r));
    /* An empty registry: an empty list, still compat 1. */
    size_t empty = 0u;
    TEST_ASSERT_TRUE(plink_home_list_build(ROWS, 0u, BLOB, sizeof(BLOB), &empty, &crc));
    TEST_ASSERT_EQUAL_size_t(PLINK_DEV_BLOB_HDR, empty);
    TEST_ASSERT_TRUE(plink_home_list_check(BLOB, empty, &count));
    TEST_ASSERT_EQUAL_UINT8(0u, count);
    TEST_ASSERT_FALSE(plink_home_list_build(ROWS, 0u, BLOB, 1u, &empty, &crc));
}

/* LINK-044: list_crc is never 0 (0 is "no list"): a CRC of 0 reads as 1. */
static void test_list_crc_never_zero(void)
{
    TEST_ASSERT_EQUAL_HEX32(0u, alarm_crc32(0u, BLOB, 0u));
    TEST_ASSERT_EQUAL_HEX32(1u, plink_home_list_crc(BLOB, 0u));
    TEST_ASSERT_EQUAL_HEX32(1u, plink_home_list_crc(NULL, 0u));
    three_rows();
    uint32_t crc = 0u;
    const size_t len = build(3u, &crc);
    TEST_ASSERT_NOT_EQUAL(0u, crc);
    TEST_ASSERT_NOT_EQUAL(0u, plink_home_ref_key(""));
    /* A change anywhere is another list: a rename. */
    strcpy(ROWS[0].info.name, "Hall lights");
    uint32_t crc2 = 0u;
    (void)build(3u, &crc2);
    TEST_ASSERT_NOT_EQUAL(crc, crc2);
    (void)len;
}

/* LINK-047: a ref is never cut or cleaned (it could name another device): a row whose ref the
 * wire can't carry - control characters, invalid UTF-8, none - is left out of the blob and the
 * records alike, so every index still names the same device in both. */
static void test_rows_the_wire_cant_carry_are_left_out(void)
{
    row(0, DEV_LIGHT, DSRC_HA, "light.a", "A", "", "on");
    row(1, DEV_LIGHT, DSRC_HA, "light.\x01x", "Bad", "", "on");
    row(2, DEV_LIGHT, DSRC_HA, "", "None", "", "on");
    row(3, DEV_LIGHT, DSRC_HA, "light.\xff", "Bad UTF-8", "", "on");
    row(4, DEV_LOCK, DSRC_HA, "lock.b", "B", "", "unlocked");
    TEST_ASSERT_TRUE(plink_home_listed(&ROWS[0]));
    TEST_ASSERT_FALSE(plink_home_listed(&ROWS[1]));
    TEST_ASSERT_FALSE(plink_home_listed(&ROWS[2]));
    TEST_ASSERT_FALSE(plink_home_listed(&ROWS[3]));
    TEST_ASSERT_FALSE(plink_home_listed(NULL));
    uint32_t crc = 0u;
    const size_t len = build(5u, &crc);
    uint8_t count = 0u;
    TEST_ASSERT_TRUE(plink_home_list_check(BLOB, len, &count));
    TEST_ASSERT_EQUAL_UINT8(2u, count);
    TEST_ASSERT_EQUAL_UINT8(2u, RECS.count);
    uint8_t i = 0u;
    TEST_ASSERT_TRUE(plink_home_list_find(BLOB, len, "lock.b", &i));
    TEST_ASSERT_EQUAL_UINT8(1u, i);
    plink_dev_rec_t r;
    TEST_ASSERT_TRUE(plink_home_rec(&RECS, i, &r));
    TEST_ASSERT_EQUAL_STRING("unlocked", r.word);
}

/* LINK-044: at most 64 rows (PLINK_DEV_MAX, the v1 limit); 64 rows with every string at its
 * longest are 12,482 B, within PLINK_DEV_BLOB_MAX; a buffer too small is refused. */
static void test_at_most_64_rows_within_the_blob(void)
{
    char ref[PLINK_REF_MAX + 1], name[PLINK_NAME_MAX + 1], area[PLINK_AREA_MAX + 1];
    for (size_t i = 0; i < 70u; i++) {
        memset(ref, 'r', PLINK_REF_MAX);
        ref[PLINK_REF_MAX] = '\0';
        ref[0] = (char)('A' + i / 26u);
        ref[1] = (char)('a' + i % 26u);
        memset(name, 'n', PLINK_NAME_MAX);
        name[PLINK_NAME_MAX] = '\0';
        memset(area, 'a', PLINK_AREA_MAX);
        area[PLINK_AREA_MAX] = '\0';
        row(i, DEV_SWITCH, DSRC_HA, ref, name, area, "on");
    }
    uint32_t crc = 0u;
    const size_t len = build(70u, &crc);
    TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_MAX, BLOB[1]);
    TEST_ASSERT_EQUAL_size_t(2u + 64u * 195u, len);
    TEST_ASSERT_TRUE(len <= PLINK_DEV_BLOB_MAX);
    TEST_ASSERT_EQUAL_UINT8(PLINK_DEV_MAX, RECS.count);
    uint8_t count = 0u;
    TEST_ASSERT_TRUE(plink_home_list_check(BLOB, len, &count));
    TEST_ASSERT_EQUAL_UINT8(64u, count);
    size_t small = 0u;
    TEST_ASSERT_FALSE(plink_home_list_build(ROWS, 70u, BLOB, len - 1u, &small, NULL));
    TEST_ASSERT_EQUAL_size_t(0u, small);
}

/* A catalog record section by hand: kind, source, then the three strings as given, then extra
 * bytes a newer master appended (n_extra of them). */
static size_t raw_cat(uint8_t *out, uint8_t kind, const char *ref, size_t ref_n, const char *name,
                      size_t n_extra)
{
    size_t n = 1u;
    out[n++] = kind;
    out[n++] = DSRC_HA;
    out[n++] = (uint8_t)ref_n;
    memcpy(&out[n], ref, ref_n);
    n += ref_n;
    out[n++] = (uint8_t)strlen(name);
    memcpy(&out[n], name, strlen(name));
    n += strlen(name);
    out[n++] = 0u; /* area */
    for (size_t i = 0; i < n_extra; i++) {
        out[n++] = (uint8_t)(0xA0u + i);
    }
    out[0] = (uint8_t)(n - 1u);
    return n;
}

/* LINK-044 (docs/features/11 "The device list", "Versions"): a keypad reads a blob leniently. A
 * field a newer master appended inside a record is skipped, a kind it doesn't know kept as it
 * came, bytes after the last record ignored; a row whose ref it can't use keeps its index and is
 * never found by ref. A newer compat, a count over 64, a record cut short, fewer records than the
 * count: refused. */
static void test_a_newer_masters_blob_is_read_leniently(void)
{
    size_t n = PLINK_DEV_BLOB_HDR;
    n += raw_cat(&BLOB[n], DEV_LIGHT, "light.a", 7u, "A", 3u);
    n += raw_cat(&BLOB[n], 0x7Eu, "fan.new", 7u, "New kind", 0u);
    n += raw_cat(&BLOB[n], DEV_LIGHT, "light.\x02", 7u, "Bad ref", 0u);
    n += raw_cat(&BLOB[n], DEV_LOCK, "lock.b", 6u, "B", 1u);
    BLOB[0] = PLINK_DEV_CAT_COMPAT;
    BLOB[1] = 4u;
    BLOB[n++] = 0x55u; /* after the last record */
    BLOB[n++] = 0x66u;
    uint8_t count = 0u;
    TEST_ASSERT_TRUE(plink_home_list_check(BLOB, n, &count));
    TEST_ASSERT_EQUAL_UINT8(4u, count);
    plink_home_walk_t w = {0u, 0u};
    plink_dev_cat_t cat;
    uint8_t index = 0u;
    bool usable = false;
    TEST_ASSERT_TRUE(plink_home_list_next(BLOB, n, &w, &index, &cat, &usable));
    TEST_ASSERT_TRUE(usable);
    TEST_ASSERT_EQUAL_STRING("light.a", cat.ref);
    TEST_ASSERT_EQUAL_STRING("A", cat.name);
    TEST_ASSERT_TRUE(plink_home_list_next(BLOB, n, &w, &index, &cat, &usable));
    TEST_ASSERT_TRUE(usable);
    TEST_ASSERT_EQUAL_UINT8(0x7Eu, cat.kind);
    TEST_ASSERT_TRUE(plink_home_list_next(BLOB, n, &w, &index, &cat, &usable));
    TEST_ASSERT_EQUAL_UINT8(2u, index);
    TEST_ASSERT_FALSE(usable);
    TEST_ASSERT_EQUAL_STRING("", cat.ref);
    TEST_ASSERT_TRUE(plink_home_list_next(BLOB, n, &w, &index, &cat, &usable));
    TEST_ASSERT_EQUAL_UINT8(3u, index);
    TEST_ASSERT_TRUE(usable);
    TEST_ASSERT_EQUAL_STRING("lock.b", cat.ref);
    TEST_ASSERT_FALSE(plink_home_list_next(BLOB, n, &w, &index, &cat, &usable));
    TEST_ASSERT_FALSE(plink_home_list_find(BLOB, n, "light.\x02", NULL));
    TEST_ASSERT_TRUE(plink_home_list_find(BLOB, n, "lock.b", &index));
    TEST_ASSERT_EQUAL_UINT8(3u, index);
    uint32_t keys[PLINK_DEV_MAX];
    plink_home_list_keys(BLOB, n, keys);
    TEST_ASSERT_EQUAL_HEX32(plink_home_ref_key("light.a"), keys[0]);
    TEST_ASSERT_EQUAL_HEX32(0u, keys[2]);
    TEST_ASSERT_EQUAL_HEX32(plink_home_ref_key("lock.b"), keys[3]);
    TEST_ASSERT_EQUAL_HEX32(0u, keys[4]);

    /* Refused: a newer compat (never guessed at), compat 0, more rows than v1 carries, a record
     * cut short, fewer records than the count, no header. */
    BLOB[0] = PLINK_DEV_CAT_COMPAT + 1u;
    TEST_ASSERT_FALSE(plink_home_list_check(BLOB, n, NULL));
    w.pos = 0u;
    w.index = 0u;
    TEST_ASSERT_FALSE(plink_home_list_next(BLOB, n, &w, NULL, NULL, NULL));
    BLOB[0] = 0u;
    TEST_ASSERT_FALSE(plink_home_list_check(BLOB, n, NULL));
    BLOB[0] = PLINK_DEV_CAT_COMPAT;
    BLOB[1] = PLINK_DEV_MAX + 1u;
    TEST_ASSERT_FALSE(plink_home_list_check(BLOB, n, NULL));
    BLOB[1] = 4u;
    TEST_ASSERT_FALSE(plink_home_list_check(BLOB, n - 4u, NULL)); /* the last record cut */
    BLOB[1] = 5u;
    TEST_ASSERT_FALSE(plink_home_list_check(BLOB, n - 2u, NULL)); /* a fifth isn't there */
    TEST_ASSERT_FALSE(plink_home_list_check(BLOB, 1u, NULL));
    TEST_ASSERT_FALSE(plink_home_list_check(NULL, 0u, NULL));
}

/* LINK-045: a record holds the registry's facts as they are: online, pending, failed and unsure
 * (hc_outcome_unknown: only a failure whose outcome isn't known), the action, every value and the
 * state word (the registry's longest fits whole). */
static void test_records_hold_the_registrys_facts(void)
{
    hc_device_t *d = row(0, DEV_CLIMATE, DSRC_HA, "climate.hall", "Hall", "", "heat");
    d->info.values.flags = DEV_VAL_SETPOINT | DEV_VAL_TEMP;
    d->info.values.level = 40u;
    d->info.values.modes = DEV_MODE_OFF | DEV_MODE_HEAT;
    d->info.values.hvac_action = DEV_HVAC_HEATING;
    d->info.values.setpoint = 2150;
    d->info.values.temp = -1234;
    d->info.values.since = 1790000000u;
    d->info.values.caps = DEV_CAP_KNOWN;
    plink_dev_rec_t r;
    plink_home_rec_of(d, &r);
    TEST_ASSERT_EQUAL_HEX8(PLINK_DEVF_ONLINE, r.flags);
    TEST_ASSERT_EQUAL_UINT8(ACT_NONE, r.action);
    TEST_ASSERT_EQUAL_HEX8(DEV_VAL_SETPOINT | DEV_VAL_TEMP, r.vflags);
    TEST_ASSERT_EQUAL_UINT8(40u, r.level);
    TEST_ASSERT_EQUAL_HEX8(DEV_MODE_OFF | DEV_MODE_HEAT, r.modes);
    TEST_ASSERT_EQUAL_UINT8(DEV_HVAC_HEATING, r.hvac_action);
    TEST_ASSERT_EQUAL_INT32(2150, r.setpoint);
    TEST_ASSERT_EQUAL_INT32(-1234, r.temp);
    TEST_ASSERT_EQUAL_UINT32(1790000000u, r.since);
    TEST_ASSERT_EQUAL_HEX8(DEV_CAP_KNOWN, r.caps);
    TEST_ASSERT_EQUAL_STRING("heat", r.word);

    d->pending = 77u;
    d->action = ACT_SET_TEMP;
    plink_home_rec_of(d, &r);
    TEST_ASSERT_EQUAL_HEX8(PLINK_DEVF_ONLINE | PLINK_DEVF_PENDING, r.flags);
    TEST_ASSERT_EQUAL_UINT8(ACT_SET_TEMP, r.action);
    d->pending = 0u;
    d->failed = true;
    d->unsure = false; /* a refusal: it wasn't done */
    d->info.online = false;
    plink_home_rec_of(d, &r);
    TEST_ASSERT_EQUAL_HEX8(PLINK_DEVF_FAILED, r.flags);
    d->unsure = true; /* no answer in time: it may have happened */
    plink_home_rec_of(d, &r);
    TEST_ASSERT_EQUAL_HEX8(PLINK_DEVF_FAILED | PLINK_DEVF_UNSURE, r.flags);

    /* The registry's longest word, 23 bytes ending in a 3-byte character, kept whole. */
    strcpy(d->info.state, "aaaaaaaaaaaaaaaaaaaa\xe2\x82\xac");
    plink_home_rec_of(d, &r);
    TEST_ASSERT_EQUAL_STRING("aaaaaaaaaaaaaaaaaaaa\xe2\x82\xac", r.word);

    /* Through the records' bytes and back. */
    plink_home_recs_build(ROWS, 1u, &RECS);
    plink_dev_rec_t back;
    TEST_ASSERT_TRUE(plink_home_rec(&RECS, 0u, &back));
    TEST_ASSERT_EQUAL_MEMORY(&r, &back, sizeof(r));
    TEST_ASSERT_TRUE(RECS.len[0] <= PLINK_DEV_REC_MAX);
    TEST_ASSERT_EQUAL_UINT8(RECS.len[0] - 1u, RECS.rec[0][0]);
}

/* LINK-045 (BL-216): a record gives a keypad's screen back the registry's values it was made
 * from, through its bytes, so both screens read the same; nothing known without one. */
static void test_records_give_back_the_values(void)
{
    hc_device_t *d = row(0, DEV_CLIMATE, DSRC_HA, "climate.hall", "Hall", "", "heat_cool");
    d->info.values.flags = DEV_VAL_SETPOINT | DEV_VAL_TEMP | DEV_VAL_LEVEL;
    d->info.values.level = 40u;
    d->info.values.modes = DEV_MODE_OFF | DEV_MODE_HEAT_COOL;
    d->info.values.hvac_action = DEV_HVAC_COOLING;
    d->info.values.setpoint = 2150;
    d->info.values.temp = -1234;
    d->info.values.since = 1790000000u;
    d->info.values.caps = DEV_CAP_KNOWN;
    plink_home_recs_build(ROWS, 1u, &RECS);
    plink_dev_rec_t r;
    TEST_ASSERT_TRUE(plink_home_rec(&RECS, 0u, &r));
    dev_values_t v;
    memset(&v, 0xA5, sizeof(v));
    plink_home_values_of(&r, &v);
    TEST_ASSERT_EQUAL_MEMORY(&d->info.values, &v, sizeof(v));

    memset(&v, 0xA5, sizeof(v));
    plink_home_values_of(NULL, &v);
    TEST_ASSERT_EQUAL_HEX8(0u, v.flags);
    TEST_ASSERT_EQUAL_HEX8(0u, v.modes);
    TEST_ASSERT_EQUAL_UINT8(DEV_HVAC_UNKNOWN, v.hvac_action);
    TEST_ASSERT_EQUAL_UINT32(0u, v.since);
    TEST_ASSERT_EQUAL_HEX8(0u, v.caps);
}

/* LINK-045: states_crc is the CRC-32 over each record that has bytes, its index then its section,
 * in index order - the bytes DEV_STATE carries - and changes with any record; a record a keypad
 * receives is kept only whole, for an index of the list, and within PLINK_DEV_REC_MAX. */
static void test_states_crc_and_records_received(void)
{
    three_rows();
    (void)build(3u, NULL);
    uint8_t all[3u * (1u + PLINK_DEV_REC_MAX)];
    size_t n = 0u;
    for (uint8_t i = 0; i < 3u; i++) {
        all[n++] = i;
        memcpy(&all[n], RECS.rec[i], RECS.len[i]);
        n += RECS.len[i];
    }
    const uint32_t crc = plink_home_states_crc(&RECS);
    TEST_ASSERT_EQUAL_HEX32(alarm_crc32(0u, all, n), crc);
    plink_dev_recs_t none;
    memset(&none, 0, sizeof(none));
    TEST_ASSERT_EQUAL_HEX32(0u, plink_home_states_crc(&none));

    /* A keypad's copy, filled record by record: the same CRC once whole. */
    memset(&OUT, 0, sizeof(OUT));
    OUT.count = 3u;
    for (uint8_t i = 3u; i-- > 0u;) {
        TEST_ASSERT_TRUE(plink_home_rec_put(&OUT, i, RECS.rec[i], RECS.len[i]));
        if (i > 0u) {
            TEST_ASSERT_NOT_EQUAL(crc, plink_home_states_crc(&OUT));
        }
    }
    TEST_ASSERT_EQUAL_HEX32(crc, plink_home_states_crc(&OUT));
    /* One record changes: another CRC. */
    strcpy(ROWS[0].info.state, "off");
    (void)build(3u, NULL);
    TEST_ASSERT_NOT_EQUAL(crc, plink_home_states_crc(&RECS));

    uint8_t sec[PLINK_DEV_REC_MAX + 2u];
    memset(sec, 0, sizeof(sec));
    sec[0] = 3u;
    TEST_ASSERT_FALSE(plink_home_rec_put(&OUT, 3u, sec, 4u));  /* past the list */
    TEST_ASSERT_FALSE(plink_home_rec_put(&OUT, 0u, sec, 3u));  /* not whole */
    TEST_ASSERT_FALSE(plink_home_rec_put(&OUT, 0u, sec, 5u));  /* more than its section */
    TEST_ASSERT_FALSE(plink_home_rec_put(&OUT, 0u, sec, 0u));
    sec[0] = PLINK_DEV_REC_MAX;
    TEST_ASSERT_FALSE(plink_home_rec_put(&OUT, 0u, sec, PLINK_DEV_REC_MAX + 1u)); /* too long */
    sec[0] = 3u;
    TEST_ASSERT_TRUE(plink_home_rec_put(&OUT, 0u, sec, 4u));
    TEST_ASSERT_EQUAL_UINT8(4u, OUT.len[0]);
}

/* LINK-044: at a new list's install each row keeps the record its ref had under the old list, so
 * a rename or a move never blinks; a new row has none until its own comes. */
static void test_records_carried_over_by_ref(void)
{
    three_rows();
    size_t len = build(3u, NULL);
    uint32_t old_keys[PLINK_DEV_MAX];
    plink_home_list_keys(BLOB, len, old_keys);
    OLD = RECS;
    /* The new list: the lock renamed and first, a new device, the light last; the switch gone. */
    hc_device_t lock = ROWS[1], light = ROWS[0];
    strcpy(lock.info.name, "Front door lock");
    memset(ROWS, 0, sizeof(ROWS));
    ROWS[0] = lock;
    row(1, DEV_FAN, DSRC_HA, "fan.attic", "Attic fan", "Attic", "off");
    ROWS[2] = light;
    len = build(3u, NULL);
    uint32_t new_keys[PLINK_DEV_MAX];
    plink_home_list_keys(BLOB, len, new_keys);
    plink_home_carry(old_keys, &OLD, new_keys, 3u, &OUT);
    TEST_ASSERT_EQUAL_UINT8(3u, OUT.count);
    TEST_ASSERT_EQUAL_UINT8(OLD.len[1], OUT.len[0]);
    TEST_ASSERT_EQUAL_MEMORY(OLD.rec[1], OUT.rec[0], OLD.len[1]);
    TEST_ASSERT_EQUAL_UINT8(0u, OUT.len[1]);
    TEST_ASSERT_EQUAL_UINT8(OLD.len[0], OUT.len[2]);
    TEST_ASSERT_EQUAL_MEMORY(OLD.rec[0], OUT.rec[2], OLD.len[0]);
    plink_dev_rec_t r;
    TEST_ASSERT_FALSE(plink_home_rec(&OUT, 1u, &r));
    TEST_ASSERT_TRUE(plink_home_rec(&OUT, 0u, &r));
    TEST_ASSERT_EQUAL_STRING("locked", r.word);
    /* Never into the records it reads from. */
    OUT.count = 9u;
    plink_home_carry(old_keys, &OUT, new_keys, 3u, &OUT);
    TEST_ASSERT_EQUAL_UINT8(9u, OUT.count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_list_build_and_read);
    RUN_TEST(test_list_crc_never_zero);
    RUN_TEST(test_rows_the_wire_cant_carry_are_left_out);
    RUN_TEST(test_at_most_64_rows_within_the_blob);
    RUN_TEST(test_a_newer_masters_blob_is_read_leniently);
    RUN_TEST(test_records_hold_the_registrys_facts);
    RUN_TEST(test_records_give_back_the_values);
    RUN_TEST(test_states_crc_and_records_received);
    RUN_TEST(test_records_carried_over_by_ref);
    return UNITY_END();
}
