/*
 * zw_trace.c - the frame trace (development builds; docs/features/30 "Z-Wave"): what the zw_link
 * task sees and sends, for the developer link's GET /dev/v1/zwave/trace.
 *
 * A ring in PSRAM, 32 KB: SLOTS entries (a sequence number, the time, the direction, where its
 * bytes are) and DATA bytes of what they carried; the oldest go first when either runs out. One
 * writer, the zw_link task (zw_trace_put). Readers (the HTTPS worker) copy one entry at a time
 * under the same mutex and format it after unlocking, so the link never waits longer than one
 * small copy - and never longer than TRACE_WAIT_MS: an entry that could not wait keeps its
 * sequence number with nothing in it, a gap the reader counts as lost.
 *
 * Wire bytes only, a clear PIN or code blanked (zw_redact.h): what S2 protects stays encrypted
 * here, and the network keys never pass the link. S0 would change that - its inclusion sends the
 * network key under a fixed all-zero key - so the build refuses the trace beside ALARM_ZWAVE_S0
 * until S0 inclusion is kept out of it.
 */
#include "zwave_internal.h"

#if CONFIG_ALARM_ZSRC_ZWAVE && CONFIG_ALARM_BUILD_DEV

#if CONFIG_ALARM_ZWAVE_S0
/* S0 inclusion sends the network key under an all-zero key: keep it out of zw_trace_put first. */
#error "the Z-Wave frame trace would record S0's network key (docs/features/30)"
#endif

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "alarm_mem.h"
#include "alarm_proto/proto_json.h"
#include "alarm_service.h"
#include "freertos/semphr.h"
#include "zwave_proto/zw_redact.h"

#define SLOTS         1024u         /* entries: about 170 exchanges of six (frame, ACK, answer) */
#define DATA          (16u * 1024u) /* the bytes they carried */
#define TRACE_WAIT_MS 5u            /* the longest the link waits for a reader's copy */
#define HEAD_ROOM     80u           /* {"api":1,"next":<20 digits>,"lost":<20>,"frames": */
#define ENTRY_ROOM    80u           /* ,{"seq":<20>,"t":<20>,"dir":"text","hex":""} */

_Static_assert((SLOTS & (SLOTS - 1u)) == 0u && (DATA & (DATA - 1u)) == 0u,
               "powers of two: the running counts stay continuous when they wrap");
_Static_assert(ZW_RX_BUF_SIZE < 8192u, "a length fits len_dir's 13 bits");

typedef struct {
    uint32_t seq;     /* its sequence number's low 32 bits: a slot holding another is a gap */
    uint32_t at;      /* where its bytes start, as a running count of the bytes written */
    uint32_t t_lo;    /* ms since boot: the low 32 bits */
    uint16_t t_hi;    /* and the next 16 (8,900 years) */
    uint16_t len_dir; /* its length << 3 | ZW_TRACE_* */
} slot_t;

/* Tasks only: PSRAM (alarm_mem.h). */
static ALARM_SLOW_BSS slot_t s_slots[SLOTS];
static ALARM_SLOW_BSS uint8_t s_data[DATA];

static struct {
    SemaphoreHandle_t lock;
    uint64_t next;    /* the next entry's sequence number; the first is 1 */
    uint64_t oldest;  /* the oldest entry still whole; == next: none */
    uint32_t head;    /* where the next entry's bytes go, as a running count */
    uint32_t skipped; /* entries that could not wait for the lock: the writer's alone */
} T;

static const char *const DIRS[] = {"rx", "tx", "crx", "ctx", "text"};
_Static_assert(sizeof DIRS / sizeof DIRS[0] == ZW_TRACE_TEXT + 1u, "a word for every direction");

void zw_trace_start(void)
{
    if (T.lock == NULL) {
        T.next = 1u;
        T.oldest = 1u;
        T.head = 0u;
        T.skipped = 0u;
        T.lock = xSemaphoreCreateMutex(); /* NULL: no trace, the link runs all the same */
    }
}

void zw_trace_put(uint8_t dir, const uint8_t *bytes, size_t len)
{
    if (T.lock == NULL || bytes == NULL || len == 0u || len > ZW_RX_BUF_SIZE ||
        dir > ZW_TRACE_TEXT) {
        return;
    }
    /* What is kept is a copy with any clear PIN or code blanked (zw_redact.h, ZW-030): a keypad
     * that fell back to no security sends its PIN in the clear. One writer: one scratch. */
    static uint8_t copy[ZW_RX_BUF_SIZE];
    if (dir == ZW_TRACE_RX || dir == ZW_TRACE_TX) {
        memcpy(copy, bytes, len);
        (void)zw_redact_frame(copy, len);
        bytes = copy;
    }
    const uint64_t now = alarm_service_now();
    const TickType_t wait = pdMS_TO_TICKS(TRACE_WAIT_MS);
    if (xSemaphoreTake(T.lock, wait > 0 ? wait : 1) != pdTRUE) {
        T.skipped++; /* its sequence number stays empty */
        return;
    }
    T.next += T.skipped;
    T.skipped = 0u;
    /* Room for it: its slot, and the bytes it overwrites, taken from the oldest entries. */
    const uint32_t end = T.head + (uint32_t)len;
    if (T.next - T.oldest >= SLOTS) {
        T.oldest = T.next - SLOTS + 1u;
    }
    while (T.oldest < T.next) {
        const slot_t *o = &s_slots[T.oldest % SLOTS];
        if (o->seq == (uint32_t)T.oldest && end - o->at <= DATA) {
            break; /* whole, and so is every later one */
        }
        T.oldest++; /* its bytes are overwritten now, or it is a gap */
    }
    const uint32_t at = T.head % DATA;
    const size_t first = len < DATA - at ? len : DATA - at;
    memcpy(&s_data[at], bytes, first);
    memcpy(s_data, bytes + first, len - first);
    slot_t *e = &s_slots[T.next % SLOTS];
    e->seq = (uint32_t)T.next;
    e->at = T.head;
    e->t_lo = (uint32_t)now;
    e->t_hi = (uint16_t)(now >> 32);
    e->len_dir = (uint16_t)((len << 3) | dir);
    T.head = end;
    T.next++;
    (void)xSemaphoreGive(T.lock);
}

