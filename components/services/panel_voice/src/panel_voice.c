// panel_voice.c - the panel's voice service (panel_voice.h; docs/features/08 voice-first-build.md
// D05, D10, D11, D12; AUD-027, AUD-028).
//
// One immutable copy of the installed pack ("primary", kind 2) lives in PSRAM with leases: the
// worker holds one for each utterance, the editor for a screen. Installing swaps in a new copy
// by generation; the old one is freed when its last lease ends.
//
// Tasks:
//   - the alarm task feeds the policy (core annunciator) through panel_voice_engine_*: each
//     copies one fact into a preallocated queue with a zero-tick send - no lock, no allocation,
//     no logging - and the worker applies them in order under s_mu (AUD-011);
//   - the worker (priority 4, a 256 KiB PSRAM stack for libopus's alloca, D10) asks the policy
//     for the next utterance, decodes it (voice_render) and feeds the sounder's speech ring; it
//     never touches flash or NVS;
//   - flash work (the start-up load, an install whose caller's stack is in PSRAM) runs on a
//     short-lived task with an internal-RAM stack, deleted when it is done.
// s_mu guards the policy, the copies, the zone words cache and the counters; nothing slow runs
// under it (no decode, no flash, no engine call), and the alarm task never takes it.
#include "panel_voice.h"

#include "sdkconfig.h"

const char *panel_voice_install_name(panel_voice_install_t r)
{
    static const char *const names[] = {"ok",       "unavailable", "too_big", "bad_signature", "bad_pack",
                                        "locked",   "busy",        "no_space", "failed"};
    return (unsigned)r < sizeof(names) / sizeof(names[0]) ? names[r] : "unknown";
}

#if CONFIG_ALARM_FEATURE_VOICE

#include <stdatomic.h>
#include <string.h>

#include "alarm_mem.h"
#include "alarm_service.h"
#include "annunciator/annunciator.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "pack_assets.h"
#include "panel_voice_internal.h"
#include "psa/crypto.h"
#include "sounder.h"
#include "sounder/speech_boost.h"
#include "voice_codec.h"
#include "voice_render.h"
#include "voice_pack/vp_accept.h"
#include "zone_service.h"

static const char *TAG = "voice";

#define PV_NAME "primary"
#define PV_KEYPAD 1u                      /* the local keypad's partition */
#define PV_WORKER_STACK (256u * 1024u)    /* PSRAM: libopus's alloca scratch (D10) */
#define PV_WORKER_PRIO 4
#define PV_FLASH_STACK 10240u             /* internal RAM, only while it runs (like lang_packs') */
#define PV_FLASH_PRIO 3
#define PV_COPIES 2u                      /* the active copy and one more: reserved, pending or retired */
#define PV_CHUNK (2u * SOUNDER_SPEECH_SLOT_SAMPLES) /* 20 ms at 32 kHz: one packet's worth */
#define PV_BELL_TEST_MS 2000u             /* the bell test pulse (zbz_wd.h ZBW_TEST_S); no OFF follows it */
#define PV_UTT_LIMIT_MS 15000u            /* 10 s of speech plus slack: a stuck utterance is cancelled */
#define PV_VERIFY_TRIES 5

typedef struct {
    bool used;
    bool active;
    uint32_t leases;
    uint8_t *buf;
    size_t len;
    vp_pack_t pack; /* points into buf */
} pv_copy_t;

typedef struct {
    bool on;
    bool rendered;      /* vr_pull returned 0: sounder_speech_finish sent */
    bool pulled_all;    /* vr_pull returned 0: what the boost's limiter holds back follows */
    ann_utt_t utt;
    pv_copy_t *copy;    /* leased for the utterance */
    uint32_t gen;       /* the sounder's speech generation */
    size_t have, off;   /* samples in s_pcm, and how many went to the ring */
    uint64_t started;
} pv_speech_t;

/* ---- state (s_mu) ---- */
static SemaphoreHandle_t s_mu;
static StaticSemaphore_t s_mu_buf;
static _Atomic bool s_inited;
static EXT_RAM_BSS_ATTR ann_t s_ann;
static EXT_RAM_BSS_ATTR pv_copy_t s_copies[PV_COPIES];
static pv_copy_t *s_active;
static panel_voice_state_t s_state = PANEL_VOICE_NONE;
static const char *s_why = "";
static panel_voice_prefs_t s_prefs = {.status = true, .chime = true, .volume = 70u};
static bool s_saving;
static uint64_t s_bell_due;          /* the bell test pulse ends; 0 = none */
static uint32_t s_preview_cutoff;    /* Preview utterances with an id up to this are stopped */
static uint32_t s_cfg_seen;          /* the newest snapshot's config_seq */
static uint32_t s_cfg_cached;        /* the config_seq the zone words came from */
static bool s_cfg_valid;
static EXT_RAM_BSS_ATTR ann_zone_words_t s_words[ALARM_MAX_ZONES];
static uint32_t s_utterances, s_completed, s_cancelled, s_underruns, s_decode_errors;

/* ---- the worker's own (no lock) ---- */
static TaskHandle_t s_worker;
static void *s_codec;
static size_t s_codec_bytes;
static EXT_RAM_BSS_ATTR vr_t s_vr;
static EXT_RAM_BSS_ATTR speech_boost_t s_sb; /* AUD-033: an utterance's or the bench's */
static EXT_RAM_BSS_ATTR int16_t s_pcm[PV_CHUNK];
static EXT_RAM_BSS_ATTR alarm_config_t s_cfg;
static pv_speech_t s_sp;
static uint32_t s_speech_gen;

static _Atomic bool s_installing;

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static void lock(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mu);
}

static void wake(void)
{
    if (s_worker != NULL) {
        xTaskNotifyGive(s_worker);
    }
}

static bool stack_is_internal(void)
{
    int probe = 0;
    return !esp_ptr_external_ram(&probe);
}

const char *pv_state_name(panel_voice_state_t s)
{
    static const char *const names[] = {"off", "none", "loading", "ready", "unavailable"};
    return (unsigned)s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}

const char *pv_preview_name(panel_voice_preview_t r)
{
    static const char *const names[] = {"ok", "unavailable", "bad_words", "too_long", "quiet"};
    return (unsigned)r < sizeof(names) / sizeof(names[0]) ? names[r] : "?";
}

/* ---- the policy's ports (called by ann_* with s_mu held) ---- */

static bool port_zone_words(void *ctx, uint16_t zone, ann_zone_words_t *out)
{
    (void)ctx;
    /* Words of an older configuration never speak: until the worker has read the new one the
     * zone has no name (the tone plays as always). */
    if (!s_cfg_valid || s_cfg_cached != s_cfg_seen || zone == 0u || zone > ALARM_MAX_ZONES) {
        return false;
    }
    if (zone_service_voice_settling(zone)) {
        return false; /* D07: a changed sensor's approval and binding aren't stored yet */
    }
    *out = s_words[zone - 1u];
    return out->count != 0u;
}

static uint32_t port_clip_samples(void *ctx, uint16_t id)
{
    (void)ctx;
    return s_active != NULL ? ann_pack_clip_samples((void *)&s_active->pack, id) : 0u;
}

static void apply_prefs_locked(uint64_t now)
{
    const ann_prefs_t p = {.voice_enabled = s_prefs.enabled,
                           .voice_status = s_prefs.status,
                           .voice_chime = s_prefs.chime,
                           .volume = s_prefs.volume,
                           .power_saving = s_saving};
    ann_set_prefs(&s_ann, &p, now);
}

/* ---- the copies and their leases (s_mu held) ---- */

static void copy_free_locked(pv_copy_t *c)
{
    heap_caps_free(c->buf);
    memset(c, 0, sizeof(*c));
}

static void copy_release_locked(pv_copy_t *c)
{
    if (c->leases > 0u) {
        c->leases--;
    }
    if (!c->active && c->leases == 0u) {
        copy_free_locked(c);
    }
}

static pv_copy_t *copy_of(const vp_pack_t *pack)
{
    for (unsigned i = 0u; i < PV_COPIES; i++) {
        if (s_copies[i].used && &s_copies[i].pack == pack) {
            return &s_copies[i];
        }
    }
    return NULL;
}

/* A committed pack waiting for the gate to open (D12): its copy is filled, not active, and
 * leased by no one. Installs answer BUSY while it waits (its slot is the spare one). */
static pv_copy_t *s_pending;
static esp_timer_handle_t s_handover_timer;
static void handover_try(void);

