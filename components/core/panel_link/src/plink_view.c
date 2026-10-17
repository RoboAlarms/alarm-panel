/*
 * plink_view.c - see include/panel_link/plink_view.h (docs/features/11 "STATUS", "Zone record",
 * "Configuration", "Roles on a P4 panel", "Versions and compatibility"; LINK-002, LINK-008,
 * LINK-019, LINK-030, LINK-039).
 *
 * The master turns its snapshot into a keypad's STATUS, every duration worked out at the moment
 * the frame is sealed; the keypad turns STATUS and ZONES back into an alarm_snapshot_t on its own
 * clock, so its UI reads the master's state as the master's UI reads its own. Also the 7-byte
 * zone record and its CRC (declared in plink_msg.h: the master's STATUS and the keypad's view
 * must agree on it), and the keypads' copy of the configuration, which holds no code material.
 */
#include "panel_link/plink_view.h"

#include <string.h>

#include "alarm/alarm_codec.h"

/* A later STATUS can read a little longer than the last one would by now: its left was worked out
 * at sealing and counts from its arrival, so it carries the network's delay, and that delay
 * varies. Within one delay (the same state) a rise below this is that jitter, and the keypad
 * keeps counting from the last value, so the seconds it shows never go up (UI-023); an exit
 * restart or a walk test extended adds far more and is taken as sent. */
#define JITTER_MS 1000u

/* The keypads' code hash filler (LINK-019): byte 0 the user slot, byte 1 this, the rest zero. */
#define CFG_FILLER 0xA5u

#define ALL_PARTS ((uint8_t)((1u << ALARM_MAX_PARTITIONS) - 1u))

static uint32_t sat32(uint64_t v)
{
    return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

/* What is left at now of a duration that was `field` at `taken`: never below 0. */
static uint64_t left_at(uint64_t field, uint64_t taken, uint64_t now)
{
    const uint64_t gone = now > taken ? now - taken : 0u;
    return field > gone ? field - gone : 0u;
}

/* ---- the zone record (docs/features/11 "Zone record") --------------------------------------- */

static void zone_rec_write(const alarm_zone_status_t *z, uint8_t out[PLINK_ZONE_REC_LEN])
{
    out[0] = (uint8_t)z->flags;
    out[1] = (uint8_t)(z->flags >> 8);
    out[2] = z->trouble;
    out[3] = z->walk;
    out[4] = (uint8_t)z->walk_first_s;
    out[5] = (uint8_t)(z->walk_first_s >> 8);
    out[6] = z->walk_trips;
}

void plink_zones_rec(const plink_zones_t *m, uint16_t i, alarm_zone_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (m == NULL || m->recs == NULL || i >= m->count) {
        return;
    }
    /* A shorter record (none in v1) reads zeros past its end; a longer one's appended fields
     * are skipped (LINK-017). */
    uint8_t b[PLINK_ZONE_REC_LEN] = {0};
    const size_t n = m->rec_len < PLINK_ZONE_REC_LEN ? m->rec_len : PLINK_ZONE_REC_LEN;
    memcpy(b, m->recs + (size_t)i * m->rec_len, n);
    out->flags = (uint16_t)((unsigned)b[0] | ((unsigned)b[1] << 8));
    out->trouble = b[2];
    out->walk = b[3];
    out->walk_first_s = (uint16_t)((unsigned)b[4] | ((unsigned)b[5] << 8));
    out->walk_trips = b[6];
}

uint32_t plink_zones_crc(const alarm_zone_status_t *zone, uint16_t zones_used)
{
    uint32_t crc = 0u;
    for (uint16_t z = 0; zone != NULL && z < zones_used; z++) {
        uint8_t rec[PLINK_ZONE_REC_LEN];
        zone_rec_write(&zone[z], rec);
        crc = alarm_crc32(crc, rec, sizeof(rec));
    }
    return crc;
}

/* ---- the master: STATUS from the snapshot ---------------------------------------------------- */

static void part_build(const alarm_partition_status_t *s, uint8_t number, uint64_t taken,
                       uint64_t now, plink_part_t *r)
{
    r->number = number;
    r->state = (uint8_t)s->state;
    r->level = (uint8_t)s->level;
    r->arm_flags = s->arm_flags;
    r->armed_by = s->armed_by;
    r->arm_src_kind = s->arm_source.kind;
    r->arm_src_id = s->arm_source.id;
    r->ready_levels = s->ready_levels;
    r->flags = (uint8_t)((s->ready ? PLINK_PF_READY : 0u) | (s->chime ? PLINK_PF_CHIME : 0u) |
                         (s->walk_test ? PLINK_PF_WALK_TEST : 0u) |
                         (s->triggered ? PLINK_PF_TRIGGERED : 0u) |
                         (s->exit_error ? PLINK_PF_EXIT_ERROR : 0u) |
                         (s->trouble_unack ? PLINK_PF_TROUBLE_UNACK : 0u) |
                         (s->quick_exit ? PLINK_PF_QUICK_EXIT : 0u));
    r->sounder = (uint8_t)s->sounder;
    r->bell = (uint8_t)s->bell;
    r->delay_left_ms = sat32(left_at(s->delay_remaining_ms, taken, now));
    r->memory = s->memory;
    r->faults = s->faults;
    r->bypassed = s->bypassed;
    r->alarms = s->alarms;
    r->troubles = s->troubles;
    r->walk_left_ms = sat32(left_at(s->walk_test_remaining_ms, taken, now));
    /* The walk test's start goes as its age at sealing, and its raw engine time as an identity
     * the keypad maps once (0 = none, so a start whose low 32 bits are 0 reads 1). */
    if (s->walk_started_ms != 0u) {
        r->walk_age_ms = sat32(now > s->walk_started_ms ? now - s->walk_started_ms : 0u);
        r->walk_id = (uint32_t)s->walk_started_ms;
        if (r->walk_id == 0u) {
            r->walk_id = 1u;
        }
    }
    r->walk_seq = s->walk_seq;
    r->walk_tested = s->walk_tested;
    r->walk_total = s->walk_total;
    r->walk_flags = s->walk_flags;
    r->walk_started_by = s->walk_started_by;
    r->walk_ended_by = s->walk_ended_by;
    r->walk_end_how = s->walk_end_how;
    r->walk_src_kind = s->walk_started_src.kind;
    r->walk_src_id = s->walk_started_src.id;
}

/* FIRE and CO: fire or CO sounding on any of the master's enabled partitions (LINK-039), so a
 * keypad sounds them whatever partitions it holds. */
static uint8_t sounding_flags(const alarm_snapshot_t *snap)
{
    uint8_t f = 0u;
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        if ((snap->partitions & (1u << p)) == 0u) {
            continue;
        }
        if (snap->part[p].sounder == ALARM_SOUND_FIRE) {
            f = (uint8_t)(f | PLINK_SF_FIRE);
        } else if (snap->part[p].sounder == ALARM_SOUND_CO) {
            f = (uint8_t)(f | PLINK_SF_CO);
        }
    }
    return f;
}

