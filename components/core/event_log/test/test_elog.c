/*
 * test_elog.c - the power-fail-safe event log ring (components/core/event_log) on a simulated NOR
 * flash that can lose power after any byte written or erased.
 * Requirements: LOG-001..006, LOG-007 (the viewer's filter paging), LOG-010
 * (docs/features/05-Event-Log)
 */
#include <stdio.h>
#include <string.h>

#include "event_log/elog.h"
#include "unity.h"

#define SECTOR  4096u
#define SECTORS 4u
#define SIZE    (SECTOR * SECTORS)
#define SLOTS   (SIZE / ELOG_RECORD_SIZE)  /* 256 */
#define PER_SEC (SECTOR / ELOG_RECORD_SIZE) /* 64 */

typedef struct {
    uint8_t mem[SIZE];
    long budget;  /* byte operations left before the power fails; < 0 = unlimited */
    bool dead;    /* power is off: every call fails */
    bool reject;  /* the next write fails without touching the flash */
    long tear;    /* the next write stops after this many bytes; the flash stays alive */
    unsigned erases[SECTORS];
} flash_t;

static flash_t F;
static elog_t L;
static elog_entry_t E[SLOTS * 2];

static bool spend(void)
{
    if (F.dead) {
        return false;
    }
    if (F.budget < 0) {
        return true;
    }
    if (F.budget == 0) {
        F.dead = true;
        return false;
    }
    F.budget--;
    return true;
}

static int f_read(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    (void)ctx;
    if (F.dead || off + len > SIZE) {
        return -2;
    }
    memcpy(buf, F.mem + off, len);
    return 0;
}

static int f_write(void *ctx, uint32_t off, const void *buf, uint32_t len)
{
    (void)ctx;
    if (F.reject) {
        F.reject = false;
        return -5;
    }
    const uint8_t *d = buf;
    for (uint32_t i = 0; i < len; i++) {
        if (F.tear > 0 && (long)i >= F.tear) {
            F.tear = 0;
            F.mem[off + i] &= (uint8_t)(d[i] | 0x5Au); /* torn record, but the flash lives on */
            return -3;
        }
        if (!spend()) {
            F.mem[off + i] &= (uint8_t)(d[i] | 0x5Au); /* some bits of this byte made it */
            return -3;
        }
        F.mem[off + i] &= d[i]; /* NOR flash: bits only go from 1 to 0 */
    }
    return 0;
}

static int f_erase(void *ctx, uint32_t off, uint32_t len)
{
    (void)ctx;
    for (uint32_t i = 0; i < len; i++) {
        if (!spend()) {
            F.mem[off + i] |= 0xA5u; /* half-erased byte */
            return -4;
        }
        F.mem[off + i] = 0xFF;
    }
    F.erases[off / SECTOR]++;
    return 0;
}

static const elog_flash_t FLASH = {NULL, SIZE, SECTOR, f_read, f_write, f_erase};

static void flash_blank(void)
{
    memset(&F, 0, sizeof(F));
    memset(F.mem, 0xFF, sizeof(F.mem));
    F.budget = -1;
}

static alarm_event_t event(uint32_t seq)
{
    alarm_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.seq = seq;
    ev.mono_ms = (alarm_ms_t)seq * 1500u;
    ev.epoch_ms = 1789857660000LL + (int64_t)seq * 1000;
    ev.type = (seq % 5u == 0u) ? ALARM_EV_ALARM : ALARM_EV_ARMED;
    ev.partition = 1;
    ev.zone = (uint16_t)(seq % 7u);
    ev.user = 2;
    ev.source.kind = ALARM_SRC_KEYPAD;
    ev.source.id = 3;
    ev.arg = (uint8_t)(seq & 0xFFu);
    ev.flags = ALARM_EVF_REPORT;
    return ev;
}

static void append_events(uint32_t first_seq, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        const alarm_event_t ev = event(first_seq + i);
        TEST_ASSERT_EQUAL_INT(0, elog_append(&L, ELOG_KIND_EVENT, &ev));
    }
}

