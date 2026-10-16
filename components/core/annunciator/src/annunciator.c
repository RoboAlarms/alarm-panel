/*
 * annunciator.c - the voice's request policy (docs/features/08 voice-first-build.md D04, D05,
 * D06, D11; docs/features/40 WALK-016). See include/annunciator/annunciator.h.
 */
#include "annunciator/annunciator.h"

#include <string.h>

#include "voice_pack/voice_pack.h"

#define TONE_PENDING 0u
#define TONE_PLAYED 1u
#define TONE_CANCELED 2u

typedef enum {
    BUILD_OK = 0,
    BUILD_EMPTY,
    BUILD_TOO_MANY,
    BUILD_BAD_CLIP,
    BUILD_TOO_LONG,
} build_t;

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

ann_priority_t ann_priority(ann_kind_t kind)
{
    switch (kind) {
    case ANN_KIND_ENTRY:
        return ANN_PRIO_ENTRY;
    case ANN_KIND_ARMING:
    case ANN_KIND_ARMED:
    case ANN_KIND_DISARMED:
    case ANN_KIND_WALK_START:
    case ANN_KIND_WALK_END:
    case ANN_KIND_WALK_WARNING:
        return ANN_PRIO_STATE;
    case ANN_KIND_NOT_READY:
    case ANN_KIND_WALK_TRIP:
        return ANN_PRIO_REFUSAL;
    case ANN_KIND_CHIME:
        return ANN_PRIO_CHIME;
    case ANN_KIND_PREVIEW:
    case ANN_KIND_NONE:
    default:
        return ANN_PRIO_PREVIEW;
    }
}

static uint32_t expiry_ms(ann_kind_t kind)
{
    switch (kind) {
    case ANN_KIND_CHIME:
    case ANN_KIND_WALK_TRIP:
    case ANN_KIND_NOT_READY:
        return ANN_EXPIRE_SHORT_MS;
    default:
        return ANN_EXPIRE_LONG_MS;
    }
}

static uint32_t gap_samples(uint16_t ms)
{
    return (uint32_t)ms * (ANN_SAMPLE_RATE / 1000u);
}

static uint16_t word_gap(const ann_t *a)
{
    return (a->pack.word_gap_ms != 0u) ? a->pack.word_gap_ms : (uint16_t)ANN_WORD_GAP_MS;
}

static uint16_t part_gap(const ann_t *a)
{
    return (a->pack.part_gap_ms != 0u) ? a->pack.part_gap_ms : (uint16_t)ANN_PART_GAP_MS;
}

static uint16_t mode_clip(uint8_t level)
{
    switch (level) {
    case ALARM_LEVEL_AWAY:
        return VP_ID_AWAY;
    case ALARM_LEVEL_STAY:
        return VP_ID_HOME;
    case ALARM_LEVEL_NIGHT:
        return VP_ID_NIGHT;
    default:
        return VP_TOKEN_NONE;
    }
}

static bool loud_sound(alarm_sound_t s)
{
    return s == ALARM_SOUND_AUX || s == ALARM_SOUND_ALARM || s == ALARM_SOUND_CO ||
           s == ALARM_SOUND_FIRE;
}

void ann_state_from_snapshot(const alarm_snapshot_t *s, uint8_t keypad_partition,
                             bool exit_sounds, ann_state_t *out)
{
    memset(out, 0, sizeof(*out));
    out->exit_sounds = exit_sounds;
    out->config_seq = s->config_seq;
    for (unsigned p = 0u; p < ALARM_MAX_PARTITIONS; p++) {
        if ((s->partitions & (1u << p)) != 0u && loud_sound(s->part[p].sounder)) {
            out->loud = true;
        }
    }
    if (keypad_partition < 1u || keypad_partition > ALARM_MAX_PARTITIONS ||
        (s->partitions & (1u << (keypad_partition - 1u))) == 0u) {
        return;
    }
    const alarm_partition_status_t *ps = &s->part[keypad_partition - 1u];
    out->state = (uint8_t)ps->state;
    out->level = (uint8_t)ps->level;
    out->arm_flags = ps->arm_flags;
    out->walk_test = ps->walk_test;
    out->walk_tested = ps->walk_tested;
    out->walk_total = ps->walk_total;
}

uint32_t ann_pack_clip_samples(void *ctx, uint16_t id)
{
    vp_clip_t clip;
    if (ctx == NULL || !vp_find((const vp_pack_t *)ctx, id, &clip)) {
        return 0u;
    }
    return clip.valid_samples;
}

