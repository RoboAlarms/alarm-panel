/*
 * test_annunciator.c - the voice's request policy (docs/features/08 voice-first-build.md D04,
 * D05, D06, D11; docs/features/40 WALK-016). Tags: AUD-008 (chime), AUD-010 (alarms, duress,
 * silent facts), AUD-012 (the queue), AUD-015 (preferences, Preview), AUD-031 (a name's last word
 * as its ending), WALK-016 (walk speech), LINK-028 (a link keypad's partition moved).
 */
#include <string.h>

#include "unity.h"

#include "annunciator/annunciator.h"
#include "voice_pack/voice_pack.h"

#include "../../voice_pack/test/voice_pack_fixture_v1.h"

/* ---- A fake platform ---- */
#define ZONES 8u
#define PACK_REV 3u
#define CLIP 8000u /* 0.5 s */

typedef struct {
    ann_zone_words_t words[ZONES + 1u];
    uint16_t missing;     /* a clip the pack lacks */
    uint16_t big_id;      /* a clip of big_samples */
    uint32_t big_samples;
    bool endings;         /* the pack has every word's ending (schema 2) */
    unsigned zone_calls;
} fake_t;

static fake_t F;
static ann_t A;
static uint64_t T;

static bool f_words(void *ctx, uint16_t zone, ann_zone_words_t *out)
{
    (void)ctx;
    F.zone_calls++;
    if (zone == 0u || zone > ZONES || F.words[zone].count == 0u) {
        return false;
    }
    *out = F.words[zone];
    return true;
}

static uint32_t f_clip(void *ctx, uint16_t id)
{
    (void)ctx;
    if (id == F.missing) {
        return 0u;
    }
    if (id == F.big_id) {
        return F.big_samples;
    }
    if (id >= VP_ENDING_FIRST && !F.endings) {
        return 0u;
    }
    return CLIP;
}

static void name_zone(uint16_t zone, const uint16_t *ids, uint8_t n)
{
    ann_zone_words_t *w = &F.words[zone];
    memset(w, 0, sizeof(*w));
    w->count = n;
    memcpy(w->ids, ids, n * sizeof(ids[0]));
    w->binding_revision = 10u + zone;
    w->approved_revision = 10u + zone;
    w->pack_revision = PACK_REV;
}

static ann_state_t S(uint8_t state, uint8_t level, uint8_t flags)
{
    ann_state_t s;
    memset(&s, 0, sizeof(s));
    s.state = state;
    s.level = level;
    s.arm_flags = flags;
    s.exit_sounds = true;
    s.config_seq = 1u;
    return s;
}

static ann_state_t DISARMED_ST(void)
{
    return S(ALARM_STATE_DISARMED, ALARM_LEVEL_NONE, 0u);
}

static ann_state_t WALK_ST(uint16_t tested, uint16_t total)
{
    ann_state_t s = DISARMED_ST();
    s.walk_test = true;
    s.walk_tested = tested;
    s.walk_total = total;
    return s;
}

static ann_prefs_t voice_on(void)
{
    ann_prefs_t p = {true, true, true, 5u, false};
    return p;
}

static void ev(alarm_event_type_t type, uint8_t partition, uint8_t arg, uint8_t arg2)
{
    alarm_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    e.partition = partition;
    e.arg = arg;
    e.arg2 = arg2;
    ann_event(&A, &e);
}

static void commit(ann_state_t s)
{
    ann_commit(&A, &s, T);
}

/* A fresh policy: voice on, a ready pack, zones 1 ("Front Door") and 2 ("Basement") named, and the
 * silent baseline taken with the given state. */
static void start(ann_state_t baseline)
{
    memset(&F, 0, sizeof(F));
    const uint16_t front_door[2] = {1u, 109u};
    const uint16_t basement[1] = {32u};
    name_zone(1u, front_door, 2u);
    name_zone(2u, basement, 1u);
    ann_ports_t ports = {NULL, f_words, f_clip};
    ann_init(&A, &ports, 1u);
    T = 1000u;
    ann_prefs_t p = voice_on();
    ann_set_prefs(&A, &p, T);
    ann_pack_t pack = {true, PACK_REV, VP_WORD_GAP_MS, VP_PART_GAP_MS};
    ann_set_pack(&A, &pack, T);
    commit(baseline);
}

static bool next(ann_utt_t *u)
{
    return ann_next(&A, T, u);
}

static void expect_nothing(void)
{
    ann_utt_t u;
    TEST_ASSERT_FALSE(next(&u));
}

/* The first `phrase` clips joined by the word gap, each later one after the part gap. */
static ann_utt_t expect_phrase(ann_kind_t kind, const uint16_t *clips, uint8_t n, uint8_t phrase)
{
    ann_utt_t u;
    TEST_ASSERT_TRUE_MESSAGE(next(&u), "an utterance should be ready");
    TEST_ASSERT_EQUAL_UINT8(kind, u.kind);
    TEST_ASSERT_EQUAL_UINT8(n, u.count);
    TEST_ASSERT_EQUAL_HEX16_ARRAY(clips, u.clips, n);
    uint32_t gaps = 0u;
    for (uint8_t i = 0u; i < n; i++) {
        const uint16_t gap = (i == 0u) ? 0u : (i < phrase) ? VP_WORD_GAP_MS : VP_PART_GAP_MS; /* the pack's */
        TEST_ASSERT_EQUAL_UINT16(gap, u.gap_ms[i]);
        gaps += gap * 16u;
    }
    TEST_ASSERT_EQUAL_UINT32(n * CLIP + gaps, u.total_samples);
    TEST_ASSERT_TRUE(ann_current(&A, u.id));
    TEST_ASSERT_EQUAL_UINT32(ann_generation(&A), u.generation);
    return u;
}

static ann_utt_t expect(ann_kind_t kind, const uint16_t *clips, uint8_t n, uint16_t gap)
{
    ann_utt_t u;
    TEST_ASSERT_TRUE_MESSAGE(next(&u), "an utterance should be ready");
    TEST_ASSERT_EQUAL_UINT8(kind, u.kind);
    TEST_ASSERT_EQUAL_UINT8(n, u.count);
    TEST_ASSERT_EQUAL_HEX16_ARRAY(clips, u.clips, n);
    for (uint8_t i = 0u; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT16((i == 0u) ? 0u : gap, u.gap_ms[i]);
    }
    TEST_ASSERT_EQUAL_UINT32(n * CLIP + (n - 1u) * gap * 16u, u.total_samples);
    TEST_ASSERT_TRUE(ann_current(&A, u.id));
    TEST_ASSERT_EQUAL_UINT32(ann_generation(&A), u.generation);
    return u;
}

static void finish(const ann_utt_t *u)
{
    ann_done(&A, u->id, T);
    TEST_ASSERT_FALSE(ann_current(&A, u->id));
}

static void play(const ann_utt_t *u)
{
    finish(u);
}

static const uint16_t FRONT_DOOR[2] = {1u, 109u};

void setUp(void)
{
}

void tearDown(void)
{
}

/* ======================================================================== */
/* The chime (D04)                                                           */
/* ======================================================================== */

/* AUD-008: the ding-dong first; the name only after the tone's PLAYED, the word gap between words. */
static void test_chime_name_after_tone(void)
{
    start(DISARMED_ST());
    ann_sound(&A, 1u, ALARM_SOUND_CHIME, 1u, 7u);
    commit(DISARMED_ST());
    expect_nothing(); /* the tone is still playing */
    T += 900u;
    ann_ticket(&A, 7u, true, T);
    ann_utt_t u = expect(ANN_KIND_CHIME, FRONT_DOOR, 2u, VP_WORD_GAP_MS);
    TEST_ASSERT_EQUAL_UINT16(1u, u.zone);
    TEST_ASSERT_EQUAL_UINT8(ANN_PRIO_CHIME, u.priority);
    play(&u);
    expect_nothing();
}

