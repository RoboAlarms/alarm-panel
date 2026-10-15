/* voice_walk - the voice on the PC, proved without a sound card (docs/features/08
 * voice-first-build.md D15, BL-202): the preview's scripted run of what the panel says.
 *
 * host_panel.c wires the core annunciator as the panel does and, with no player plugged in,
 * renders every utterance it hands out to build/ui_preview/voice/<n>-<kind>.wav (16-bit mono
 * 32 kHz) with a trace line "voice: <kind> <zone> <clip ids> <total ms>". This run drives the
 * engine in virtual time and asserts that trace: a chime on a zone with an approved spoken name
 * gives the chime tone, then that zone's words (AUD-008); arming away gives Arming, Away, "You may
 * exit now"; a disarm gives Disarmed; a walk test start and a trip give the walk words and the
 * zone's name; with Announcements off nothing is said (AUD-014). Each WAV is what the panel's
 * mixer (snd_render_pcm) would put out - 16-bit stereo 32 kHz with the tones, the user volume, the
 * speech level under the entry and fast exit beeps and the exit beeps' wait (AUD-034) - for the
 * time the utterance played: a completed one holds at least
 * its vr_total_samples_32k, a cancelled one (an alarm, power saving) stops where it was cancelled;
 * the walk renders the same words through a copy of the mixer and compares sample for sample.
 * It also drives the power policy, the sensor binds and long clock advances with automations off.
 * A last pass plays a chime through the simulator's own audio core (host/ui_sim/sim_audio.c,
 * offline), the mixer the PC simulator hears. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif

#include "annunciator/annunciator.h"
#include "host_panel.h"
#include "sim_audio.h"
#include "sounder/sounder_mix.h"
#include "sounder/speech_boost.h"
#include "voice_pack/voice_ids.h"
#include "voice_pack/voice_pack.h"
#include "voice_walk.h"
#include "zone_hub/zsrc_sim.h"

#define ZONE 3u /* Living Room Window: a perimeter zone */

static int s_checks, s_failed;

static void check(bool ok, const char *what)
{
    s_checks++;
    if (!ok) {
        s_failed++;
        printf("FAIL (voice walk): %s\n", what);
    }
}

/* D07: Done's identity now (the configuration's generation, the zone's binding, the pack's
 * revision), as the editor captures it. */
static ui_voice_seen_t voice_seen_now(const ui_backend_t *b, uint16_t zone)
{
    ui_voice_seen_t seen = {0};
    ui_voice_info_t info;
    b->voice_info(NULL, &info);
    if (!b->voice_seen(NULL, zone, &seen)) {
        seen.generation = UINT32_MAX; /* nothing to approve against */
    }
    seen.pack_revision = info.pack_revision;
    return seen;
}

/* D07: approve words already stored (Done on a review: no save). */
static uint8_t approve_stored(const ui_backend_t *b, uint16_t zone, const uint16_t *ids, size_t n)
{
    const ui_voice_seen_t seen = voice_seen_now(b, zone);
    alarm_result_t saved;
    return b->voice_apply_approve(NULL, NULL, zone, &seen, ids, n, &saved);
}

static host_voice_log_t s_log[64];
static unsigned s_n;

static void refresh(void)
{
    s_n = host_panel_voice_log(s_log, 64u);
}

/* The entries after index `from`: speech of `kind`, or any when kind < 0. */
static int find_speech(unsigned from, int kind)
{
    for (unsigned i = from; i < s_n; i++) {
        if (s_log[i].speech && (kind < 0 || s_log[i].kind == (unsigned)kind)) {
            return (int)i;
        }
    }
    return -1;
}

static bool clips_are(const host_voice_log_t *e, const uint16_t *want, unsigned n)
{
    if (e->count != n) {
        return false;
    }
    for (unsigned i = 0; i < n; i++) {
        if (e->clips[i] != want[i]) {
            return false;
        }
    }
    return true;
}