void plink_status_build(const alarm_snapshot_t *snap, uint64_t now_ms,
                        const plink_status_extra_t *x, plink_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (snap == NULL) {
        return;
    }
    uint8_t fw_flags = 0u;
    if (x != NULL) {
        out->epoch_ms = x->epoch_ms;
        out->home = x->home;
        out->lockout_left_ms = x->lockout_left_ms;
        out->report = x->report;
        out->cfg_crc = x->cfg_crc;
        out->cfg_len = x->cfg_len;
        fw_flags = (uint8_t)((x->updating ? PLINK_SF_UPDATING : 0u) |
                             (x->hold ? PLINK_SF_HOLD : 0u));
    }
    const uint64_t taken = snap->taken_ms;
    out->partitions = (uint8_t)(snap->partitions & ALL_PARTS);
    out->sys_troubles = snap->sys_troubles;
    out->sys_restored = snap->sys_troubles_restored;
    out->dev_troubles = snap->dev_troubles;
    out->default_codes = snap->default_codes;
    out->flags = (uint8_t)((snap->program ? PLINK_SF_PROGRAM : 0u) |
                           (snap->exit_sounds ? PLINK_SF_EXIT_SOUNDS : 0u) |
                           sounding_flags(snap) | fw_flags);
    out->recovery_left_ms = sat32(left_at(snap->recovery_left_ms, taken, now_ms));
    out->zones_used = snap->zones_used <= ALARM_MAX_ZONES ? snap->zones_used : ALARM_MAX_ZONES;
    out->last_event_seq = snap->last_event_seq;
    /* Over the zones this keypad can hold: one with fewer slots than the master uses compares
     * its own zones 1..its slots, and must not ask for the rest forever. */
    uint16_t crc_zones = out->zones_used;
    if (x != NULL && x->zones_cap != 0u && x->zones_cap < crc_zones) {
        crc_zones = x->zones_cap;
    }
    out->zones_crc = plink_zones_crc(snap->zone, crc_zones);
    out->mono_ms = now_ms; /* a keypad's voice tells an old event without the wall clock */
    for (uint8_t p = 1u; p <= ALARM_MAX_PARTITIONS && out->count < PLINK_PARTS_MAX; p++) {
        if ((out->partitions & (1u << (p - 1u))) != 0u) {
            part_build(&snap->part[p - 1u], p, taken, now_ms, &out->part[out->count++]);
        }
    }
}