/* Reserves the slot a new copy will fill, before anything is allocated or written: NULL while
 * the spare slot holds a retired copy still leased, a reservation or a pending copy. A reserved
 * slot is used, not active, with no buffer; copy_free_locked() gives it back. */
static pv_copy_t *slot_reserve_locked(void)
{
    for (unsigned i = 0u; i < PV_COPIES; i++) {
        if (!s_copies[i].used) {
            s_copies[i] = (pv_copy_t){.used = true};
            return &s_copies[i];
        }
    }
    return NULL;
}

/* ---- the worker ---- */

static void set_unavailable_locked(const char *why)
{
    s_state = PANEL_VOICE_UNAVAILABLE;
    s_why = why;
    const ann_pack_t none = {0};
    ann_set_pack(&s_ann, &none, now_ms());
}

static void speech_end(uint32_t *counter)
{
    lock();
    ann_done(&s_ann, s_sp.utt.id, now_ms());
    if (counter != NULL) {
        (*counter)++;
    }
    copy_release_locked(s_sp.copy);
    unlock();
    memset(&s_sp, 0, sizeof(s_sp));
}

static void speech_start(void)
{
    ann_utt_t utt;
    pv_copy_t *copy = NULL;
    lock();
    const bool got = s_active != NULL && ann_next(&s_ann, now_ms(), &utt);
    if (got) {
        copy = s_active;
        copy->leases++;
    }
    unlock();
    if (!got) {
        return;
    }
    s_sp = (pv_speech_t){.on = true, .utt = utt, .copy = copy, .started = now_ms()};
    lock();
    const bool stopped = utt.kind == (uint8_t)ANN_KIND_PREVIEW && utt.id <= s_preview_cutoff;
    s_utterances++;
    unlock();
    if (stopped) {
        speech_end(&s_cancelled);
        return;
    }
    vr_item_t items[ANN_CLIPS_MAX];
    for (unsigned i = 0u; i < utt.count && i < ANN_CLIPS_MAX; i++) {
        items[i] = (vr_item_t){.clip_id = utt.clips[i], .gap_before_ms = utt.gap_ms[i]};
    }
    const int rc = vr_begin(&s_vr, &copy->pack, items, utt.count, s_codec, s_codec_bytes);
    if (rc < 0) {
        ESP_LOGW(TAG, "utterance %u (kind %u) not started: %d", (unsigned)utt.id, (unsigned)utt.kind, rc);
        speech_end(&s_decode_errors);
        return;
    }
    if (++s_speech_gen == 0u) {
        s_speech_gen = 1u;
    }
    s_sp.gen = s_speech_gen;
    lock();
    const uint8_t volume = s_prefs.volume;
    unlock();
    sb_begin(&s_sb, volume); /* the Voice volume, up to +9 dB (AUD-033); it stays to the end */
}

/* Fills s_pcm from the renderer, through the Voice volume's boost, then with what it held back.
 * false: the utterance failed to decode (cancelled). */
static bool speech_fill(void)
{
    s_sp.have = 0u;
    s_sp.off = 0u;
    while (s_sp.have < PV_CHUNK) {
        if (s_sp.pulled_all) {
            const size_t held = sb_flush(&s_sb, s_pcm + s_sp.have, PV_CHUNK - s_sp.have);
            if (held == 0u) {
                break;
            }
            s_sp.have += held;
            continue;
        }
        const int32_t n = vr_pull(&s_vr, s_pcm + s_sp.have, PV_CHUNK - s_sp.have);
        if (n < 0) {
            return s_sp.have > 0u; /* what came first plays; the next pull reports the error */
        }
        if (n == 0) {
            s_sp.pulled_all = true;
            continue;
        }
        sb_process(&s_sb, s_pcm + s_sp.have, (size_t)n);
        s_sp.have += (size_t)n;
    }
    return true;
}

/* One step of the active utterance. */
static void speech_step(void)
{
    lock();
    const bool current = ann_current(&s_ann, s_sp.utt.id) &&
                         !(s_sp.utt.kind == (uint8_t)ANN_KIND_PREVIEW && s_sp.utt.id <= s_preview_cutoff);
    unlock();
    if (!current || now_ms() - s_sp.started > PV_UTT_LIMIT_MS) {
        sounder_speech_cancel(s_sp.gen);
        speech_end(&s_cancelled);
        return;
    }
    sounder_speech_status_t st;
    sounder_speech_status(&st);
    if (st.rejected_gen == s_sp.gen) {
        speech_end(&s_cancelled);
        return;
    }
    if (st.underrun_gen == s_sp.gen) {
        speech_end(&s_underruns);
        return;
    }
    if (s_sp.rendered) {
        if (st.done_gen == s_sp.gen) {
            speech_end(&s_completed);
        }
        return;
    }
    for (unsigned guard = 0u; guard < SOUNDER_SPEECH_SLOTS; guard++) {
        if (s_sp.off == s_sp.have) {
            if (!speech_fill()) {
                sounder_speech_cancel(s_sp.gen);
                speech_end(&s_decode_errors);
                return;
            }
            if (s_sp.have == 0u) {
                sounder_speech_finish(s_sp.gen);
                s_sp.rendered = true;
                return;
            }
        }
        const size_t left = s_sp.have - s_sp.off;
        sounder_speech_status(&st);
        if (st.free_samples < (left < SOUNDER_SPEECH_SLOT_SAMPLES ? left : SOUNDER_SPEECH_SLOT_SAMPLES)) {
            return; /* the ring is full: the next wake */
        }
        const size_t took = sounder_speech_write(s_pcm + s_sp.off, left, s_sp.gen);
        if (took == 0u) {
            return; /* refused (the status says why at the next step) */
        }
        s_sp.off += took;
    }
}

#if CONFIG_ALARM_BUILD_DEV
/* ---- the audio bench (development builds; AUD-030) ----------------------------------------
 * PCM from the PC, 16 kHz mono, upsampled by D11's rule (vr_up) and fed to the sounder's speech
 * ring as an utterance is: the Voice volume's boost (AUD-033), the panel's speaker, mixer and
 * bench silence, without a pack or the policy. The worker plays it between utterances; a newer
 * one replaces it. */
#define PV_BENCH_SLACK_MS 5000u
static int16_t *s_bench_next;   /* s_mu: waiting for the worker */
static size_t s_bench_next_n;
typedef struct {
    bool on, rendered;
    int16_t *pcm;
    size_t n, pos;      /* 16 kHz samples, and how many went to the upsampler */
    size_t have, off;   /* 32 kHz samples in s_pcm, and how many went to the ring */
    bool up_done;       /* the upsampler flushed: the boost's held samples follow */
    uint32_t gen;
    uint64_t started;
    vr_up_t up;
} pv_bench_t;
static pv_bench_t s_bench; /* the worker's own */

static bool bench_on(void)
{
    return s_bench.on;
}

static void bench_end(void)
{
    heap_caps_free(s_bench.pcm);
    memset(&s_bench, 0, sizeof(s_bench));
}

static bool bench_waiting(void)
{
    lock();
    const bool waiting = s_bench_next != NULL;
    unlock();
    return waiting;
}

static void bench_start(void)
{
    lock();
    int16_t *pcm = s_bench_next;
    const size_t n = s_bench_next_n;
    s_bench_next = NULL;
    unlock();
    if (pcm == NULL) {
        return;
    }
    s_bench = (pv_bench_t){.on = true, .pcm = pcm, .n = n, .started = now_ms()};
    vr_up_reset(&s_bench.up);
    if (++s_speech_gen == 0u) {
        s_speech_gen = 1u;
    }
    s_bench.gen = s_speech_gen;
    lock();
    const uint8_t volume = s_prefs.volume;
    unlock();
    sb_begin(&s_sb, volume); /* as an utterance (AUD-033) */
    ESP_LOGI(TAG, "bench: %u ms of audio at Voice %u %%", (unsigned)(n / 16u), (unsigned)volume);
}

static void bench_fill(void)
{
    s_bench.have = 0u;
    s_bench.off = 0u;
    while (s_bench.have + 2u <= PV_CHUNK && s_bench.pos < s_bench.n) {
        s_bench.have += vr_up_push(&s_bench.up, s_bench.pcm[s_bench.pos++], s_pcm + s_bench.have);
    }
    if (s_bench.pos == s_bench.n && !s_bench.up_done && s_bench.have + 2u <= PV_CHUNK) {
        s_bench.have += vr_up_flush(&s_bench.up, s_pcm + s_bench.have);
        s_bench.up_done = true;
    }
    sb_process(&s_sb, s_pcm, s_bench.have); /* the Voice volume, as an utterance's (AUD-033) */
    if (s_bench.up_done) {                  /* then what its limiter held back */
        s_bench.have += sb_flush(&s_sb, s_pcm + s_bench.have, PV_CHUNK - s_bench.have);
    }
}