/* Everything readable, newest first, paged in small steps. */
static size_t read_all(elog_t *log, elog_entry_t *out, size_t max)
{
    size_t n = 0;
    uint32_t before = UINT32_MAX;
    while (n < max) {
        uint32_t next = 0;
        n += elog_read(log, before, NULL, NULL, out + n, (max - n) < 7u ? (max - n) : 7u, 0, &next);
        if (next == 0u) {
            break;
        }
        before = next;
    }
    return n;
}

/* Everything readable, oldest first, paged forward in small steps. */
static size_t read_all_forward(elog_t *log, uint32_t after, elog_entry_t *out, size_t max)
{
    size_t n = 0;
    while (n < max) {
        uint32_t next = 0;
        const size_t got = elog_read_after(log, after, NULL, NULL, out + n,
                                           (max - n) < 5u ? (max - n) : 5u, 0, &next);
        n += got;
        if (got == 0u) {
            TEST_ASSERT_EQUAL_UINT32(after, next);
            break;
        }
        TEST_ASSERT_TRUE(next > after);
        after = next;
    }
    return n;
}

void setUp(void)
{
    flash_blank();
    TEST_ASSERT_EQUAL_INT(0, elog_open(&L, &FLASH, 7));
}

void tearDown(void) {}

/* LOG-004: records keep every event field, wall-clock time (or 0) and boot + uptime. */
static void test_record_codec(void)
{
    uint8_t rec[ELOG_RECORD_SIZE];
    alarm_event_t ev = event(0x12345678u);
    ev.zone = 999;
    ev.source.id = 0xFFFE;
    ev.arg2 = 0x42;
    ev.flags = ALARM_EVF_SILENT | ALARM_EVF_SYSTEM;
    elog_encode(rec, 77, ELOG_KIND_EVENT, 5, &ev);
    elog_entry_t e;
    TEST_ASSERT_TRUE(elog_decode(rec, &e));
    TEST_ASSERT_EQUAL_UINT32(77, e.index);
    TEST_ASSERT_EQUAL_UINT32(5, e.boot);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(ev.mono_ms / 1000u), e.uptime_s);
    TEST_ASSERT_EQUAL_UINT32(ev.seq, e.ev.seq);
    TEST_ASSERT_TRUE(ev.epoch_ms == e.ev.epoch_ms);
    TEST_ASSERT_EQUAL_INT(ev.type, e.ev.type);
    TEST_ASSERT_EQUAL_UINT16(999, e.ev.zone);
    TEST_ASSERT_EQUAL_UINT16(0xFFFE, e.ev.source.id);
    TEST_ASSERT_EQUAL_UINT8(ALARM_SRC_KEYPAD, e.ev.source.kind);
    TEST_ASSERT_EQUAL_UINT8(0x42, e.ev.arg2);
    TEST_ASSERT_EQUAL_UINT8(ALARM_EVF_SILENT | ALARM_EVF_SYSTEM, e.ev.flags);

    ev.epoch_ms = 0; /* clock not set: "time unknown" */
    elog_encode(rec, 78, ELOG_KIND_REPORTED, 5, &ev);
    TEST_ASSERT_TRUE(elog_decode(rec, &e));
    TEST_ASSERT_TRUE(e.ev.epoch_ms == 0);
    TEST_ASSERT_EQUAL_UINT8(ELOG_KIND_REPORTED, e.kind);

    /* LOG-003: any flipped bit is detected. */
    for (unsigned bit = 0; bit < ELOG_RECORD_SIZE * 8u; bit++) {
        rec[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
        TEST_ASSERT_FALSE(elog_decode(rec, &e));
        rec[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
    }
}

/* LOG-001: appended events come back newest first, filtered and paged. */
static void test_append_and_read(void)
{
    append_events(100, 20);
    const alarm_event_t marker = event(105);
    TEST_ASSERT_EQUAL_INT(0, elog_append(&L, ELOG_KIND_REPORTED, &marker));
    TEST_ASSERT_EQUAL_UINT32(21, L.count);

    const size_t n = read_all(&L, E, 64);
    TEST_ASSERT_EQUAL_UINT(21, n);
    TEST_ASSERT_EQUAL_UINT8(ELOG_KIND_REPORTED, E[0].kind);
    TEST_ASSERT_EQUAL_UINT32(105, E[0].ev.seq);
    for (size_t i = 1; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT32(E[i - 1].index - 1u, E[i].index);
        TEST_ASSERT_EQUAL_UINT32(120u - i, E[i].ev.seq);
        TEST_ASSERT_EQUAL_UINT32(7, E[i].boot);
    }

    /* Page of three, then the next page continues below it. */
    uint32_t next = 0;
    TEST_ASSERT_EQUAL_UINT(3, elog_read(&L, UINT32_MAX, NULL, NULL, E, 3, 0, &next));
    TEST_ASSERT_EQUAL_UINT32(E[2].index, next);
    TEST_ASSERT_EQUAL_UINT(3, elog_read(&L, next, NULL, NULL, E, 3, 0, &next));
    TEST_ASSERT_EQUAL_UINT32(117, E[0].ev.seq);
}

static bool only_alarms(void *ctx, const elog_entry_t *e)
{
    (void)ctx;
    return e->kind == ELOG_KIND_EVENT && e->ev.type == ALARM_EV_ALARM;
}

/* LOG-007 support: a sparse filter is paged with a scan limit and never stalls or skips. */
static void test_sparse_filter_with_scan_limit(void)
{
    append_events(1, 150);
    size_t n = 0;
    uint32_t before = UINT32_MAX;
    unsigned calls = 0;
    for (;;) {
        uint32_t next = 0;
        n += elog_read(&L, before, only_alarms, NULL, E + n, 50, 16, &next);
        calls++;
        TEST_ASSERT_TRUE(calls < 100);
        if (next == 0u) {
            break;
        }
        TEST_ASSERT_TRUE(next < before);
        before = next;
    }
    TEST_ASSERT_EQUAL_UINT(30, n); /* seq 5, 10, ... 150 */
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT32(150u - 5u * i, E[i].ev.seq);
    }
}