/* AUD-008: a canceled or dropped (not admitted) chime takes its name with it. */
static void test_chime_canceled_or_dropped_tone(void)
{
    start(DISARMED_ST());
    ann_sound(&A, 1u, ALARM_SOUND_CHIME, 1u, 7u);
    commit(DISARMED_ST());
    ann_ticket(&A, 7u, false, T);
    expect_nothing();
    TEST_ASSERT_EQUAL_size_t(0u, ann_waiting(&A));

    ann_sound(&A, 1u, ALARM_SOUND_CHIME, 1u, 0u); /* the sounder didn't admit it */
    commit(DISARMED_ST());
    expect_nothing();
    TEST_ASSERT_EQUAL_size_t(0u, ann_waiting(&A));

    /* the outcome may arrive before the commit */
    ann_sound(&A, 1u, ALARM_SOUND_CHIME, 1u, 8u);
    ann_ticket(&A, 8u, true, T);
    commit(DISARMED_ST());
    expect(ANN_KIND_CHIME, FRONT_DOOR, 2u, VP_WORD_GAP_MS);
}

/* AUD-008: only a current approved name speaks; otherwise the tone stays alone. */
static void test_chime_eligibility(void)
{
    const uint16_t big[1] = {50u};
    struct {
        const char *why;
        int change;
    } cases[] = {
        {"no spoken name", 0}, {"never approved", 1}, {"renamed: binding moved on", 2},
        {"approved on another pack", 3}, {"a token outside the namespace", 4},
        {"a clip the pack lacks", 5}, {"over ten seconds", 6}, {"zone chime switch off", 7},
        {"no pack", 8}, {"announcements off", 9},
    };
    for (unsigned i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        start(DISARMED_ST());
        ann_zone_words_t *w = &F.words[1];
        ann_prefs_t p = voice_on();
        switch (cases[i].change) {
        case 0: w->count = 0u; break;
        case 1: w->approved_revision = 0u; break;
        case 2: w->binding_revision = 99u; break;
        case 3: w->pack_revision = PACK_REV + 1u; break;
        case 4: w->ids[1] = VP_ID_ARMED; break;
        case 5: F.missing = 109u; break;
        case 6: name_zone(1u, big, 1u); F.big_id = 50u; F.big_samples = ANN_SAMPLES_MAX + 1u; break;
        case 7: p.voice_chime = false; ann_set_prefs(&A, &p, T); break;
        case 8: { ann_pack_t none = {false, 0u, 0u, 0u}; ann_set_pack(&A, &none, T); } break;
        case 9: p.voice_enabled = false; ann_set_prefs(&A, &p, T); break;
        default: break;
        }
        ann_sound(&A, 1u, ALARM_SOUND_CHIME, 1u, 7u);
        commit(DISARMED_ST());
        ann_ticket(&A, 7u, true, T);
        ann_utt_t u;
        TEST_ASSERT_FALSE_MESSAGE(next(&u), cases[i].why);
    }
}

/* AUD-008 / D02: a name of exactly ten seconds, joins included, speaks; one sample more doesn't. */
static void test_chime_ten_second_ceiling(void)
{
    const uint16_t ids[3] = {40u, 41u, 42u};
    /* 41 and 42 are CLIP each, two word joins (the pack's): 40 gets the rest */
    const uint32_t rest = ANN_SAMPLES_MAX - 2u * CLIP - 2u * VP_WORD_GAP_MS * 16u;
    for (unsigned extra = 0u; extra < 2u; extra++) {
        start(DISARMED_ST());
        name_zone(3u, ids, 3u);
        F.big_id = 40u;
        F.big_samples = rest + extra;
        ann_sound(&A, 1u, ALARM_SOUND_CHIME, 3u, 7u);
        commit(DISARMED_ST());
        ann_ticket(&A, 7u, true, T);
        ann_utt_t u;
        TEST_ASSERT_EQUAL(extra == 0u, next(&u));
        if (extra == 0u) {
            TEST_ASSERT_EQUAL_UINT32(ANN_SAMPLES_MAX, u.total_samples);
        }
    }
}

/* ======================================================================== */
/* The walk test (WALK-016)                                                  */
/* ======================================================================== */

/* WALK-016: the three beeps, then the name, on every trip - repeats too, without chime and with
 * Announcements off; a zone without words and volume 0 keep the beeps alone. */
static void test_walk_trip_name(void)
{
    start(WALK_ST(0u, 3u));
    ann_prefs_t p = voice_on();
    p.voice_chime = false; /* the trip's name doesn't need the zone chime */
    ann_set_prefs(&A, &p, T);
    for (unsigned rep = 0u; rep < 2u; rep++) {
        ann_sound(&A, 1u, ALARM_SOUND_WALK_TEST, 1u, 20u + rep);
        commit(WALK_ST(1u, 3u));
        expect_nothing();
        ann_ticket(&A, 20u + rep, true, T);
        ann_utt_t u = expect(ANN_KIND_WALK_TRIP, FRONT_DOOR, 2u, VP_WORD_GAP_MS);
        TEST_ASSERT_EQUAL_UINT8(ANN_PRIO_REFUSAL, u.priority);
        play(&u);
    }
    /* Announcements off: the walk test has no switch and still says the name */
    p.voice_enabled = false;
    ann_set_prefs(&A, &p, T);
    ann_sound(&A, 1u, ALARM_SOUND_WALK_TEST, 1u, 30u);
    commit(WALK_ST(1u, 3u));
    ann_ticket(&A, 30u, true, T);
    ann_utt_t u = expect(ANN_KIND_WALK_TRIP, FRONT_DOOR, 2u, VP_WORD_GAP_MS);
    play(&u);
    /* a zone without words: beeps only */
    ann_sound(&A, 1u, ALARM_SOUND_WALK_TEST, 3u, 31u);
    commit(WALK_ST(2u, 3u));
    ann_ticket(&A, 31u, true, T);
    expect_nothing();
    /* volume 0 still silences it */
    p.volume = 0u;
    ann_set_prefs(&A, &p, T);
    ann_sound(&A, 1u, ALARM_SOUND_WALK_TEST, 1u, 32u);
    commit(WALK_ST(2u, 3u));
    ann_ticket(&A, 32u, true, T);
    expect_nothing();
}

/* WALK-016: "Walk test started" after the acknowledgement and the bell test pulse. */
static void test_walk_start_waits_for_ack_and_bell(void)
{
    start(DISARMED_ST());
    ev(ALARM_EV_WALK_TEST_START, 1u, 0u, 0u);
    ann_output(&A, ALARM_OUT_BELL, 1u, ALARM_PAT_TEST);
    ann_sound(&A, 1u, ALARM_SOUND_ACK, 0u, 5u);
    commit(WALK_ST(0u, 3u));
    expect_nothing();
    ann_ticket(&A, 5u, true, T);
    expect_nothing(); /* the bell test pulse still runs */
    T += 500u;
    ann_bell_test_done(&A, T);
    const uint16_t c[2] = {VP_ID_WALK_TEST, VP_ID_WALK_STARTED};
    expect_phrase(ANN_KIND_WALK_START, c, 2u, 2u);
}

/* WALK-016: the end's summary: all tested (zero of zero too) or some not. */
static void test_walk_end_summary(void)
{
    const struct {
        uint16_t tested, total;
        uint16_t last;
    } cases[] = {{3u, 3u, VP_ID_WALK_ALL_TESTED},
                 {2u, 3u, VP_ID_WALK_SOME_UNTESTED},
                 {0u, 0u, VP_ID_WALK_ALL_TESTED}};
    for (unsigned i = 0u; i < 3u; i++) {
        start(WALK_ST(0u, cases[i].total));
        ev(ALARM_EV_WALK_TEST_END, 1u, (uint8_t)cases[i].tested, (uint8_t)cases[i].total);
        ann_sound(&A, 1u, ALARM_SOUND_ACK, 0u, 5u);
        ann_state_t s = DISARMED_ST();
        s.walk_tested = cases[i].tested;
        s.walk_total = cases[i].total;
        commit(s);
        ann_ticket(&A, 5u, true, T);
        const uint16_t c[3] = {VP_ID_WALK_TEST, VP_ID_WALK_ENDED, cases[i].last};
        expect_phrase(ANN_KIND_WALK_END, c, 3u, 2u);
    }
}