/* One step of the bench's audio, as speech_step steps an utterance. */
static void bench_step(void)
{
    if (bench_waiting() || now_ms() - s_bench.started > s_bench.n / 16u + PV_BENCH_SLACK_MS) {
        sounder_speech_cancel(s_bench.gen); /* replaced, or stuck */
        bench_end();
        return;
    }
    sounder_speech_status_t st;
    sounder_speech_status(&st);
    if (st.rejected_gen == s_bench.gen || st.underrun_gen == s_bench.gen) {
        ESP_LOGW(TAG, "bench: %s", st.rejected_gen == s_bench.gen ? "refused (an alarm sounds)" : "underrun");
        bench_end();
        return;
    }
    if (s_bench.rendered) {
        if (st.done_gen == s_bench.gen) {
            bench_end();
        }
        return;
    }
    for (unsigned guard = 0u; guard < SOUNDER_SPEECH_SLOTS; guard++) {
        if (s_bench.off == s_bench.have) {
            bench_fill();
            if (s_bench.have == 0u) {
                sounder_speech_finish(s_bench.gen);
                s_bench.rendered = true;
                return;
            }
        }
        const size_t left = s_bench.have - s_bench.off;
        sounder_speech_status(&st);
        if (st.free_samples < (left < SOUNDER_SPEECH_SLOT_SAMPLES ? left : SOUNDER_SPEECH_SLOT_SAMPLES)) {
            return; /* the ring is full: the next wake */
        }
        const size_t took = sounder_speech_write(s_pcm + s_bench.off, left, s_bench.gen);
        if (took == 0u) {
            return; /* refused (the status says why at the next step) */
        }
        s_bench.off += took;
    }
}

panel_voice_bench_t panel_voice_bench_play(int16_t *pcm, size_t n)
{
    if (pcm == NULL || n == 0u || n > PANEL_VOICE_BENCH_MAX_SAMPLES) {
        return PANEL_VOICE_BENCH_TOO_LONG;
    }
    if (!atomic_load(&s_inited) || s_worker == NULL) {
        return PANEL_VOICE_BENCH_UNAVAILABLE;
    }
    lock();
    int16_t *old = s_bench_next;
    s_bench_next = pcm;
    s_bench_next_n = n;
    unlock();
    heap_caps_free(old);
    wake();
    return PANEL_VOICE_BENCH_OK;
}
#else
static bool bench_on(void)
{
    return false;
}
static bool bench_waiting(void)
{
    return false;
}
static void bench_start(void)
{
}
static void bench_step(void)
{
}
panel_voice_bench_t panel_voice_bench_play(int16_t *pcm, size_t n)
{
    (void)pcm;
    (void)n;
    return PANEL_VOICE_BENCH_UNAVAILABLE;
}
#endif

/* Reads the configuration again when it changed (off the alarm task: the engine is called here,
 * never from a listener). */
static void words_refresh(void)
{
    lock();
    const bool stale = !s_cfg_valid || s_cfg_cached != s_cfg_seen;
    const uint32_t seq = s_cfg_seen;
    unlock();
    if (!stale || alarm_service_get_config(&s_cfg) != ESP_OK) {
        return;
    }
    lock();
    for (unsigned z = 0u; z < ALARM_MAX_ZONES; z++) {
        const alarm_zone_config_t *zc = &s_cfg.zones[z];
        ann_zone_words_t *w = &s_words[z];
        memset(w, 0, sizeof(*w));
        const unsigned n = alarm_zone_word_count(zc);
        for (unsigned i = 0u; i < n && i < ANN_CLIPS_MAX; i++) {
            w->ids[i] = alarm_zone_word(zc, i);
        }
        w->count = (uint8_t)(n < ANN_CLIPS_MAX ? n : ANN_CLIPS_MAX);
        w->binding_revision = zc->voice_binding_revision;
        w->approved_revision = zc->voice_approved_revision;
        w->pack_revision = zc->voice_pack_revision;
    }
    s_cfg_cached = seq;
    s_cfg_valid = true;
    unlock();
}

/* the engine's facts, queued (below "the engine's facts, queued") */
static void q_start(void);
static void q_take_baseline(void);
static void q_drain(void);
static void q_tickets(void);
static bool q_waiting(void);

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        q_drain(); /* the engine's facts first: a tone's outcome must find its ann_sound */
        words_refresh();
        q_tickets();
        lock();
        if (s_bell_due != 0u && now_ms() >= s_bell_due) {
            s_bell_due = 0u;
            ann_bell_test_done(&s_ann, now_ms());
        }
        const bool due_now = ann_due(&s_ann, now_ms());
        unlock();
        /* AUD-034: as soon as the arming words are decided, while their tone still plays, so
         * the exit beeps after it wait for them */
        sounder_speech_due(due_now);
        if (!s_sp.on && !bench_on() && bench_waiting()) {
            bench_start(); /* the audio bench (dev builds): between utterances */
        }
        if (bench_on()) {
            bench_step();
        }
        if (!s_sp.on && !bench_on()) {
            speech_start();
        }
        if (s_sp.on) {
            speech_step();
        }
        /* Speaking: back within 10 ms to keep the 120 ms ring full. Waiting (requests, tones
         * whose outcome comes from the sounder, the bell pulse): 20 ms. Idle: a notification. */
        lock();
        const bool waiting = ann_waiting(&s_ann) > 0u || s_bell_due != 0u || s_ann.active_used;
        const bool due = ann_due(&s_ann, now_ms());
        unlock();
        sounder_speech_due(due); /* AUD-034: and off once the words have ended or expired */
        const bool held = q_waiting(); /* an outcome whose tone isn't drained yet, a baseline */
        TickType_t wait = (s_sp.on || bench_on()) ? pdMS_TO_TICKS(10)
                          : (waiting || held) ? pdMS_TO_TICKS(20)
                                              : pdMS_TO_TICKS(1000);
        if (wait == 0) {
            wait = 1;
        }
        (void)ulTaskNotifyTake(pdTRUE, wait);
    }
}

/* The decoder state and the worker, once, before the first copy is marked ready (D10). false:
 * the voice can't play on this panel. */
static bool resources_once(const char **why)
{
    if (s_worker != NULL) {
        return true;
    }
    sounder_speech_status_t st;
    sounder_speech_status(&st);
    if (!st.ready) { /* a rate alone is no proof that anything will be heard */
        *why = st.rate_hz != 0u ? "no memory for the speech buffer"
                                : "no speaker (the sounder did not start)";
        return false;
    }
    if (st.rate_hz != VR_SAMPLE_RATE) {
        *why = "the speaker's rate is not 32 kHz";
        return false;
    }
    if (s_codec == NULL) {
        s_codec_bytes = vc_state_bytes();
        if (s_codec_bytes == 0u) {
            *why = "decoder state over 64 KiB";
            return false;
        }
        s_codec = heap_caps_malloc(s_codec_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_codec == NULL || vc_init(s_codec, s_codec_bytes) != 0) {
            heap_caps_free(s_codec);
            s_codec = NULL;
            *why = "no memory for the decoder";
            return false;
        }
    }
    if (alarm_mem_task_create(worker, "voice", PV_WORKER_STACK, NULL, PV_WORKER_PRIO, &s_worker) != pdPASS) {
        s_worker = NULL;
        *why = "no memory for the voice worker";
        return false;
    }
    return true;
}

/* Makes a filled, reserved slot (an accepted pack whose identity record matches) the active copy;
 * takes the slot in every case. When the decoder can't be prepared the slot is given back and the
 * working copy, if any, stays active. */
