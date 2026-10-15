// panel_voice - the panel's voice service (docs/features/08 voice-first-build.md D10-D12): the
// installed voice pack (kind 2, name "primary", through the shared assets broker pack_assets),
// one decode worker feeding the sounder's speech ring, Preview, and the install transaction.
//
// With CONFIG_ALARM_FEATURE_VOICE off every call is a stub: status says PANEL_VOICE_OFF, an
// install answers PANEL_VOICE_INSTALL_UNAVAILABLE, nothing plays.
//
// Thread rules: any task may call these. None blocks on the alarm task's engine (state comes from
// snapshot copies). panel_voice_install() writes flash: call it from a task whose stack is in
// internal RAM (AGENTS.md "Rules for the firmware"), or it hands the flash work to one.
#ifndef PANEL_VOICE_H
#define PANEL_VOICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_snapshot.h"
#include "alarm/alarm_types.h"
#include "esp_err.h"
#include "voice_pack/voice_pack.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PANEL_VOICE_OFF = 0,     // the feature is not in this build
    PANEL_VOICE_NONE,        // no voice pack installed
    PANEL_VOICE_LOADING,     // installed; being read and checked at start-up
    PANEL_VOICE_READY,       // installed, signature and full check good, decoder resources held
    PANEL_VOICE_UNAVAILABLE, // installed but not usable (damaged, too little memory, decoder)
} panel_voice_state_t;

typedef struct {
    panel_voice_state_t state;
    bool draft;                        // the pack's DRAFT flag (takes not yet accepted)
    char locale[VP_LOCALE_SIZE];       // e.g. "en-US"; "" without a pack
    char voice_id[VP_VOICE_ID_SIZE];   // e.g. "qwen3-designed_female"
    uint32_t pack_revision;            // 0 without a pack
    uint16_t clips;
    uint32_t file_bytes;               // the signed file's size in the store
    const char *why;                   // UNAVAILABLE: a short English reason for logs, else ""
    // Counters since start-up (console `voice`, metrics).
    uint32_t utterances, completed, cancelled, underruns, decode_errors;
    // What it speaks with now: the preferences as last set, and the gap between a phrase's words
    // (the pack's, or the audio bench's while it sets one; 0 without a pack).
    bool enabled;
    uint8_t volume;
    uint16_t word_gap_ms;
} panel_voice_status_t;

// Start after the sounder and the alarm service run (panel_app): it takes the silent baseline on
// the alarm task (alarm_service_call). Loads "primary" into PSRAM on
// a worker: the call returns at once. Never fails the boot: a bad pack leaves tones as they are.
esp_err_t panel_voice_start(void);

void panel_voice_status(panel_voice_status_t *out);

// Whether automatic announcements may be switched on now (D06: the signed pack, mandatory clips
// and decoder resources are valid). The Settings page and the Panel Manager refuse otherwise.
bool panel_voice_ready(void);

// ---- what the panel may say (D06) ----------------------------------------------------------
// The three Announcements switches and the Voice volume, from panel_prefs_t. Call at start-up
// after panel_prefs_load() and whenever one changes (Settings, the Panel Manager, the volume
// slider). A change that forbids speech cancels it at once (AUD-015).
typedef struct {
    bool enabled; // voice_enabled: automatic announcements (default off)
    bool status;  // voice_status
    bool chime;   // voice_chime
    uint8_t volume; // the Voice volume 0..100 (AUD-032); 0 = no speech at all
} panel_voice_prefs_t;
void panel_voice_set_prefs(const panel_voice_prefs_t *prefs);
// The volume slider alone (it knows no switches): the other preferences stay as last set.
void panel_voice_set_volume(uint8_t volume);
// Power saving (on battery): no automatic speech, no Preview, as for the keypad tones.
void panel_voice_set_power_saving(bool saving);

// ---- the engine's facts (panel_app's sounder listener, on the alarm task) -----------------
// alarm_service.c's listeners are full: panel_app's sounder listener hands these on. Each only
// copies the fact (the snapshot as the policy's state, with the configuration's exit_sounds as
// the engine has it now) into a preallocated queue with a zero-tick send: no lock, no
// allocation, no logging, no engine call. The worker applies them in order. They copy nothing
// until panel_voice_start has queued a silent baseline on the alarm task; a full queue makes the
// voice silent until the next snapshot, a new baseline. sound: the ticket sounder_play_tracked()
// returned for the tone, 0 when it wasn't played or admitted.
void panel_voice_engine_event(const alarm_event_t *ev);
void panel_voice_engine_sound(uint8_t partition, alarm_sound_t sound, uint16_t zone, uint32_t ticket);
void panel_voice_engine_output(alarm_output_t output, uint8_t partition, alarm_pattern_t pattern);
void panel_voice_engine_snapshot(const alarm_snapshot_t *snap);

// ---- the words, for the editor (D03, D09) ------------------------------------------------
// A lease on the active pack: valid until released, even if a new pack is installed meanwhile
// (the retired copy is freed when its last lease ends). NULL when not READY. Keep leases short
// (a screen's lifetime at most) and release from the same task.
const vp_pack_t *panel_voice_pack_acquire(void);
void panel_voice_pack_release(const vp_pack_t *pack);