/* Fills the utterance's clips and gaps and checks the 10-second ceiling (D02). The first `phrase`
 * clips are one phrase, joined by the word gap ("Arming" "Away"); each clip after them starts a
 * new sentence, after the part gap ("You may exit now."). A phrase that is a spoken name (it
 * ends in a word) ends in that word's ending when the pack has one (AUD-031): the form a word
 * takes at the end of a name, long and falling, where its own clip suits the words before. */
static build_t build(const ann_t *a, ann_utt_t *u, const uint16_t *clips, size_t n,
                     size_t phrase)
{
    if (n == 0u) {
        return BUILD_EMPTY;
    }
    if (n > ANN_CLIPS_MAX) {
        return BUILD_TOO_MANY;
    }
    uint32_t total = 0u;
    for (size_t i = 0u; i < n; i++) {
        uint16_t id = clips[i];
        if (i + 1u == phrase && id >= VP_TOKEN_FIRST && id <= VP_TOKEN_LAST &&
            a->ports.clip_samples != NULL) {
            const uint16_t end = vp_ending_id(id);
            if (end != VP_TOKEN_NONE && a->ports.clip_samples(a->ports.ctx, end) > 0u) {
                id = end;
            }
        }
        const uint32_t samples = (a->ports.clip_samples != NULL && id != VP_TOKEN_NONE)
                                     ? a->ports.clip_samples(a->ports.ctx, id)
                                     : 0u;
        if (samples == 0u) {
            return BUILD_BAD_CLIP;
        }
        if (samples > ANN_SAMPLES_MAX) {
            return BUILD_TOO_LONG;
        }
        const uint16_t gap_ms = (i == 0u) ? 0u : (i < phrase) ? word_gap(a) : part_gap(a);
        total += samples + gap_samples(gap_ms);
        if (total > ANN_SAMPLES_MAX) {
            return BUILD_TOO_LONG;
        }
        u->clips[i] = id;
        u->gap_ms[i] = gap_ms;
    }
    u->count = (uint8_t)n;
    u->total_samples = total;
    return BUILD_OK;
}

static void req_start(ann_t *a, ann_req_t *r, ann_kind_t kind, uint16_t zone, uint64_t now)
{
    memset(r, 0, sizeof(*r));
    r->utt.kind = (uint8_t)kind;
    r->utt.priority = (uint8_t)ann_priority(kind);
    r->utt.zone = zone;
    r->deadline = now + expiry_ms(kind);
    r->walk_gen = a->walk_gen;
    r->warn_gen = a->warn_gen;
    r->config_seq = a->st.config_seq;
    r->action_gen = a->action_gen;
}

/* The tone a request follows. A tone that wasn't admitted (ticket 0) or was canceled takes its
 * speech with it (D04, D11); `required`: no tone at all means no speech either. */
static bool attach(ann_req_t *r, const ann_tone_t *t, bool required)
{
    if (!t->present) {
        return !required;
    }
    if (t->ticket == 0u || t->outcome == TONE_CANCELED) {
        return false;
    }
    r->tone_ticket = (t->outcome == TONE_PLAYED) ? 0u : t->ticket;
    return true;
}

static bool auto_allowed(const ann_t *a, ann_kind_t kind)
{
    const ann_prefs_t *p = &a->prefs;
    if (!a->pack.ready || p->volume == 0u || p->power_saving || a->st.loud) {
        return false;
    }
    switch (kind) {
    case ANN_KIND_PREVIEW:
        return true; /* explicit: the main switch doesn't apply (D06) */
    case ANN_KIND_WALK_START:
    case ANN_KIND_WALK_END:
    case ANN_KIND_WALK_WARNING:
    case ANN_KIND_WALK_TRIP:
        return true; /* the walk test always speaks, Announcements off too (WALK-016) */
    default:
        break;
    }
    if (!p->voice_enabled) {
        return false;
    }
    switch (kind) {
    case ANN_KIND_ENTRY:
    case ANN_KIND_ARMING:
    case ANN_KIND_ARMED:
    case ANN_KIND_DISARMED:
    case ANN_KIND_NOT_READY:
        return p->voice_status;
    case ANN_KIND_CHIME:
        return p->voice_chime;
    default:
        return false;
    }
}