static void activate(pv_copy_t *slot)
{
    const char *why = "";
    const bool res = resources_once(&why);
    lock();
    if (s_pending == slot) {
        s_pending = NULL;
    }
    if (!res) {
        copy_free_locked(slot);
        const bool kept = s_active != NULL;
        if (!kept) {
            set_unavailable_locked(why);
        }
        unlock();
        ESP_LOGE(TAG, "voice pack not usable: %s (%s)", why, kept ? "the working voice stays" : "tones unchanged");
        return;
    }
    slot->active = true;
    pv_copy_t *old = s_active;
    s_active = slot;
    if (old != NULL) {
        old->active = false;
        if (old->leases == 0u) {
            copy_free_locked(old);
        }
    }
    s_state = PANEL_VOICE_READY;
    s_why = "";
    ann_cancel_all(&s_ann); /* whatever plays is from the copy going away */
    const vp_pack_t pack = slot->pack;
    const ann_pack_t p = {.ready = true,
                          .revision = pack.pack_revision,
                          .word_gap_ms = pack.word_gap_ms,
                          .part_gap_ms = pack.part_gap_ms};
    ann_set_pack(&s_ann, &p, now_ms());
    unlock();
    wake();
    ESP_LOGI(TAG, "voice pack ready: %s %s revision %u, %u clips%s", pack.locale, pack.voice_id,
             (unsigned)pack.pack_revision, (unsigned)pack.clip_count, vp_is_draft(&pack) ? " (DRAFT)" : "");
}

/* ---- acceptance and identity records (D08, D12; NVS: internal-RAM stacks only) ---- */

#if CONFIG_ALARM_BUILD_RELEASE
#define PV_RELEASE true
#else
#define PV_RELEASE false
#endif
#define PV_ID_NS "voice_id" /* one 88-byte blob per identity (vp_accept.h), never removed */
#define PV_ID_MAX 16u       /* identities this panel remembers; the 17th is refused */

typedef enum {
    PV_ID_OK = 0,  /* boot: the record matches; install: it matches or was just written */
    PV_ID_UNKNOWN, /* boot: no record (a pack from before the records, or the Flasher's): recorded then */
    PV_ID_REFUSED, /* another digest under this identity, or a key collision */
    PV_ID_FULL,    /* install or boot: PV_ID_MAX records already */
    PV_ID_ERROR,   /* NVS could not be read or written, or the digest not computed */
} pv_id_t;

static bool payload_digest(const vp_pack_t *pack, uint8_t out[VP_DIGEST_SIZE])
{
    size_t n = 0u;
    return psa_crypto_init() == PSA_SUCCESS &&
           psa_hash_compute(PSA_ALG_SHA_256, pack->base, pack->payload_length, out, VP_DIGEST_SIZE, &n) ==
               PSA_SUCCESS &&
           n == VP_DIGEST_SIZE;
}

/* What the key holds against the candidate; false when NVS could not be read. */
static bool id_lookup(nvs_handle_t h, const char *key, const vp_identity_t *cand, vp_id_verdict_t *v)
{
    uint8_t rec[VP_ID_RECORD_SIZE];
    size_t n = sizeof(rec);
    const esp_err_t err = nvs_get_blob(h, key, rec, &n);
    vp_identity_t stored = {0};
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *v = vp_identity_judge(NULL, false, cand);
    } else if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        *v = vp_identity_judge(&stored, false, cand); /* not a record of ours: a collision */
    } else if (err == ESP_OK) {
        const bool ok = vp_identity_decode(rec, n, &stored);
        *v = vp_identity_judge(&stored, ok, cand);
    } else {
        ESP_LOGE(TAG, "voice identity %s: read failed (%s)", key, esp_err_to_name(err));
        return false;
    }
    return true;
}

static pv_id_t id_of_verdict(vp_id_verdict_t v, const char *key)
{
    switch (v) {
    case VP_ID_SAME:
        return PV_ID_OK;
    case VP_ID_NEW:
        return PV_ID_UNKNOWN;
    case VP_ID_OTHER_AUDIO:
        ESP_LOGE(TAG, "voice identity %s: this revision was installed before with other audio", key);
        return PV_ID_REFUSED;
    case VP_ID_COLLISION:
    default:
        ESP_LOGE(TAG, "voice identity %s: the key holds another identity", key);
        return PV_ID_REFUSED;
    }
}

/* Digest and identity of an accepted pack. */
static bool identity_of(const vp_pack_t *pack, vp_identity_t *id, char key[VP_ID_KEY_SIZE])
{
    uint8_t d[VP_DIGEST_SIZE];
    if (!payload_digest(pack, d)) {
        ESP_LOGE(TAG, "voice pack digest failed");
        return false;
    }
    vp_identity_of(pack, d, id);
    vp_identity_key(id, key);
    return true;
}

/* Boot: the stored pack must match a record written when it was installed; load() records one
 * it has none of. Reads only. */
static pv_id_t id_verify(const vp_pack_t *pack)
{
    vp_identity_t id;
    char key[VP_ID_KEY_SIZE];
    if (!identity_of(pack, &id, key)) {
        return PV_ID_ERROR;
    }
    nvs_handle_t h;
    const esp_err_t err = nvs_open(PV_ID_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return PV_ID_UNKNOWN; /* no record ever written */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "voice identities: open failed (%s)", esp_err_to_name(err));
        return PV_ID_ERROR;
    }
    vp_id_verdict_t v;
    const bool read = id_lookup(h, key, &id, &v);
    nvs_close(h);
    return read ? id_of_verdict(v, key) : PV_ID_ERROR;
}

static unsigned id_count(void)
{
    unsigned n = 0u;
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, PV_ID_NS, NVS_TYPE_ANY, &it);
    while (err == ESP_OK) {
        n++;
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    return n;
}

/* Install, before the table commit: a known identity must carry the same digest; a new one is
 * written and committed durably first. Refuses NVS errors, exhaustion and key collisions. */
static pv_id_t id_reserve(const vp_pack_t *pack)
{
    if (!stack_is_internal()) {
        return PV_ID_ERROR; /* never: validate runs on the install task */
    }
    vp_identity_t id;
    char key[VP_ID_KEY_SIZE];
    if (!identity_of(pack, &id, key)) {
        return PV_ID_ERROR;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(PV_ID_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "voice identities: open failed (%s)", esp_err_to_name(err));
        return PV_ID_ERROR;
    }
    vp_id_verdict_t v;
    pv_id_t r = PV_ID_ERROR;
    if (id_lookup(h, key, &id, &v)) {
        r = id_of_verdict(v, key);
    }
    if (r == PV_ID_UNKNOWN) {
        if (id_count() >= PV_ID_MAX) {
            ESP_LOGE(TAG, "voice identities full (%u): this panel takes no new pack revision", (unsigned)PV_ID_MAX);
            r = PV_ID_FULL;
        } else {
            uint8_t rec[VP_ID_RECORD_SIZE];
            vp_identity_encode(&id, rec);
            err = nvs_set_blob(h, key, rec, sizeof(rec));
            if (err == ESP_OK) {
                err = nvs_commit(h);
            }
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "voice identity %s recorded: %s %s revision %u", key, id.locale, id.voice_id,
                         (unsigned)id.revision);
                r = PV_ID_OK;
            } else {
                ESP_LOGE(TAG, "voice identity %s: write failed (%s)", key, esp_err_to_name(err));
                r = err == ESP_ERR_NVS_NOT_ENOUGH_SPACE ? PV_ID_FULL : PV_ID_ERROR;
            }
        }
    }
    nvs_close(h);
    return r;
}

/* The panel's acceptance of a reader-valid pack, for the boot load and the install alike. */
static bool accepted(const vp_pack_t *pack)
{
    const vp_accept_t a = vp_accept(pack, PV_RELEASE);
    if (a != VP_ACCEPT_OK) {
        ESP_LOGE(TAG, "voice pack refused (%s): %s %s revision %u%s", vp_accept_name(a), pack->locale,
                 pack->voice_id, (unsigned)pack->pack_revision, vp_is_draft(pack) ? " DRAFT" : "");
        return false;
    }
    return true;
}

/* ---- start-up: read "primary" (internal-stack task) ---- */

static const char *verify_why(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_IMAGE_INVALID:
        return "not signed for this panel";
    case ESP_ERR_NOT_SUPPORTED:
        return "this firmware checks no signatures";
    case ESP_ERR_NOT_FINISHED:
        return "the signature check was busy (an update)";
    default:
        return "the signature check failed";
    }
}