/* WALK-016: the end cancels waiting trip names; a disarm ending a test says no "Disarmed". */
static void test_walk_end_cancels_trips(void)
{
    start(WALK_ST(0u, 3u));
    ann_sound(&A, 1u, ALARM_SOUND_WALK_TEST, 1u, 20u);
    ann_sound(&A, 1u, ALARM_SOUND_WALK_TEST, 2u, 21u);
    commit(WALK_ST(2u, 3u));
    TEST_ASSERT_EQUAL_size_t(2u, ann_waiting(&A));
    /* code + OFF ends it: no ALARM_EV_DISARMED (already disarmed), the walk-end words */
    ev(ALARM_EV_WALK_TEST_END, 1u, 2u, 3u);
    ann_sound(&A, 1u, ALARM_SOUND_ACK, 0u, 22u);
    ann_state_t s = DISARMED_ST();
    s.walk_tested = 2u;
    s.walk_total = 3u;
    commit(s);
    TEST_ASSERT_EQUAL_size_t(1u, ann_waiting(&A));
    ann_ticket(&A, 20u, true, T);
    ann_ticket(&A, 21u, true, T);
    ann_ticket(&A, 22u, true, T);
    const uint16_t c[3] = {VP_ID_WALK_TEST, VP_ID_WALK_ENDED, VP_ID_WALK_SOME_UNTESTED};
    ann_utt_t u = expect_phrase(ANN_KIND_WALK_END, c, 3u, 2u);
    play(&u);
    expect_nothing();
}

/* WALK-016: five minutes left speaks after the double beep; one minute stays a double beep; an
 * extension makes a waiting warning stale. */
static void test_walk_warnings(void)
{
    start(WALK_ST(1u, 3u));
    ev(ALARM_EV_WALK_TEST_WARNING, 1u, 5u, 0u);
    ann_sound(&A, 1u, ALARM_SOUND_WALK_TEST_WARNING, 0u, 40u);
    commit(WALK_ST(1u, 3u));
    expect_nothing();
    ann_ticket(&A, 40u, true, T);
    const uint16_t c[2] = {VP_ID_WALK_TEST, VP_ID_WALK_ENDS_SOON};
    ann_utt_t u = expect(ANN_KIND_WALK_WARNING, c, 2u, 300u);
    play(&u);

    ev(ALARM_EV_WALK_TEST_WARNING, 1u, 1u, 0u);
    ann_sound(&A, 1u, ALARM_SOUND_WALK_TEST_WARNING, 0u, 41u);
    commit(WALK_ST(1u, 3u));
    ann_ticket(&A, 41u, true, T);
    expect_nothing();

    ev(ALARM_EV_WALK_TEST_WARNING, 1u, 5u, 0u);
    ann_sound(&A, 1u, ALARM_SOUND_WALK_TEST_WARNING, 0u, 42u);
    commit(WALK_ST(1u, 3u));
    ev(ALARM_EV_WALK_TEST_EXTEND, 1u, 0u, 0u);
    commit(WALK_ST(1u, 3u));
    ann_ticket(&A, 42u, true, T);
    expect_nothing();
}

/* ======================================================================== */
/* Arming and disarming (D05)                                                */
/* ======================================================================== */

static void arm(uint8_t level, uint8_t flags, uint32_t ack_ticket)
{
    ev(ALARM_EV_ARMED, 1u, level, flags);
    ann_sound(&A, 1u, ALARM_SOUND_ACK, 0u, ack_ticket);
}

/* AUD-015 (status messages): the accepted arm with exit time, after the acknowledgement, and the
 * actual armed state when exit time ends - for each mode. */
static void test_arming_then_armed_each_mode(void)
{
    const struct {
        uint8_t level;
        uint16_t mode;
    } modes[] = {{ALARM_LEVEL_AWAY, VP_ID_AWAY},
                 {ALARM_LEVEL_STAY, VP_ID_HOME},
                 {ALARM_LEVEL_NIGHT, VP_ID_NIGHT}};
    for (unsigned i = 0u; i < 3u; i++) {
        start(DISARMED_ST());
        arm(modes[i].level, 0u, 5u);
        commit(S(ALARM_STATE_EXIT_DELAY, modes[i].level, 0u));
        expect_nothing();
        ann_ticket(&A, 5u, true, T);
        const uint16_t arming[3] = {VP_ID_ARMING, modes[i].mode, VP_ID_EXIT_NOW};
        ann_utt_t u = expect_phrase(ANN_KIND_ARMING, arming, 3u, 2u);
        TEST_ASSERT_EQUAL_UINT8(ANN_PRIO_STATE, u.priority);
        play(&u);
        T += 30000u;
        commit(S(ALARM_STATE_ARMED, modes[i].level, 0u));
        const uint16_t armed[2] = {VP_ID_ARMED, modes[i].mode};
        u = expect_phrase(ANN_KIND_ARMED, armed, 2u, 2u);
        play(&u);
        /* an exit restart and its end don't say Armed twice in one cycle */
        commit(S(ALARM_STATE_EXIT_DELAY, modes[i].level, 0u));
        commit(S(ALARM_STATE_ARMED, modes[i].level, 0u));
        expect_nothing();
    }
}

/* AUD-034: speech is due from the moment the arming words are decided - while they wait for the
 * acknowledgement and while they play - and no longer once they end, so the sounder can hold the
 * exit beeps until then. A request nobody starts stops being due at its deadline. */
static void test_speech_is_due_from_the_arm_to_the_words_end(void)
{
    start(DISARMED_ST());
    TEST_ASSERT_FALSE(ann_due(&A, T));
    arm(ALARM_LEVEL_AWAY, 0u, 5u);
    commit(S(ALARM_STATE_EXIT_DELAY, ALARM_LEVEL_AWAY, 0u));
    TEST_ASSERT_TRUE(ann_due(&A, T)); /* waiting for the acknowledgement to be played */
    ann_ticket(&A, 5u, true, T);
    const uint16_t arming[3] = {VP_ID_ARMING, VP_ID_AWAY, VP_ID_EXIT_NOW};
    const ann_utt_t u = expect_phrase(ANN_KIND_ARMING, arming, 3u, 2u);
    TEST_ASSERT_TRUE(ann_due(&A, T)); /* playing */
    play(&u);
    TEST_ASSERT_FALSE(ann_due(&A, T));

    start(DISARMED_ST());
    arm(ALARM_LEVEL_AWAY, 0u, 6u); /* its acknowledgement never ends */
    commit(S(ALARM_STATE_EXIT_DELAY, ALARM_LEVEL_AWAY, 0u));
    TEST_ASSERT_TRUE(ann_due(&A, T + ANN_EXPIRE_LONG_MS - 1u));
    TEST_ASSERT_FALSE(ann_due(&A, T + ANN_EXPIRE_LONG_MS));
}

/* D05: an instant arm (no exit time) skips "Arming" and says the armed state with its flags. */
static void test_instant_arm(void)
{
    start(DISARMED_ST());
    arm(ALARM_LEVEL_STAY, ALARM_ARM_NO_ENTRY_DELAY, 5u);
    commit(S(ALARM_STATE_ARMED, ALARM_LEVEL_STAY, ALARM_ARM_NO_ENTRY_DELAY));
    ann_ticket(&A, 5u, true, T);
    const uint16_t c[3] = {VP_ID_ARMED, VP_ID_HOME, VP_ID_NO_ENTRY_DELAY};
    expect_phrase(ANN_KIND_ARMED, c, 3u, 2u);
}

/* D05: silent exit, latched at acceptance, silences the whole cycle - also after auto-stay clears
 * the flag. */
static void test_silent_exit_latched(void)
{
    start(DISARMED_ST());
    arm(ALARM_LEVEL_AWAY, ALARM_ARM_SILENT_EXIT, 5u);
    commit(S(ALARM_STATE_EXIT_DELAY, ALARM_LEVEL_AWAY, ALARM_ARM_SILENT_EXIT));
    ann_ticket(&A, 5u, true, T);
    expect_nothing();
    ev(ALARM_EV_AUTO_STAY, 1u, ALARM_LEVEL_STAY, 0u);
    commit(S(ALARM_STATE_ARMED, ALARM_LEVEL_STAY, 0u)); /* the flag is gone now */
    expect_nothing();
}