/* Is the request still worth saying with what the policy knows now (D11: preferences, pack,
 * alarms, state, the state action, walk test and configuration generations)? */
static bool still_valid(const ann_t *a, const ann_req_t *r)
{
    const ann_kind_t kind = (ann_kind_t)r->utt.kind;
    if (!auto_allowed(a, kind)) {
        return false;
    }
    const ann_state_t *st = &a->st;
    const bool same_action = r->action_gen == a->action_gen;
    switch (kind) {
    case ANN_KIND_ENTRY:
        return same_action && st->state == ALARM_STATE_ENTRY_DELAY;
    case ANN_KIND_ARMING:
        return same_action && st->state == ALARM_STATE_EXIT_DELAY;
    case ANN_KIND_ARMED:
        return same_action && st->state == ALARM_STATE_ARMED;
    case ANN_KIND_DISARMED:
        return same_action && st->state == ALARM_STATE_DISARMED;
    case ANN_KIND_NOT_READY:
        return same_action;
    case ANN_KIND_CHIME:
        return st->state == ALARM_STATE_DISARMED && r->config_seq == st->config_seq;
    case ANN_KIND_WALK_TRIP:
        return st->walk_test && r->walk_gen == a->walk_gen && r->config_seq == st->config_seq;
    case ANN_KIND_WALK_START:
        return st->walk_test && r->walk_gen == a->walk_gen;
    case ANN_KIND_WALK_WARNING:
        return st->walk_test && r->walk_gen == a->walk_gen && r->warn_gen == a->warn_gen;
    case ANN_KIND_WALK_END:
        return !st->walk_test && r->walk_gen == a->walk_gen;
    default:
        return true;
    }
}

static void cancel_active(ann_t *a)
{
    if (a->active_used) {
        a->active_used = false;
        a->generation++;
    }
}

static bool ready(const ann_req_t *r)
{
    return r->tone_ticket == 0u && !r->wait_bell;
}

/* Drops what is no longer valid, waiting or active. */
static void sweep(ann_t *a)
{
    for (unsigned i = 0u; i < ANN_WAITING_MAX; i++) {
        if (a->waiting[i].used && !still_valid(a, &a->waiting[i])) {
            a->waiting[i].used = false;
        }
    }
    if (a->active_used && !still_valid(a, &a->active)) {
        cancel_active(a);
    }
}

/* Higher-priority speech that is ready supersedes lower active speech (D11). */
static void reconcile(ann_t *a, uint64_t now)
{
    if (!a->active_used) {
        return;
    }
    for (unsigned i = 0u; i < ANN_WAITING_MAX; i++) {
        const ann_req_t *r = &a->waiting[i];
        if (r->used && ready(r) && now < r->deadline &&
            r->utt.priority > a->active.utt.priority) {
            cancel_active(a);
            return;
        }
    }
}

/* Queues a request: coalesces with a waiting one of the same kind and zone, else takes a free
 * slot, else evicts the lowest-priority newest one when it is lower than this. */
static bool enqueue(ann_t *a, ann_req_t *r)
{
    if (!still_valid(a, r)) {
        return false;
    }
    ann_req_t *slot = NULL;
    for (unsigned i = 0u; i < ANN_WAITING_MAX; i++) {
        ann_req_t *w = &a->waiting[i];
        if (w->used && w->utt.kind == r->utt.kind && w->utt.zone == r->utt.zone) {
            r->order = w->order; /* keeps its place */
            slot = w;
            break;
        }
    }
    if (slot == NULL) {
        r->order = a->next_order++;
        for (unsigned i = 0u; i < ANN_WAITING_MAX && slot == NULL; i++) {
            if (!a->waiting[i].used) {
                slot = &a->waiting[i];
            }
        }
    }
    if (slot == NULL) {
        ann_req_t *low = NULL;
        for (unsigned i = 0u; i < ANN_WAITING_MAX; i++) {
            ann_req_t *w = &a->waiting[i];
            if (low == NULL || w->utt.priority < low->utt.priority ||
                (w->utt.priority == low->utt.priority && w->order > low->order)) {
                low = w;
            }
        }
        if (low == NULL || low->utt.priority >= r->utt.priority) {
            return false; /* the queue holds only speech at least as important */
        }
        slot = low;
    }
    if (++a->next_id == 0u) {
        a->next_id = 1u;
    }
    r->utt.id = a->next_id;
    r->used = true;
    *slot = *r;
    return true;
}