/* LOG-002: the ring overwrites the oldest records, keeps at least its capacity, and spreads
 * erases evenly over the sectors. */
static void test_wraps_and_levels_wear(void)
{
    TEST_ASSERT_EQUAL_UINT32(SLOTS - PER_SEC, elog_capacity(&L));
    append_events(1, SLOTS * 10u + 17u);
    TEST_ASSERT_TRUE(L.count >= elog_capacity(&L));
    TEST_ASSERT_TRUE(L.count < SLOTS);
    const size_t n = read_all(&L, E, SLOTS * 2u);
    TEST_ASSERT_EQUAL_UINT(L.count, n);
    TEST_ASSERT_EQUAL_UINT32(SLOTS * 10u + 17u, E[0].ev.seq);
    for (size_t i = 1; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT32(E[i - 1].ev.seq - 1u, E[i].ev.seq);
    }
    unsigned lo = F.erases[0];
    unsigned hi = F.erases[0];
    for (unsigned s = 1; s < SECTORS; s++) {
        lo = F.erases[s] < lo ? F.erases[s] : lo;
        hi = F.erases[s] > hi ? F.erases[s] : hi;
    }
    TEST_ASSERT_TRUE(hi - lo <= 1u);

    /* LOG-005: a reboot finds the same records and the newest sequence number. */
    const uint32_t count = L.count;
    const uint32_t next_index = L.next_index;
    TEST_ASSERT_EQUAL_INT(0, elog_open(&L, &FLASH, 8));
    TEST_ASSERT_EQUAL_UINT32(count, L.count);
    TEST_ASSERT_EQUAL_UINT32(next_index, L.next_index);
    TEST_ASSERT_EQUAL_UINT32(SLOTS * 10u + 17u, L.last_seq);
    append_events(SLOTS * 10u + 18u, 1);
    TEST_ASSERT_EQUAL_UINT(1, elog_read(&L, UINT32_MAX, NULL, NULL, E, 1, 0, NULL));
    TEST_ASSERT_EQUAL_UINT32(8, E[0].boot);
}