/* D05: auto-stay is announced as the mode it became. */
static void test_auto_stay_actual_mode(void)
{
    start(DISARMED_ST());
    arm(ALARM_LEVEL_AWAY, 0u, 5u);
    commit(S(ALARM_STATE_EXIT_DELAY, ALARM_LEVEL_AWAY, 0u));
    ann_ticket(&A, 5u, true, T);
    ann_utt_t u;
    TEST_ASSERT_TRUE(next(&u));
    play(&u);
    ev(ALARM_EV_AUTO_STAY, 1u, ALARM_LEVEL_STAY, 0u);
    commit(S(ALARM_STATE_ARMED, ALARM_LEVEL_STAY, 0u));
    const uint16_t c[2] = {VP_ID_ARMED, VP_ID_HOME};
    expect_phrase(ANN_KIND_ARMED, c, 2u, 2u);
}

/* D05: exit_sounds off silences "Arming" but keeps the final confirmation. */
static void test_exit_sounds_off(void)
{
    ann_state_t base = DISARMED_ST();
    base.exit_sounds = false;
    start(base);
    arm(ALARM_LEVEL_AWAY, 0u, 5u);
    ann_state_t s = S(ALARM_STATE_EXIT_DELAY, ALARM_LEVEL_AWAY, 0u);
    s.exit_sounds = false;
    commit(s);
    ann_ticket(&A, 5u, true, T);
    expect_nothing();
    s.state = ALARM_STATE_ARMED;
    commit(s);
    const uint16_t c[2] = {VP_ID_ARMED, VP_ID_AWAY};
    expect_phrase(ANN_KIND_ARMED, c, 2u, 2u);
}

/* D05: a refused arm says no "Arming"; only not-ready speaks, after the error tone. */
static void test_refused_arm(void)
{
    start(DISARMED_ST());
    ev(ALARM_EV_ARM_FAILED, 1u, (uint8_t)ALARM_ERR_NOT_READY, 0u);
    ann_sound(&A, 1u, ALARM_SOUND_ERROR, 0u, 6u);
    commit(DISARMED_ST());
    expect_nothing();
    ann_ticket(&A, 6u, true, T);
    const uint16_t c[1] = {VP_ID_NOT_READY};
    ann_utt_t u = expect(ANN_KIND_NOT_READY, c, 1u, 300u);
    TEST_ASSERT_EQUAL_UINT8(ANN_PRIO_REFUSAL, u.priority);
    play(&u);

    /* another refusal (a wrong code): the error tone only */
    ev(ALARM_EV_ARM_FAILED, 1u, (uint8_t)ALARM_ERR_INVALID_CODE, 0u);
    ev(ALARM_EV_INVALID_CODE, 1u, 0u, 0u);
    ann_sound(&A, 1u, ALARM_SOUND_ERROR, 0u, 7u);
    commit(DISARMED_ST());
    ann_ticket(&A, 7u, true, T);
    expect_nothing();

    /* not ready without its error tone (an automation's refusal after a silent alarm) */
    ev(ALARM_EV_ARM_FAILED, 1u, (uint8_t)ALARM_ERR_NOT_READY, 0u);
    commit(DISARMED_ST());
    expect_nothing();
}

/* D05: "Disarmed" only for a real change; an already-disarmed code + OFF says nothing; a disarm
 * cancels the arming speech still waiting. */
static void test_disarm(void)
{
    start(DISARMED_ST());
    arm(ALARM_LEVEL_AWAY, 0u, 5u);
    commit(S(ALARM_STATE_EXIT_DELAY, ALARM_LEVEL_AWAY, 0u));
    TEST_ASSERT_EQUAL_size_t(1u, ann_waiting(&A));
    ev(ALARM_EV_DISARMED, 1u, 0u, 0u);
    ann_sound(&A, 1u, ALARM_SOUND_ACK, 0u, 8u);
    commit(DISARMED_ST());
    ann_ticket(&A, 5u, true, T);
    ann_ticket(&A, 8u, true, T);
    const uint16_t c[1] = {VP_ID_DISARMED};
    ann_utt_t u = expect(ANN_KIND_DISARMED, c, 1u, 300u);
    play(&u);
    expect_nothing();

    /* already disarmed: memory cleared, troubles acknowledged, an ACK - no "Disarmed" */
    ev(ALARM_EV_MEMORY_CLEARED, 1u, 0u, 0u);
    ann_sound(&A, 1u, ALARM_SOUND_ACK, 0u, 9u);
    commit(DISARMED_ST());
    ann_ticket(&A, 9u, true, T);
    expect_nothing();
}

/* D05: "Entry" at once, highest priority; gone when the entry time ends (an entry that ends is not
 * a new arming). */
static void test_entry(void)
{
    start(S(ALARM_STATE_ARMED, ALARM_LEVEL_AWAY, 0u));
    ev(ALARM_EV_ENTRY_DELAY, 1u, 0u, 0u);
    commit(S(ALARM_STATE_ENTRY_DELAY, ALARM_LEVEL_AWAY, 0u));
    const uint16_t c[1] = {VP_ID_ENTRY};
    ann_utt_t u = expect(ANN_KIND_ENTRY, c, 1u, 300u);
    TEST_ASSERT_EQUAL_UINT8(ANN_PRIO_ENTRY, u.priority);
    /* the entry time runs out while it speaks: canceled, and no "Armed" */
    commit(S(ALARM_STATE_ARMED, ALARM_LEVEL_AWAY, 0u));
    TEST_ASSERT_FALSE(ann_current(&A, u.id));
    TEST_ASSERT_NOT_EQUAL(u.generation, ann_generation(&A));
    expect_nothing();
}

/* AUD-012 / D11 (the state-action generation): "Not ready", waiting for its error tone or
 * already speaking, is dropped by a successful arm after a bypass - even a silent one that says
 * nothing of its own. */
static void test_not_ready_dropped_by_bypass_arm(void)
{
    /* waiting for its tone: the arm's own speech plays, never the refusal */
    start(DISARMED_ST());
    ev(ALARM_EV_ARM_FAILED, 1u, (uint8_t)ALARM_ERR_NOT_READY, 0u);
    ann_sound(&A, 1u, ALARM_SOUND_ERROR, 0u, 6u);
    commit(DISARMED_ST());
    TEST_ASSERT_EQUAL_size_t(1u, ann_waiting(&A));
    ev(ALARM_EV_BYPASS, 1u, 3u, 0u);
    arm(ALARM_LEVEL_AWAY, 0u, 7u);
    commit(S(ALARM_STATE_EXIT_DELAY, ALARM_LEVEL_AWAY, 0u));
    ann_ticket(&A, 6u, true, T);
    ann_ticket(&A, 7u, true, T);
    const uint16_t arming[3] = {VP_ID_ARMING, VP_ID_AWAY, VP_ID_EXIT_NOW};
    ann_utt_t u = expect_phrase(ANN_KIND_ARMING, arming, 3u, 2u);
    play(&u);
    expect_nothing();

    /* speaking: a silent bypass arm cancels it (a new generation) and says nothing */
    start(DISARMED_ST());
    ev(ALARM_EV_ARM_FAILED, 1u, (uint8_t)ALARM_ERR_NOT_READY, 0u);
    ann_sound(&A, 1u, ALARM_SOUND_ERROR, 0u, 6u);
    commit(DISARMED_ST());
    ann_ticket(&A, 6u, true, T);
    const uint16_t nr[1] = {VP_ID_NOT_READY};
    u = expect(ANN_KIND_NOT_READY, nr, 1u, 300u);
    ev(ALARM_EV_BYPASS, 1u, 3u, 0u);
    arm(ALARM_LEVEL_AWAY, ALARM_ARM_SILENT_EXIT, 7u);
    commit(S(ALARM_STATE_EXIT_DELAY, ALARM_LEVEL_AWAY, ALARM_ARM_SILENT_EXIT));
    TEST_ASSERT_FALSE(ann_current(&A, u.id));
    TEST_ASSERT_NOT_EQUAL(u.generation, ann_generation(&A));
    ann_ticket(&A, 7u, true, T);
    expect_nothing();
}

/* AUD-012 / D11: "Armed Away" still speaking is dropped by an instant change to another mode;
 * only the new mode is said. */
