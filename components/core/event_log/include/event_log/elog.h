/*
 * elog.h - power-fail-safe event log: a ring of fixed 64-byte records in raw flash.
 *
 * Portable C11 like alarm_core: no heap, no globals, no RTOS. The flash is reached through
 * elog_flash_t (NOR semantics: write only clears bits, erase sets whole sectors to 0xFF), so
 * the same code runs on an ESP-IDF partition, in the PC preview and in host tests that cut
 * the power at every byte. One writer at a time: the platform serializes calls.
 *
 * Records are appended sector by sector. Before the head enters a sector that still holds
 * records (the oldest ones), that sector is erased. Every record carries a CRC, so a write
 * or erase interrupted by a power cut leaves at most one unreadable slot, which is skipped
 * and never rewritten in place. Record layout: see elog.c (external contract).
 */
#ifndef ELOG_H
#define ELOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ELOG_RECORD_SIZE 64u

typedef enum {
    ELOG_KIND_EVENT = 1,    /* an engine event */
    ELOG_KIND_REPORTED = 2, /* the event with ev.seq became due for remote reporting */
} elog_kind_t;

typedef struct {
    void *ctx;
    uint32_t size;        /* bytes: a multiple of sector_size, at least two sectors */
    uint32_t sector_size; /* erase unit: a multiple of ELOG_RECORD_SIZE */
    /* All return 0 on success. */
    int (*read)(void *ctx, uint32_t offset, void *buf, uint32_t len);
    int (*write)(void *ctx, uint32_t offset, const void *buf, uint32_t len);
    int (*erase)(void *ctx, uint32_t offset, uint32_t len); /* whole sectors */
} elog_flash_t;

typedef struct {
    uint32_t index;    /* order in the log: +1 per record, never reused */
    uint8_t kind;      /* elog_kind_t */
    uint32_t boot;     /* boot counter when it was written */
    uint32_t uptime_s; /* seconds since that boot (ev.mono_ms / 1000 when written) */
    alarm_event_t ev;  /* ELOG_KIND_REPORTED: ev.seq names the event; other fields are copies */
} elog_entry_t;

typedef struct {
    elog_flash_t flash;
    uint32_t slots;      /* records the ring can hold */
    uint32_t per_sector; /* records per sector */
    uint32_t head;       /* next slot to write */
    uint32_t next_index; /* index of the next record */
    uint32_t count;      /* readable records in the ring */
    uint32_t last_seq;   /* newest engine event sequence number in the log, 0 = none */
    uint32_t boot;       /* stamped on new records */
    bool ready;
} elog_t;

/* Records always kept: the ring minus the sector erased ahead of the head. */
static inline uint32_t elog_capacity(const elog_t *log)
{
    return log->slots - log->per_sector;
}

/* Mount: scan every slot, find the newest record, count what's readable and prepare the head.
 * boot is stamped on records appended from now on. Returns 0, -1 for a bad geometry, or the
 * flash driver's error. */
int elog_open(elog_t *log, const elog_flash_t *flash, uint32_t boot);

/* Append one record; its index is assigned here. Returns 0 or the flash driver's error (the
 * log stays usable: a failed slot is skipped). */
int elog_append(elog_t *log, elog_kind_t kind, const alarm_event_t *ev);

/* Filter for elog_read: return true to keep an entry. */
typedef bool (*elog_filter_fn)(void *ctx, const elog_entry_t *entry);

/* Newest first: up to max entries with index < before (UINT32_MAX = from the newest) that
 * pass the filter (NULL = all). At most scan slots are looked at (0 = no limit), so a sparse
 * filter can be paged without stalling the caller. *next_before (may be NULL) receives the
 * 'before' for the next page, 0 when nothing older is left. Returns the entries written. */
size_t elog_read(elog_t *log, uint32_t before, elog_filter_fn filter, void *filter_ctx,
                 elog_entry_t *out, size_t max, uint32_t scan, uint32_t *next_before);

/* Oldest first: up to max entries with index > after that pass the filter (NULL = all). When
 * records after 'after' were overwritten already, reading starts with the oldest one kept. At
 * most scan records are looked at (0 = no limit; unreadable slots don't count, so every call
 * with records left makes progress). *next_after (may be NULL) receives the 'after' for the
 * next call: the index of the newest record looked at, or 'after' when there was none. */
size_t elog_read_after(elog_t *log, uint32_t after, elog_filter_fn filter, void *filter_ctx,
                       elog_entry_t *out, size_t max, uint32_t scan, uint32_t *next_after);

/* Index of the newest record, 0 when the log is empty or not mounted. */
static inline uint32_t elog_newest_index(const elog_t *log)
{
    return (log != NULL && log->ready) ? log->next_index - 1u : 0u;
}

/* Erase every record (factory reset). Indexes restart at 1. */
int elog_clear(elog_t *log);

/* Record codec, exposed for tests and tools. */
void elog_encode(uint8_t rec[ELOG_RECORD_SIZE], uint32_t index, elog_kind_t kind, uint32_t boot,
                 const alarm_event_t *ev);
bool elog_decode(const uint8_t rec[ELOG_RECORD_SIZE], elog_entry_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ELOG_H */
