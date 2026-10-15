/*
 * sim_audio.c - waveOut playback of components/core/sounder for the panel simulator.
 *
 * One thread keeps a handful of small buffers in flight: it renders the mixer 10 ms at a
 * time, exactly as the panel's sounder task does, and hands each filled buffer to Windows.
 * Silence is rendered and written too, so the stream never stops and restarts - a gap in a
 * waveOut stream is audible as a click, and a panel that clicks at people is no better than
 * one that flashes at them.
 *
 * The mixer is touched from two threads: this one and whichever thread the UI and the alarm
 * engine run on. A critical section guards it, and nothing that blocks (waveOutWrite, the
 * wait for a free buffer) is ever done while holding it.
 */
#include "sim_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sounder/sounder_mix.h"
#include "sounder/speech_boost.h"
#include "voice_render.h"

#ifdef _WIN32
#include <windows.h>
/* After windows.h, and needed explicitly: WIN32_LEAN_AND_MEAN leaves mmsystem.h out. */
#include <mmsystem.h>
static CRITICAL_SECTION s_lock;
#define LOCK()   EnterCriticalSection(&s_lock)
#define UNLOCK() LeaveCriticalSection(&s_lock)
#else
/* No sound card and one thread: the same code runs offline (sim_audio_start_offline) so the
 * preview can prove it. */
#define LOCK()   ((void)0)
#define UNLOCK() ((void)0)
#endif

/* ---- the mixer, its speech and the tickets: the same on every host ------------------------- */

#define SPEECH_RING 3840u /* 120 ms of 32 kHz mono, like the panel's speech ring */
#define SPEECH_PULL 640u

static bool s_ready;          /* the mixer is initialised (a device, or offline) */
static snd_mixer_t s_mix;     /* guarded by s_lock */
static bool s_mute;
static alarm_sound_t s_pattern;

static struct {
    bool on;                  /* an utterance is playing */
    bool finished;            /* vr_pull returned 0 and the boost gave back what it held */
    bool pulled_all;          /* vr_pull returned 0 */
    bool failed;              /* a decode error: the utterance is cancelled */
    uint32_t id;              /* ann_utt_t.id */
    vr_t r;
    speech_boost_t sb;        /* AUD-033, as the panel's worker */
    int16_t ring[SPEECH_RING];
    size_t ring_n;
    bool done_pending;        /* an end the policy has not been told of yet */
    uint32_t done_id;
    bool done_ok;
} s_sp;
static void *s_codec;
static size_t s_codec_bytes;
static bool s_due; /* speech queued or playing (sim_audio_speech_due): the exit beeps wait */

static void speech_end(bool ok)
{
    s_sp.on = false;
    s_sp.ring_n = 0u;
    s_sp.done_pending = true;
    s_sp.done_id = s_sp.id;
    s_sp.done_ok = ok;
}

/* One 10 ms block of stereo: the tones as the panel renders them, the speech on top (the panel's
 * mixer, snd_render_pcm). Locked. */
static void render_block_locked(int16_t *stereo)
{
    while (s_sp.on && !s_sp.finished && !s_sp.failed && s_sp.ring_n < SPEECH_RING) {
        size_t room = SPEECH_RING - s_sp.ring_n;
        if (room > SPEECH_PULL) {
            room = SPEECH_PULL;
        }
        if (s_sp.pulled_all) {
            const size_t held = sb_flush(&s_sp.sb, s_sp.ring + s_sp.ring_n, room);
            s_sp.ring_n += held;
            s_sp.finished = held == 0u;
            continue;
        }
        const int32_t n = vr_pull(&s_sp.r, s_sp.ring + s_sp.ring_n, room);
        if (n < 0) {
            s_sp.failed = true;
        } else if (n == 0) {
            s_sp.pulled_all = true;
        } else {
            sb_process(&s_sp.sb, s_sp.ring + s_sp.ring_n, (size_t)n);
            s_sp.ring_n += (size_t)n;
        }
    }
    snd_set_speech_due(&s_mix, s_due); /* AUD-034 */
    const snd_pcm_block_t b =
        snd_render_pcm(&s_mix, stereo, SND_BLOCK_FRAMES, s_sp.ring, s_sp.ring_n, s_sp.on);
    if (b.speech_consumed > 0u) {
        s_sp.ring_n -= b.speech_consumed;
        memmove(s_sp.ring, s_sp.ring + b.speech_consumed, s_sp.ring_n * sizeof(int16_t));
    }
    if (!s_sp.on) {
        return;
    }
    if (b.speech_rejected) {
        speech_end(false); /* over an alarm, fire, CO or aux */
    } else if (s_sp.failed) {
        speech_end(false);
    } else if (s_sp.finished && s_sp.ring_n == 0u) {
        speech_end(true);
    }
}

/* Renders `blocks` 10 ms blocks into p (2 * SND_BLOCK_FRAMES samples each) and trims them to desk
 * level, or silences them while muted. */