static void test_armed_dropped_by_instant_mode_change(void)
{
    start(DISARMED_ST());
    arm(ALARM_LEVEL_AWAY, 0u, 5u);
    commit(S(ALARM_STATE_ARMED, ALARM_LEVEL_AWAY, 0u));
    ann_ticket(&A, 5u, true, T);
    const uint16_t away[2] = {VP_ID_ARMED, VP_ID_AWAY};
    ann_utt_t u = expect_phrase(ANN_KIND_ARMED, away, 2u, 2u);
    arm(ALARM_LEVEL_STAY, 0u, 6u);
    commit(S(ALARM_STATE_ARMED, ALARM_LEVEL_STAY, 0u)); /* ARMED -> ARMED: the state alone can't tell */
    TEST_ASSERT_FALSE(ann_current(&A, u.id));
    TEST_ASSERT_NOT_EQUAL(u.generation, ann_generation(&A));
    ann_ticket(&A, 6u, true, T);
    const uint16_t home[2] = {VP_ID_ARMED, VP_ID_HOME};
    u = expect_phrase(ANN_KIND_ARMED, home, 2u, 2u);
    play(&u);
    expect_nothing();
}

/* AUD-012 / D11: "Armed" still waiting or speaking is dropped by a new silent arming cycle. */
static void test_armed_dropped_by_new_silent_cycle(void)
{
    /* speaking */
    start(DISARMED_ST());
    arm(ALARM_LEVEL_AWAY, 0u, 5u);
    commit(S(ALARM_STATE_ARMED, ALARM_LEVEL_AWAY, 0u));
    ann_ticket(&A, 5u, true, T);
    const uint16_t away[2] = {VP_ID_ARMED, VP_ID_AWAY};
    ann_utt_t u = expect_phrase(ANN_KIND_ARMED, away, 2u, 2u);
    arm(ALARM_LEVEL_AWAY, ALARM_ARM_SILENT_EXIT, 6u);
    commit(S(ALARM_STATE_ARMED, ALARM_LEVEL_AWAY, ALARM_ARM_SILENT_EXIT));
    TEST_ASSERT_FALSE(ann_current(&A, u.id));
    ann_ticket(&A, 6u, true, T);
    expect_nothing();

    /* waiting for its acknowledgement */
    start(DISARMED_ST());
    arm(ALARM_LEVEL_STAY, 0u, 5u);
    commit(S(ALARM_STATE_ARMED, ALARM_LEVEL_STAY, 0u));
    TEST_ASSERT_EQUAL_size_t(1u, ann_waiting(&A));
    arm(ALARM_LEVEL_STAY, ALARM_ARM_SILENT_EXIT, 6u);
    commit(S(ALARM_STATE_ARMED, ALARM_LEVEL_STAY, ALARM_ARM_SILENT_EXIT));
    TEST_ASSERT_EQUAL_size_t(0u, ann_waiting(&A));
    ann_ticket(&A, 5u, true, T);
    ann_ticket(&A, 6u, true, T);
    expect_nothing();
}

/* AUD-012 / D11: with no later action, a disarm's speech waits for its tone and plays to its end
 * through commits that change nothing. */
static void test_disarm_kept_without_new_action(void)
{
    start(S(ALARM_STATE_ARMED, ALARM_LEVEL_AWAY, 0u));
    ev(ALARM_EV_DISARMED, 1u, 0u, 0u);
    ann_sound(&A, 1u, ALARM_SOUND_ACK, 0u, 8u);
    commit(DISARMED_ST());
    T += 500u;
    commit(DISARMED_ST());
    TEST_ASSERT_EQUAL_size_t(1u, ann_waiting(&A));
    ann_ticket(&A, 8u, true, T);
    const uint16_t c[1] = {VP_ID_DISARMED};
    ann_utt_t u = expect(ANN_KIND_DISARMED, c, 1u, 300u);
    const uint32_t gen = ann_generation(&A);
    T += 500u;
    commit(DISARMED_ST());
    TEST_ASSERT_TRUE(ann_current(&A, u.id));
    TEST_ASSERT_EQUAL_UINT32(gen, ann_generation(&A));
    play(&u);
    expect_nothing();
}

/* AUD-010: a duress code's arm and disarm sound exactly like the ordinary ones. */
static void run_arm_disarm(bool duress, ann_utt_t out[3])
{
    start(DISARMED_ST());
    arm(ALARM_LEVEL_AWAY, 0u, 5u);
    if (duress) {
        alarm_event_t e;
        memset(&e, 0, sizeof(e));
        e.type = ALARM_EV_DURESS;
        e.partition = 1u;
        e.flags = ALARM_EVF_SILENT | ALARM_EVF_REPORT;
        ann_event(&A, &e);
    }
    commit(S(ALARM_STATE_EXIT_DELAY, ALARM_LEVEL_AWAY, 0u));
    ann_ticket(&A, 5u, true, T);
    TEST_ASSERT_TRUE(next(&out[0]));
    play(&out[0]);
    commit(S(ALARM_STATE_ARMED, ALARM_LEVEL_AWAY, 0u));
    TEST_ASSERT_TRUE(next(&out[1]));
    play(&out[1]);
    ev(ALARM_EV_DISARMED, 1u, 0u, 0u);
    if (duress) {
        ev(ALARM_EV_DURESS, 1u, 0u, 0u);
    }
    ann_sound(&A, 1u, ALARM_SOUND_ACK, 0u, 6u);
    commit(DISARMED_ST());
    ann_ticket(&A, 6u, true, T);
    TEST_ASSERT_TRUE(next(&out[2]));
    play(&out[2]);
    expect_nothing();
}

static void test_duress_identical(void)
{
    ann_utt_t ordinary[3];
    ann_utt_t duress[3];
    run_arm_disarm(false, ordinary);
    run_arm_disarm(true, duress);
    for (unsigned i = 0u; i < 3u; i++) {
        TEST_ASSERT_EQUAL_UINT8(ordinary[i].kind, duress[i].kind);
        TEST_ASSERT_EQUAL_UINT8(ordinary[i].count, duress[i].count);
        TEST_ASSERT_EQUAL_HEX16_ARRAY(ordinary[i].clips, duress[i].clips, ordinary[i].count);
        TEST_ASSERT_EQUAL_HEX16_ARRAY(ordinary[i].gap_ms, duress[i].gap_ms, ordinary[i].count);
        TEST_ASSERT_EQUAL_UINT32(ordinary[i].generation, duress[i].generation);
    }
}

/* ======================================================================== */
/* What never speaks (D05, D06)                                              */
/* ======================================================================== */

/* D05: other partitions' events and sounds aren't spoken. */
static void test_other_partition_silent(void)
{
    start(DISARMED_ST());
    ev(ALARM_EV_ARMED, 2u, ALARM_LEVEL_AWAY, 0u);
    ann_sound(&A, 2u, ALARM_SOUND_ACK, 0u, 5u);
    ann_sound(&A, 2u, ALARM_SOUND_CHIME, 1u, 6u);
    ev(ALARM_EV_ARM_FAILED, 2u, (uint8_t)ALARM_ERR_NOT_READY, 0u);
    ann_sound(&A, 2u, ALARM_SOUND_ERROR, 0u, 7u);
    commit(DISARMED_ST());
    ann_ticket(&A, 5u, true, T);
    ann_ticket(&A, 6u, true, T);
    ann_ticket(&A, 7u, true, T);
    expect_nothing();
    TEST_ASSERT_EQUAL_UINT(0u, F.zone_calls);
}

/* D05 (on a link keypad: its home partition), LINK-028: moved to another partition, the policy
 * drops what waits of the old one, takes the next commit as a silent baseline, then speaks for the
 * new partition only. */