static alarm_result_t command(alarm_cmd_type_t type, alarm_arm_level_t level, uint8_t walk_op)
{
    alarm_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = type;
    cmd.partition = 1;
    cmd.level = level;
    cmd.walk_op = walk_op;
    snprintf(cmd.code, sizeof(cmd.code), "123456"); /* the factory master code */
    return host_panel_remote_command(&cmd);
}

static long file_size(const char *dir, unsigned n, const char *kind)
{
    char path[320];
    snprintf(path, sizeof(path), "%s/%u-%s.wav", dir, n, kind);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fclose(f);
    return size;
}

static const char *kind_file(unsigned kind)
{
    static const char *const names[] = {"none",       "entry",      "arming",       "armed",
                                        "disarmed",   "walk-start", "walk-end",     "walk-warning",
                                        "not-ready",  "walk-trip",  "chime",        "preview"};
    return names[kind];
}

static unsigned s_speech;
static char s_dir[260];

/* Every speech entry has its WAV, exactly as long as the renderer says; then the log starts over. */
static void clear_log(void)
{
    refresh();
    for (unsigned i = 0; i < s_n; i++) {
        if (!s_log[i].speech) {
            continue;
        }
        s_speech++;
        const host_voice_log_t *e = &s_log[i];
        check(e->wav_number != 0u && e->total_32k > 0u && e->completed != e->cancelled,
              "each utterance has its WAV and ends completed or cancelled");
        if (e->completed) {
            /* at least the words; the mixer adds its silent warm-up block and a partial last one */
            check(e->wav_samples >= e->total_32k && e->wav_samples <= e->total_32k + 3u * 320u,
                  "a completed utterance's WAV holds its words (and the mixer's warm-up)");
        } else {
            check(e->wav_samples < e->total_32k + 320u,
                  "a cancelled utterance's WAV stops where it was cancelled");
        }
        check(file_size(s_dir, e->wav_number, kind_file(e->kind)) == 44 + 4 * (long)e->wav_samples,
              "each WAV file is a 44-byte header and 4 bytes a stereo frame");
    }
    host_panel_voice_log_clear();
}

static void settle(unsigned ms)
{
    host_panel_advance(ms);
}

static void zone_pulse(void)
{
    host_panel_zone(ZONE, ALARM_ZEV_FAULT);
    settle(3000);
    host_panel_zone(ZONE, ALARM_ZEV_RESTORE);
    settle(3000);
}

/* ---- the mixer, the power policy, the binds ---- */

#define REF_MAX (32000u * 8u)
static int16_t s_words[REF_MAX];
static int16_t s_wav[REF_MAX * 2u];

/* The WAV's samples (stereo, interleaved); the frames, or 0. */
static unsigned read_wav(const host_voice_log_t *e)
{
    char path[320];
    snprintf(path, sizeof(path), "%s/%u-%s.wav", s_dir, e->wav_number, kind_file(e->kind));
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    fseek(f, 44, SEEK_SET);
    uint8_t b[2];
    unsigned n = 0;
    while (n < e->wav_samples * 2u && n < REF_MAX * 2u && fread(b, 1, 2, f) == 2u) {
        s_wav[n++] = (int16_t)((uint16_t)b[0] | (uint16_t)((uint16_t)b[1] << 8));
    }
    fclose(f);
    return n / 2u;
}

static unsigned peak_of(unsigned frames)
{
    unsigned peak = 0;
    for (unsigned i = 0; i < frames * 2u; i++) {
        const unsigned v = (unsigned)abs(s_wav[i]);
        peak = v > peak ? v : peak;
    }
    return peak;
}

/* Renders the entry's words, through the Voice volume's boost at the utterance's volume (AUD-033),
 * then through `mix` (a copy of the mixer at the utterance's start) one 10 ms block at a time, as
 * the card does, and compares with the WAV. */