static void render_locked(int16_t *p, unsigned blocks)
{
    for (unsigned b = 0; b < blocks; b++) {
        render_block_locked(p + (size_t)b * SND_BLOCK_FRAMES * 2u);
    }
    const size_t n = (size_t)blocks * SND_BLOCK_FRAMES * 2u;
    if (s_mute) {
        memset(p, 0, n * sizeof(int16_t));
        return;
    }
    for (size_t s = 0; s < n; s++) {
        p[s] = (int16_t)((int32_t)p[s] * SIM_AUDIO_TRIM_PCT / 100);
    }
}

static void core_start(void)
{
    snd_init(&s_mix);
    memset(&s_sp, 0, sizeof(s_sp));
    s_pattern = ALARM_SOUND_NONE;
    s_mute = false;
    s_ready = true;
}

void sim_audio_render(int16_t *stereo, unsigned blocks)
{
    if (!s_ready) {
        memset(stereo, 0, (size_t)blocks * SND_BLOCK_FRAMES * 2u * sizeof(int16_t));
        return;
    }
    LOCK();
    render_locked(stereo, blocks);
    UNLOCK();
}

bool sim_audio_tone(void *ctx, alarm_sound_t sound, uint32_t ticket)
{
    (void)ctx;
    bool queued = false;
    if (s_ready) {
        LOCK();
        queued = snd_play_tracked(&s_mix, sound, ticket);
        UNLOCK();
    }
    return queued;
}

bool sim_audio_ticket(void *ctx, uint32_t *ticket, bool *played)
{
    (void)ctx;
    snd_ticket_t t;
    bool got = false;
    if (s_ready) {
        LOCK();
        got = snd_ticket_next(&s_mix, &t);
        UNLOCK();
    }
    if (got) {
        *ticket = t.ticket;
        *played = t.outcome == SND_TICKET_PLAYED;
    }
    return got;
}

bool sim_audio_speak(void *ctx, const vp_pack_t *pack, const vr_item_t *items, size_t n,
                     uint32_t id)
{
    (void)ctx;
    if (!s_ready) {
        return false;
    }
    if (s_codec == NULL) {
        s_codec_bytes = vc_state_bytes();
        s_codec = malloc(s_codec_bytes);
        if (s_codec == NULL) {
            return false;
        }
    }
    LOCK();
    bool ok = vr_begin(&s_sp.r, pack, items, n, s_codec, s_codec_bytes) == 0;
    if (ok) {
        s_sp.on = true;
        s_sp.finished = false;
        s_sp.pulled_all = false;
        s_sp.failed = false;
        sb_begin(&s_sp.sb, s_mix.voice_volume);
        s_sp.id = id;
        s_sp.ring_n = 0u;
        s_sp.done_pending = false;
    }
    UNLOCK();
    return ok;
}

void sim_audio_speech_due(void *ctx, bool due)
{
    (void)ctx;
    if (!s_ready) {
        return;
    }
    LOCK();
    s_due = due;
    UNLOCK();
}

void sim_audio_speech_cancel(void *ctx)
{
    (void)ctx;
    if (!s_ready) {
        return;
    }
    LOCK();
    s_sp.on = false;
    s_sp.ring_n = 0u;
    s_sp.done_pending = false;
    UNLOCK();
}

bool sim_audio_speech_done(void *ctx, uint32_t *id, bool *completed)
{
    (void)ctx;
    bool got = false;
    if (s_ready) {
        LOCK();
        got = s_sp.done_pending;
        if (got) {
            *id = s_sp.done_id;
            *completed = s_sp.done_ok;
            s_sp.done_pending = false;
        }
        UNLOCK();
    }
    return got;
}

bool sim_audio_start_offline(void)
{
    if (!s_ready) {
#ifdef _WIN32
        InitializeCriticalSection(&s_lock);
#endif
        core_start();
    }
    return true;
}

void sim_audio_continuous(void *ctx, alarm_sound_t sound)
{
    (void)ctx;
    if (s_ready) {
        LOCK();
        snd_set_continuous(&s_mix, sound);
        s_pattern = sound;
        UNLOCK();
    }
}

void sim_audio_one_shot(void *ctx, alarm_sound_t sound)
{
    (void)ctx;
    if (s_ready) {
        LOCK();
        snd_play(&s_mix, sound);
        UNLOCK();
    }
}

void sim_audio_key_click(void *ctx)
{
    (void)ctx;
    if (s_ready) {
        LOCK();
        snd_key_click(&s_mix);
        UNLOCK();
    }
}

void sim_audio_volume(void *ctx, uint8_t percent)
{
    (void)ctx;
    if (s_ready) {
        LOCK();
        snd_set_volume(&s_mix, percent);
        UNLOCK();
    }
}

void sim_audio_chime_volume(void *ctx, uint8_t percent)
{
    (void)ctx;
    if (s_ready) {
        LOCK();
        snd_set_chime_volume(&s_mix, percent);
        UNLOCK();
    }
}