/* One entry, copied out of the ring. */
typedef struct {
    uint64_t t;
    uint16_t len;
    uint8_t dir;
    uint8_t bytes[ZW_RX_BUF_SIZE];
} entry_t;

typedef enum { TAKE_NONE, TAKE_GAP, TAKE_ENTRY } take_t;

/* Entry *s into e, under the lock; *s moves to the oldest entry when it is older (what that skips
 * added to *lost), or to the oldest when it is past the newest (the panel restarted since the
 * reader's last call: its numbers are an earlier start's). NONE: nothing from *s on yet. */
static take_t take(uint64_t *s, uint64_t *lost, entry_t *e)
{
    take_t r = TAKE_NONE;
    (void)xSemaphoreTake(T.lock, portMAX_DELAY);
    if (*s > T.next) {
        *s = T.oldest;
    }
    if (*s < T.oldest) {
        *lost += T.oldest - *s;
        *s = T.oldest;
    }
    if (*s < T.next) {
        const slot_t *o = &s_slots[*s % SLOTS];
        const size_t len = (size_t)(o->len_dir >> 3);
        if (o->seq != (uint32_t)*s || len == 0u || len > sizeof e->bytes) {
            r = TAKE_GAP;
        } else {
            e->t = ((uint64_t)o->t_hi << 32) | o->t_lo;
            e->len = (uint16_t)len;
            e->dir = (uint8_t)(o->len_dir & 7u);
            const uint32_t at = o->at % DATA;
            const size_t first = len < DATA - at ? len : DATA - at;
            memcpy(e->bytes, &s_data[at], first);
            memcpy(e->bytes + first, s_data, len - first);
            r = TAKE_ENTRY;
        }
    }
    (void)xSemaphoreGive(T.lock);
    return r;
}

size_t zwave_service_trace_json(uint64_t since, char *out, size_t cap)
{
    static const char digits[] = "0123456789abcdef";
    if (out == NULL || cap < HEAD_ROOM + ENTRY_ROOM + 2u * ZW_RX_BUF_SIZE + 8u) {
        return 0u; /* not even one frame would fit */
    }
    const size_t budget = cap < ZWAVE_SERVICE_TRACE_JSON_MAX ? cap : ZWAVE_SERVICE_TRACE_JSON_MAX;
    /* The entries first, behind room for the header: "next" and "lost" are known only after. */
    pj_writer_t w;
    pj_init(&w, out + HEAD_ROOM, budget - HEAD_ROOM - 1u); /* 1: the closing brace */
    pj_arr(&w);
    uint64_t s = since > 0u ? since : 1u;
    uint64_t lost = 0u;
    if (T.lock == NULL) {
        s = 1u; /* no radio started: nothing recorded, the first entry will be 1 */
    }
    entry_t e;
    char hex[2u * ZW_RX_BUF_SIZE + 1u];
    for (unsigned count = 0u; T.lock != NULL && count < ZWAVE_SERVICE_TRACE_ENTRIES_MAX;) {
        const take_t r = take(&s, &lost, &e);
        if (r == TAKE_NONE) {
            break;
        }
        if (r == TAKE_GAP) {
            lost++;
            s++;
            continue;
        }
        if (w.len + ENTRY_ROOM + 2u * (size_t)e.len + 2u > w.cap) {
            break; /* the next answer starts with it */
        }
        for (size_t i = 0; i < e.len; i++) {
            hex[2u * i] = digits[e.bytes[i] >> 4];
            hex[2u * i + 1u] = digits[e.bytes[i] & 15u];
        }
        hex[2u * (size_t)e.len] = '\0';
        pj_obj(&w);
        pj_kuint(&w, "seq", s);
        pj_kuint(&w, "t", e.t);
        pj_kstr(&w, "dir", DIRS[e.dir <= ZW_TRACE_TEXT ? e.dir : ZW_TRACE_TEXT]);
        pj_kstr(&w, "hex", hex);
        pj_obj_end(&w);
        count++;
        s++;
    }
    pj_arr_end(&w);
    const size_t n = pj_finish(&w);
    char head[HEAD_ROOM + 16u];
    const int h = snprintf(head, sizeof head, "{\"api\":1,\"next\":%" PRIu64 ",\"lost\":%" PRIu64
                                              ",\"frames\":", s, lost);
    if (n == 0u || h <= 0 || (size_t)h > HEAD_ROOM) {
        return 0u;
    }
    memmove(out + h, out + HEAD_ROOM, n);
    memcpy(out, head, (size_t)h);
    out[(size_t)h + n] = '}';
    out[(size_t)h + n + 1u] = '\0';
    return (size_t)h + n + 1u;
}

#endif /* CONFIG_ALARM_ZSRC_ZWAVE && CONFIG_ALARM_BUILD_DEV */