static bool matches_mixer(const host_voice_log_t *e, const vp_pack_t *pack, snd_mixer_t mix)
{
    const unsigned frames = read_wav(e);
    if (frames == 0u || frames != e->wav_samples) {
        return false;
    }
    vr_item_t items[8];
    for (unsigned i = 0; i < e->count; i++) {
        items[i] = (vr_item_t){.clip_id = e->clips[i], .gap_before_ms = e->gap_ms[i]};
    }
    const size_t bytes = vc_state_bytes();
    void *codec = malloc(bytes);
    vr_t vr;
    if (codec == NULL || vr_begin(&vr, pack, items, e->count, codec, bytes) != 0) {
        free(codec);
        return false;
    }
    size_t total = 0;
    int32_t got;
    static speech_boost_t sb;
    sb_begin(&sb, mix.voice_volume);
    while (total < REF_MAX && (got = vr_pull(&vr, s_words + total, 640u)) > 0) {
        sb_process(&sb, s_words + total, (size_t)got);
        total += (size_t)got;
    }
    total += sb_flush(&sb, s_words + total, REF_MAX - total);
    free(codec);
    size_t pos = 0;
    bool same = true;
    for (unsigned blk = 0; blk < frames / 320u && same; blk++) {
        int16_t out[320 * 2];
        const size_t avail = total - pos < 320u ? total - pos : 320u;
        const snd_pcm_block_t r = snd_render_pcm(&mix, out, 320u, s_words + pos, avail, true);
        pos += r.speech_consumed;
        same = memcmp(out, s_wav + (size_t)blk * 640u, sizeof(out)) == 0;
    }
    return same;
}

/* The latest speech of kind (or any), from the log. */
static const host_voice_log_t *last_speech(int kind)
{
    refresh();
    const host_voice_log_t *e = NULL;
    for (unsigned i = 0; i < s_n; i++) {
        if (s_log[i].speech && (kind < 0 || s_log[i].kind == (unsigned)kind)) {
            e = &s_log[i];
        }
    }
    return e;
}