/* ------------------------------------------------------------------------ */
/* Requests from engine facts (D04, D05)                                     */
/* ------------------------------------------------------------------------ */

static void want(ann_t *a, ann_kind_t kind, const uint16_t *clips, size_t n, size_t phrase,
                 const ann_tone_t *tone, bool tone_required, uint64_t now)
{
    ann_req_t r;
    req_start(a, &r, kind, 0u, now);
    if (!auto_allowed(a, kind) || !attach(&r, tone, tone_required)) {
        return;
    }
    if (build(a, &r.utt, clips, n, phrase) != BUILD_OK) {
        return;
    }
    (void)enqueue(a, &r);
}

static void want_zone(ann_t *a, const ann_zone_sound_t *zs, uint64_t now)
{
    const ann_kind_t kind = (ann_kind_t)zs->kind;
    ann_req_t r;
    req_start(a, &r, kind, zs->zone, now);
    if (!auto_allowed(a, kind) || !attach(&r, &zs->tone, true) || a->ports.zone_words == NULL) {
        return;
    }
    ann_zone_words_t w;
    memset(&w, 0, sizeof(w));
    if (!a->ports.zone_words(a->ports.ctx, zs->zone, &w)) {
        return;
    }
    /* AUD-008: only a current approved name - approved against the zone's present binding and
     * the active pack - is ever spoken; anything else leaves the tone alone. */
    if (w.count == 0u || w.count > ANN_CLIPS_MAX || w.binding_revision == 0u ||
        w.approved_revision != w.binding_revision || w.pack_revision != a->pack.revision) {
        return;
    }
    for (unsigned i = 0u; i < w.count; i++) {
        if (w.ids[i] < VP_TOKEN_FIRST || w.ids[i] > VP_TOKEN_LAST) {
            return;
        }
    }
    if (build(a, &r.utt, w.ids, w.count, w.count) != BUILD_OK) {
        return;
    }
    (void)enqueue(a, &r);
}

static void armed_words(ann_t *a, uint8_t level, uint8_t flags, const ann_tone_t *tone,
                        uint64_t now)
{
    const uint16_t mode = mode_clip(level);
    if (mode == VP_TOKEN_NONE) {
        return;
    }
    uint16_t c[3] = {VP_ID_ARMED, mode, VP_ID_NO_ENTRY_DELAY};
    const size_t n = ((flags & ALARM_ARM_NO_ENTRY_DELAY) != 0u) ? 3u : 2u;
    want(a, ANN_KIND_ARMED, c, n, 2u, tone, false, now); /* "Armed away." "No entry delay." */
}