/* ---- the keypad: the snapshot rebuilt on its own clock ------------------------------------ */

void plink_view_init(plink_view_t *v)
{
    if (v != NULL) {
        memset(v, 0, sizeof(*v));
    }
}

static bool in_delay(alarm_arm_state_t st)
{
    return st == ALARM_STATE_EXIT_DELAY || st == ALARM_STATE_ENTRY_DELAY;
}

/* The new left of a countdown that, before this frame, would read `before` now: within one run
 * of it (`same`), a rise below JITTER_MS keeps `before`. */
static uint64_t steady(uint64_t before, uint64_t sent, bool same)
{
    return same && sent > before && sent - before < JITTER_MS ? before : sent;
}

/* ---- fail-safe decoding (LINK-039; docs/features/11 "Versions and compatibility") ------------ */

/* An arm state this firmware doesn't know is armed: a keypad never shows Ready for it. */
static alarm_arm_state_t state_of(uint8_t v)
{
    return v <= (uint8_t)ALARM_STATE_ENTRY_DELAY ? (alarm_arm_state_t)v : ALARM_STATE_ARMED;
}

/* A level this firmware doesn't know is Away on a partition that isn't disarmed (none on one
 * that is); an armed state this firmware didn't know (state_known false) without a level it knows
 * is Away too. */
static alarm_arm_level_t level_of(uint8_t v, alarm_arm_state_t state, bool state_known)
{
    const bool known = v <= (uint8_t)ALARM_LEVEL_NIGHT;
    if (known && (state_known || v != (uint8_t)ALARM_LEVEL_NONE)) {
        return (alarm_arm_level_t)v;
    }
    return state == ALARM_STATE_DISARMED ? ALARM_LEVEL_NONE : ALARM_LEVEL_AWAY;
}

/* A continuous pattern this firmware doesn't know sounds as the alarm: never silence. */
static alarm_sound_t sounder_of(uint8_t v)
{
    return v <= (uint8_t)ALARM_SOUND_FIRE ? (alarm_sound_t)v : ALARM_SOUND_ALARM;
}

static void part_apply(plink_view_t *v, const plink_part_t *r, uint64_t now)
{
    const unsigned p = r->number - 1u;
    alarm_partition_status_t *s = &v->snap.part[p];
    const alarm_partition_status_t was = *s;
    const uint64_t old_taken = v->snap.taken_ms;
    const bool had = v->have_status && (v->snap.partitions & (1u << p)) != 0u;

    memset(s, 0, sizeof(*s));
    s->state = state_of(r->state);
    s->level = level_of(r->level, s->state, r->state <= (uint8_t)ALARM_STATE_ENTRY_DELAY);
    s->arm_flags = r->arm_flags;
    s->armed_by = r->armed_by;
    s->arm_source.kind = r->arm_src_kind;
    s->arm_source.id = r->arm_src_id;
    s->ready_levels = r->ready_levels;
    s->ready = (r->flags & PLINK_PF_READY) != 0u;
    s->chime = (r->flags & PLINK_PF_CHIME) != 0u;
    s->walk_test = (r->flags & PLINK_PF_WALK_TEST) != 0u;
    s->triggered = (r->flags & PLINK_PF_TRIGGERED) != 0u;
    s->exit_error = (r->flags & PLINK_PF_EXIT_ERROR) != 0u;
    s->trouble_unack = (r->flags & PLINK_PF_TROUBLE_UNACK) != 0u;
    s->quick_exit = (r->flags & PLINK_PF_QUICK_EXIT) != 0u;
    s->sounder = sounder_of(r->sounder);
    s->bell = (alarm_pattern_t)r->bell;
    s->delay_remaining_ms =
        steady(left_at(was.delay_remaining_ms, old_taken, now), r->delay_left_ms,
               had && in_delay(was.state) && was.state == s->state);
    s->memory = r->memory;
    s->faults = r->faults;
    s->bypassed = r->bypassed;
    s->alarms = r->alarms;
    s->troubles = r->troubles;
    s->walk_test_remaining_ms = steady(left_at(was.walk_test_remaining_ms, old_taken, now),
                                       r->walk_left_ms, had && was.walk_test && s->walk_test);
    /* The walk test's start on this clock, mapped once per distinct start: later frames carry
     * a growing age with the same id and must not move it. */
    if (r->walk_id == 0u) {
        v->walk_id[p] = 0u;
        v->walk_local[p] = 0u;
    } else if (r->walk_id != v->walk_id[p] || v->walk_local[p] == 0u) {
        const uint64_t start = now > r->walk_age_ms ? now - r->walk_age_ms : 0u;
        v->walk_id[p] = r->walk_id;
        v->walk_local[p] = start != 0u ? start : 1u;
    }
    s->walk_started_ms = v->walk_local[p];
    s->walk_seq = r->walk_seq;
    s->walk_tested = r->walk_tested;
    s->walk_total = r->walk_total;
    s->walk_flags = r->walk_flags;
    s->walk_started_by = r->walk_started_by;
    s->walk_ended_by = r->walk_ended_by;
    s->walk_end_how = r->walk_end_how;
    s->walk_started_src.kind = r->walk_src_kind;
    s->walk_started_src.id = r->walk_src_id;
}