static void test_keypad_moved_to_another_partition(void)
{
    start(DISARMED_ST());
    arm(ALARM_LEVEL_AWAY, 0u, 5u); /* partition 1's arming words wait for their ACK */
    commit(S(ALARM_STATE_EXIT_DELAY, ALARM_LEVEL_AWAY, 0u));
    TEST_ASSERT_EQUAL_size_t(1u, ann_waiting(&A));
    const uint32_t gen = ann_generation(&A);
    ann_set_keypad(&A, 2u);
    TEST_ASSERT_EQUAL_UINT8(2u, A.keypad);
    TEST_ASSERT_EQUAL_size_t(0u, ann_waiting(&A));
    TEST_ASSERT_NOT_EQUAL(gen, ann_generation(&A));
    ann_ticket(&A, 5u, true, T);
    expect_nothing();
    /* the baseline: silent, whatever its batch holds */
    ev(ALARM_EV_DISARMED, 2u, 0u, 0u);
    ann_sound(&A, 2u, ALARM_SOUND_ACK, 0u, 6u);
    commit(DISARMED_ST());
    ann_ticket(&A, 6u, true, T);
    expect_nothing();
    /* partition 1 says nothing now: its arm, its chime */
    ev(ALARM_EV_ARMED, 1u, ALARM_LEVEL_AWAY, 0u);
    ann_sound(&A, 1u, ALARM_SOUND_ACK, 0u, 7u);
    ann_sound(&A, 1u, ALARM_SOUND_CHIME, 1u, 8u);
    commit(DISARMED_ST());
    ann_ticket(&A, 7u, true, T);
    ann_ticket(&A, 8u, true, T);
    expect_nothing();
    /* partition 2 speaks */
    ev(ALARM_EV_ARMED, 2u, ALARM_LEVEL_AWAY, 0u);
    ann_sound(&A, 2u, ALARM_SOUND_ACK, 0u, 9u);
    commit(S(ALARM_STATE_EXIT_DELAY, ALARM_LEVEL_AWAY, 0u));
    ann_ticket(&A, 9u, true, T);
    const uint16_t arming[3] = {VP_ID_ARMING, VP_ID_AWAY, VP_ID_EXIT_NOW};
    ann_utt_t u = expect_phrase(ANN_KIND_ARMING, arming, 3u, 2u);
    play(&u);
    ann_sound(&A, 2u, ALARM_SOUND_CHIME, 2u, 10u);
    commit(S(ALARM_STATE_EXIT_DELAY, ALARM_LEVEL_AWAY, 0u));
    ann_ticket(&A, 10u, true, T);
    expect_nothing(); /* a chime speaks only while disarmed */
}

/* D05: the first commit is a silent baseline (start-up, restore); so is the first after
 * ann_rebaseline() (reconnection), and a restored exit time that ends is no known cycle. */
static void test_baseline_silent(void)
{
    memset(&F, 0, sizeof(F));
    name_zone(1u, FRONT_DOOR, 2u);
    ann_ports_t ports = {NULL, f_words, f_clip};
    ann_init(&A, &ports, 1u);
    ann_prefs_t p = voice_on();
    ann_set_prefs(&A, &p, T);
    ann_pack_t pack = {true, PACK_REV, VP_WORD_GAP_MS, VP_PART_GAP_MS};
    ann_set_pack(&A, &pack, T);
    arm(ALARM_LEVEL_AWAY, 0u, 5u);
    ann_sound(&A, 1u, ALARM_SOUND_CHIME, 1u, 6u);
    commit(S(ALARM_STATE_EXIT_DELAY, ALARM_LEVEL_AWAY, 0u));
    ann_ticket(&A, 5u, true, T);
    ann_ticket(&A, 6u, true, T);
    expect_nothing();
    commit(S(ALARM_STATE_ARMED, ALARM_LEVEL_AWAY, 0u)); /* the restored exit time ends */
    expect_nothing();

    ann_rebaseline(&A);
    ev(ALARM_EV_DISARMED, 1u, 0u, 0u);
    commit(DISARMED_ST());
    expect_nothing();
}

/* AUD-015: voice on by default (D06, as modern panels), the status switch, volume 0 and power
 * saving silence automatic speech; preferences survive the main switch. */
static void test_suppression_by_preferences(void)
{
    ann_t fresh;
    ann_init(&fresh, NULL, 1u);
    TEST_ASSERT_TRUE(fresh.prefs.voice_enabled);
    TEST_ASSERT_TRUE(fresh.prefs.voice_status);
    TEST_ASSERT_TRUE(fresh.prefs.voice_chime);

    for (unsigned c = 0u; c < 4u; c++) {
        start(DISARMED_ST());
        ann_prefs_t p = voice_on();
        if (c == 0u) p.voice_enabled = false;
        if (c == 1u) p.voice_status = false;
        if (c == 2u) p.volume = 0u;
        if (c == 3u) p.power_saving = true;
        ann_set_prefs(&A, &p, T);
        arm(ALARM_LEVEL_AWAY, 0u, 5u);
        commit(S(ALARM_STATE_EXIT_DELAY, ALARM_LEVEL_AWAY, 0u));
        ann_ticket(&A, 5u, true, T);
        expect_nothing();
    }

    /* turning the voice off cancels what plays */
    start(DISARMED_ST());
    ann_sound(&A, 1u, ALARM_SOUND_CHIME, 1u, 7u);
    ann_ticket(&A, 7u, true, T);
    commit(DISARMED_ST());
    ann_utt_t u = expect(ANN_KIND_CHIME, FRONT_DOOR, 2u, VP_WORD_GAP_MS);
    ann_prefs_t p = voice_on();
    p.volume = 0u;
    ann_set_prefs(&A, &p, T);
    TEST_ASSERT_FALSE(ann_current(&A, u.id));
}

/* ======================================================================== */
/* The queue (D11)                                                           */
/* ======================================================================== */

static void chime(uint16_t zone, uint32_t ticket)
{
    ann_sound(&A, 1u, ALARM_SOUND_CHIME, zone, ticket);
    commit(DISARMED_ST());
    ann_ticket(&A, ticket, true, T);
}

/* AUD-010: an audible pattern anywhere cancels all speech at once (a new generation) and nothing
 * new queues while it sounds; a silent alarm changes nothing. */
static void test_loud_cancels_everything(void)
{
    start(DISARMED_ST());
    chime(1u, 7u);
    chime(2u, 8u);
    ann_utt_t u;
    TEST_ASSERT_TRUE(next(&u));
    TEST_ASSERT_EQUAL_size_t(1u, ann_waiting(&A));

    /* a silent panic: its event alone, nothing audible */
    ev(ALARM_EV_ALARM, 1u, 0u, 0u);
    commit(DISARMED_ST());
    TEST_ASSERT_TRUE(ann_current(&A, u.id));
    TEST_ASSERT_EQUAL_UINT32(u.generation, ann_generation(&A));

    ann_state_t loud = DISARMED_ST();
    loud.loud = true; /* fire on partition 2 */
    commit(loud);
    TEST_ASSERT_FALSE(ann_current(&A, u.id));
    TEST_ASSERT_NOT_EQUAL(u.generation, ann_generation(&A));
    TEST_ASSERT_EQUAL_size_t(0u, ann_waiting(&A));
    ann_sound(&A, 1u, ALARM_SOUND_CHIME, 1u, 9u);
    ann_ticket(&A, 9u, true, T);
    commit(loud);
    expect_nothing();
    const uint16_t ids[1] = {1u};
    TEST_ASSERT_EQUAL(ANN_PREVIEW_ALARM, ann_preview(&A, ids, 1u, false, T));
}

/* AUD-012: one active and four waiting; a fifth of no higher priority is dropped, a higher one
 * takes the place of the lowest, newest. */
static void test_queue_overflow(void)
{
    start(DISARMED_ST());
    for (uint16_t z = 1u; z <= 5u; z++) {
        const uint16_t w[1] = {(uint16_t)(z + 100u)};
        name_zone(z, w, 1u);
    }
    name_zone(6u, FRONT_DOOR, 2u);
    chime(1u, 1u);
    ann_utt_t active;
    TEST_ASSERT_TRUE(next(&active));
    chime(2u, 2u);
    chime(3u, 3u);
    chime(4u, 4u);
    chime(5u, 5u);
    TEST_ASSERT_EQUAL_size_t(4u, ann_waiting(&A));
    chime(6u, 6u); /* the same priority: dropped */
    TEST_ASSERT_EQUAL_size_t(4u, ann_waiting(&A));

    ev(ALARM_EV_ARM_FAILED, 1u, (uint8_t)ALARM_ERR_NOT_READY, 0u);
    ann_sound(&A, 1u, ALARM_SOUND_ERROR, 0u, 9u);
    commit(DISARMED_ST());
    TEST_ASSERT_EQUAL_size_t(4u, ann_waiting(&A));
    TEST_ASSERT_TRUE(ann_current(&A, active.id)); /* its tone hasn't played yet */
    ann_ticket(&A, 9u, true, T);
    /* the refusal is ready and higher: it supersedes the chime now playing */
    TEST_ASSERT_FALSE(ann_current(&A, active.id));
    const uint16_t c[1] = {VP_ID_NOT_READY};
    ann_utt_t u = expect(ANN_KIND_NOT_READY, c, 1u, 300u);
    play(&u);
    /* zones 2, 3, 4 remain in order; 5 was the newest of the lowest and was evicted */
    for (uint16_t z = 2u; z <= 4u; z++) {
        TEST_ASSERT_TRUE(next(&u));
        TEST_ASSERT_EQUAL_UINT16(z, u.zone);
        play(&u);
    }
    expect_nothing();
}