static void mixer_pass(ui_backend_t *B, const uint16_t *words)
{
    const vp_pack_t *pack = B->voice_pack_acquire(NULL);
    snd_mixer_t start;
    unsigned peaks[3] = {0, 0, 0};
    const uint8_t vols[3] = {30, 70, 100};
    for (unsigned v = 0; v < 3u; v++) { /* nonzero Voice volumes, against the shared mixer */
        settle(5000);
        B->set_voice_volume(NULL, vols[v]);
        host_panel_voice_log_clear();
        check(B->voice_preview(NULL, words, 2) == UI_VOICE_PREVIEW_OK, "Preview plays");
        settle(3000);
        const host_voice_log_t *e = last_speech(ANN_KIND_PREVIEW);
        check(e != NULL && e->completed, "Preview completes");
        check(e != NULL && host_panel_voice_mixer_at_start(&start) && start.voice_volume == vols[v] &&
                  matches_mixer(e, pack, start),
              "the WAV is the shared mixer's output at the Voice volume");
        peaks[v] = e != NULL ? peak_of(read_wav(e)) : 0u;
    }
    check(peaks[0] > 0u && peaks[0] < peaks[1] && peaks[1] < peaks[2],
          "a louder Voice volume gives a louder WAV");
    B->set_voice_volume(NULL, 70);

    /* Arming: the words play during the exit delay; the slow exit beeps wait for them (AUD-034). */
    settle(5000);
    host_panel_voice_log_clear();
    check(command(ALARM_CMD_ARM, ALARM_LEVEL_AWAY, 0) == ALARM_OK, "arm away (mixing)");
    settle(6000);
    const host_voice_log_t *e = last_speech(ANN_KIND_ARMING);
    check(e != NULL && e->completed && host_panel_voice_mixer_at_start(&start) &&
              (start.cont_sound == ALARM_SOUND_EXIT || start.cont_sound == ALARM_SOUND_EXIT_FAST),
          "arming speaks over the exit pattern");
    check(e != NULL && matches_mixer(e, pack, start),
          "exit-delay mixing: the WAV is the shared mixer's output");
    /* AUD-034: the exit beeps wait for the words, so none sounds under them. On this card the words
     * start in the block after the acknowledgement's PLAYED, so this proves the hold under the
     * words only; the panel's gap of a few blocks before them, held by speech due, is the sounder
     * suite's (test_the_first_exit_beep_waits_for_the_arming_words). */
    snd_mixer_t quiet = start;
    snd_set_continuous(&quiet, ALARM_SOUND_NONE);
    check(e != NULL && start.cont_sound == ALARM_SOUND_EXIT && matches_mixer(e, pack, quiet),
          "no exit beep under the arming words (AUD-034)");
    check(command(ALARM_CMD_DISARM, ALARM_LEVEL_NONE, 0) == ALARM_OK, "disarm (mixing)");
    settle(5000);

    /* An alarm preempts: the chime's words are cut and the WAV stops there. */
    host_panel_voice_log_clear();
    host_panel_zone(ZONE, ALARM_ZEV_FAULT);
    unsigned t = 0;
    while (t < 5000u && find_speech(0, ANN_KIND_CHIME) < (refresh(), 0)) {
        settle(10);
        t += 10u;
    }
    settle(300);
    host_panel_zone(6, ALARM_ZEV_FAULT); /* the kitchen smoke detector */
    settle(2000);
    e = last_speech(ANN_KIND_CHIME);
    check(e != NULL && e->cancelled && !e->completed, "an alarm cancels the speech being said");
    check(e != NULL && e->wav_samples > 0u && e->wav_samples < e->total_32k,
          "the cancelled utterance's WAV stops at the cancellation");
    check(e != NULL && host_panel_voice_mixer_at_start(&start) && matches_mixer(e, pack, start),
          "the recorded part is the shared mixer's output");
    host_panel_zone(6, ALARM_ZEV_RESTORE);
    host_panel_zone(ZONE, ALARM_ZEV_RESTORE);
    check(command(ALARM_CMD_DISARM, ALARM_LEVEL_NONE, 0) == ALARM_OK, "disarm after the alarm");
    settle(5000);
    B->voice_pack_release(NULL, pack);
}

static void power_pass(ui_backend_t *B, const uint16_t *words)
{
    const power_sample_t mains = {.ac_present = POWER_YES, .battery_mv = -1, .battery_percent = -1};
    const power_sample_t battery = {.ac_present = POWER_NO, .battery_present = POWER_YES,
                                    .battery_mv = 3900, .battery_percent = -1};
    const power_sample_t unknown = {.battery_mv = -1, .battery_percent = -1};
    host_panel_power_sample(&mains);
    settle(5000);
    host_panel_voice_log_clear();
    check(!host_panel_power_saving(), "power: mains is not saving");
    check(B->voice_preview(NULL, words, 2) == UI_VOICE_PREVIEW_OK, "power: Preview on mains");
    settle(3000);

    /* qualified battery operation: 15 s of evidence, and the speech is cancelled on entry */
    host_panel_power_sample(&battery);
    settle(14700); /* a two-word Preview lasts about half a second */
    check(!host_panel_power_saving(), "power: 14.7 s of battery is not enough");
    host_panel_voice_log_clear();
    check(B->voice_preview(NULL, words, 2) == UI_VOICE_PREVIEW_OK, "power: Preview just before saving");
    settle(200);
    check(!host_panel_power_saving(), "power: still not saving at 14.9 s");
    settle(600);
    check(host_panel_power_saving(), "power: 15 s of battery is saving");
    const host_voice_log_t *e = last_speech(ANN_KIND_PREVIEW);
    check(e != NULL && e->cancelled && !e->completed && e->wav_samples < e->total_32k,
          "power: entering saving cancels the speech");
    settle(3000);
    check(B->voice_preview(NULL, words, 2) == UI_VOICE_PREVIEW_QUIET, "power: no Preview while saving");
    host_panel_voice_log_clear();
    zone_pulse();
    refresh();
    check(find_speech(0, -1) < 0, "power: no automatic speech while saving");

    /* unknown readings keep it */
    host_panel_power_sample(&unknown);
    settle(40000);
    check(host_panel_power_saving(), "power: unknown readings retain saving");

    /* confirmed external power restores it after 15 s */
    host_panel_power_sample(&mains);
    settle(14000);
    check(host_panel_power_saving(), "power: mains for 14 s is not yet restored");
    settle(2000);
    check(!host_panel_power_saving(), "power: mains for 15 s restores");
    host_panel_voice_log_clear();
    check(B->voice_preview(NULL, words, 2) == UI_VOICE_PREVIEW_OK, "power: Preview after restoration");
    settle(3000);
    check(last_speech(ANN_KIND_PREVIEW) != NULL, "power: the speech plays again");
}