/* A record this frame doesn't carry (a small transport's STATUS comes in pieces) keeps its last
 * values, its countdowns aged to now, since taken_ms moves to now (UI-023). */
static void part_age(alarm_partition_status_t *s, uint64_t taken, uint64_t now)
{
    s->delay_remaining_ms = left_at(s->delay_remaining_ms, taken, now);
    s->walk_test_remaining_ms = left_at(s->walk_test_remaining_ms, taken, now);
}

void plink_view_apply_status(plink_view_t *v, const plink_status_t *st, uint64_t now_ms)
{
    if (v == NULL || st == NULL) {
        return;
    }
    alarm_snapshot_t *s = &v->snap;
    const uint8_t mask = (uint8_t)(st->partitions & ALL_PARTS);
    const unsigned count = st->count < PLINK_PARTS_MAX ? st->count : PLINK_PARTS_MAX;

    bool carried[ALARM_MAX_PARTITIONS] = {false};
    for (unsigned i = 0; i < count; i++) {
        const uint8_t n = st->part[i].number;
        if (n >= 1u && n <= ALARM_MAX_PARTITIONS && (mask & (1u << (n - 1u))) != 0u) {
            carried[n - 1u] = true;
        }
    }
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        if ((mask & (1u << p)) == 0u) {
            memset(&s->part[p], 0, sizeof(s->part[p]));
            v->walk_id[p] = 0u;
            v->walk_local[p] = 0u;
        }
    }
    for (unsigned i = 0; i < count; i++) {
        const uint8_t n = st->part[i].number; /* records beyond this build's: ignored */
        if (n >= 1u && n <= ALARM_MAX_PARTITIONS && carried[n - 1u]) {
            part_apply(v, &st->part[i], now_ms);
        }
    }
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        if ((mask & (1u << p)) != 0u && !carried[p] && v->have_status) {
            part_age(&s->part[p], s->taken_ms, now_ms);
        }
    }

    /* The keypad's own lockout (an end on this clock) and the recovery window count down under
     * the same rule as the delays: a lockout still running only moves later by a second or more. */
    const alarm_ms_t lock_was = v->have_status ? s->local_lockout_until : 0u;
    const alarm_ms_t lock_new = st->lockout_left_ms != 0u ? now_ms + st->lockout_left_ms : 0u;
    s->local_lockout_until =
        lock_was > now_ms && lock_new > lock_was && lock_new - lock_was < JITTER_MS ? lock_was
                                                                                    : lock_new;
    s->recovery_left_ms =
        (uint32_t)steady(v->have_status ? left_at(s->recovery_left_ms, s->taken_ms, now_ms) : 0u,
                         st->recovery_left_ms, v->have_status && s->recovery_left_ms != 0u);
    s->taken_ms = now_ms;
    s->partitions = mask;
    s->sys_troubles = st->sys_troubles;
    s->sys_troubles_restored = st->sys_restored;
    s->dev_troubles = st->dev_troubles;
    s->default_codes = st->default_codes;
    s->program = (st->flags & PLINK_SF_PROGRAM) != 0u;
    s->exit_sounds = (st->flags & PLINK_SF_EXIT_SOUNDS) != 0u;
    const uint16_t used = st->zones_used <= ALARM_MAX_ZONES ? st->zones_used : ALARM_MAX_ZONES;
    s->zones_used = used;
    memset(&s->zone[used], 0, (size_t)(ALARM_MAX_ZONES - used) * sizeof(s->zone[0]));
    s->last_event_seq = st->last_event_seq;

    v->have_status = true;
    v->home = st->home;
    v->epoch_ms = st->epoch_ms;
    v->epoch_at_ms = now_ms;
    v->report = st->report;
    v->cfg_crc = st->cfg_crc;
    v->cfg_len = st->cfg_len;
    v->zones_crc = st->zones_crc;
    v->mono_ms = st->mono_ms;
    v->mono_at_ms = now_ms;
    v->flags = st->flags;
}