/* AUD-012: repeated waiting requests for one zone coalesce; every tone was still played. */
static void test_coalescing(void)
{
    start(WALK_ST(0u, 3u));
    ann_sound(&A, 1u, ALARM_SOUND_WALK_TEST, 1u, 1u);
    commit(WALK_ST(1u, 3u));
    ann_sound(&A, 1u, ALARM_SOUND_WALK_TEST, 1u, 2u);
    commit(WALK_ST(1u, 3u));
    ann_sound(&A, 1u, ALARM_SOUND_WALK_TEST, 1u, 3u);
    commit(WALK_ST(1u, 3u));
    TEST_ASSERT_EQUAL_size_t(1u, ann_waiting(&A));
    ann_ticket(&A, 1u, true, T);
    ann_ticket(&A, 2u, true, T);
    expect_nothing(); /* it follows the newest tone */
    ann_ticket(&A, 3u, true, T);
    ann_utt_t u = expect(ANN_KIND_WALK_TRIP, FRONT_DOOR, 2u, VP_WORD_GAP_MS);
    play(&u);
    expect_nothing();
}

/* AUD-012: chime, trip and refusal expire after 2 s, status after 5 s. */
static void test_expiry(void)
{
    start(DISARMED_ST());
    ann_sound(&A, 1u, ALARM_SOUND_CHIME, 1u, 7u);
    commit(DISARMED_ST());
    T += ANN_EXPIRE_SHORT_MS;
    ann_ticket(&A, 7u, true, T);
    expect_nothing();

    ann_sound(&A, 1u, ALARM_SOUND_CHIME, 1u, 8u);
    commit(DISARMED_ST());
    T += ANN_EXPIRE_SHORT_MS - 1u;
    ann_ticket(&A, 8u, true, T);
    ann_utt_t u = expect(ANN_KIND_CHIME, FRONT_DOOR, 2u, VP_WORD_GAP_MS);
    play(&u);

    arm(ALARM_LEVEL_AWAY, 0u, 5u);
    commit(S(ALARM_STATE_EXIT_DELAY, ALARM_LEVEL_AWAY, 0u));
    T += ANN_EXPIRE_LONG_MS - 1u;
    ann_ticket(&A, 5u, true, T);
    TEST_ASSERT_TRUE(next(&u));
    play(&u);
    ev(ALARM_EV_DISARMED, 1u, 0u, 0u);
    ann_sound(&A, 1u, ALARM_SOUND_ACK, 0u, 6u);
    commit(DISARMED_ST());
    T += ANN_EXPIRE_LONG_MS;
    ann_ticket(&A, 6u, true, T);
    expect_nothing();
}

/* AUD-012: a changed configuration makes a waiting zone name stale; a new pack cancels all. */
static void test_generations(void)
{
    start(DISARMED_ST());
    ann_sound(&A, 1u, ALARM_SOUND_CHIME, 1u, 7u);
    commit(DISARMED_ST());
    ann_state_t s = DISARMED_ST();
    s.config_seq = 2u;
    commit(s);
    ann_ticket(&A, 7u, true, T);
    expect_nothing();

    ann_sound(&A, 1u, ALARM_SOUND_CHIME, 1u, 8u);
    ann_ticket(&A, 8u, true, T);
    commit(s);
    ann_utt_t u;
    TEST_ASSERT_TRUE(next(&u));
    const uint32_t gen = ann_generation(&A);
    ann_pack_t pack = {true, PACK_REV + 1u, VP_WORD_GAP_MS, VP_PART_GAP_MS};
    ann_set_pack(&A, &pack, T);
    TEST_ASSERT_FALSE(ann_current(&A, u.id));
    TEST_ASSERT_NOT_EQUAL(gen, ann_generation(&A));
}

/* AUD-012: a higher request waiting for its tone doesn't cut in until the tone has played. */
static void test_supersession_waits_for_tone(void)
{
    start(DISARMED_ST());
    chime(1u, 7u);
    ann_utt_t u;
    TEST_ASSERT_TRUE(next(&u));
    arm(ALARM_LEVEL_AWAY, 0u, 5u);
    commit(S(ALARM_STATE_EXIT_DELAY, ALARM_LEVEL_AWAY, 0u));
    /* armed: the chime's state rule (disarmed only) already makes it stale */
    TEST_ASSERT_FALSE(ann_current(&A, u.id));

    start(DISARMED_ST());
    ann_utt_t p;
    const uint16_t ids[2] = {1u, 32u};
    TEST_ASSERT_EQUAL(ANN_PREVIEW_OK, ann_preview(&A, ids, 2u, false, T));
    TEST_ASSERT_TRUE(next(&p));
    ann_sound(&A, 1u, ALARM_SOUND_CHIME, 1u, 7u);
    commit(DISARMED_ST());
    TEST_ASSERT_TRUE(ann_current(&A, p.id)); /* the ding-dong is still playing */
    ann_ticket(&A, 7u, true, T);
    TEST_ASSERT_FALSE(ann_current(&A, p.id));
    expect(ANN_KIND_CHIME, FRONT_DOOR, 2u, VP_WORD_GAP_MS);
}

/* ======================================================================== */
/* Preview (D03, D06)                                                        */
/* ======================================================================== */

/* AUD-015: Preview plays with the voice off and never turns it on; it respects the pack, volume,
 * power saving and the ten-second ceiling; a new one replaces the one playing. */
static void test_preview(void)
{
    start(DISARMED_ST());
    ann_prefs_t p = voice_on();
    p.voice_enabled = false;
    ann_set_prefs(&A, &p, T);
    const uint16_t ids[2] = {1u, 109u};
    TEST_ASSERT_EQUAL(ANN_PREVIEW_OK, ann_preview(&A, ids, 2u, false, T));
    ann_utt_t u = expect(ANN_KIND_PREVIEW, ids, 2u, VP_WORD_GAP_MS);
    TEST_ASSERT_FALSE(A.prefs.voice_enabled);
    const uint16_t parts[2] = {VP_ID_WALK_TEST, VP_ID_WALK_STARTED};
    TEST_ASSERT_EQUAL(ANN_PREVIEW_OK, ann_preview(&A, parts, 2u, true, T));
    TEST_ASSERT_FALSE(ann_current(&A, u.id));
    expect(ANN_KIND_PREVIEW, parts, 2u, 300u);

    TEST_ASSERT_EQUAL(ANN_PREVIEW_EMPTY, ann_preview(&A, ids, 0u, false, T));
    const uint16_t nine[9] = {1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u};
    TEST_ASSERT_EQUAL(ANN_PREVIEW_TOO_MANY, ann_preview(&A, nine, 9u, false, T));
    F.missing = 109u;
    TEST_ASSERT_EQUAL(ANN_PREVIEW_BAD_CLIP, ann_preview(&A, ids, 2u, false, T));
    F.missing = 0u;
    F.big_id = 109u;
    F.big_samples = ANN_SAMPLES_MAX;
    TEST_ASSERT_EQUAL(ANN_PREVIEW_TOO_LONG, ann_preview(&A, ids, 2u, false, T));
    F.big_id = 0u;
    p.volume = 0u;
    ann_set_prefs(&A, &p, T);
    TEST_ASSERT_EQUAL(ANN_PREVIEW_MUTED, ann_preview(&A, ids, 2u, false, T));
    p.volume = 5u;
    p.power_saving = true;
    ann_set_prefs(&A, &p, T);
    TEST_ASSERT_EQUAL(ANN_PREVIEW_MUTED, ann_preview(&A, ids, 2u, false, T));
    p.power_saving = false;
    ann_set_prefs(&A, &p, T);
    ann_pack_t none = {false, 0u, 0u, 0u};
    ann_set_pack(&A, &none, T);
    TEST_ASSERT_EQUAL(ANN_PREVIEW_NO_PACK, ann_preview(&A, ids, 2u, false, T));
    TEST_ASSERT_FALSE(A.prefs.voice_enabled);
}