/* LOG-005: the last sequence number counts events only (report markers point back). */
static void test_last_seq_counts_events(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, L.last_seq);
    append_events(40, 3);
    const alarm_event_t odd = event(9000);
    TEST_ASSERT_EQUAL_INT(0, elog_append(&L, ELOG_KIND_REPORTED, &odd));
    TEST_ASSERT_EQUAL_UINT32(42, L.last_seq);
    TEST_ASSERT_EQUAL_INT(0, elog_open(&L, &FLASH, 8));
    TEST_ASSERT_EQUAL_UINT32(42, L.last_seq);
}

/* LOG-003: power fails after any byte written or erased while appending across a sector
 * boundary (which erases the oldest sector). After the reboot every record outside that
 * sector and every completed append is intact, order is kept, and logging continues. */
static void test_power_cut_at_every_byte(void)
{
    /* Baseline: wrapped once, head two slots before the end of a sector. */
    append_events(1, SLOTS + PER_SEC - 2u);
    TEST_ASSERT_EQUAL_UINT32(PER_SEC - 2u, L.head % PER_SEC);
    static uint8_t baseline[SIZE];
    memcpy(baseline, F.mem, SIZE);
    const elog_t base_log = L;
    const uint32_t doomed_sector = (L.head / PER_SEC + 1u) % SECTORS;

    /* Which sequence numbers exist before the cut, and which of them live in the sector that
     * the appends below erase ahead of the head. */
    enum { MAX_SEQ = SLOTS * 2 };
    static bool existed[MAX_SEQ];
    static bool doomed[MAX_SEQ];
    memset(existed, 0, sizeof(existed));
    memset(doomed, 0, sizeof(doomed));
    for (uint32_t s = 0; s < SLOTS; s++) {
        elog_entry_t e;
        if (elog_decode(baseline + s * ELOG_RECORD_SIZE, &e)) {
            TEST_ASSERT_TRUE(e.ev.seq < MAX_SEQ);
            existed[e.ev.seq] = true;
            doomed[e.ev.seq] = s / PER_SEC == doomed_sector;
        }
    }
    const uint32_t first_new = base_log.last_seq + 1u;
    const long total = 3 * ELOG_RECORD_SIZE + SECTOR + 16;

    for (long budget = 0; budget <= total; budget++) {
        memcpy(F.mem, baseline, SIZE);
        L = base_log;
        F.dead = false;
        F.budget = budget;
        uint32_t committed = 0;
        for (uint32_t k = 0; k < 3u && !F.dead; k++) {
            const alarm_event_t ev = event(first_new + k);
            if (elog_append(&L, ELOG_KIND_EVENT, &ev) == 0 && !F.dead) {
                committed = k + 1u;
            }
        }

        /* Power comes back. */
        F.dead = false;
        F.budget = -1;
        char msg[48];
        snprintf(msg, sizeof(msg), "power cut after %ld bytes", budget);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, elog_open(&L, &FLASH, 9), msg);
        static elog_entry_t after[SLOTS];
        static bool present[MAX_SEQ];
        memset(present, 0, sizeof(present));
        const size_t n = read_all(&L, after, SLOTS);
        for (size_t i = 0; i < n; i++) {
            TEST_ASSERT_TRUE_MESSAGE(after[i].ev.seq < MAX_SEQ, msg);
            present[after[i].ev.seq] = true;
            if (i > 0) {
                TEST_ASSERT_TRUE_MESSAGE(after[i].index < after[i - 1].index, msg);
                TEST_ASSERT_EQUAL_UINT32_MESSAGE(after[i - 1].ev.seq - (after[i - 1].index - after[i].index),
                                                 after[i].ev.seq, msg);
            }
        }
        /* Completed appends survived; older records too, unless they lived in the sector erased
         * ahead of the head. */
        for (uint32_t k = 0; k < committed; k++) {
            TEST_ASSERT_TRUE_MESSAGE(present[first_new + k], msg);
        }
        for (uint32_t seq = 0; seq < MAX_SEQ; seq++) {
            if (existed[seq] && !doomed[seq]) {
                TEST_ASSERT_TRUE_MESSAGE(present[seq], msg);
            }
        }
        /* LOG-006: reading forward finds the same records, oldest first. */
        static elog_entry_t fwd[SLOTS];
        TEST_ASSERT_EQUAL_UINT_MESSAGE(n, read_all_forward(&L, 0, fwd, SLOTS), msg);
        for (size_t i = 0; i < n; i++) {
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(after[n - 1u - i].index, fwd[i].index, msg);
        }
        /* Logging continues and the newest record is the new one. */
        const alarm_event_t ev = event(first_new + 10u);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, elog_append(&L, ELOG_KIND_EVENT, &ev), msg);
        TEST_ASSERT_EQUAL_UINT(1, elog_read(&L, UINT32_MAX, NULL, NULL, after, 1, 0, NULL));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(first_new + 10u, after[0].ev.seq, msg);
    }
}

