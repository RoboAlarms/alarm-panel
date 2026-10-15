/*
 * elog.c - power-fail-safe event log ring (see elog.h).
 *
 * Record layout, 64 bytes little-endian (external contract: fields are only ever added in the
 * reserved bytes, so older firmware can still read newer records):
 *    0  'E' 'L'         magic
 *    2  u8   version    1
 *    3  u8   kind       elog_kind_t
 *    4  u32  index      log order
 *    8  u32  seq        engine event sequence number
 *   12  u64  epoch_ms   wall clock, 0 = unknown
 *   20  u32  boot       boot counter
 *   24  u32  uptime_s   seconds since that boot
 *   28  u8   type, partition, user, source kind
 *   32  u16  zone, source id
 *   36  u8   arg, arg2, flags
 *   39  reserved (0)
 *   60  u32  crc32 of bytes 0..59
 */
#include "event_log/elog.h"

#include <string.h>

#include "alarm/alarm_codec.h"

#define REC ELOG_RECORD_SIZE
#define OFF_CRC 60u
#define REC_VERSION 1u

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16);
}

void elog_encode(uint8_t rec[ELOG_RECORD_SIZE], uint32_t index, elog_kind_t kind, uint32_t boot,
                 const alarm_event_t *ev)
{
    memset(rec, 0, REC);
    rec[0] = 'E';
    rec[1] = 'L';
    rec[2] = REC_VERSION;
    rec[3] = (uint8_t)kind;
    put32(rec + 4, index);
    put32(rec + 8, ev->seq);
    const uint64_t epoch = (uint64_t)ev->epoch_ms;
    put32(rec + 12, (uint32_t)epoch);
    put32(rec + 16, (uint32_t)(epoch >> 32));
    put32(rec + 20, boot);
    const alarm_ms_t up = ev->mono_ms / 1000u;
    put32(rec + 24, up > UINT32_MAX ? UINT32_MAX : (uint32_t)up);
    rec[28] = (uint8_t)ev->type;
    rec[29] = ev->partition;
    rec[30] = ev->user;
    rec[31] = ev->source.kind;
    put16(rec + 32, ev->zone);
    put16(rec + 34, ev->source.id);
    rec[36] = ev->arg;
    rec[37] = ev->arg2;
    rec[38] = ev->flags;
    put32(rec + OFF_CRC, alarm_crc32(0u, rec, OFF_CRC));
}