/* Each engine call's events are committed with its own snapshot, however long the advance. */
static void commit_pass(void)
{
    settle(5000);
    host_panel_voice_log_clear();
    check(command(ALARM_CMD_WALK_TEST, ALARM_LEVEL_NONE, ALARM_WALK_START) == ALARM_OK, "walk (long)");
    settle(4000);
    refresh();
    const int started = find_speech(0, ANN_KIND_WALK_START);
    const uint64_t walk_at = started >= 0 ? s_log[started].at_ms : 0u;
    host_panel_voice_log_clear();
    const alarm_ms_t span = 3u * 60u * 60u * 1000u; /* one call: the walk expires inside it */
    host_panel_advance(span);
    refresh();
    const int s = find_speech(0, ANN_KIND_WALK_END);
    check(s >= 0, "a walk that expires inside one long advance says its end");
    check(s >= 0 && s_log[s].completed, "walk expiry: said to the end");
    if (s >= 0) { /* said when it expired (an hour), not held to the end of the advance */
        check(started >= 0 && s_log[s].at_ms < walk_at + 90u * 60u * 1000u,
              "walk expiry: said when it happened, not at the end of the advance");
    }
    host_panel_voice_log_clear();
    settle(5000);
    check(command(ALARM_CMD_ARM, ALARM_LEVEL_AWAY, 0) == ALARM_OK, "arm away (long)");
    host_panel_voice_log_clear();
    host_panel_advance(180000u); /* the exit delay ends inside it */
    refresh();
    const int arm = find_speech(0, ANN_KIND_ARMED);
    check(arm < 0 || (s_log[arm].count >= 2u && s_log[arm].completed),
          "arming over a long advance: the armed words are whole");
    (void)command(ALARM_CMD_DISARM, ALARM_LEVEL_NONE, 0);
    settle(5000);
}

