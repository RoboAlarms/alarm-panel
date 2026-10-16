/*
 * annunciator.h - what the panel says, and when: the voice's request policy (docs/features/08
 * voice-first-build.md D04 "The chime", D05 "What the panel says, and when", D06 "Settings", D11
 * "Playback"; docs/features/40 WALK-016).
 *
 * The policy turns the engine's facts into utterances - bounded lists of clip IDs with the
 * silence before each - and decides which one plays: one active and four waiting, by priority,
 * with coalescing, expiry and cancellation. It decodes nothing and owns no audio: the platform's
 * voice worker asks ann_next() for the next utterance, decodes its clips, feeds the sounder's mix
 * and reports ann_done(). It never touches the engine and keeps no pointer into it.
 *
 * How the platform feeds it, on the thread that owns it (one engine call = one batch):
 *   ann_event() for each event, ann_sound() for each one-shot (with the sounder ticket
 *   sounder_play_tracked() returned, 0 when the tone was not admitted), ann_output() for the bell,
 *   then ann_commit() with the state after the call (ann_state_from_snapshot()).
 *   Later: ann_ticket() as each tracked tone ends (PLAYED or CANCELED), ann_bell_test_done() when
 *   the bell test pulse has ended. The first commit after ann_init() or ann_rebaseline() is a
 *   silent baseline: no speech at start-up, restore, reconnection or history replay.
 *
 * Plain C11, portable (components/core rules): no heap, no globals, time comes in as ms.
 */
#ifndef ANNUNCIATOR_ANNUNCIATOR_H
#define ANNUNCIATOR_ANNUNCIATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_snapshot.h"
#include "alarm/alarm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Limits (D02, D11) ---- */
#define ANN_CLIPS_MAX 8u               /* clips in one utterance (a spoken name: 8 entries) */
#define ANN_WAITING_MAX 4u             /* waiting requests beside the active one */
#define ANN_SAMPLE_RATE 16000u
#define ANN_SAMPLES_MAX 160000u        /* 10 s at 16 kHz, gaps included */
#define ANN_WORD_GAP_MS 20u            /* between words of one phrase (when the pack gives none) */
#define ANN_PART_GAP_MS 300u           /* before a new sentence (when the pack gives none) */
#define ANN_EXPIRE_SHORT_MS 2000u      /* chime, walk trip, not-ready refusal */
#define ANN_EXPIRE_LONG_MS 5000u       /* status, walk control, Preview */
#define ANN_ZONE_SOUNDS_MAX 4u         /* chime / trip tones remembered in one batch */

/* What an utterance says. The order is not a priority (ann_priority()). */
typedef enum {
    ANN_KIND_NONE = 0,
    ANN_KIND_ENTRY,        /* status.entry */
    ANN_KIND_ARMING,       /* status.arming + mode + status.exit_now */
    ANN_KIND_ARMED,        /* status.armed + mode [+ status.no_entry_delay] */
    ANN_KIND_DISARMED,     /* status.disarmed */
    ANN_KIND_WALK_START,   /* walk.walk_test + walk.started */
    ANN_KIND_WALK_END,     /* walk.walk_test + walk.ended + all_tested / some_untested */
    ANN_KIND_WALK_WARNING, /* walk.walk_test + walk.ends_soon (the 5-minute warning) */
    ANN_KIND_NOT_READY,    /* status.not_ready */
    ANN_KIND_WALK_TRIP,    /* the zone's spoken name */
    ANN_KIND_CHIME,        /* the zone's spoken name */
    ANN_KIND_PREVIEW,      /* an explicit Preview */
} ann_kind_t;

/* Priorities, highest first (D11): entry > state and walk control > refusal and walk trip >
 * chime > Preview. */
typedef enum {
    ANN_PRIO_PREVIEW = 0,
    ANN_PRIO_CHIME = 1,
    ANN_PRIO_REFUSAL = 2,
    ANN_PRIO_STATE = 3,
    ANN_PRIO_ENTRY = 4,
} ann_priority_t;