static void load(void)
{
    const char *why = NULL;
    uint8_t *buf = NULL;
    size_t len = 0u;
    pa_lock();
    const ps_entry_t *e = pa_find(PS_KIND_VOICE, PV_NAME);
    if (e != NULL) {
        len = e->length;
        if (len < PA_MIN_FILE || len > PA_VOICE_MAX_FILE || len % 4096u != 0u) {
            why = "stored copy has a wrong size";
        } else if ((buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) == NULL) {
            why = "no memory for the pack";
        } else if (!pa_read(e, buf)) {
            why = "could not be read";
        }
    }
    pa_unlock();
    if (e == NULL) {
        lock();
        s_state = PANEL_VOICE_NONE;
        unlock();
        ESP_LOGI(TAG, "no voice pack installed: tones only");
        return;
    }
    if (why == NULL) {
        esp_err_t err = ESP_ERR_NOT_FINISHED;
        for (int i = 0; i < PV_VERIFY_TRIES && err == ESP_ERR_NOT_FINISHED; i++) {
            err = pa_verify(buf, (uint32_t)len, false);
            if (err == ESP_ERR_NOT_FINISHED) {
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
        }
        if (err != ESP_OK) {
            why = verify_why(err);
        }
    }
    vp_pack_t pack;
    vp_problem_t pr = {0};
    if (why == NULL && vp_open_signed(buf, len, &pack, &pr) != VP_OK) {
        ESP_LOGE(TAG, "voice pack refused: %s (entry %u)", vp_err_name(pr.err), (unsigned)pr.entry);
        why = "damaged or not a voice pack";
    }
    if (why == NULL && !accepted(&pack)) {
        why = vp_is_draft(&pack) && PV_RELEASE ? "a draft pack: this build takes released packs only"
                                               : "not this panel's voice";
    }
    if (why == NULL) {
        switch (id_verify(&pack)) {
        case PV_ID_OK:
            break;
        case PV_ID_UNKNOWN:
            /* Stored without this panel's install (the Flasher writes the store over USB): signed for
             * this panel and accepted above, so its identity is recorded now, as an install records
             * it. A known identity with other audio stays refused (PV_ID_REFUSED). */
            switch (id_reserve(&pack)) {
            case PV_ID_OK:
                break;
            case PV_ID_FULL:
                why = "the panel has recorded too many voice revisions";
                break;
            case PV_ID_REFUSED:
                why = "differs from its recorded identity: reinstall the pack";
                break;
            default:
                why = "its identity could not be recorded";
                break;
            }
            break;
        case PV_ID_REFUSED:
            why = "differs from its recorded identity: reinstall the pack";
            break;
        default:
            why = "its identity could not be checked";
            break;
        }
    }
    pv_copy_t *slot = NULL;
    if (why == NULL) {
        lock();
        slot = slot_reserve_locked();
        if (slot != NULL) {
            slot->buf = buf;
            slot->len = len;
            slot->pack = pack;
        }
        unlock();
        if (slot == NULL) {
            why = "older copies still in use: restart the panel";
        }
    }
    if (why != NULL) {
        heap_caps_free(buf);
        lock();
        set_unavailable_locked(why);
        unlock();
        ESP_LOGE(TAG, "voice pack not usable: %s (tones unchanged)", why);
        return;
    }
    activate(slot);
}

static void load_task(void *arg)
{
    (void)arg;
    load();
    vTaskDelete(NULL);
}

esp_err_t panel_voice_start(void)
{
    if (atomic_load(&s_inited)) {
        return ESP_OK;
    }
    s_mu = xSemaphoreCreateMutexStatic(&s_mu_buf);
    q_start();
    const ann_ports_t ports = {.ctx = NULL, .zone_words = port_zone_words, .clip_samples = port_clip_samples};
    ann_init(&s_ann, &ports, PV_KEYPAD);
    apply_prefs_locked(now_ms());
    s_state = PANEL_VOICE_LOADING;
    atomic_store(&s_inited, true);
    q_take_baseline(); /* the silent baseline, before any live fact (D05) */

    const pa_result_t r = pa_init(); /* once; whichever of languages and voice starts first */
    if (r != PA_OK) {
        lock();
        set_unavailable_locked("no assets store");
        unlock();
        ESP_LOGW(TAG, "no voice: the assets store is %s", pa_result_name(r));
        return ESP_OK;
    }
    if (xTaskCreate(load_task, "voice_load", PV_FLASH_STACK, NULL, PV_FLASH_PRIO, NULL) != pdPASS) {
        lock();
        set_unavailable_locked("no memory to load the pack");
        unlock();
        ESP_LOGE(TAG, "voice pack not loaded: no memory for its task");
    }
    return ESP_OK;
}

void panel_voice_status(panel_voice_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->why = "";
    if (!atomic_load(&s_inited)) {
        out->state = PANEL_VOICE_NONE;
        return;
    }
    handover_try();
    lock();
    out->state = s_state;
    out->why = s_why;
    if (s_active != NULL && s_state == PANEL_VOICE_READY) {
        const vp_pack_t *p = &s_active->pack;
        out->draft = vp_is_draft(p);
        memcpy(out->locale, p->locale, sizeof(out->locale));
        memcpy(out->voice_id, p->voice_id, sizeof(out->voice_id));
        out->pack_revision = p->pack_revision;
        out->clips = p->clip_count;
        out->file_bytes = (uint32_t)s_active->len;
    }
    out->utterances = s_utterances;
    out->completed = s_completed;
    out->cancelled = s_cancelled;
    out->underruns = s_underruns;
    out->decode_errors = s_decode_errors;
    out->enabled = s_prefs.enabled;
    out->volume = s_prefs.volume;
    if (s_active != NULL && s_state == PANEL_VOICE_READY) {
        out->word_gap_ms = s_ann.pack.word_gap_ms != 0u ? s_ann.pack.word_gap_ms : (uint16_t)ANN_WORD_GAP_MS;
    }
    unlock();
}

bool panel_voice_ready(void)
{
    if (!atomic_load(&s_inited)) {
        return false;
    }
    lock();
    const bool ready = s_state == PANEL_VOICE_READY;
    unlock();
    return ready;
}

/* ---- preferences ---- */

void panel_voice_set_prefs(const panel_voice_prefs_t *prefs)
{
    if (prefs == NULL) {
        return;
    }
    if (!atomic_load(&s_inited)) {
        s_prefs = *prefs; /* the start-up task, before panel_voice_start */
        return;
    }
    lock();
    s_prefs = *prefs;
    apply_prefs_locked(now_ms());
    unlock();
    wake();
}

void panel_voice_set_volume(uint8_t volume)
{
    if (!atomic_load(&s_inited)) {
        s_prefs.volume = volume;
        return;
    }
    lock();
    s_prefs.volume = volume;
    apply_prefs_locked(now_ms());
    unlock();
    wake();
}

void panel_voice_set_power_saving(bool saving)
{
    if (!atomic_load(&s_inited)) {
        s_saving = saving;
        return;
    }
    lock();
    s_saving = saving;
    apply_prefs_locked(now_ms());
    unlock();
    wake();
}

/* ---- the engine's facts, queued (AUD-011; docs/features/08 D05, D11) ----
 * The alarm task copies each fact into s_q with a zero-tick send; the worker applies them in
 * order. A full queue loses a fact: s_q_drops counts it and every later item carries the count,
 * so the worker cancels all speech (ann_rebaseline), drops what it has of the broken stretch and
 * takes the next snapshot sent after the loss as a silent baseline, asking the alarm task for
 * one at once. The worker also starts that way: the first snapshot it applies is the baseline
 * panel_voice_start took on the alarm task before the listeners were turned on. */

#define PV_Q_LEN 48u              /* one engine call's events, tones, outputs and snapshot, many times */
#define PV_TICKET_HOLD_MS 50u     /* an outcome waits this long for its tone's ann_sound at most */

typedef enum { PVQ_EVENT = 0, PVQ_SOUND, PVQ_OUTPUT, PVQ_SNAPSHOT } pv_kind_t;

typedef struct {
    uint8_t kind;      /* pv_kind_t */
    bool baseline;     /* the snapshot q_baseline() took (a call, not a change) */
    uint32_t epoch;    /* s_q_drops when it was sent */
    uint64_t at;       /* captured (now_ms) */
    union {
        alarm_event_t ev;
        struct {
            uint32_t ticket;
            alarm_sound_t sound;
            uint16_t zone;
            uint8_t partition;
        } snd;
        struct {
            alarm_output_t output;
            alarm_pattern_t pattern;
            uint8_t partition;
        } out;
        ann_state_t st;
    } u;
} pv_item_t;

/* Only the alarm task sends; only the worker receives. The storage is touched by no ISR or DMA. */
static QueueHandle_t s_q;
static StaticQueue_t s_q_buf;
static EXT_RAM_BSS_ATTR pv_item_t s_q_store[PV_Q_LEN];
static _Atomic bool s_collect;      /* the listeners copy (set on the alarm task, with the baseline) */
static _Atomic uint32_t s_q_drops;  /* facts lost to a full queue */
static EXT_RAM_BSS_ATTR alarm_snapshot_t s_q_snap; /* q_baseline's, on the alarm task */

/* the worker's own */
static uint32_t s_q_epoch;          /* the drops the worker has dealt with */
static bool s_q_resync = true;      /* waiting for a baseline: from start-up, after a loss */
static bool s_q_asked;              /* a baseline was asked of the alarm task for this resync */
static uint32_t s_q_ticket_fed;     /* the newest tone ticket handed to ann_sound */
static bool s_tk_held;
static sounder_ticket_t s_tk;
static uint64_t s_tk_since;

/* Alarm task only (a listener): never blocks, never logs. */
static void q_put(pv_item_t *it)
{
    it->epoch = atomic_load(&s_q_drops);
    if (xQueueSendToBack(s_q, it, 0) != pdTRUE) {
        atomic_fetch_add(&s_q_drops, 1u);
    }
    wake();
}

/* Runs on the alarm task (alarm_service_call at start, alarm_service_post after a loss): queues
 * the state as it is now as a silent baseline, then turns the listeners on - on that same task,
 * so no live fact can come before it (D05). */
static void q_baseline(alarm_engine_t *e, alarm_ms_t now, void *ctx)
{
    (void)now;
    (void)ctx;
    alarm_service_get_snapshot(&s_q_snap); /* current: the alarm task refreshes it after every call */
    pv_item_t it = {.kind = PVQ_SNAPSHOT, .baseline = true, .at = now_ms()};
    ann_state_from_snapshot(&s_q_snap, PV_KEYPAD, alarm_engine_config(e)->opt.exit_sounds, &it.u.st);
    q_put(&it);
    atomic_store(&s_collect, true);
}

static void q_start(void)
{
    s_q = xQueueCreateStatic(PV_Q_LEN, sizeof(pv_item_t), (uint8_t *)s_q_store, &s_q_buf);
}

/* panel_voice_start (the start-up task): the baseline is taken on the alarm task, which turns the
 * listeners on there. Not taken (the engine isn't running): the worker asks for it once it runs. */
static void q_take_baseline(void)
{
    s_q_asked = alarm_service_call(q_baseline, NULL) == ESP_OK;
    if (!s_q_asked) {
        ESP_LOGW(TAG, "voice baseline not taken yet: the alarm task isn't running");
    }
}

/* Worker: a fact was lost since the last look - cancel everything and wait for a baseline. */
static void q_check_drops(void)
{
    const uint32_t d = atomic_load(&s_q_drops);
    if (d == s_q_epoch) {
        return;
    }
    s_q_epoch = d;
    lock();
    ann_rebaseline(&s_ann);
    s_bell_due = 0u;
    unlock();
    if (!s_q_resync) {
        ESP_LOGW(TAG, "voice: the engine's facts overflowed; silent until a new baseline");
    }
    s_q_resync = true;
    s_q_asked = false;
}

static void q_apply(const pv_item_t *it)
{
    q_check_drops();
    if (it->kind == PVQ_SOUND && it->u.snd.ticket != 0u &&
        (int32_t)(it->u.snd.ticket - s_q_ticket_fed) > 0) {
        s_q_ticket_fed = it->u.snd.ticket; /* dropped or applied, it has come */
    }
    if (s_q_resync) {
        /* Facts before the loss, or of a call cut by it, are dropped; the first snapshot sent
         * after it is a complete state: the silent baseline (the policy was rebaselined). */
        if (it->kind != PVQ_SNAPSHOT || it->epoch != s_q_epoch) {
            return;
        }
        s_q_resync = false;
    } else if (it->baseline) {
        return; /* asked for, but a live snapshot came first: nothing new in it */
    }
    lock();
    switch ((pv_kind_t)it->kind) {
    case PVQ_EVENT:
        ann_event(&s_ann, &it->u.ev);
        break;
    case PVQ_SOUND:
        ann_sound(&s_ann, it->u.snd.partition, it->u.snd.sound, it->u.snd.zone, it->u.snd.ticket);
        break;
    case PVQ_OUTPUT:
        ann_output(&s_ann, it->u.out.output, it->u.out.partition, it->u.out.pattern);
        if (it->u.out.output == ALARM_OUT_BELL && it->u.out.partition == PV_KEYPAD &&
            it->u.out.pattern == ALARM_PAT_TEST) {
            s_bell_due = it->at + PV_BELL_TEST_MS;
        }
        break;
    case PVQ_SNAPSHOT:
        s_cfg_seen = it->u.st.config_seq;
        ann_commit(&s_ann, &it->u.st, it->at);
        break;
    }
    unlock();
}

static void q_drain(void)
{
    pv_item_t it;
    while (xQueueReceive(s_q, &it, 0) == pdTRUE) {
        q_apply(&it);
    }
    q_check_drops();
    if (s_q_resync && !s_q_asked) {
        s_q_asked = alarm_service_post(q_baseline, NULL) == ESP_OK; /* else the next round */
    }
}

/* The sounder's tone outcomes, in order. One whose tone the worker hasn't drained yet (the alarm
 * task is between sounder_play_tracked and its send) waits for it, PV_TICKET_HOLD_MS at most - its
 * fact was lost then, and ann_ticket ignores a ticket it doesn't know. */
static void q_tickets(void)
{
    for (;;) {
        if (!s_tk_held) {
            if (!sounder_ticket_next(&s_tk)) {
                return;
            }
            s_tk_held = true;
            s_tk_since = now_ms();
        }
        if ((int32_t)(s_tk.ticket - s_q_ticket_fed) > 0) {
            q_drain();
            if ((int32_t)(s_tk.ticket - s_q_ticket_fed) > 0 && now_ms() - s_tk_since < PV_TICKET_HOLD_MS) {
                return; /* the next round */
            }
        }
        s_tk_held = false;
        lock();
        ann_ticket(&s_ann, s_tk.ticket, s_tk.outcome == SND_TICKET_PLAYED, now_ms());
        unlock();
    }
}

static bool q_waiting(void)
{
    return s_tk_held || (s_q_resync && !s_q_asked);
}

/* ---- the engine's facts (alarm task: copy into the queue, never a lock, never block) ---- */

void panel_voice_engine_event(const alarm_event_t *ev)
{
    if (!atomic_load(&s_collect) || ev == NULL) {
        return;
    }
    pv_item_t it = {.kind = PVQ_EVENT, .at = now_ms()};
    it.u.ev = *ev;
    q_put(&it);
}

void panel_voice_engine_sound(uint8_t partition, alarm_sound_t sound, uint16_t zone, uint32_t ticket)
{
    if (!atomic_load(&s_collect)) {
        return;
    }
    pv_item_t it = {.kind = PVQ_SOUND, .at = now_ms()};
    it.u.snd.partition = partition;
    it.u.snd.sound = sound;
    it.u.snd.zone = zone;
    it.u.snd.ticket = ticket;
    q_put(&it);
}

void panel_voice_engine_output(alarm_output_t output, uint8_t partition, alarm_pattern_t pattern)
{
    if (!atomic_load(&s_collect)) {
        return;
    }
    pv_item_t it = {.kind = PVQ_OUTPUT, .at = now_ms()};
    it.u.out.output = output;
    it.u.out.partition = partition;
    it.u.out.pattern = pattern;
    q_put(&it);
}

/* The state the policy reads, computed here (a pure function) with the configuration's
 * exit_sounds as the engine has it now - never the worker's cached copy (D05). Loud patterns
 * need nothing faster than this queue: the sounder's mixer itself rejects and ends speech in the
 * same block as the keypad partition's alarm, fire, CO or aux pattern (sounder_mix.c
 * snd_render_pcm, AUD-010), whatever the worker has drained; another partition's alarm is not
 * played on this speaker, and the policy cancels speech for it when it drains the snapshot. */
void panel_voice_engine_snapshot(const alarm_snapshot_t *snap)
{
    if (!atomic_load(&s_collect) || snap == NULL) {
        return;
    }
    pv_item_t it = {.kind = PVQ_SNAPSHOT, .at = now_ms()};
    ann_state_from_snapshot(snap, PV_KEYPAD, snap->exit_sounds, &it.u.st);
    q_put(&it);
}

/* ---- leases ---- */

const vp_pack_t *panel_voice_pack_acquire(void)
{
    if (!atomic_load(&s_inited)) {
        return NULL;
    }
    lock();
    const vp_pack_t *p = NULL;
    if (s_state == PANEL_VOICE_READY && s_active != NULL) {
        s_active->leases++;
        p = &s_active->pack;
    }
    unlock();
    return p;
}

void panel_voice_pack_release(const vp_pack_t *pack)
{
    if (pack == NULL || !atomic_load(&s_inited)) {
        return;
    }
    lock();
    pv_copy_t *c = copy_of(pack);
    if (c != NULL) {
        copy_release_locked(c);
    }
    unlock();
}

/* ---- Preview ---- */

static panel_voice_preview_t preview(const uint16_t *ids, size_t n)
{
    lock();
    const ann_preview_result_t r = ann_preview(&s_ann, ids, n, false, now_ms());
    unlock();
    wake();
    switch (r) {
    case ANN_PREVIEW_OK:
        return PANEL_VOICE_PREVIEW_OK;
    case ANN_PREVIEW_NO_PACK:
        return PANEL_VOICE_PREVIEW_UNAVAILABLE;
    case ANN_PREVIEW_EMPTY:
    case ANN_PREVIEW_TOO_MANY:
    case ANN_PREVIEW_BAD_CLIP:
        return PANEL_VOICE_PREVIEW_BAD_WORDS;
    case ANN_PREVIEW_TOO_LONG:
        return PANEL_VOICE_PREVIEW_TOO_LONG;
    case ANN_PREVIEW_MUTED:
    case ANN_PREVIEW_ALARM:
    case ANN_PREVIEW_BUSY:
    default:
        return PANEL_VOICE_PREVIEW_QUIET;
    }
}

panel_voice_preview_t panel_voice_preview(const uint16_t *ids, size_t n)
{
    const vp_pack_t *pack = panel_voice_pack_acquire();
    if (pack == NULL) {
        return PANEL_VOICE_PREVIEW_UNAVAILABLE;
    }
    vp_recipe_check_t chk;
    (void)vp_check_recipe(pack, ids, ids != NULL ? n : 0u, &chk);
    panel_voice_pack_release(pack);
    switch (chk.verdict) {
    case VP_RECIPE_OK:
        return preview(ids, n);
    case VP_RECIPE_TOO_LONG:
        return PANEL_VOICE_PREVIEW_TOO_LONG;
    default:
        return PANEL_VOICE_PREVIEW_BAD_WORDS;
    }
}

panel_voice_preview_t pv_preview_raw(const uint16_t *ids, size_t n)
{
    if (!panel_voice_ready()) {
        return PANEL_VOICE_PREVIEW_UNAVAILABLE;
    }
    return preview(ids, n);
}

#if CONFIG_ALARM_BUILD_DEV
panel_voice_preview_t panel_voice_bench_say(const uint16_t *ids, size_t n, uint16_t gap_ms,
                                            uint16_t *gap_used, uint8_t *volume)
{
    if (gap_ms > PANEL_VOICE_BENCH_GAP_MAX_MS) {
        return PANEL_VOICE_PREVIEW_BAD_WORDS;
    }
    if (!panel_voice_ready()) {
        return PANEL_VOICE_PREVIEW_UNAVAILABLE;
    }
    lock();
    if (s_active == NULL) {
        unlock();
        return PANEL_VOICE_PREVIEW_UNAVAILABLE;
    }
    ann_pack_t p = s_ann.pack;
    p.word_gap_ms = gap_ms != 0u ? gap_ms : s_active->pack.word_gap_ms;
    ann_set_pack(&s_ann, &p, now_ms()); /* the same revision: nothing waiting is dropped */
    const uint16_t used = p.word_gap_ms != 0u ? p.word_gap_ms : (uint16_t)ANN_WORD_GAP_MS;
    const uint8_t vol = s_prefs.volume;
    unlock();
    if (gap_used != NULL) {
        *gap_used = used;
    }
    if (volume != NULL) {
        *volume = vol;
    }
    ESP_LOGI(TAG, "bench say: %u words, %u ms apart, at Voice %u %%", (unsigned)n, (unsigned)used,
             (unsigned)vol);
    return preview(ids, n);
}
#else
panel_voice_preview_t panel_voice_bench_say(const uint16_t *ids, size_t n, uint16_t gap_ms,
                                            uint16_t *gap_used, uint8_t *volume)
{
    (void)ids;
    (void)n;
    (void)gap_ms;
    (void)gap_used;
    (void)volume;
    return PANEL_VOICE_PREVIEW_UNAVAILABLE;
}
#endif

void panel_voice_preview_stop(void)
{
    if (!atomic_load(&s_inited)) {
        return;
    }
    lock();
    s_preview_cutoff = s_ann.next_id; /* every Preview queued so far: ids only grow */
    unlock();
    wake();
}

/* ---- install (D12) ---- */

typedef struct {
    uint8_t *copy;
    size_t len;
    vp_pack_t pack;
    pv_copy_t *slot;                /* reserved before the copy was allocated */
    bool slot_taken;                /* install_done filled it (active or pending) */
    bool pending;                   /* committed while the gate was closed */
    panel_voice_install_t refusal;  /* validate's answer when it refuses */
} pv_install_ctx_t;

/* Armed (exit and entry included), an alarm, a walk test, installer mode: no install. The engine
 * has no fire test of its own yet; OTA is refused by pa_install's update claim. When it cannot
 * tell, it says locked. */
static bool install_locked(void)
{
    alarm_snapshot_t *snap = heap_caps_malloc(sizeof(*snap), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (snap == NULL) {
        return true;
    }
    alarm_service_get_snapshot(snap);
    bool locked = alarm_snapshot_locked(snap) || snap->program;
    for (unsigned p = 0u; p < ALARM_MAX_PARTITIONS; p++) {
        if ((snap->partitions & (1u << p)) != 0u && snap->part[p].walk_test) {
            locked = true;
        }
    }
    heap_caps_free(snap);
    return locked;
}

/* The pack's checks before anything is written (the store's table, the identity record): the
 * reader, the panel's acceptance, then the identity, recorded durably when it is new. */
static int install_validate(const uint8_t *file, size_t len, uint32_t payload_length, char name[PS_NAME_MAX + 1],
                            void *ctx)
{
    pv_install_ctx_t *ic = ctx;
    ic->refusal = PANEL_VOICE_INSTALL_BAD_PACK;
    vp_problem_t pr = {0};
    const vp_err_t err = vp_open_signed(file, len, &ic->pack, &pr);
    if (err != VP_OK) {
        ESP_LOGW(TAG, "install refused: %s (entry %u)", vp_err_name(pr.err), (unsigned)pr.entry);
        return (int)err;
    }
    if (ic->pack.payload_length != payload_length || !accepted(&ic->pack)) {
        return 100;
    }
    switch (id_reserve(&ic->pack)) {
    case PV_ID_OK:
        break;
    case PV_ID_REFUSED:
        return 101;
    case PV_ID_FULL:
        ic->refusal = PANEL_VOICE_INSTALL_NO_SPACE;
        return 102;
    default:
        ic->refusal = PANEL_VOICE_INSTALL_FAILED;
        return 103;
    }
    strlcpy(name, PV_NAME, PS_NAME_MAX + 1);
    return 0;
}

static bool install_gate(void *ctx)
{
    (void)ctx;
    return !install_locked();
}

/* The pending copy becomes active once the gate is open (D12): a RAM-only swap, from the
 * handover timer, a status call or the next install. */
static void handover_try(void)
{
    lock();
    const bool pending = s_pending != NULL;
    unlock();
    if (!pending || install_locked()) {
        return;
    }
    lock();
    pv_copy_t *slot = s_pending;
    s_pending = NULL; /* claimed: one caller swaps; the slot stays used, so installs stay BUSY */
    unlock();
    if (slot == NULL) {
        return;
    }
    if (s_handover_timer != NULL) {
        (void)esp_timer_stop(s_handover_timer);
    }
    ESP_LOGI(TAG, "install: the panel is open again, the new voice takes over");
    activate(slot);
}

static void handover_tick(void *arg)
{
    (void)arg;
    handover_try();
}

static void handover_arm(void)
{
    if (s_handover_timer == NULL) {
        const esp_timer_create_args_t a = {.callback = handover_tick, .name = "voice_handover"};
        if (esp_timer_create(&a, &s_handover_timer) != ESP_OK) {
            s_handover_timer = NULL;
            ESP_LOGW(TAG, "install: no handover timer; the next status call or install swaps");
            return;
        }
    }
    (void)esp_timer_stop(s_handover_timer);
    (void)esp_timer_start_periodic(s_handover_timer, 1000000u);
}

/* After the table commit (mutex and claim held): fills the reserved slot with the verified copy
 * and activates it, unless the gate closed meanwhile: then the working copy keeps playing and the
 * new one waits, pending, for the gate to open. Keeps nothing of ctx. */
static void install_done(const ps_entry_t *e, void *ctx)
{
    (void)e;
    pv_install_ctx_t *ic = ctx;
    const bool locked = install_locked();
    lock();
    ic->slot->buf = ic->copy;
    ic->slot->len = ic->len;
    ic->slot->pack = ic->pack;
    if (locked) {
        s_pending = ic->slot;
    }
    unlock();
    ic->copy = NULL;
    ic->slot_taken = true;
    if (locked) {
        ic->pending = true;
        ESP_LOGW(TAG, "install: stored, but the panel locked before activation; the working voice stays until it opens");
        handover_arm();
        return;
    }
    activate(ic->slot);
}

static panel_voice_install_t install_now(const uint8_t *file, size_t len)
{
    const char *why = "";
    if (!resources_once(&why)) { /* the decoder and worker before anything is committed */
        ESP_LOGE(TAG, "install refused: %s", why);
        return PANEL_VOICE_INSTALL_UNAVAILABLE;
    }
    pv_install_ctx_t ic = {.len = len, .refusal = PANEL_VOICE_INSTALL_BAD_PACK};
    lock();
    ic.slot = slot_reserve_locked();
    unlock();
    if (ic.slot == NULL) {
        ESP_LOGW(TAG, "install: busy (an older copy is still in use, or a new one waits)");
        return PANEL_VOICE_INSTALL_BUSY;
    }
    ic.copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ic.copy == NULL) {
        lock();
        copy_free_locked(ic.slot);
        unlock();
        return PANEL_VOICE_INSTALL_FAILED;
    }
    memcpy(ic.copy, file, len); /* the copy that will play: the caller's buffer is theirs */
    const pa_install_t req = {.kind = PS_KIND_VOICE,
                              .file = ic.copy,
                              .len = len,
                              .ff_padding = true,
                              .validate = install_validate,
                              .gate = install_gate,
                              .done = install_done,
                              .ctx = &ic};
    int reject = 0;
    const pa_result_t r = pa_install(&req, &reject);
    if (ic.copy != NULL) {
        heap_caps_free(ic.copy);
    }
    if (!ic.slot_taken) {
        lock();
        copy_free_locked(ic.slot); /* the reservation, with no buffer */
        unlock();
    }
    ESP_LOGI(TAG, "install: %s%s", pa_result_name(r), ic.pending ? " (pending: locked)" : "");
    switch (r) {
    case PA_OK:
        return ic.pending ? PANEL_VOICE_INSTALL_LOCKED : PANEL_VOICE_INSTALL_OK;
    case PA_ERR_REJECTED:
        return ic.refusal;
    case PA_ERR_OFF:
        return PANEL_VOICE_INSTALL_UNAVAILABLE;
    case PA_ERR_SIZE:
        return PANEL_VOICE_INSTALL_TOO_BIG;
    case PA_ERR_SIGNATURE:
    case PA_ERR_FORMAT:
        return PANEL_VOICE_INSTALL_BAD_SIGNATURE;
    case PA_ERR_BUSY:
        return PANEL_VOICE_INSTALL_BUSY;
    case PA_ERR_CLOSED:
        return PANEL_VOICE_INSTALL_LOCKED;
    case PA_ERR_NO_SPACE:
        return PANEL_VOICE_INSTALL_NO_SPACE;
    case PA_ERR_FLASH:
    default:
        return PANEL_VOICE_INSTALL_FAILED;
    }
}

typedef struct {
    const uint8_t *file;
    size_t len;
    panel_voice_install_t result;
    SemaphoreHandle_t done;
} pv_install_job_t;

static void install_task(void *arg)
{
    pv_install_job_t *job = arg;
    job->result = install_now(job->file, job->len);
    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
}

panel_voice_install_t panel_voice_install(const uint8_t *file, size_t len)
{
    if (!atomic_load(&s_inited) || !pa_ready()) {
        return PANEL_VOICE_INSTALL_UNAVAILABLE;
    }
    if (file == NULL || len < PA_MIN_FILE || len > PA_VOICE_MAX_FILE || len % 4096u != 0u) {
        return PANEL_VOICE_INSTALL_TOO_BIG;
    }
    if (((uintptr_t)file & 3u) != 0u) {
        return PANEL_VOICE_INSTALL_BAD_SIGNATURE; /* the envelope's words can't be read */
    }
    lock();
    const bool loading = s_state == PANEL_VOICE_LOADING;
    unlock();
    if (loading) {
        return PANEL_VOICE_INSTALL_BUSY; /* the start-up load would activate the older copy after it */
    }
    handover_try(); /* a pending copy first; still pending (locked): the reservation says BUSY */
    if (install_locked()) {
        return PANEL_VOICE_INSTALL_LOCKED;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_installing, &expected, true)) {
        return PANEL_VOICE_INSTALL_BUSY;
    }
    panel_voice_install_t r;
    if (stack_is_internal()) {
        r = install_now(file, len);
    } else {
        pv_install_job_t job = {.file = file, .len = len, .result = PANEL_VOICE_INSTALL_FAILED};
        job.done = xSemaphoreCreateBinary();
        if (job.done == NULL ||
            xTaskCreate(install_task, "voice_inst", PV_FLASH_STACK, &job, PV_FLASH_PRIO, NULL) != pdPASS) {
            r = PANEL_VOICE_INSTALL_FAILED;
        } else {
            xSemaphoreTake(job.done, portMAX_DELAY);
            r = job.result;
        }
        if (job.done != NULL) {
            vSemaphoreDelete(job.done);
        }
    }
    atomic_store(&s_installing, false);
    return r;
}