// ---- Preview (D03, D06) ------------------------------------------------------------------
typedef enum {
    PANEL_VOICE_PREVIEW_OK = 0,      // queued (plays after anything more important)
    PANEL_VOICE_PREVIEW_UNAVAILABLE, // not READY
    PANEL_VOICE_PREVIEW_BAD_WORDS,   // empty, over 8 entries, an ID not in the pack or not selectable
    PANEL_VOICE_PREVIEW_TOO_LONG,    // over 10 s with its joins (D02)
    PANEL_VOICE_PREVIEW_QUIET,       // volume 0, power saving, an alarm sounding, or four more
                                     // important requests waiting: nothing plays
} panel_voice_preview_t;

// Plays the given words once, as a zone's name would sound. Works with announcements off; never
// changes a preference or an approval; respects volume, power saving, alarms and the bench
// silence (AUD-025). Expires after 5 s if it cannot start.
panel_voice_preview_t panel_voice_preview(const uint16_t *ids, size_t n);
void panel_voice_preview_stop(void);

// ---- the audio bench (development builds only, AUD-030) ----------------------------------
// Audio from the PC (the dev link's PUT /dev/v1/play) heard as the panel says its words: 16 kHz
// mono PCM16, upsampled by D11's rule and fed to the sounder's speech ring by the worker, so the
// panel's own speaker, volume, mixer and bench silence apply; no pack, no policy, no approval.
// It plays between utterances; a newer one replaces one waiting or playing; an alarm refuses it.
typedef enum {
    PANEL_VOICE_BENCH_OK = 0,      // taken: the buffer is the voice's now, freed once played
    PANEL_VOICE_BENCH_UNAVAILABLE, // a release build, or no worker yet (no pack has loaded)
    PANEL_VOICE_BENCH_TOO_LONG,    // empty, or over PANEL_VOICE_BENCH_MAX_SAMPLES
} panel_voice_bench_t;
#define PANEL_VOICE_BENCH_MAX_SAMPLES (30u * 16000u) // 30 s at 16 kHz
// pcm: heap_caps_malloc'd (PSRAM is fine); taken only on OK, else still the caller's to free.
panel_voice_bench_t panel_voice_bench_play(int16_t *pcm, size_t n);

// Clip IDs from the installed pack said as a Preview says them (the dev link's POST /dev/v1/say):
// the panel's own renderer, word gap, Voice volume and mixer, for trying a voice's joins on the
// panel itself. gap_ms 1..PANEL_VOICE_BENCH_GAP_MAX_MS sets the gap between words for every
// utterance from now on, until the next call, a restart or a new pack; 0 puts the pack's back.
// gap_used and volume (either may be NULL) get what it plays with. A release build: UNAVAILABLE.
#define PANEL_VOICE_BENCH_GAP_MAX_MS 250u
panel_voice_preview_t panel_voice_bench_say(const uint16_t *ids, size_t n, uint16_t gap_ms,
                                            uint16_t *gap_used, uint8_t *volume);

// ---- installing a pack (D12) ---------------------------------------------------------------
typedef enum {
    PANEL_VOICE_INSTALL_OK = 0,
    PANEL_VOICE_INSTALL_UNAVAILABLE,   // the feature is not in this build, or no assets store
    PANEL_VOICE_INSTALL_TOO_BIG,       // outside 8192..970752 bytes or not a multiple of 4096
    PANEL_VOICE_INSTALL_BAD_SIGNATURE, // not signed for this panel, or the envelope is wrong
    PANEL_VOICE_INSTALL_BAD_PACK,      // signed, but not a valid RVOC 1 pack (vp_open_signed)
    PANEL_VOICE_INSTALL_LOCKED,        // armed, an alarm, entry/exit, a walk or fire test,
                                       // maintenance: also when the gate closes mid-install
    PANEL_VOICE_INSTALL_BUSY,          // an update or another install runs
    PANEL_VOICE_INSTALL_NO_SPACE,      // no free run holds the new copy beside the working one
    PANEL_VOICE_INSTALL_FAILED,        // flash erase/write/read-back/commit failed
} panel_voice_install_t;

// Installs a whole signed .robovoice file (the caller's buffer, PSRAM is fine, 4-byte aligned),
// replacing "primary" only after the new copy is written and read back; then activates it. Never
// changes voice_enabled, the other preferences or the bench silence (AUD-025). Blocking.
panel_voice_install_t panel_voice_install(const uint8_t *file, size_t len);

// The service console's `voice` command (status, preview <zone>, say <clip-id>...).
esp_err_t panel_voice_console_register(void);

// "ok", "unavailable", "too_big", "bad_signature", "bad_pack", "locked", "busy", "no_space",
// "failed": the words the dev link and the Panel Manager's API answer with.
const char *panel_voice_install_name(panel_voice_install_t r);

#ifdef __cplusplus
}
#endif

#endif // PANEL_VOICE_H