/* ======================================================================== */
/* Adapters                                                                  */
/* ======================================================================== */

/* AUD-010 / D05: loudness is every partition's; the rest is the keypad partition's. */
static void test_state_from_snapshot(void)
{
    static alarm_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.partitions = 0x03u;
    snap.config_seq = 4u;
    snap.part[0].state = ALARM_STATE_EXIT_DELAY;
    snap.part[0].level = ALARM_LEVEL_NIGHT;
    snap.part[0].arm_flags = ALARM_ARM_NO_ENTRY_DELAY;
    snap.part[0].walk_tested = 2u;
    snap.part[0].walk_total = 7u;
    snap.part[0].sounder = ALARM_SOUND_EXIT;
    ann_state_t st;
    ann_state_from_snapshot(&snap, 1u, true, &st);
    TEST_ASSERT_EQUAL_UINT8(ALARM_STATE_EXIT_DELAY, st.state);
    TEST_ASSERT_EQUAL_UINT8(ALARM_LEVEL_NIGHT, st.level);
    TEST_ASSERT_EQUAL_UINT8(ALARM_ARM_NO_ENTRY_DELAY, st.arm_flags);
    TEST_ASSERT_EQUAL_UINT16(7u, st.walk_total);
    TEST_ASSERT_EQUAL_UINT32(4u, st.config_seq);
    TEST_ASSERT_TRUE(st.exit_sounds);
    TEST_ASSERT_FALSE(st.loud);
    const alarm_sound_t loud[4] = {ALARM_SOUND_AUX, ALARM_SOUND_ALARM, ALARM_SOUND_CO,
                                   ALARM_SOUND_FIRE};
    for (unsigned i = 0u; i < 4u; i++) {
        snap.part[1].sounder = loud[i];
        ann_state_from_snapshot(&snap, 1u, true, &st);
        TEST_ASSERT_TRUE(st.loud);
    }
    snap.part[1].sounder = ALARM_SOUND_TROUBLE;
    ann_state_from_snapshot(&snap, 1u, true, &st);
    TEST_ASSERT_FALSE(st.loud);
    snap.partitions = 0x01u; /* a disabled partition's leftovers don't count */
    snap.part[1].sounder = ALARM_SOUND_FIRE;
    ann_state_from_snapshot(&snap, 1u, true, &st);
    TEST_ASSERT_FALSE(st.loud);
}

/* AUD-012: clip lengths from a real pack (the reader's frozen fixture). */
/* AUD-031: with endings in the pack, a spoken name's last word plays as its ending - the chime's
 * name, a walk trip's and a Preview's - and the words before it as themselves; a word without an
 * ending plays as itself; status and walk phrases never change. */
static void test_name_ends_in_its_ending(void)
{
    start(DISARMED_ST());
    F.endings = true;
    const uint16_t front_door_end[2] = {FRONT_DOOR[0], (uint16_t)(VP_ENDING_BASE + FRONT_DOOR[1])};
    ann_sound(&A, 1u, ALARM_SOUND_CHIME, 1u, 7u);
    commit(DISARMED_ST());
    T += 900u;
    ann_ticket(&A, 7u, true, T);
    ann_utt_t u = expect(ANN_KIND_CHIME, front_door_end, 2u, VP_WORD_GAP_MS);
    play(&u);

    TEST_ASSERT_EQUAL(ANN_PREVIEW_OK, ann_preview(&A, FRONT_DOOR, 2u, false, T));
    u = expect(ANN_KIND_PREVIEW, front_door_end, 2u, VP_WORD_GAP_MS);
    play(&u);
    const uint16_t one[1] = {109u};
    const uint16_t one_end[1] = {(uint16_t)(VP_ENDING_BASE + 109u)};
    TEST_ASSERT_EQUAL(ANN_PREVIEW_OK, ann_preview(&A, one, 1u, false, T));
    u = expect(ANN_KIND_PREVIEW, one_end, 1u, VP_WORD_GAP_MS);
    play(&u);

    F.missing = (uint16_t)(VP_ENDING_BASE + 109u); /* this word has no ending */
    TEST_ASSERT_EQUAL(ANN_PREVIEW_OK, ann_preview(&A, FRONT_DOOR, 2u, false, T));
    u = expect(ANN_KIND_PREVIEW, FRONT_DOOR, 2u, VP_WORD_GAP_MS);
    play(&u);
    F.missing = 0u;

    const uint16_t parts[2] = {VP_ID_WALK_TEST, VP_ID_WALK_STARTED};
    TEST_ASSERT_EQUAL(ANN_PREVIEW_OK, ann_preview(&A, parts, 2u, true, T));
    u = expect(ANN_KIND_PREVIEW, parts, 2u, 300u);
    play(&u);
    const uint16_t armed[2] = {VP_ID_ARMED, VP_ID_AWAY};
    TEST_ASSERT_EQUAL(ANN_PREVIEW_OK, ann_preview(&A, armed, 2u, false, T));
    expect(ANN_KIND_PREVIEW, armed, 2u, VP_WORD_GAP_MS);
}

static void test_pack_clip_samples(void)
{
    vp_pack_t pack;
    TEST_ASSERT_EQUAL(VP_OK, vp_open(voice_pack_fixture_v1, sizeof(voice_pack_fixture_v1), &pack,
                                     NULL));
    TEST_ASSERT_EQUAL_UINT32(756u, ann_pack_clip_samples(&pack, 1u));
    TEST_ASSERT_EQUAL_UINT32(209u, ann_pack_clip_samples(&pack, 109u));
    TEST_ASSERT_EQUAL_UINT32(166u, ann_pack_clip_samples(&pack, VP_ID_ARMING));
    TEST_ASSERT_EQUAL_UINT32(0u, ann_pack_clip_samples(&pack, 2u));
    TEST_ASSERT_EQUAL_UINT32(0u, ann_pack_clip_samples(NULL, 1u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_chime_name_after_tone);
    RUN_TEST(test_chime_canceled_or_dropped_tone);
    RUN_TEST(test_chime_eligibility);
    RUN_TEST(test_chime_ten_second_ceiling);
    RUN_TEST(test_walk_trip_name);
    RUN_TEST(test_walk_start_waits_for_ack_and_bell);
    RUN_TEST(test_walk_end_summary);
    RUN_TEST(test_walk_end_cancels_trips);
    RUN_TEST(test_walk_warnings);
    RUN_TEST(test_arming_then_armed_each_mode);
    RUN_TEST(test_speech_is_due_from_the_arm_to_the_words_end);
    RUN_TEST(test_instant_arm);
    RUN_TEST(test_silent_exit_latched);
    RUN_TEST(test_auto_stay_actual_mode);
    RUN_TEST(test_exit_sounds_off);
    RUN_TEST(test_refused_arm);
    RUN_TEST(test_disarm);
    RUN_TEST(test_entry);
    RUN_TEST(test_not_ready_dropped_by_bypass_arm);
    RUN_TEST(test_armed_dropped_by_instant_mode_change);
    RUN_TEST(test_armed_dropped_by_new_silent_cycle);
    RUN_TEST(test_disarm_kept_without_new_action);
    RUN_TEST(test_duress_identical);
    RUN_TEST(test_other_partition_silent);
    RUN_TEST(test_keypad_moved_to_another_partition);
    RUN_TEST(test_baseline_silent);
    RUN_TEST(test_suppression_by_preferences);
    RUN_TEST(test_loud_cancels_everything);
    RUN_TEST(test_queue_overflow);
    RUN_TEST(test_coalescing);
    RUN_TEST(test_expiry);
    RUN_TEST(test_generations);
    RUN_TEST(test_supersession_waits_for_tone);
    RUN_TEST(test_preview);
    RUN_TEST(test_state_from_snapshot);
    RUN_TEST(test_pack_clip_samples);
    RUN_TEST(test_name_ends_in_its_ending);
    return UNITY_END();
}