/* One utterance, as handed to the voice worker. gap_ms[i] is the silence before clips[i]: the
 * word gap inside a phrase ("Armed" "Away", a zone's words), the part gap before a new sentence
 * ("No entry delay."); gap_ms[0] is 0. total_samples (clips + gaps, 16 kHz) never exceeds ANN_SAMPLES_MAX. */
typedef struct {
    uint32_t id;           /* nonzero, never reused while the policy lives */
    uint32_t generation;   /* ann_generation() when it was handed out */
    uint8_t kind;          /* ann_kind_t */
    uint8_t priority;      /* ann_priority_t */
    uint8_t count;         /* 1..ANN_CLIPS_MAX */
    uint16_t zone;         /* chime / trip: the zone, else 0 */
    uint16_t clips[ANN_CLIPS_MAX];
    uint16_t gap_ms[ANN_CLIPS_MAX];
    uint32_t total_samples;
} ann_utt_t;

/* A zone's spoken name, as the configuration stores it (D07), in the policy's own words. */
typedef struct {
    uint8_t count;                  /* entries, 0 = no spoken name */
    uint16_t ids[ANN_CLIPS_MAX];    /* selectable token IDs (0x0001-0x7FFF) */
    uint32_t binding_revision;      /* the zone's current binding revision, 0 = none */
    uint32_t approved_revision;     /* the binding revision approval recorded, 0 = not approved */
    uint32_t pack_revision;         /* the pack revision approval recorded */
} ann_zone_words_t;

/* What the policy asks of the platform. Neither callback may call back into the policy. */
typedef struct {
    void *ctx;
    /* A zone's spoken name; false when the zone has none (or isn't configured). */
    bool (*zone_words)(void *ctx, uint16_t zone, ann_zone_words_t *out);
    /* A clip's valid samples at 16 kHz in the active pack, 0 when it has no such clip.
     * ann_pack_clip_samples() answers it from a vp_pack_t. */
    uint32_t (*clip_samples)(void *ctx, uint16_t id);
} ann_ports_t;

/* The keypad's announcement preferences (D06) and the existing sound policy. The walk test has no
 * switch: it speaks whenever a voice is ready, Announcements off too (WALK-016). */
typedef struct {
    bool voice_enabled;   /* Announcements (default true, D06) */
    bool voice_status;    /* Status messages (default true) */
    bool voice_chime;     /* Zone chime (default true) */
    uint8_t volume;       /* the user volume; 0 = no speech at all */
    bool power_saving;    /* no speech while saving power */
} ann_prefs_t;

/* The active voice pack. */
typedef struct {
    bool ready;              /* signed, opened, decoder resources and mandatory clips valid */
    uint32_t revision;       /* its pack_revision */
    uint16_t word_gap_ms;    /* 0 = ANN_WORD_GAP_MS */
    uint16_t part_gap_ms;    /* 0 = ANN_PART_GAP_MS */
} ann_pack_t;

/* What the policy reads of the state after an engine call. */
typedef struct {
    uint8_t state;           /* alarm_arm_state_t of the keypad partition */
    uint8_t level;           /* alarm_arm_level_t */
    uint8_t arm_flags;       /* ALARM_ARM_* */
    bool quick_exit;         /* the exit time is a quick exit, not an arming */
    bool exit_sounds;        /* the configuration's exit_sounds option */
    bool walk_test;          /* a walk test runs on the keypad partition */
    uint16_t walk_tested;
    uint16_t walk_total;
    bool loud;               /* any enabled partition sounds fire, CO, burglary or aux */
    uint32_t config_seq;     /* the configuration's generation */
} ann_state_t;

/* Fills *out from a snapshot: the keypad partition's facts and the loudness of every partition. */
void ann_state_from_snapshot(const alarm_snapshot_t *s, uint8_t keypad_partition,
                             bool exit_sounds, ann_state_t *out);

/* ann_ports_t.clip_samples over a pack vp_open() accepted: ctx is a `const vp_pack_t *`. */
uint32_t ann_pack_clip_samples(void *ctx, uint16_t id);