static void decide(ann_t *a, const ann_state_t *prev, uint64_t now)
{
    const ann_batch_t *b = &a->batch;
    const ann_state_t *st = &a->st;
    const ann_tone_t none = {0u, TONE_PENDING, false};

    /* The arming cycle (D05): latched at acceptance, so auto-stay's clearing of the silent-exit
     * flag doesn't make the final confirmation speak. */
    if (b->armed) {
        a->action_gen++; /* every accepted arm, silent or not, instant or not */
        a->cycle = true;
        a->cycle_silent = (b->armed_flags & ALARM_ARM_SILENT_EXIT) != 0u;
        a->cycle_armed_said = false;
        const uint16_t mode = mode_clip(b->armed_level);
        if (st->state == ALARM_STATE_EXIT_DELAY) {
            if (!a->cycle_silent && st->exit_sounds && mode != VP_TOKEN_NONE) {
                const uint16_t c[3] = {VP_ID_ARMING, mode, VP_ID_EXIT_NOW};
                want(a, ANN_KIND_ARMING, c, 3u, 2u, &b->ack, false, now);
            }
        } else if (st->state == ALARM_STATE_ARMED) {
            /* An instant arm: no arming sequence, only the armed state. */
            a->cycle_armed_said = true;
            if (!a->cycle_silent) {
                armed_words(a, st->level, st->arm_flags, &b->ack, now);
            }
        }
    } else if (prev->state == ALARM_STATE_EXIT_DELAY && st->state == ALARM_STATE_ARMED &&
               a->cycle && !a->cycle_armed_said) {
        /* Exit time ended: the actual mode (auto-stay's Home too). An entry delay that ends is
         * not EXIT -> ARMED and never says Armed. */
        a->cycle_armed_said = true;
        if (!a->cycle_silent) {
            armed_words(a, st->level, st->arm_flags, &none, now);
        }
    }
    if (st->state == ALARM_STATE_DISARMED) {
        a->cycle = false;
    }

    if (b->disarmed && st->state == ALARM_STATE_DISARMED &&
        prev->state != ALARM_STATE_DISARMED) {
        a->action_gen++;
        const uint16_t c[1] = {VP_ID_DISARMED};
        want(a, ANN_KIND_DISARMED, c, 1u, 1u, &b->ack, false, now);
    }
    if (b->entry && st->state == ALARM_STATE_ENTRY_DELAY) {
        a->action_gen++;
        const uint16_t c[1] = {VP_ID_ENTRY};
        want(a, ANN_KIND_ENTRY, c, 1u, 1u, &none, false, now);
    }
    if (b->not_ready) {
        const uint16_t c[1] = {VP_ID_NOT_READY};
        want(a, ANN_KIND_NOT_READY, c, 1u, 1u, &b->error, true, now);
    }
    if (b->walk_start && st->walk_test) {
        ann_req_t r;
        req_start(a, &r, ANN_KIND_WALK_START, 0u, now);
        const uint16_t c[2] = {VP_ID_WALK_TEST, VP_ID_WALK_STARTED};
        if (auto_allowed(a, ANN_KIND_WALK_START) && attach(&r, &b->ack, false) &&
            build(a, &r.utt, c, 2u, 2u) == BUILD_OK) { /* "Walk test started." */
            r.wait_bell = b->bell_test && !b->bell_done;
            (void)enqueue(a, &r);
        }
    }
    if (b->walk_end && !st->walk_test) {
        const uint16_t c[3] = {VP_ID_WALK_TEST, VP_ID_WALK_ENDED,
                               (st->walk_tested >= st->walk_total) ? VP_ID_WALK_ALL_TESTED
                                                                   : VP_ID_WALK_SOME_UNTESTED};
        want(a, ANN_KIND_WALK_END, c, 3u, 2u, &b->ack, false, now); /* "Walk test ended." ... */
    }
    if (b->walk_warn5 && st->walk_test) {
        const uint16_t c[2] = {VP_ID_WALK_TEST, VP_ID_WALK_ENDS_SOON};
        want(a, ANN_KIND_WALK_WARNING, c, 2u, 1u, &b->warning, true, now); /* two sentences */
    }
    for (unsigned i = 0u; i < b->zone_sounds; i++) {
        want_zone(a, &b->zone_sound[i], now);
    }
}

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

void ann_init(ann_t *a, const ann_ports_t *ports, uint8_t keypad_partition)
{
    memset(a, 0, sizeof(*a));
    if (ports != NULL) {
        a->ports = *ports;
    }
    a->keypad = keypad_partition;
    /* D06's factory preferences: Announcements on (decided 2026-10-12); nothing is spoken
     * until a voice is ready. */
    a->prefs.voice_enabled = true;
    a->prefs.voice_status = true;
    a->prefs.voice_chime = true;
}

void ann_cancel_all(ann_t *a)
{
    for (unsigned i = 0u; i < ANN_WAITING_MAX; i++) {
        a->waiting[i].used = false;
    }
    a->active_used = false;
    a->generation++; /* always: the mixer drops whatever it still holds */
}

void ann_rebaseline(ann_t *a)
{
    ann_cancel_all(a);
    a->have_baseline = false;
    a->cycle = false;
    memset(&a->batch, 0, sizeof(a->batch));
}

void ann_set_keypad(ann_t *a, uint8_t keypad_partition)
{
    a->keypad = keypad_partition;
    ann_rebaseline(a); /* the old partition's state and cycle mean nothing for the new one */
}

void ann_set_prefs(ann_t *a, const ann_prefs_t *prefs, uint64_t now_ms)
{
    a->prefs = *prefs;
    sweep(a);
    reconcile(a, now_ms);
}

void ann_set_pack(ann_t *a, const ann_pack_t *pack, uint64_t now_ms)
{
    const bool changed = pack->ready != a->pack.ready || pack->revision != a->pack.revision;
    a->pack = *pack;
    if (changed) {
        ann_cancel_all(a); /* every request was built from the old pack's clips */
    }
    sweep(a);
    reconcile(a, now_ms);
}

static void tone_set(ann_tone_t *t, uint32_t ticket)
{
    t->present = true;
    t->ticket = ticket;
    t->outcome = TONE_PENDING;
}