#else /* !CONFIG_ALARM_FEATURE_VOICE: stubs (docs/features/33 step 6) */

esp_err_t panel_voice_start(void)
{
    return ESP_OK;
}

void panel_voice_status(panel_voice_status_t *out)
{
    *out = (panel_voice_status_t){.state = PANEL_VOICE_OFF, .why = ""};
}

bool panel_voice_ready(void)
{
    return false;
}

void panel_voice_set_prefs(const panel_voice_prefs_t *prefs)
{
    (void)prefs;
}

void panel_voice_set_volume(uint8_t volume)
{
    (void)volume;
}

void panel_voice_set_power_saving(bool saving)
{
    (void)saving;
}

void panel_voice_engine_event(const alarm_event_t *ev)
{
    (void)ev;
}

void panel_voice_engine_sound(uint8_t partition, alarm_sound_t sound, uint16_t zone, uint32_t ticket)
{
    (void)partition;
    (void)sound;
    (void)zone;
    (void)ticket;
}

void panel_voice_engine_output(alarm_output_t output, uint8_t partition, alarm_pattern_t pattern)
{
    (void)output;
    (void)partition;
    (void)pattern;
}

void panel_voice_engine_snapshot(const alarm_snapshot_t *snap)
{
    (void)snap;
}

const vp_pack_t *panel_voice_pack_acquire(void)
{
    return NULL;
}

void panel_voice_pack_release(const vp_pack_t *pack)
{
    (void)pack;
}

panel_voice_preview_t panel_voice_preview(const uint16_t *ids, size_t n)
{
    (void)ids;
    (void)n;
    return PANEL_VOICE_PREVIEW_UNAVAILABLE;
}

void panel_voice_preview_stop(void)
{
}

panel_voice_install_t panel_voice_install(const uint8_t *file, size_t len)
{
    (void)file;
    (void)len;
    return PANEL_VOICE_INSTALL_UNAVAILABLE;
}

panel_voice_bench_t panel_voice_bench_play(int16_t *pcm, size_t n)
{
    (void)pcm;
    (void)n;
    return PANEL_VOICE_BENCH_UNAVAILABLE;
}

panel_voice_preview_t panel_voice_bench_say(const uint16_t *ids, size_t n, uint16_t gap_ms,
                                            uint16_t *gap_used, uint8_t *volume)
{
    (void)ids;
    (void)n;
    (void)gap_ms;
    (void)gap_used;
    (void)volume;
    return PANEL_VOICE_PREVIEW_UNAVAILABLE;
}

#endif /* CONFIG_ALARM_FEATURE_VOICE */