/* Why a Preview was refused. */
typedef enum {
    ANN_PREVIEW_OK = 0,
    ANN_PREVIEW_NO_PACK,     /* no valid pack */
    ANN_PREVIEW_MUTED,       /* volume 0 or power saving */
    ANN_PREVIEW_ALARM,       /* a loud pattern sounds */
    ANN_PREVIEW_EMPTY,       /* no clips */
    ANN_PREVIEW_TOO_MANY,    /* more than ANN_CLIPS_MAX */
    ANN_PREVIEW_BAD_CLIP,    /* a clip the pack doesn't have */
    ANN_PREVIEW_TOO_LONG,    /* over 10 s with its gaps */
    ANN_PREVIEW_BUSY,        /* four more important requests wait */
} ann_preview_result_t;

/* The waiting request's internal record (in the open so ann_t can live on the caller's side). */
typedef struct {
    ann_utt_t utt;
    uint64_t deadline;       /* dropped if not started by then */
    uint32_t order;          /* first-come order among equal priorities */
    uint32_t tone_ticket;    /* the tone it follows, 0 = none or already played */
    uint32_t walk_gen;       /* walk test it belongs to */
    uint32_t warn_gen;       /* warning generation (extensions) */
    uint32_t config_seq;     /* configuration it was built from */
    uint32_t action_gen;     /* state action it was decided after (status kinds) */
    bool wait_bell;          /* walk start: waits for the bell test pulse */
    bool used;
} ann_req_t;

/* One engine call's facts, gathered until ann_commit(). */
typedef struct {
    uint32_t ticket;         /* 0 = the tone was not admitted */
    uint8_t outcome;         /* 0 = pending, 1 = played, 2 = canceled */
    bool present;
} ann_tone_t;

typedef struct {
    uint16_t zone;
    uint8_t kind;            /* ANN_KIND_CHIME or ANN_KIND_WALK_TRIP */
    ann_tone_t tone;
} ann_zone_sound_t;

typedef struct {
    bool armed;              /* ALARM_EV_ARMED */
    uint8_t armed_level;
    uint8_t armed_flags;
    bool disarmed;
    bool entry;
    bool not_ready;          /* ALARM_EV_ARM_FAILED with ALARM_ERR_NOT_READY */
    bool walk_start;
    bool walk_end;
    bool walk_extend;
    bool walk_warn5;
    bool bell_test;          /* ALARM_PAT_TEST on the keypad partition's bell */
    bool bell_done;
    ann_tone_t ack;
    ann_tone_t error;
    ann_tone_t warning;
    uint8_t zone_sounds;
    ann_zone_sound_t zone_sound[ANN_ZONE_SOUNDS_MAX];
} ann_batch_t;

/* The policy. Caller-owned; all state lives here. */
typedef struct {
    ann_ports_t ports;
    uint8_t keypad;          /* the partition it speaks for (1; a link keypad's home) */
    ann_prefs_t prefs;
    ann_pack_t pack;
    ann_state_t st;          /* the state after the last commit */
    bool have_baseline;
    /* The arming cycle the panel knows of (started by ALARM_EV_ARMED since the baseline). */
    bool cycle;
    bool cycle_silent;       /* silent exit, latched at acceptance */
    bool cycle_armed_said;
    bool join_exit;          /* a baseline in the exit time joins its cycle (a link keypad) */
    uint32_t walk_gen;
    uint32_t warn_gen;
    /* Bumped before deciding each accepted arm, state-changing disarm and entry start: status
     * speech from an earlier action never outlives a later one (D11). */
    uint32_t action_gen;
    uint32_t next_id;
    uint32_t next_order;
    uint32_t generation;     /* bumped whenever the active utterance is canceled */
    bool active_used;
    ann_req_t active;
    ann_req_t waiting[ANN_WAITING_MAX];
    ann_batch_t batch;
} ann_t;

/* Starts the policy with the factory preferences (voice off), no pack and no baseline. */
void ann_init(ann_t *a, const ann_ports_t *ports, uint8_t keypad_partition);

/* The next commit is a silent baseline again (restore, reconnection). Cancels all speech. */
void ann_rebaseline(ann_t *a);