static bool only_markers(void *ctx, const elog_entry_t *e)
{
    unsigned *looked = ctx;
    if (looked != NULL) {
        (*looked)++;
    }
    return e->kind == ELOG_KIND_REPORTED;
}

/* LOG-006: reading forward after an index returns the newer records oldest first, pages
 * without gaps, and finds report markers with a bounded scan. */
static void test_read_after_in_order(void)
{
    uint32_t next = 99;
    TEST_ASSERT_EQUAL_UINT32(0, elog_newest_index(&L));
    TEST_ASSERT_EQUAL_UINT(0, elog_read_after(&L, 0, NULL, NULL, E, 4, 0, &next));
    TEST_ASSERT_EQUAL_UINT32(0, next);

    for (uint32_t seq = 1; seq <= 60; seq++) {
        const alarm_event_t ev = event(seq);
        TEST_ASSERT_EQUAL_INT(0, elog_append(&L, ELOG_KIND_EVENT, &ev));
        if (seq % 10u == 0u) {
            TEST_ASSERT_EQUAL_INT(0, elog_append(&L, ELOG_KIND_REPORTED, &ev));
        }
    }
    TEST_ASSERT_EQUAL_UINT32(66, elog_newest_index(&L));

    TEST_ASSERT_EQUAL_UINT(66, read_all_forward(&L, 0, E, SLOTS));
    for (size_t i = 0; i < 66; i++) {
        TEST_ASSERT_EQUAL_UINT32(i + 1u, E[i].index);
    }
    TEST_ASSERT_EQUAL_UINT(3, elog_read_after(&L, 40, NULL, NULL, E, 3, 0, &next));
    TEST_ASSERT_EQUAL_UINT32(41, E[0].index);
    TEST_ASSERT_EQUAL_UINT32(43, next);
    TEST_ASSERT_EQUAL_UINT(0, elog_read_after(&L, 66, NULL, NULL, E, 3, 0, &next));
    TEST_ASSERT_EQUAL_UINT32(66, next);
    TEST_ASSERT_EQUAL_UINT(0, elog_read_after(&L, 500, NULL, NULL, E, 3, 0, &next));
    TEST_ASSERT_EQUAL_UINT32(500, next);

    /* Markers only, at most 4 records per call: every call makes progress. */
    uint32_t after = 0;
    size_t n = 0;
    unsigned calls = 0;
    for (;;) {
        unsigned looked = 0;
        const size_t got = elog_read_after(&L, after, only_markers, &looked, E + n, 10, 4, &next);
        TEST_ASSERT_TRUE(looked <= 4u);
        n += got;
        calls++;
        if (next == after) {
            break;
        }
        after = next;
    }
    TEST_ASSERT_EQUAL_UINT(6, n);
    TEST_ASSERT_TRUE(calls >= 66u / 4u);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT8(ELOG_KIND_REPORTED, E[i].kind);
        TEST_ASSERT_EQUAL_UINT32(10u * (i + 1u), E[i].ev.seq);
    }
}