void ann_event(ann_t *a, const alarm_event_t *ev)
{
    if (ev == NULL || ev->partition != a->keypad) {
        return; /* only the keypad's partition speaks (D05) */
    }
    ann_batch_t *b = &a->batch;
    switch (ev->type) {
    case ALARM_EV_ARMED:
        b->armed = true;
        b->armed_level = ev->arg;
        b->armed_flags = (uint8_t)(ev->arg2 & 0x07u);
        break;
    case ALARM_EV_ARM_FAILED:
        if (ev->arg == (uint8_t)ALARM_ERR_NOT_READY) {
            b->not_ready = true;
        }
        break;
    case ALARM_EV_DISARMED:
        b->disarmed = true;
        break;
    case ALARM_EV_ENTRY_DELAY:
        b->entry = true;
        break;
    case ALARM_EV_WALK_TEST_START:
        b->walk_start = true;
        break;
    case ALARM_EV_WALK_TEST_END:
        b->walk_end = true;
        break;
    case ALARM_EV_WALK_TEST_EXTEND:
        b->walk_extend = true;
        break;
    case ALARM_EV_WALK_TEST_WARNING:
        if (ev->arg == 5u) {
            b->walk_warn5 = true; /* the one-minute warning stays a double beep (WALK-016) */
        }
        break;
    default:
        /* ALARM_EV_DURESS and every silent or hidden event change nothing audible (AUD-010). */
        break;
    }
}

void ann_sound(ann_t *a, uint8_t partition, alarm_sound_t sound, uint16_t zone, uint32_t ticket)
{
    if (partition != a->keypad && partition != 0u) {
        return;
    }
    ann_batch_t *b = &a->batch;
    switch (sound) {
    case ALARM_SOUND_ACK:
        tone_set(&b->ack, ticket);
        break;
    case ALARM_SOUND_ERROR:
        tone_set(&b->error, ticket);
        break;
    case ALARM_SOUND_WALK_TEST_WARNING:
        tone_set(&b->warning, ticket);
        break;
    case ALARM_SOUND_CHIME:
    case ALARM_SOUND_WALK_TEST:
        if (zone != 0u && b->zone_sounds < ANN_ZONE_SOUNDS_MAX) {
            ann_zone_sound_t *zs = &b->zone_sound[b->zone_sounds++];
            zs->zone = zone;
            zs->kind = (uint8_t)((sound == ALARM_SOUND_CHIME) ? ANN_KIND_CHIME
                                                                : ANN_KIND_WALK_TRIP);
            tone_set(&zs->tone, ticket);
        }
        break;
    default:
        break;
    }
}

void ann_output(ann_t *a, alarm_output_t output, uint8_t partition, alarm_pattern_t pattern)
{
    if (output == ALARM_OUT_BELL && partition == a->keypad && pattern == ALARM_PAT_TEST) {
        a->batch.bell_test = true;
        a->batch.bell_done = false;
    }
}

void ann_commit(ann_t *a, const ann_state_t *st, uint64_t now_ms)
{
    if (!a->have_baseline) {
        /* The silent baseline: whatever the state is, it isn't news. */
        a->st = *st;
        a->have_baseline = true;
        a->cycle = false;
        memset(&a->batch, 0, sizeof(a->batch));
        return;
    }
    const ann_state_t prev = a->st;
    a->st = *st;
    const ann_batch_t *b = &a->batch;
    if (b->walk_start || b->walk_end) {
        a->walk_gen++;
    }
    if (b->walk_start || b->walk_extend) {
        a->warn_gen++;
    }
    if (st->loud) {
        /* AUD-010: an audible fire, CO, burglary or aux pattern anywhere cancels all speech at
         * once, and nothing new is queued while it sounds. */
        ann_cancel_all(a);
    }
    decide(a, &prev, now_ms);
    memset(&a->batch, 0, sizeof(a->batch));
    sweep(a);
    reconcile(a, now_ms);
}

static void tone_outcome(ann_tone_t *t, uint32_t ticket, bool played)
{
    if (t->present && t->ticket == ticket) {
        t->outcome = played ? TONE_PLAYED : TONE_CANCELED;
    }
}