static void bind_pass(ui_backend_t *B, const uint16_t *words)
{
    static alarm_config_t cfg;
    B->get_config(NULL, &cfg);
    const alarm_zone_config_t *z = &cfg.zones[ZONE - 1u];
    check(z->voice_binding_revision != 0u && z->voice_approved_revision == z->voice_binding_revision,
          "binds: the name is approved");
    zone_hub_info_t info;
    check(B->zone_info(NULL, ZONE, &info), "binds: the zone has a sensor");
    zone_hub_binding_t same = info.binding;
    same.in[ZSRC_ROLE_MAIN].map.invert = !same.in[ZSRC_ROLE_MAIN].map.invert;
    check(B->zone_bind(NULL, ZONE, &same) == ZONE_HUB_OK, "binds: a mapping-only change binds");
    B->get_config(NULL, &cfg);
    check(cfg.zones[ZONE - 1u].voice_approved_revision == cfg.zones[ZONE - 1u].voice_binding_revision,
          "binds: a mapping-only change keeps the approval");
    check(B->zone_bind(NULL, ZONE, &info.binding) == ZONE_HUB_OK, "binds: mapping restored");

    zone_hub_binding_t other = info.binding;
    other.in[ZSRC_ROLE_MAIN].addr = zsrc_sim_address(30);
    check(B->zone_bind(NULL, ZONE, &other) == ZONE_HUB_OK, "binds: the zone gets another sensor");
    B->get_config(NULL, &cfg);
    check(cfg.zones[ZONE - 1u].voice_approved_revision != cfg.zones[ZONE - 1u].voice_binding_revision,
          "binds: a reassigned sensor loses the approval");
    host_panel_voice_log_clear();
    host_panel_zone(ZONE, ALARM_ZEV_FAULT);
    settle(3000);
    host_panel_zone(ZONE, ALARM_ZEV_RESTORE);
    settle(3000);
    refresh();
    check(find_speech(0, ANN_KIND_CHIME) < 0, "binds: the old name is not said for the new sensor");

    B->zone_unbind(NULL, ZONE);
    check(B->zone_bind(NULL, ZONE, &info.binding) == ZONE_HUB_OK, "binds: unbind and rebind");
    B->get_config(NULL, &cfg);
    check(cfg.zones[ZONE - 1u].voice_approved_revision != cfg.zones[ZONE - 1u].voice_binding_revision,
          "binds: a rebound zone needs its name approved again");
    check(approve_stored(B, ZONE, words, 2) == UI_VOICE_APPROVE_OK, "binds: approved again");
    settle(100);
    host_panel_voice_log_clear();
    host_panel_zone(ZONE, ALARM_ZEV_FAULT);
    settle(3000);
    host_panel_zone(ZONE, ALARM_ZEV_RESTORE);
    settle(3000);
    refresh();
    check(find_speech(0, ANN_KIND_CHIME) >= 0, "binds: the approved name is said again");
}

/* The pass through the simulator's audio core: chime tone, then the words, mixed by
 * snd_render_pcm. */
static void sim_pass(void)
{
    sim_audio_start_offline();
    const host_panel_sound_t snd = {.ctx = NULL,
                                    .continuous = sim_audio_continuous,
                                    .volume = sim_audio_volume,
                                    .tone = sim_audio_tone,
                                    .ticket = sim_audio_ticket,
                                    .speak = sim_audio_speak,
                                    .speech_cancel = sim_audio_speech_cancel,
                                    .speech_done = sim_audio_speech_done,
                                    .speech_due = sim_audio_speech_due};
    host_panel_sound(&snd);
    host_panel_voice_log_clear();
    host_panel_zone(ZONE, ALARM_ZEV_FAULT);
    unsigned loud_blocks = 0, speech_ms = 0;
    uint32_t start_ms = 0;
    for (unsigned t = 0; t < 4000u; t += 10u) {
        host_panel_advance(10);
        int16_t block[320 * 2];
        sim_audio_render(block, 1);
        bool loud = false;
        for (unsigned i = 0; i < 320u * 2u; i++) {
            loud = loud || block[i] != 0;
        }
        loud_blocks += loud ? 1u : 0u;
        refresh();
        const int s = find_speech(0, ANN_KIND_CHIME);
        if (s >= 0 && start_ms == 0u) {
            start_ms = t;
        }
        if (s >= 0) {
            speech_ms = (s_log[s].total_32k + 31u) / 32u;
        }
    }
    refresh();
    check(find_speech(0, ANN_KIND_CHIME) >= 0, "simulator audio: the chime's words start");
    /* 980 ms of chime, then the words (some of their length is silence between the words) */
    check(loud_blocks * 10u > 980u + speech_ms / 2u,
          "simulator audio: the mixer rendered the tone and then the speech");
    host_panel_zone(ZONE, ALARM_ZEV_RESTORE);
    settle(100);
    host_panel_sound(NULL);
}