uint8_t plink_view_continuous(const plink_view_t *v, uint8_t home)
{
    if (v == NULL || !v->have_status) {
        return ALARM_SOUND_NONE;
    }
    bool fire = (v->flags & PLINK_SF_FIRE) != 0u;
    bool co = (v->flags & PLINK_SF_CO) != 0u;
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS; p++) {
        if ((v->snap.partitions & (1u << p)) != 0u) {
            fire = fire || v->snap.part[p].sounder == ALARM_SOUND_FIRE;
            co = co || v->snap.part[p].sounder == ALARM_SOUND_CO;
        }
    }
    if (fire) {
        return ALARM_SOUND_FIRE;
    }
    if (co) {
        return ALARM_SOUND_CO;
    }
    if (home < 1u || home > ALARM_MAX_PARTITIONS ||
        (v->snap.partitions & (1u << (home - 1u))) == 0u) {
        return ALARM_SOUND_NONE;
    }
    return (uint8_t)v->snap.part[home - 1u].sounder;
}

void plink_view_apply_zones(plink_view_t *v, const plink_zones_t *z)
{
    if (v == NULL || z == NULL || z->recs == NULL) {
        return;
    }
    for (uint16_t i = 0; i < z->count; i++) {
        const uint32_t zone = (uint32_t)z->first + i;
        if (zone >= 1u && zone <= ALARM_MAX_ZONES) {
            plink_zones_rec(z, i, &v->snap.zone[zone - 1u]);
        }
    }
}

uint32_t plink_view_zones_crc(const plink_view_t *v)
{
    if (v == NULL) {
        return 0u;
    }
    const uint16_t used =
        v->snap.zones_used <= ALARM_MAX_ZONES ? v->snap.zones_used : ALARM_MAX_ZONES;
    return plink_zones_crc(v->snap.zone, used);
}

/* ---- the configuration copy (LINK-019) ---------------------------------------------------- */

alarm_result_t plink_cfg_for_keypads(const alarm_config_t *cfg, alarm_config_t *scratch,
                                     uint8_t *out, size_t cap, size_t *len, uint32_t *crc)
{
    if (cfg == NULL || scratch == NULL || scratch == cfg || out == NULL) {
        return ALARM_ERR_BAD_ARG;
    }
    *scratch = *cfg;
    memset(scratch->code_salt, 0, sizeof(scratch->code_salt));
    for (unsigned u = 0; u < ALARM_MAX_USERS; u++) {
        alarm_user_t *user = &scratch->users[u];
        memset(user->code_hash, 0, sizeof(user->code_hash));
        if (user->authority == ALARM_AUTH_NONE) {
            continue;
        }
        /* Unique per slot, so the copy still passes the check that no two users share a code,
         * and made of nothing but the slot number. */
        user->code_hash[0] = (uint8_t)(u + 1u);
        user->code_hash[1] = CFG_FILLER;
        if (user->authority == ALARM_AUTH_DURESS) {
            user->authority = ALARM_AUTH_STANDARD; /* a keypad never tells duress apart */
        }
    }
    size_t n = 0u;
    const alarm_result_t r = alarm_config_encode(scratch, out, cap, &n);
    if (r != ALARM_OK) {
        return r;
    }
    if (len != NULL) {
        *len = n;
    }
    if (crc != NULL) {
        *crc = alarm_crc32(0u, out, n);
    }
    return ALARM_OK;
}

alarm_result_t plink_cfg_decode(const uint8_t *blob, size_t len, alarm_config_t *out)
{
    return alarm_config_decode_lenient(blob, len, out);
}