void ann_ticket(ann_t *a, uint32_t ticket, bool played, uint64_t now_ms)
{
    if (ticket == 0u) {
        return;
    }
    ann_batch_t *b = &a->batch;
    tone_outcome(&b->ack, ticket, played);
    tone_outcome(&b->error, ticket, played);
    tone_outcome(&b->warning, ticket, played);
    for (unsigned i = 0u; i < b->zone_sounds; i++) {
        tone_outcome(&b->zone_sound[i].tone, ticket, played);
    }
    for (unsigned i = 0u; i < ANN_WAITING_MAX; i++) {
        ann_req_t *r = &a->waiting[i];
        if (r->used && r->tone_ticket == ticket) {
            if (played) {
                r->tone_ticket = 0u;
            } else {
                r->used = false; /* a canceled tone cancels its speech (D04) */
            }
        }
    }
    reconcile(a, now_ms);
}

void ann_bell_test_done(ann_t *a, uint64_t now_ms)
{
    a->batch.bell_done = true;
    for (unsigned i = 0u; i < ANN_WAITING_MAX; i++) {
        a->waiting[i].wait_bell = false;
    }
    reconcile(a, now_ms);
}

ann_preview_result_t ann_preview(ann_t *a, const uint16_t *ids, size_t n, bool parts,
                                 uint64_t now_ms)
{
    if (ids == NULL || n == 0u) {
        return ANN_PREVIEW_EMPTY;
    }
    if (n > ANN_CLIPS_MAX) {
        return ANN_PREVIEW_TOO_MANY;
    }
    if (!a->pack.ready) {
        return ANN_PREVIEW_NO_PACK;
    }
    if (a->prefs.volume == 0u || a->prefs.power_saving) {
        return ANN_PREVIEW_MUTED;
    }
    if (a->st.loud) {
        return ANN_PREVIEW_ALARM;
    }
    ann_req_t r;
    req_start(a, &r, ANN_KIND_PREVIEW, 0u, now_ms);
    switch (build(a, &r.utt, ids, n, parts ? 1u : n)) {
    case BUILD_OK:
        break;
    case BUILD_TOO_LONG:
        return ANN_PREVIEW_TOO_LONG;
    default:
        return ANN_PREVIEW_BAD_CLIP;
    }
    if (a->active_used && a->active.utt.kind == (uint8_t)ANN_KIND_PREVIEW) {
        cancel_active(a); /* a new Preview replaces the one playing */
    }
    if (!enqueue(a, &r)) {
        return ANN_PREVIEW_BUSY;
    }
    return ANN_PREVIEW_OK;
}

bool ann_next(ann_t *a, uint64_t now_ms, ann_utt_t *out)
{
    for (unsigned i = 0u; i < ANN_WAITING_MAX; i++) {
        if (a->waiting[i].used && now_ms >= a->waiting[i].deadline) {
            a->waiting[i].used = false; /* too late to be worth saying (AUD-012) */
        }
    }
    reconcile(a, now_ms);
    if (a->active_used) {
        return false;
    }
    ann_req_t *best = NULL;
    for (unsigned i = 0u; i < ANN_WAITING_MAX; i++) {
        ann_req_t *r = &a->waiting[i];
        if (!r->used || !ready(r)) {
            continue;
        }
        if (best == NULL || r->utt.priority > best->utt.priority ||
            (r->utt.priority == best->utt.priority && r->order < best->order)) {
            best = r;
        }
    }
    if (best == NULL) {
        return false;
    }
    a->active = *best;
    a->active.utt.generation = a->generation;
    a->active_used = true;
    best->used = false;
    if (out != NULL) {
        *out = a->active.utt;
    }
    return true;
}

bool ann_current(const ann_t *a, uint32_t id)
{
    return a->active_used && a->active.utt.id == id && id != 0u;
}

void ann_done(ann_t *a, uint32_t id, uint64_t now_ms)
{
    (void)now_ms;
    if (ann_current(a, id)) {
        a->active_used = false;
    }
}

uint32_t ann_generation(const ann_t *a)
{
    return a->generation;
}

bool ann_due(const ann_t *a, uint64_t now_ms)
{
    if (a->active_used) {
        return true;
    }
    for (unsigned i = 0u; i < ANN_WAITING_MAX; i++) {
        if (a->waiting[i].used && now_ms < a->waiting[i].deadline) {
            return true;
        }
    }
    return false;
}

size_t ann_waiting(const ann_t *a)
{
    size_t n = 0u;
    for (unsigned i = 0u; i < ANN_WAITING_MAX; i++) {
        if (a->waiting[i].used) {
            n++;
        }
    }
    return n;
}