int voice_walk(const char *shots_dir)
{
    s_checks = s_failed = 0;
    s_speech = 0;
    char *dir = s_dir;
    snprintf(s_dir, sizeof(s_dir), "%s/../voice", shots_dir);
    host_panel_init(1789857660000LL);
    host_panel_automations(false);
    ui_backend_t B;
    host_panel_backend(&B);
    ui_voice_info_t info;
    B.voice_info(NULL, &info);
    if (info.state != UI_VOICE_READY) {
        printf("voice walk: no voice in this build: skipped\n");
        return 0;
    }
#if defined(_WIN32)
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
    /* What the walks before this one said (the editor's Previews) had no WAV folder: not ours. */
    host_panel_voice_log_clear();
    host_panel_voice_out(dir);
    clear_log();

    /* A spoken name on the window zone, approved against the pack. */
    const vp_pack_t *pack = B.voice_pack_acquire(NULL);
    uint16_t cursor = 0, words[2] = {0, 0};
    vp_clip_t clip;
    for (unsigned i = 0; pack != NULL && i < 2u && vp_next_in_category(pack, 0, &cursor, &clip); i++) {
        words[i] = clip.id;
    }
    check(words[0] != 0u && words[1] != 0u, "two selectable words in the pack");
    static alarm_config_t cfg;
    B.get_config(NULL, &cfg);
    for (unsigned i = 0; i < ALARM_ZONE_WORDS; i++) {
        alarm_zone_set_word(&cfg.zones[ZONE - 1u], i, i < 2u ? words[i] : 0u);
    }
    /* D07: saved and approved in one transaction, against the identity seen before the save */
    const ui_voice_seen_t seen = voice_seen_now(&B, ZONE);
    alarm_result_t saved = ALARM_ERR_BAD_STATE;
    check(B.voice_apply_approve(NULL, &cfg, ZONE, &seen, words, 2, &saved) == UI_VOICE_APPROVE_OK,
          "the zone's words approved");
    check(saved == ALARM_OK, "the zone's words stored");
    /* What the panel says for them: a name's last word as its ending, when the pack has one
     * (AUD-031); a pack made before word forms has none, and the word itself plays. */
    uint16_t said[2] = {words[0], words[1]};
    vp_clip_t end;
    if (pack != NULL && vp_find_ending(pack, words[1], &end)) {
        said[1] = end.id;
    }
    check(pack == NULL || (pack->ending_count == 0u) == (said[1] == words[1]),
          "the name's last word played as its ending when the pack has endings");
    B.voice_pack_release(NULL, pack);
    settle(100);
    check(command(ALARM_CMD_CHIME, ALARM_LEVEL_NONE, 0) == ALARM_OK, "chime mode on");
    settle(3000);

    /* Announcements off (the factory setting): the chime plays its tone and nothing is said. */
    host_panel_voice_prefs(false, true, true);
    clear_log();
    zone_pulse();
    refresh();
    check(s_n >= 1u && !s_log[0].speech && s_log[0].sound == ALARM_SOUND_CHIME,
          "Announcements off: the chime tone plays");
    check(find_speech(0, -1) < 0, "Announcements off: nothing is said");

    /* On: the chime tone, then the zone's words. */
    host_panel_voice_prefs(true, true, true);
    clear_log();
    host_panel_zone(ZONE, ALARM_ZEV_FAULT);
    settle(3000);
    host_panel_zone(ZONE, ALARM_ZEV_RESTORE);
    settle(3000);
    refresh();
    int s = find_speech(0, ANN_KIND_CHIME);
    check(s_n >= 2u && !s_log[0].speech && s_log[0].sound == ALARM_SOUND_CHIME && s == 1,
          "chime: the tone first, then the words");
    check(s >= 0 && s_log[s].zone == ZONE && clips_are(&s_log[s], said, 2),
          "chime: the zone's spoken name");
    check(s >= 0 && s_log[s].at_ms >= s_log[0].at_ms + 980u,
          "chime: the words wait for the tone's last block");

    /* Arming away: Arming, Away, You may exit now. */
    clear_log();
    check(command(ALARM_CMD_ARM, ALARM_LEVEL_AWAY, 0) == ALARM_OK, "arm away");
    settle(4000);
    refresh();
    s = find_speech(0, ANN_KIND_ARMING);
    const uint16_t arming[3] = {VP_ID_ARMING, VP_ID_AWAY, VP_ID_EXIT_NOW};
    check(s >= 0 && clips_are(&s_log[s], arming, 3), "arming away: Arming, Away, You may exit now");

    /* Disarm. */
    clear_log();
    check(command(ALARM_CMD_DISARM, ALARM_LEVEL_NONE, 0) == ALARM_OK, "disarm");
    settle(2000);
    refresh();
    s = find_speech(0, ANN_KIND_DISARMED);
    const uint16_t disarmed[1] = {VP_ID_DISARMED};
    check(s >= 0 && clips_are(&s_log[s], disarmed, 1), "disarm: Disarmed");

    /* Walk test: its start words, a trip says the zone's name, its end. */
    clear_log();
    check(command(ALARM_CMD_WALK_TEST, ALARM_LEVEL_NONE, ALARM_WALK_START) == ALARM_OK, "walk start");
    settle(4000);
    host_panel_zone(ZONE, ALARM_ZEV_FAULT);
    settle(4000);
    host_panel_zone(ZONE, ALARM_ZEV_RESTORE);
    settle(2000);
    check(command(ALARM_CMD_WALK_TEST, ALARM_LEVEL_NONE, ALARM_WALK_END) == ALARM_OK, "walk end");
    settle(4000);
    refresh();
    const uint16_t wstart[2] = {VP_ID_WALK_TEST, VP_ID_WALK_STARTED};
    s = find_speech(0, ANN_KIND_WALK_START);
    check(s >= 0 && clips_are(&s_log[s], wstart, 2), "walk test: Walk test, started");
    s = find_speech(0, ANN_KIND_WALK_TRIP);
    check(s >= 0 && s_log[s].zone == ZONE && clips_are(&s_log[s], said, 2),
          "walk test: a trip says the zone's name");
    check(find_speech(0, ANN_KIND_WALK_END) >= 0, "walk test: the end is said");

    /* Announcements off again: arming, disarming and a chime say nothing. */
    host_panel_voice_prefs(false, true, true);
    clear_log();
    (void)command(ALARM_CMD_ARM, ALARM_LEVEL_AWAY, 0);
    settle(2000);
    (void)command(ALARM_CMD_DISARM, ALARM_LEVEL_NONE, 0);
    settle(2000);
    zone_pulse();
    refresh();
    check(find_speech(0, -1) < 0, "Announcements off: arming, disarming and the chime say nothing");

    /* Announcements off, a walk test still says a zone's words: it has no switch (WALK-016). */
    clear_log();
    check(command(ALARM_CMD_WALK_TEST, ALARM_LEVEL_NONE, ALARM_WALK_START) == ALARM_OK,
          "walk start, Announcements off");
    settle(4000);
    host_panel_zone(ZONE, ALARM_ZEV_FAULT);
    settle(4000);
    host_panel_zone(ZONE, ALARM_ZEV_RESTORE);
    settle(2000);
    check(command(ALARM_CMD_WALK_TEST, ALARM_LEVEL_NONE, ALARM_WALK_END) == ALARM_OK,
          "walk end, Announcements off");
    settle(4000);
    refresh();
    s = find_speech(0, ANN_KIND_WALK_TRIP);
    check(s >= 0 && s_log[s].zone == ZONE && clips_are(&s_log[s], said, 2),
          "Announcements off: a walk test trip still says the zone's name");

    clear_log();
    check(s_speech >= 6u, "the walk produced its utterances");

    host_panel_voice_prefs(true, true, true);
    mixer_pass(&B, words);
    clear_log();
    bind_pass(&B, words);
    clear_log();
    commit_pass();
    clear_log();
    power_pass(&B, words);
    clear_log();

    /* The simulator's mixer, offline. */
    sim_pass();

    printf("voice walk: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed;
}