bool elog_decode(const uint8_t rec[ELOG_RECORD_SIZE], elog_entry_t *out)
{
    if (rec[0] != 'E' || rec[1] != 'L' || rec[2] == 0u || rec[2] == 0xFFu ||
        (rec[3] != ELOG_KIND_EVENT && rec[3] != ELOG_KIND_REPORTED) ||
        get32(rec + OFF_CRC) != alarm_crc32(0u, rec, OFF_CRC)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->index = get32(rec + 4);
    out->kind = rec[3];
    out->boot = get32(rec + 20);
    out->uptime_s = get32(rec + 24);
    alarm_event_t *ev = &out->ev;
    ev->seq = get32(rec + 8);
    ev->epoch_ms = (int64_t)((uint64_t)get32(rec + 12) | ((uint64_t)get32(rec + 16) << 32));
    ev->mono_ms = (alarm_ms_t)out->uptime_s * 1000u;
    ev->type = (alarm_event_type_t)rec[28];
    ev->partition = rec[29];
    ev->user = rec[30];
    ev->source.kind = rec[31];
    ev->zone = get16(rec + 32);
    ev->source.id = get16(rec + 34);
    ev->arg = rec[36];
    ev->arg2 = rec[37];
    ev->flags = rec[38];
    return true;
}

/* ---- slots ------------------------------------------------------------------------------- */

static int read_slot(const elog_t *log, uint32_t slot, uint8_t rec[REC])
{
    return log->flash.read(log->flash.ctx, slot * REC, rec, REC);
}

static bool blank(const uint8_t rec[REC])
{
    for (unsigned i = 0; i < REC; i++) {
        if (rec[i] != 0xFFu) {
            return false;
        }
    }
    return true;
}

static uint32_t prev_slot(const elog_t *log, uint32_t slot)
{
    return (slot == 0u) ? log->slots - 1u : slot - 1u;
}

/* Erase the sector that starts at 'slot' unless it's blank already; keeps count exact. */
static int erase_sector(elog_t *log, uint32_t slot)
{
    uint32_t readable = 0;
    bool all_blank = true;
    for (uint32_t i = 0; i < log->per_sector; i++) {
        uint8_t rec[REC];
        elog_entry_t e;
        if (read_slot(log, slot + i, rec) != 0) {
            all_blank = false;
            continue;
        }
        if (!blank(rec)) {
            all_blank = false;
        }
        if (elog_decode(rec, &e)) {
            readable++;
        }
    }
    if (all_blank) {
        return 0; /* first lap of a fresh partition: no wear */
    }
    const int err = log->flash.erase(log->flash.ctx, slot * REC, log->flash.sector_size);
    if (err == 0) {
        log->count -= (readable < log->count) ? readable : log->count;
    }
    return err;
}

/* Make the head a blank slot: at a sector start erase the sector ahead (the oldest records);
 * elsewhere skip slots a power cut left half written. */
static int prepare_head(elog_t *log)
{
    for (uint32_t guard = 0; guard <= log->slots; guard++) {
        if (log->head % log->per_sector == 0u) {
            return erase_sector(log, log->head);
        }
        uint8_t rec[REC];
        const int err = read_slot(log, log->head, rec);
        if (err != 0) {
            return err;
        }
        if (blank(rec)) {
            return 0;
        }
        log->head = (log->head + 1u) % log->slots; /* torn write: never rewritten in place */
    }
    return -1;
}

/* ---- API --------------------------------------------------------------------------------- */

int elog_open(elog_t *log, const elog_flash_t *flash, uint32_t boot)
{
    memset(log, 0, sizeof(*log));
    if (flash == NULL || flash->read == NULL || flash->write == NULL || flash->erase == NULL ||
        flash->sector_size == 0u || flash->sector_size % REC != 0u ||
        flash->size % flash->sector_size != 0u || flash->size / flash->sector_size < 2u) {
        return -1;
    }
    log->flash = *flash;
    log->slots = flash->size / REC;
    log->per_sector = flash->sector_size / REC;
    log->boot = boot;

    bool any = false;
    uint32_t newest_index = 0;
    uint32_t newest_slot = 0;
    for (uint32_t slot = 0; slot < log->slots; slot++) {
        uint8_t rec[REC];
        elog_entry_t e;
        const int err = read_slot(log, slot, rec);
        if (err != 0) {
            return err;
        }
        if (!elog_decode(rec, &e)) {
            continue;
        }
        log->count++;
        if (!any || e.index > newest_index) {
            any = true;
            newest_index = e.index;
            newest_slot = slot;
        }
        if (e.kind == ELOG_KIND_EVENT && e.ev.seq > log->last_seq) {
            log->last_seq = e.ev.seq;
        }
    }
    log->next_index = any ? newest_index + 1u : 1u;
    log->head = any ? (newest_slot + 1u) % log->slots : 0u;
    const int err = prepare_head(log);
    if (err != 0) {
        return err;
    }
    log->ready = true;
    return 0;
}

int elog_append(elog_t *log, elog_kind_t kind, const alarm_event_t *ev)
{
    if (log == NULL || !log->ready || ev == NULL ||
        (kind != ELOG_KIND_EVENT && kind != ELOG_KIND_REPORTED)) {
        return -1;
    }
    uint8_t rec[REC];
    elog_encode(rec, log->next_index, kind, log->boot, ev);
    const int err = log->flash.write(log->flash.ctx, log->head * REC, rec, REC);
    if (err != 0) {
        uint8_t back[REC];
        if (read_slot(log, log->head, back) == 0 && blank(back)) {
            return err; /* nothing reached the flash: the slot and the index stay free */
        }
    } else {
        log->count++;
        if (kind == ELOG_KIND_EVENT && ev->seq > log->last_seq) {
            log->last_seq = ev->seq;
        }
    }
    log->next_index++;
    log->head = (log->head + 1u) % log->slots;
    const int prep = prepare_head(log);
    return (err != 0) ? err : prep;
}

size_t elog_read(elog_t *log, uint32_t before, elog_filter_fn filter, void *filter_ctx,
                 elog_entry_t *out, size_t max, uint32_t scan, uint32_t *next_before)
{
    uint32_t next = 0;
    size_t n = 0;
    if (log == NULL || !log->ready || out == NULL || max == 0u) {
        if (next_before != NULL) {
            *next_before = 0;
        }
        return 0;
    }
    if (before > log->next_index) {
        before = log->next_index;
    }
    /* The record with index before-1 sits at least (next_index - before + 1) slots behind the
     * head: slots skipped after power cuts only push older records further back. */
    const uint32_t back = log->next_index - before + 1u;
    if (before > 1u && back <= log->slots) {
        uint32_t slot = (log->head + log->slots - back) % log->slots;
        const uint32_t remaining = log->slots - back + 1u; /* slots until we'd reach the head */
        const uint32_t limit = (scan == 0u || scan > remaining) ? remaining : scan;
        uint32_t lowest = before;
        bool end = limit == remaining;
        for (uint32_t i = 0; i < limit; i++, slot = prev_slot(log, slot)) {
            uint8_t rec[REC];
            elog_entry_t e;
            if (read_slot(log, slot, rec) != 0) {
                continue;
            }
            if (blank(rec)) {
                end = true; /* erased or never written: nothing older */
                break;
            }
            if (!elog_decode(rec, &e) || e.index >= lowest) {
                continue; /* torn slot, or newer than asked for */
            }
            lowest = e.index;
            if (filter == NULL || filter(filter_ctx, &e)) {
                out[n++] = e;
                if (n == max) {
                    end = false;
                    break;
                }
            }
        }
        if (!end) {
            next = (lowest < before) ? lowest : before - 1u;
        }
    }
    if (next_before != NULL) {
        *next_before = next;
    }
    return n;
}

size_t elog_read_after(elog_t *log, uint32_t after, elog_filter_fn filter, void *filter_ctx,
                       elog_entry_t *out, size_t max, uint32_t scan, uint32_t *next_after)
{
    uint32_t next = after;
    size_t n = 0;
    if (log != NULL && log->ready && out != NULL && max != 0u && after < log->next_index - 1u) {
        /* The record with index after+1 sits at least (next_index - 1 - after) slots behind the
         * head (slots skipped after power cuts only push it further back). Past the ring size it
         * was overwritten: start just after the head, where the oldest records follow the
         * erased sector. */
        const uint32_t newer = log->next_index - 1u - after;
        const uint32_t back = (newer < log->slots) ? newer : log->slots - 1u;
        uint32_t slot = (log->head + log->slots - back) % log->slots;
        /* Walk back over skipped slots to the newest record at or before 'after'. */
        for (uint32_t b = back; b < log->slots - 1u; b++) {
            const uint32_t prev = prev_slot(log, slot);
            uint8_t rec[REC];
            elog_entry_t e;
            if (read_slot(log, prev, rec) == 0 &&
                (blank(rec) || (elog_decode(rec, &e) && e.index <= after))) {
                break;
            }
            slot = prev;
        }
        uint32_t looked = 0;
        for (; slot != log->head; slot = (slot + 1u) % log->slots) {
            uint8_t rec[REC];
            elog_entry_t e;
            if (read_slot(log, slot, rec) != 0 || !elog_decode(rec, &e) || e.index <= after) {
                continue; /* blank ahead of the oldest record, torn, or not newer */
            }
            next = e.index;
            looked++;
            if (filter == NULL || filter(filter_ctx, &e)) {
                out[n++] = e;
                if (n == max) {
                    break;
                }
            }
            if (scan != 0u && looked >= scan) {
                break;
            }
        }
    }
    if (next_after != NULL) {
        *next_after = next;
    }
    return n;
}

int elog_clear(elog_t *log)
{
    if (log == NULL || !log->ready) {
        return -1;
    }
    for (uint32_t slot = 0; slot < log->slots; slot += log->per_sector) {
        const int err = log->flash.erase(log->flash.ctx, slot * REC, log->flash.sector_size);
        if (err != 0) {
            return err;
        }
    }
    log->head = 0;
    log->next_index = 1;
    log->count = 0;
    log->last_seq = 0;
    return 0;
}