void sim_audio_voice_volume(void *ctx, uint8_t percent)
{
    (void)ctx;
    if (s_ready) {
        LOCK();
        snd_set_voice_volume(&s_mix, percent);
        UNLOCK();
    }
}

void sim_audio_power_saving(void *ctx, bool saving)
{
    (void)ctx;
    if (s_ready) {
        LOCK();
        snd_set_power_saving(&s_mix, saving);
        UNLOCK();
    }
}

void sim_audio_mute(bool mute)
{
    if (s_ready) {
        LOCK();
        s_mute = mute;
        UNLOCK();
    }
}

bool sim_audio_muted(void)
{
    bool mute = false;
    if (s_ready) {
        LOCK();
        mute = s_mute;
        UNLOCK();
    }
    return mute;
}

alarm_sound_t sim_audio_pattern(void)
{
    alarm_sound_t sound = ALARM_SOUND_NONE;
    if (s_ready) {
        LOCK();
        sound = s_pattern;
        UNLOCK();
    }
    return sound;
}

#ifdef _WIN32

#define BUF_BLOCKS 2u                                /* 20 ms in one buffer */
#define BUF_FRAMES (SND_BLOCK_FRAMES * BUF_BLOCKS)
#define BUFFERS    4u                                /* 80 ms in flight: no underruns */

static HWAVEOUT s_wave;
static HANDLE s_thread;
static HANDLE s_done;         /* Windows signals it when a buffer has been played */
static volatile LONG s_stop;
static bool s_open;

static WAVEHDR s_hdr[BUFFERS];
static int16_t s_pcm[BUFFERS][BUF_FRAMES * 2u];

/* Render one buffer's worth of the panel's sound (and speech) and trim it to desk level. */
static void fill(unsigned i)
{
    sim_audio_render(s_pcm[i], BUF_BLOCKS);
}

static DWORD WINAPI audio_thread(LPVOID arg)
{
    (void)arg;
    while (InterlockedCompareExchange(&s_stop, 0, 0) == 0) {
        bool wrote = false;
        for (unsigned i = 0; i < BUFFERS; i++) {
            if ((s_hdr[i].dwFlags & WHDR_INQUEUE) != 0u) {
                continue; /* Windows still has this one */
            }
            fill(i);
            if (waveOutWrite(s_wave, &s_hdr[i], sizeof(s_hdr[i])) != MMSYSERR_NOERROR) {
                Sleep(10);
                break;
            }
            wrote = true;
        }
        if (!wrote) {
            /* Everything is queued: wait for one to come back (with a timeout, so the thread
             * still notices sim_audio_stop() if the device stops signalling). */
            WaitForSingleObject(s_done, 50);
        }
    }
    return 0;
}

bool sim_audio_start(void)
{
    if (s_open) {
        return true;
    }
    InitializeCriticalSection(&s_lock);
    core_start();

    WAVEFORMATEX fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = SND_SAMPLE_RATE;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = (WORD)(fmt.nChannels * fmt.wBitsPerSample / 8u);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

    s_done = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (s_done == NULL) {
        s_ready = false;
        DeleteCriticalSection(&s_lock);
        return false;
    }
    if (waveOutOpen(&s_wave, WAVE_MAPPER, &fmt, (DWORD_PTR)s_done, 0, CALLBACK_EVENT) !=
        MMSYSERR_NOERROR) {
        CloseHandle(s_done);
        s_done = NULL;
        s_ready = false;
        DeleteCriticalSection(&s_lock);
        return false;
    }
    for (unsigned i = 0; i < BUFFERS; i++) {
        memset(&s_hdr[i], 0, sizeof(s_hdr[i]));
        s_hdr[i].lpData = (LPSTR)s_pcm[i];
        s_hdr[i].dwBufferLength = (DWORD)sizeof(s_pcm[i]);
        waveOutPrepareHeader(s_wave, &s_hdr[i], sizeof(s_hdr[i]));
    }
    s_stop = 0;
    s_thread = CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
    if (s_thread == NULL) {
        waveOutClose(s_wave);
        CloseHandle(s_done);
        s_done = NULL;
        s_ready = false;
        DeleteCriticalSection(&s_lock);
        return false;
    }
    s_open = true;
    return true;
}

void sim_audio_stop(void)
{
    if (!s_open) {
        return;
    }
    InterlockedExchange(&s_stop, 1);
    SetEvent(s_done);
    WaitForSingleObject(s_thread, 1000);
    CloseHandle(s_thread);
    s_thread = NULL;
    waveOutReset(s_wave);
    for (unsigned i = 0; i < BUFFERS; i++) {
        waveOutUnprepareHeader(s_wave, &s_hdr[i], sizeof(s_hdr[i]));
    }
    waveOutClose(s_wave);
    CloseHandle(s_done);
    s_done = NULL;
    s_ready = false;
    DeleteCriticalSection(&s_lock);
    s_open = false;
}

#else /* the simulator itself is Windows-only; keep the file buildable everywhere */

bool sim_audio_start(void)
{
    return false;
}
void sim_audio_stop(void) {}

#endif