/* The partition it speaks for (D05: an alarm panel's keypad partition; a link keypad's home,
 * which its master may move): set, then a rebaseline - the next commit is silent, and nothing of
 * the old partition is said after it. */
void ann_set_keypad(ann_t *a, uint8_t keypad_partition);

/* A link keypad (docs/features/11 LINK-028): its baseline comes whenever its link does, often in
 * the middle of an arming its master accepted - so a baseline taken in an arming's exit time
 * joins that cycle, and its end says the armed mode as the master's voice does; so does a move
 * from disarmed into an exit time whose arm it didn't hear (its event came too late to say). The
 * silent exit is latched from the state's flags (auto-stay clears it only at the end). Nothing of
 * the arming itself is said, and a quick exit's time (no arming) is never joined. Off on an alarm
 * panel: a restored exit time that ends is no known cycle (D05). */
void ann_join_exit(ann_t *a, bool on);

/* Preferences and pack. A change that forbids speech cancels it (AUD-015); a new or lost pack
 * cancels everything (its generation). */
void ann_set_prefs(ann_t *a, const ann_prefs_t *prefs, uint64_t now_ms);
void ann_set_pack(ann_t *a, const ann_pack_t *pack, uint64_t now_ms);

/* One engine call's facts. ticket: sounder_play_tracked()'s ticket for the tone, 0 when the
 * tone was not admitted. Partitions other than the keypad's are ignored (0 = all keypads, for
 * sounds). */
void ann_event(ann_t *a, const alarm_event_t *ev);
void ann_sound(ann_t *a, uint8_t partition, alarm_sound_t sound, uint16_t zone, uint32_t ticket);
void ann_output(ann_t *a, alarm_output_t output, uint8_t partition, alarm_pattern_t pattern);

/* Ends the batch with the state after the engine call; decides what to say. */
void ann_commit(ann_t *a, const ann_state_t *st, uint64_t now_ms);

/* A tracked tone ended: played to its last block, or canceled / dropped (its speech too). */
void ann_ticket(ann_t *a, uint32_t ticket, bool played, uint64_t now_ms);

/* The bell test pulse (ALARM_PAT_TEST) has ended: a waiting walk start may speak. */
void ann_bell_test_done(ann_t *a, uint64_t now_ms);

/* An explicit Preview (D03, D06): plays with the voice off, never changes preferences, but
 * respects the pack, the volume, power saving and alarms. parts: 300 ms gaps (status and walk
 * clips) instead of the word gap. Replaces an earlier Preview. */
ann_preview_result_t ann_preview(ann_t *a, const uint16_t *ids, size_t n, bool parts,
                                 uint64_t now_ms);

/* The next utterance to play: drops expired requests, and when none is active hands out the
 * highest-priority ready one (it becomes active). False when nothing may start now. */
bool ann_next(ann_t *a, uint64_t now_ms, ann_utt_t *out);

/* True while utterance `id` is the active one: the worker checks it at each block. */
bool ann_current(const ann_t *a, uint32_t id);

/* The worker finished the active utterance (played, underrun, decode failure, or it saw it
 * canceled). Unknown ids are ignored. */
void ann_done(ann_t *a, uint32_t id, uint64_t now_ms);

/* Cancels all speech now (the generation moves). */
void ann_cancel_all(ann_t *a);

/* The speech generation: an utterance whose generation differs is canceled. The mixer keeps a
 * copy to reject stale speech without asking the policy. */
uint32_t ann_generation(const ann_t *a);

/* Requests waiting (not counting the active one). */
size_t ann_waiting(const ann_t *a);

/* Speech is on its way: an utterance is active, or a request waits that may still start (before
 * its deadline; one waiting for its tone or the bell pulse counts). The sounder holds the exit
 * beeps while it is (AUD-034), so the arming words come before them. */
bool ann_due(const ann_t *a, uint64_t now_ms);

/* The priority of a kind. */
ann_priority_t ann_priority(ann_kind_t kind);

#ifdef __cplusplus
}
#endif

#endif /* ANNUNCIATOR_ANNUNCIATOR_H */
