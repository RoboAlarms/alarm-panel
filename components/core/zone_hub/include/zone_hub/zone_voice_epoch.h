/*
 * zone_voice_epoch.h - which zones' changed sensors still owe a spoken-name invalidation, and
 * which are still settling (docs/features/08 voice-first-build.md D07, CORE-CFG-021).
 *
 * Every changed-sensor bind or unbind of a zone moves that zone's source-change epoch on. Two
 * marks follow it:
 *  - invalidated: the epoch whose approval invalidation is durably stored. The zone owes an
 *    invalidation while it differs from the epoch. An invalidation clears only the epoch it
 *    captured: a second change of the same zone while it runs keeps the obligation.
 *  - stored: the epoch whose binding is durably stored. The zone is settling (approval and
 *    automatic zone speech refuse it) while it differs from the epoch.
 * Comparisons are for equality only, so the counters may wrap.
 *
 * Portable and lock-free of its own: the caller serializes every call under its lock (the
 * zone service's), and runs one invalidation and one store at a time (the two capture slots
 * are the caller's single store sequence). Publishing zone_voice_settling() to readers that
 * take no lock (the alarm task) is the caller's, after each change, with its own atomics.
 */
#ifndef ZONE_VOICE_EPOCH_H
#define ZONE_VOICE_EPOCH_H

#include <stdbool.h>
#include <stdint.h>

#include "alarm/alarm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t epoch[ALARM_MAX_ZONES];       /* source changes, index zone - 1 */
    uint32_t invalidated[ALARM_MAX_ZONES]; /* epoch whose invalidation is stored */
    uint32_t stored[ALARM_MAX_ZONES];      /* epoch whose binding is stored */
    uint32_t inv_cap[ALARM_MAX_ZONES];     /* captured by zone_voice_owed() */
    uint32_t store_cap[ALARM_MAX_ZONES];   /* captured by zone_voice_capture() */
} zone_voice_epochs_t;

/* Nothing owed, nothing settling (what was loaded at start is stored). */
void zone_voice_init(zone_voice_epochs_t *v);

/* A changed sensor for this zone: call it before the change can be seen (the zone is settling
 * from here). False for a zone out of range. */
bool zone_voice_note_change(zone_voice_epochs_t *v, uint16_t zone);

/* The change just noted didn't happen after all (the bind was refused or changed nothing):
 * withdrawn when nothing moved the zone's epoch since, so it neither owes nor settles for it.
 * Call it only while that epoch's invalidation hasn't been marked (zone_voice_invalidated()).
 * True when withdrawn. */
bool zone_voice_undo_change(zone_voice_epochs_t *v, uint16_t zone, uint32_t noted);

/* The zone's current epoch (0 out of range): what zone_voice_undo_change() is given. */
uint32_t zone_voice_epoch(const zone_voice_epochs_t *v, uint16_t zone);

/* The zones that owe an invalidation, bit zone - 1 into out (ALARM_ZONE_BITMAP_BYTES), their
 * epochs captured for zone_voice_invalidated(). True when there is any. */
bool zone_voice_owed(zone_voice_epochs_t *v, uint8_t *out);

/* The invalidation of 'zones' (as zone_voice_owed() gave them) is durably stored: each clears
 * its obligation for the epoch captured then; a zone changed again since still owes one. */
void zone_voice_invalidated(zone_voice_epochs_t *v, const uint8_t *zones);

/* Any zone owes an invalidation: its binding must not be stored yet. */
bool zone_voice_any_owed(const zone_voice_epochs_t *v);

/* The bindings are about to be encoded for storage: capture every zone's epoch. */
void zone_voice_capture(zone_voice_epochs_t *v);

/* What zone_voice_capture() captured is durably stored. */
void zone_voice_stored(zone_voice_epochs_t *v);

/* The zone's changed sensor isn't durably settled yet (its invalidation or its binding is not
 * stored). False for a zone out of range. */
bool zone_voice_settling(const zone_voice_epochs_t *v, uint16_t zone);

#ifdef __cplusplus
}
#endif

#endif /* ZONE_VOICE_EPOCH_H */
