/*
 * sounder.c - the ESP-IDF task that plays the portable mixer through the board's speakers.
 *
 * All the sound is in components/core/sounder; what is left here is the part that cannot be
 * portable: a queue so any task can ask for a sound without blocking (the task is woken by its
 * notification, which takes no queue slot), the task that renders
 * one 10 ms block at a time, and the amplifier the board switches on and off - plus the two
 * lock-free rings voice speech crosses (docs/features/08 voice-first-build.md D11): PCM in from
 * panel_voice, tone-ticket outcomes back out to it.
 */
#include "sounder.h"
#include "alarm_mem.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "sounder";

/* Only requests that carry something: a wake-up (speech, the continuous pattern) is the task's
 * notification, never a queue slot a chime, beep or command tone could have had. */
typedef enum {
    REQ_ONESHOT,
    REQ_VOLUME,
    REQ_BACKGROUND,
    REQ_CHIME_VOLUME,
    REQ_VOICE_VOLUME
} req_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t value;
    uint32_t ticket; /* REQ_ONESHOT from sounder_play_tracked; 0 = untracked */
} req_t;

static sounder_sink_t s_sink;
static QueueHandle_t s_queue;
/* Set once, after the task exists; nothing is queued before it (send_ticket checks it). */
static TaskHandle_t s_task;
/* Read on the sounder task, written from whichever task noticed the power change: it is one
 * flag, so it is carried by an atomic rather than through the queue - a screen full of key
 * clicks must never be able to push a change of power state out of a full queue. */
static bool s_power_saving;
/* The continuous pattern the engine wants (fire, CO, alarm, entry and exit time, trouble) is
 * state, not a request: the latest value wins, carried the same way, so a queue full of key
 * clicks and beeps can never drop the start - or the end - of an alarm. The task's notification
 * wakes it for a change. */
static uint8_t s_continuous = ALARM_SOUND_NONE;
/* Bench silence (development builds): read by the sounder task for every block. */
static bool s_silent;

/* ---- tone tickets: the sounder task produces, panel_voice consumes ---------------------- */
static uint32_t s_next_ticket;
static sounder_ticket_t s_tk[SOUNDER_TICKET_RING];
static uint32_t s_tk_head; /* written by the sounder task */
static uint32_t s_tk_tail; /* written by the reader */

/* ---- speech: panel_voice produces, the sounder task consumes ---------------------------- */
/* Speech is on its way (sounder_speech_due): read by the sounder task for every block. */
static bool s_speech_due;
#define SP_SLOTS SOUNDER_SPEECH_SLOTS
#define SP_WRAP  (2u * SP_SLOTS) /* head and tail run 0..SP_WRAP-1: full and empty differ */

typedef struct {
    uint32_t gen;
    uint32_t n; /* samples in pcm[] */
    int16_t pcm[SOUNDER_SPEECH_SLOT_SAMPLES];
} speech_slot_t;

static speech_slot_t *s_slots; /* SP_SLOTS, then the task's block of gathered speech */
static int16_t *s_gathered;
static uint32_t s_sp_head;      /* producer */
static uint32_t s_sp_tail;      /* consumer */
static uint32_t s_cancel_gen;   /* producer: this one and older are dropped */
static uint32_t s_finish_gen;   /* producer: this one and older get no more samples */
static uint32_t s_dead_gen;     /* consumer: rejected or ran dry, this one and older */
/* True once the task runs with the speech ring: before then, or without the ring, nothing
 * written would ever be heard (sounder_speech_status_t.ready). */
static bool s_speech_ready;
static uint32_t s_rate_hz; /* 0 until the task runs: then the output's */
static uint32_t s_sp_consumed, s_done_gen, s_rejected_gen, s_underrun_gen, s_rejected_n,
    s_underrun_n; /* written by the task, read by anyone */

static uint32_t ring_count(uint32_t head, uint32_t tail)
{
    return (head + SP_WRAP - tail) % SP_WRAP;
}

static uint32_t ring_next(uint32_t i)
{
    return (i + 1u) % SP_WRAP;
}

static bool gen_dead(uint32_t gen)
{
    return gen <= __atomic_load_n(&s_cancel_gen, __ATOMIC_ACQUIRE) ||
           gen <= __atomic_load_n(&s_dead_gen, __ATOMIC_ACQUIRE);
}