/* LOG-006: after the ring overwrote the records following 'after', reading starts with the
 * oldest record kept; slots skipped after a power cut are stepped over. */
static void test_read_after_overwritten_and_torn(void)
{
    append_events(1, SLOTS * 3u + 5u);
    const uint32_t newest = elog_newest_index(&L);
    TEST_ASSERT_EQUAL_UINT(L.count, read_all_forward(&L, 1, E, SLOTS));
    TEST_ASSERT_EQUAL_UINT32(newest - L.count + 1u, E[0].index);
    TEST_ASSERT_EQUAL_UINT32(newest, E[L.count - 1u].index);

    /* A power cut in the middle of a write leaves a torn slot; after the reboot a later append
     * lands behind it. */
    F.budget = 20;
    const alarm_event_t cut = event(9000);
    TEST_ASSERT_NOT_EQUAL(0, elog_append(&L, ELOG_KIND_EVENT, &cut));
    F.dead = false;
    F.budget = -1;
    TEST_ASSERT_EQUAL_INT(0, elog_open(&L, &FLASH, 8));
    append_events(9001, 3);
    const uint32_t last = elog_newest_index(&L);
    uint32_t next = 0;
    TEST_ASSERT_EQUAL_UINT(3, elog_read_after(&L, newest, NULL, NULL, E, 8, 0, &next));
    TEST_ASSERT_EQUAL_UINT32(9001, E[0].ev.seq);
    TEST_ASSERT_EQUAL_UINT32(9003, E[2].ev.seq);
    TEST_ASSERT_EQUAL_UINT32(last, next);
    TEST_ASSERT_EQUAL_UINT(2, elog_read_after(&L, E[0].index, NULL, NULL, E, 8, 0, &next));
    TEST_ASSERT_EQUAL_UINT32(9002, E[0].ev.seq);
}

/* A write that fails before touching the flash keeps its slot and index. */
static void test_rejected_write_keeps_slot(void)
{
    append_events(1, 3);
    const uint32_t head = L.head;
    const uint32_t index = L.next_index;
    F.reject = true;
    const alarm_event_t ev = event(4);
    TEST_ASSERT_NOT_EQUAL(0, elog_append(&L, ELOG_KIND_EVENT, &ev));
    TEST_ASSERT_EQUAL_UINT32(head, L.head);
    TEST_ASSERT_EQUAL_UINT32(index, L.next_index);
    append_events(4, 1);
    TEST_ASSERT_EQUAL_UINT(4, read_all(&L, E, 16));
}

/* LOG-010: a failed append can simply be tried again, whether the write was refused outright
 * or left a torn record behind. The panel's writer task relies on this (panel_log.c
 * write_item): it only counts a record lost once its attempts run out, so a burst or a busy
 * flash never quietly drops a report marker. */