static void atomic_max(uint32_t *v, uint32_t x)
{
    uint32_t cur = __atomic_load_n(v, __ATOMIC_RELAXED);
    while (cur < x &&
           !__atomic_compare_exchange_n(v, &cur, x, true, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
    }
}

/* What one block may play: the samples of the oldest live generation, gathered into
 * s_gathered (at most 'frames'). */
typedef struct {
    uint32_t gen;  /* 0: no speech this block */
    size_t avail;  /* samples handed to the mixer */
    size_t total;  /* samples of gen buffered in all */
    bool finished; /* gen gets no more samples */
} speech_take_t;

static uint32_t s_sp_off;   /* consumer: samples already played from the tail slot */
static uint32_t s_open_gen; /* consumer: the generation playing, until its outcome is known */

static void speech_kill(uint32_t gen)
{
    atomic_max(&s_dead_gen, gen); /* its slots are dropped, its writes refused */
    if (gen == s_open_gen) {
        s_open_gen = 0u;
    }
}

static void speech_underrun(uint32_t gen)
{
    /* Ran dry: the utterance ends here; nothing is repeated or waited for (D11). */
    speech_kill(gen);
    __atomic_store_n(&s_underrun_gen, gen, __ATOMIC_RELEASE);
    __atomic_fetch_add(&s_underrun_n, 1u, __ATOMIC_RELAXED);
}

static void speech_done(uint32_t gen)
{
    speech_kill(gen);
    __atomic_store_n(&s_done_gen, gen, __ATOMIC_RELEASE);
}

static speech_take_t speech_gather(size_t frames)
{
    speech_take_t t = {0};
    if (s_slots == NULL) {
        return t;
    }
    /* The finish mark before the ring: every sample written before it is then in view. */
    const uint32_t fin = __atomic_load_n(&s_finish_gen, __ATOMIC_ACQUIRE);
    const uint32_t head = __atomic_load_n(&s_sp_head, __ATOMIC_ACQUIRE);
    uint32_t tail = s_sp_tail;
    while (tail != head && gen_dead(s_slots[tail % SP_SLOTS].gen)) {
        tail = ring_next(tail); /* canceled, rejected, ran dry or done: thrown away (AUD-028) */
        s_sp_off = 0u;
    }
    __atomic_store_n(&s_sp_tail, tail, __ATOMIC_RELEASE);
    if (s_open_gen != 0u && gen_dead(s_open_gen)) {
        s_open_gen = 0u; /* canceled by the producer: it knows */
    }
    if (s_open_gen != 0u && (tail == head || s_slots[tail % SP_SLOTS].gen != s_open_gen)) {
        if (s_open_gen <= fin) {
            speech_done(s_open_gen);
        } else {
            speech_underrun(s_open_gen);
        }
    }
    if (tail == head) {
        return t;
    }
    t.gen = s_slots[tail % SP_SLOTS].gen;
    t.finished = t.gen <= fin;
    uint32_t off = s_sp_off;
    for (uint32_t i = tail; i != head && s_slots[i % SP_SLOTS].gen == t.gen; i = ring_next(i)) {
        const speech_slot_t *sl = &s_slots[i % SP_SLOTS];
        const size_t have = sl->n - off;
        t.total += have;
        if (t.avail < frames) {
            const size_t k = have < frames - t.avail ? have : frames - t.avail;
            memcpy(s_gathered + t.avail, sl->pcm + off, k * sizeof(int16_t));
            t.avail += k;
        }
        off = 0u;
    }
    if (!t.finished && t.gen == s_open_gen && t.avail < frames) {
        speech_underrun(t.gen); /* never a block with a hole in an open utterance */
        t = (speech_take_t){0};
        return t;
    }
    const size_t prefetch = (size_t)s_rate_hz * SOUNDER_SPEECH_PREFETCH_MS / 1000u;
    if (!t.finished && t.gen != s_open_gen && t.total < prefetch) {
        t.avail = 0u; /* still filling: the amplifier warms up meanwhile */
    }
    return t;
}

static void speech_advance(size_t used)
{
    uint32_t tail = s_sp_tail;
    uint32_t off = s_sp_off;
    while (used > 0u) {
        const size_t have = s_slots[tail % SP_SLOTS].n - off;
        if (used >= have) {
            used -= have;
            tail = ring_next(tail);
            off = 0u;
        } else {
            off += (uint32_t)used;
            used = 0u;
        }
    }
    s_sp_off = off;
    __atomic_store_n(&s_sp_tail, tail, __ATOMIC_RELEASE);
}

/* After the block: what became of the generation offered. */
static void speech_settle(const speech_take_t *t, const snd_pcm_block_t *r)
{
    if (t->gen == 0u) {
        return;
    }
    if (r->speech_rejected) { /* alarm, fire, CO or aux (AUD-010) */
        speech_kill(t->gen);
        __atomic_store_n(&s_rejected_gen, t->gen, __ATOMIC_RELEASE);
        __atomic_fetch_add(&s_rejected_n, 1u, __ATOMIC_RELAXED);
        return;
    }
    if (r->speech_consumed > 0u) {
        speech_advance(r->speech_consumed);
        s_open_gen = t->gen;
        __atomic_fetch_add(&s_sp_consumed, (uint32_t)r->speech_consumed, __ATOMIC_RELAXED);
        if (t->finished && r->speech_consumed == t->total) {
            speech_done(t->gen);
        }
    }
}

static bool speech_buffered(void)
{
    return s_slots != NULL && __atomic_load_n(&s_sp_head, __ATOMIC_SEQ_CST) !=
                                  __atomic_load_n(&s_sp_tail, __ATOMIC_RELAXED);
}

/* Outcomes from the mixer's ring to the reader's, as far as there is room. */
static void tickets_drain(snd_mixer_t *mix)
{
    const uint32_t tail = __atomic_load_n(&s_tk_tail, __ATOMIC_ACQUIRE);
    uint32_t head = s_tk_head;
    snd_ticket_t t;
    while (head - tail < SOUNDER_TICKET_RING && snd_ticket_next(mix, &t)) {
        s_tk[head % SOUNDER_TICKET_RING] = t;
        head++;
    }
    __atomic_store_n(&s_tk_head, head, __ATOMIC_RELEASE);
}

/* Wake the sounder task: after every request queued, every speech write, finish and cancel, and
 * every change of the continuous pattern. A notification is latched, so one given before the task
 * looks is never lost; giving it twice costs nothing. */
static void notify_task(void)
{
    TaskHandle_t t = __atomic_load_n(&s_task, __ATOMIC_ACQUIRE);
    if (t != NULL) {
        (void)xTaskNotifyGive(t);
    }
}

static void sounder_task(void *arg)
{
    (void)arg;
    static int16_t block[SND_BLOCK_FRAMES * 2u];
    snd_mixer_t mix;
    snd_init(&mix);
    if (s_sink.sample_rate_hz != 0u && !snd_set_rate(&mix, s_sink.sample_rate_hz)) {
        ESP_LOGE(TAG, "the sounder can't play at %u Hz: %u Hz instead",
                 (unsigned)s_sink.sample_rate_hz, (unsigned)snd_rate(&mix));
    }
    const size_t frames = snd_block_frames(&mix); /* 10 ms at the output's rate */
    __atomic_store_n(&s_rate_hz, snd_rate(&mix), __ATOMIC_RELEASE); /* what speech is written at */
    __atomic_store_n(&s_speech_ready, s_slots != NULL, __ATOMIC_RELEASE);
    bool amp_on = false;
    uint8_t cont = ALARM_SOUND_NONE; /* the pattern the mixer was last given */

    for (;;) {
        req_t req;
        bool slept = false;
        /* Idle with the amplifier off and no speech: sleep until something needs to play - a
         * request, speech, a new continuous pattern. Each is published before its notification,
         * and the notification is latched, so one that lands after these looks still wakes us.
         * Woken for nothing new (a finish or cancel, a notification already served): back to
         * sleep, as the blocking receive this replaces did. */
        if (snd_idle(&mix) && !speech_buffered()) {
            while (uxQueueMessagesWaiting(s_queue) == 0u && !speech_buffered() &&
                   __atomic_load_n(&s_continuous, __ATOMIC_RELAXED) == cont) {
                (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            }
            slept = true;
        }
        while (xQueueReceive(s_queue, &req, 0) == pdTRUE) {
            if (req.kind == REQ_VOLUME) {
                snd_set_volume(&mix, req.value);
            } else if (req.kind == REQ_CHIME_VOLUME) {
                snd_set_chime_volume(&mix, req.value); /* AUD-032 */
            } else if (req.kind == REQ_VOICE_VOLUME) {
                snd_set_voice_volume(&mix, req.value); /* AUD-032 */
            } else if (req.kind == REQ_BACKGROUND) {
                /* the continuous pattern is set again below, before this block renders: one that
                 * starts now cuts the background sound off in that call (D22) */
                (void)snd_play_background(&mix, (alarm_sound_t)req.value);
            } else if (req.value == SND_KEY_CLICK) {
                snd_key_click(&mix);
            } else {
                (void)snd_play_tracked(&mix, (alarm_sound_t)req.value, req.ticket);
            }
            if (slept) {
                break; /* one request on waking, the rest from the next block: as before */
            }
        }
        snd_set_power_saving(&mix, __atomic_load_n(&s_power_saving, __ATOMIC_RELAXED));
        /* Every block: a change is picked up within 10 ms while anything plays. */
        cont = __atomic_load_n(&s_continuous, __ATOMIC_RELAXED);
        snd_set_continuous(&mix, (alarm_sound_t)cont);

        snd_set_speech_due(&mix, __atomic_load_n(&s_speech_due, __ATOMIC_RELAXED)); /* AUD-034 */
        const speech_take_t sp = speech_gather(frames);
        /* What the ticket ring held before this block: a failed write takes back its PLAYED. */
        const uint8_t tk_n_before = mix.done_n;
        const uint32_t tk_lost_before = mix.done_lost;
        const snd_pcm_block_t r =
            snd_render_pcm(&mix, block, frames, s_gathered, sp.avail, sp.gen != 0u);
        const snd_block_t b = r.block;
        speech_settle(&sp, &r);
        /* Bench silence (AUD-025), on the final mix of tones and speech: the mixer ran and the
         * speech was consumed as usual; the speaker stays off and hears only zeros (AUD-028). */
        const bool silent = __atomic_load_n(&s_silent, __ATOMIC_RELAXED);
        const bool amp = b.amp && !silent;
        if (amp != amp_on) {
            if (s_sink.amp != NULL) {
                s_sink.amp(s_sink.ctx, amp);
            }
            amp_on = amp;
        }
        if (silent) {
            memset(block, 0, frames * 2u * sizeof(block[0]));
        }
        if (s_sink.write(s_sink.ctx, block, frames) != ESP_OK) {
            /* The block never reached the speaker: no tone in it was heard to its end, and the
             * one still playing has lost part of itself. Its sound plays on unchanged. */
            snd_tickets_cancel_newest(&mix, tk_lost_before, tk_n_before);
            snd_ticket_abandon_active(&mix);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        tickets_drain(&mix); /* after the write: PLAYED means the tone's last block is out */
    }
}

#if CONFIG_ALARM_BUILD_DEV
#define SILENT_NS "sounder"
#define SILENT_KEY "silent"

static void silent_load(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(SILENT_NS, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u8(h, SILENT_KEY, &v);
        nvs_close(h);
    }
    __atomic_store_n(&s_silent, v != 0u, __ATOMIC_RELAXED);
    if (v != 0u) {
        ESP_LOGW(TAG, "SPEAKER OFF (bench silence): this panel's speaker stays silent, alarms "
                      "included (paired keypads and sirens still sound); 'speaker on' at the "
                      "console turns it back on");
    }
}
#endif

esp_err_t sounder_start(const sounder_sink_t *sink)
{
    if (sink == NULL || sink->write == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Started once: s_queue is set only while a worker exists (M02). A start that failed left
     * nothing behind, so it can be tried again. */
    if (s_queue != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Built privately first; nothing is published until the queue exists too. PSRAM is fine
     * for the speech buffer (no ISR or DMA reads it). Without it, tones only. */
    speech_slot_t *slots = alarm_mem_slow_calloc(1, SP_SLOTS * sizeof(speech_slot_t) +
                                                        SND_BLOCK_FRAMES * sizeof(int16_t));
    if (slots == NULL) {
        ESP_LOGE(TAG, "no memory for the speech buffer: tones only");
    }
    QueueHandle_t queue = xQueueCreate(16, sizeof(req_t));
    if (queue == NULL) {
        free(slots);
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_ALARM_BUILD_DEV
    silent_load(); /* before the task's first block */
#endif
    /* The task reads these from its first instruction, so they are published before it is
     * created. No speech producer runs yet (panel_voice starts after the sounder), and nothing is
     * queued before s_task is set (send_ticket), so a failed creation can take them back. */
    s_sink = *sink;
    s_slots = slots;
    s_gathered = slots != NULL ? (int16_t *)(void *)(slots + SP_SLOTS) : NULL;
    s_queue = queue;
    TaskHandle_t task = NULL;
    if (alarm_mem_task_create(sounder_task, "sounder", 4096, NULL, 6, &task) != pdPASS) {
        s_queue = NULL;
        s_gathered = NULL;
        s_slots = NULL;
        vQueueDelete(queue);
        free(slots);
        return ESP_ERR_NO_MEM;
    }
    __atomic_store_n(&s_task, task, __ATOMIC_RELEASE);
    notify_task(); /* speech written before the handle was published */
    ESP_LOGI(TAG, "sounder ready (%u Hz)",
             (unsigned)(s_sink.sample_rate_hz != 0u ? s_sink.sample_rate_hz : SND_SAMPLE_RATE));
    return ESP_OK;
}

static bool send_ticket(req_kind_t kind, uint8_t value, uint32_t ticket)
{
    if (__atomic_load_n(&s_task, __ATOMIC_ACQUIRE) == NULL) {
        return false; /* not started: nothing would wake the task for it */
    }
    const req_t r = {.kind = (uint8_t)kind, .value = value, .ticket = ticket};
    if (xQueueSend(s_queue, &r, 0) != pdTRUE) {
        return false;
    }
    notify_task();
    return true;
}

static void send(req_kind_t kind, uint8_t value)
{
    (void)send_ticket(kind, value, 0u);
}

void sounder_set_continuous(alarm_sound_t sound)
{
    const uint8_t want = (sound <= ALARM_SOUND_FIRE) ? (uint8_t)sound : (uint8_t)ALARM_SOUND_NONE;
    if (__atomic_exchange_n(&s_continuous, want, __ATOMIC_RELAXED) != want) {
        notify_task(); /* the value is the message: no queue slot (an idle task sleeps) */
    }
}

void sounder_play(alarm_sound_t sound)
{
    /* Checked here as well as in the mixer, so a chime cannot take a queue slot it will only
     * be thrown out of. */
    if (sound == ALARM_SOUND_CHIME && __atomic_load_n(&s_power_saving, __ATOMIC_RELAXED)) {
        return;
    }
    if (sound >= ALARM_SOUND_ACK) {
        send(REQ_ONESHOT, (uint8_t)sound);
    }
}

uint32_t sounder_play_tracked(alarm_sound_t sound)
{
    if (sound < ALARM_SOUND_ACK || (unsigned)sound >= SND_KEY_CLICK ||
        (sound == ALARM_SOUND_CHIME && __atomic_load_n(&s_power_saving, __ATOMIC_RELAXED))) {
        return 0u;
    }
    uint32_t ticket = __atomic_add_fetch(&s_next_ticket, 1u, __ATOMIC_RELAXED);
    if (ticket == 0u) {
        ticket = __atomic_add_fetch(&s_next_ticket, 1u, __ATOMIC_RELAXED); /* 0 is untracked */
    }
    return send_ticket(REQ_ONESHOT, (uint8_t)sound, ticket) ? ticket : 0u;
}

bool sounder_ticket_next(sounder_ticket_t *out)
{
    const uint32_t head = __atomic_load_n(&s_tk_head, __ATOMIC_ACQUIRE);
    const uint32_t tail = s_tk_tail;
    if (out == NULL || head == tail) {
        return false;
    }
    *out = s_tk[tail % SOUNDER_TICKET_RING];
    __atomic_store_n(&s_tk_tail, tail + 1u, __ATOMIC_RELEASE);
    return true;
}

size_t sounder_speech_write(const int16_t *pcm, size_t n, uint32_t gen)
{
    if (s_slots == NULL || pcm == NULL || n == 0u || gen == 0u || gen_dead(gen) ||
        gen <= __atomic_load_n(&s_finish_gen, __ATOMIC_ACQUIRE)) {
        return 0u;
    }
    uint32_t head = s_sp_head;
    size_t taken = 0u;
    while (taken < n &&
           ring_count(head, __atomic_load_n(&s_sp_tail, __ATOMIC_ACQUIRE)) < SP_SLOTS) {
        speech_slot_t *sl = &s_slots[head % SP_SLOTS];
        const size_t k = n - taken < SOUNDER_SPEECH_SLOT_SAMPLES ? n - taken
                                                                  : SOUNDER_SPEECH_SLOT_SAMPLES;
        memcpy(sl->pcm, pcm + taken, k * sizeof(int16_t));
        sl->gen = gen;
        sl->n = (uint32_t)k;
        head = ring_next(head);
        __atomic_store_n(&s_sp_head, head, __ATOMIC_SEQ_CST);
        taken += k;
    }
    if (taken > 0u) {
        notify_task();
    }
    return taken;
}

void sounder_speech_finish(uint32_t gen)
{
    atomic_max(&s_finish_gen, gen);
    notify_task();
}

void sounder_speech_cancel(uint32_t gen)
{
    atomic_max(&s_cancel_gen, gen);
    notify_task();
}

void sounder_speech_due(bool due)
{
    __atomic_store_n(&s_speech_due, due, __ATOMIC_RELAXED);
}

void sounder_speech_status(sounder_speech_status_t *out)
{
    if (out == NULL) {
        return;
    }
    out->ready = __atomic_load_n(&s_speech_ready, __ATOMIC_ACQUIRE);
    out->rate_hz = __atomic_load_n(&s_rate_hz, __ATOMIC_ACQUIRE);
    /* The two cursors are read apart, so they may disagree (used > SP_SLOTS): that is 0 free,
     * never a wrapped huge capacity. */
    out->free_samples = s_slots == NULL
                            ? 0u
                            : snd_speech_free(__atomic_load_n(&s_sp_head, __ATOMIC_ACQUIRE),
                                              __atomic_load_n(&s_sp_tail, __ATOMIC_ACQUIRE),
                                              SP_SLOTS, SOUNDER_SPEECH_SLOT_SAMPLES);
    out->consumed = __atomic_load_n(&s_sp_consumed, __ATOMIC_RELAXED);
    out->done_gen = __atomic_load_n(&s_done_gen, __ATOMIC_ACQUIRE);
    out->rejected_gen = __atomic_load_n(&s_rejected_gen, __ATOMIC_ACQUIRE);
    out->underrun_gen = __atomic_load_n(&s_underrun_gen, __ATOMIC_ACQUIRE);
    out->rejected = __atomic_load_n(&s_rejected_n, __ATOMIC_RELAXED);
    out->underruns = __atomic_load_n(&s_underrun_n, __ATOMIC_RELAXED);
}

void sounder_play_background(alarm_sound_t sound)
{
    if (sound >= ALARM_SOUND_ACK && !__atomic_load_n(&s_power_saving, __ATOMIC_RELAXED)) {
        send(REQ_BACKGROUND, (uint8_t)sound);
    }
}

void sounder_key_click(void)
{
    if (!__atomic_load_n(&s_power_saving, __ATOMIC_RELAXED)) {
        send(REQ_ONESHOT, SND_KEY_CLICK);
    }
}

void sounder_set_volume(uint8_t percent)
{
    send(REQ_VOLUME, percent > 100u ? 100u : percent);
}

void sounder_set_chime_volume(uint8_t percent)
{
    send(REQ_CHIME_VOLUME, percent > 100u ? 100u : percent);
}

void sounder_set_voice_volume(uint8_t percent)
{
    send(REQ_VOICE_VOLUME, percent > 100u ? 100u : percent);
}

void sounder_set_power_saving(bool saving)
{
    __atomic_store_n(&s_power_saving, saving, __ATOMIC_RELAXED);
}

bool sounder_silenced(void)
{
    return __atomic_load_n(&s_silent, __ATOMIC_RELAXED);
}

esp_err_t sounder_set_silenced(bool silent)
{
#if CONFIG_ALARM_BUILD_DEV
    uint8_t probe = 0;
    esp_err_t err = ESP_ERR_INVALID_STATE; /* never NVS from a PSRAM stack (AGENTS.md) */
    if (!esp_ptr_external_ram(&probe)) {
        nvs_handle_t h;
        err = nvs_open(SILENT_NS, NVS_READWRITE, &h);
        if (err == ESP_OK) {
            err = silent ? nvs_set_u8(h, SILENT_KEY, 1u) : nvs_erase_key(h, SILENT_KEY);
            if (err == ESP_ERR_NVS_NOT_FOUND) {
                err = ESP_OK;
            }
            if (err == ESP_OK) {
                err = nvs_commit(h);
            }
            nvs_close(h);
        }
    }
    if (silent || err == ESP_OK) {
        __atomic_store_n(&s_silent, silent, __ATOMIC_RELAXED);
    }
    ESP_LOGW(TAG, "speaker %s (bench silence)%s%s", sounder_silenced() ? "OFF" : "on",
             err == ESP_OK ? "" : "; not kept: ", err == ESP_OK ? "" : esp_err_to_name(err));
    return err;
#else
    (void)silent;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