static void test_failed_append_is_retried(void)
{
    append_events(1, 3);
    const uint32_t head = L.head;
    const uint32_t index = L.next_index;

    /* Refused before it touched the flash: the slot and index stay free, and the retry uses
     * them, so nothing is duplicated. */
    F.reject = true;
    const alarm_event_t ev4 = event(4);
    TEST_ASSERT_NOT_EQUAL(0, elog_append(&L, ELOG_KIND_EVENT, &ev4));
    TEST_ASSERT_EQUAL_UINT32(head, L.head);
    TEST_ASSERT_EQUAL_UINT32(index, L.next_index);
    TEST_ASSERT_EQUAL_INT(0, elog_append(&L, ELOG_KIND_EVENT, &ev4));
    TEST_ASSERT_EQUAL_UINT32(index + 1u, L.next_index);

    /* Torn in the middle: that slot is spent, and the retry of the same report marker lands in
     * the next one. */
    const alarm_event_t ev5 = event(5);
    F.tear = 20;
    TEST_ASSERT_NOT_EQUAL(0, elog_append(&L, ELOG_KIND_REPORTED, &ev5));
    TEST_ASSERT_EQUAL_INT(0, elog_append(&L, ELOG_KIND_REPORTED, &ev5));

    /* Four events and one marker are readable; the torn slot is skipped, not counted. */
    TEST_ASSERT_EQUAL_UINT32(5, L.count);
    TEST_ASSERT_EQUAL_UINT(5, read_all(&L, E, 16));
    TEST_ASSERT_EQUAL_UINT32(4, L.last_seq);

    /* LOG-006: the marker is where MQTT replay looks for it. */
    uint32_t next = 0;
    TEST_ASSERT_EQUAL_UINT(1, elog_read_after(&L, 0, only_markers, NULL, E, 4, 0, &next));
    TEST_ASSERT_EQUAL_UINT8(ELOG_KIND_REPORTED, E[0].kind);
    TEST_ASSERT_EQUAL_UINT32(5, E[0].ev.seq);

    /* And it survives the restart that follows a power cut. */
    TEST_ASSERT_EQUAL_INT(0, elog_open(&L, &FLASH, 9));
    TEST_ASSERT_EQUAL_UINT32(5, L.count);
    TEST_ASSERT_EQUAL_UINT(1, elog_read_after(&L, 0, only_markers, NULL, E, 4, 0, &next));
    TEST_ASSERT_EQUAL_UINT32(5, E[0].ev.seq);
}

/* A partition full of garbage (never erased) mounts and only shows new records. */
static void test_garbage_partition(void)
{
    uint32_t x = 0x12345678u;
    for (unsigned i = 0; i < SIZE; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        F.mem[i] = (uint8_t)x;
    }
    TEST_ASSERT_EQUAL_INT(0, elog_open(&L, &FLASH, 1));
    TEST_ASSERT_EQUAL_UINT32(0, L.count);
    append_events(1, PER_SEC + 5u); /* crosses into a garbage sector */
    TEST_ASSERT_EQUAL_UINT(PER_SEC + 5u, read_all(&L, E, SLOTS));
    TEST_ASSERT_EQUAL_UINT32(PER_SEC + 5u, E[0].ev.seq);
}

/* Factory reset clears the log; bad geometry is refused. */
static void test_clear_and_geometry(void)
{
    append_events(1, 10);
    TEST_ASSERT_EQUAL_INT(0, elog_clear(&L));
    TEST_ASSERT_EQUAL_UINT32(0, L.count);
    TEST_ASSERT_EQUAL_UINT(0, read_all(&L, E, 16));
    TEST_ASSERT_EQUAL_INT(0, elog_open(&L, &FLASH, 2));
    TEST_ASSERT_EQUAL_UINT32(0, L.count);
    TEST_ASSERT_EQUAL_UINT32(1, L.next_index);
    append_events(11, 1);
    TEST_ASSERT_EQUAL_UINT32(1, L.count);

    elog_flash_t bad = FLASH;
    bad.sector_size = 100;
    TEST_ASSERT_EQUAL_INT(-1, elog_open(&L, &bad, 1));
    bad = FLASH;
    bad.size = SECTOR;
    TEST_ASSERT_EQUAL_INT(-1, elog_open(&L, &bad, 1));
    TEST_ASSERT_EQUAL_INT(-1, elog_append(&L, ELOG_KIND_EVENT, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_record_codec);
    RUN_TEST(test_append_and_read);
    RUN_TEST(test_sparse_filter_with_scan_limit);
    RUN_TEST(test_wraps_and_levels_wear);
    RUN_TEST(test_last_seq_counts_events);
    RUN_TEST(test_read_after_in_order);
    RUN_TEST(test_read_after_overwritten_and_torn);
    RUN_TEST(test_power_cut_at_every_byte);
    RUN_TEST(test_rejected_write_keeps_slot);
    RUN_TEST(test_failed_append_is_retried);
    RUN_TEST(test_garbage_partition);
    RUN_TEST(test_clear_and_geometry);
    return UNITY_END();
}
