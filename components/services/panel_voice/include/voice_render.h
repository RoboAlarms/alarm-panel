// voice_render - turns one spoken utterance of a voice pack into 32 kHz mono PCM, in small pulls
// (docs/features/08 voice-first-build.md D11). The panel's worker pulls 320-640 samples at a
// time into the sounder's ring; the PC simulator and tests pull the same way.
//
// Plain C11: no ESP-IDF calls, no heap, no globals. The caller owns the vr_t, the codec state
// (vc_state_bytes() bytes, PSRAM on the panel) and the output buffer. Only one packet (320
// samples) is ever decoded ahead; a whole clip is never decoded at once.
//
//     vr_t r;                                   // about 1 KiB: caller's, not on a small stack
//     vr_item_t items[2] = {{id_a, 0}, {id_b, VP_WORD_GAP_MS}};
//     if (vr_begin(&r, &pack, items, 2, codec_state, codec_bytes) < 0) ...
//     int32_t n;
//     while ((n = vr_pull(&r, buf, 640)) > 0) feed(buf, n);
//     if (n < 0) cancelled
//
// Each item is a clip, preceded by gap_before_ms of silence. The first item's gap is IGNORED
// (an utterance starts with its first clip, never with silence). A gap of 60 ms is 960 samples
// at 16 kHz and 1920 at 32 kHz; the gap is exact.
//
// Each clip is decoded (voice_codec: reset, pre-skip and final trim from the clip's index entry)
// and upsampled 16 -> 32 kHz by D11's rule: x[n], then (x[n]+x[n+1])/2 in int32 with truncation
// toward zero, the last sample of a clip repeated for its last position, the lookahead carried
// across packets and reset at each clip boundary: exactly twice valid_samples per clip. There is
// no gain and no normalisation (the mixer owns gain).
#ifndef VOICE_RENDER_H
#define VOICE_RENDER_H

#include <stddef.h>
#include <stdint.h>

#include "voice_codec.h"
#include "voice_pack/voice_pack.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VR_ITEMS_MAX 16u
#define VR_SAMPLE_RATE 32000u
#define VR_SAMPLES_PER_MS 32u
// The longest utterance, 10 s at 32 kHz (VP_SPOKEN_SAMPLES_MAX at 16 kHz, doubled).
#define VR_TOTAL_MAX (2u * VP_SPOKEN_SAMPLES_MAX)

// Errors: negative.
#define VR_ERR_ARG -1        // NULL, no items or more than VR_ITEMS_MAX, max == 0
#define VR_ERR_CLIP -2       // an item's clip id is not in the pack
#define VR_ERR_STATE -3      // the codec state is too small or could not be initialised
#define VR_ERR_TOO_LONG -4   // the utterance would exceed VR_TOTAL_MAX samples (10 s)
#define VR_ERR_DECODE -5     // a packet was refused, or a clip ran out of packets: cancelled
#define VR_ERR_CANCELLED -6  // pulled again after an error

typedef struct {
    uint16_t clip_id;
    uint16_t gap_before_ms;  // silence before this clip (ignored for the first item)
} vr_item_t;

// ---- The upsampler on its own (tested on hand-worked vectors) ----
typedef struct {
    int16_t prev;
    uint8_t has_prev;
} vr_up_t;

void vr_up_reset(vr_up_t *up);
// Feed one 16 kHz sample. Returns the samples written to out[0..1]: 0 for the first of a clip,
// else 2 (the previous sample, then the mean of the previous and this one).
size_t vr_up_push(vr_up_t *up, int16_t x, int16_t out[2]);
// End of a clip: the final pair (the last sample twice), 0 if nothing was fed. Resets.
size_t vr_up_flush(vr_up_t *up, int16_t out[2]);

// ---- The renderer ----
typedef enum { VR_PHASE_NEXT = 0, VR_PHASE_GAP, VR_PHASE_CLIP, VR_PHASE_DONE, VR_PHASE_FAILED } vr_phase_t;

typedef struct {
    const vp_pack_t *pack;
    void *codec;
    vr_item_t items[VR_ITEMS_MAX];
    uint8_t count;
    uint8_t next_item;
    uint8_t phase;
    uint8_t q_n;  // output samples queued (0..2), q[q_i ..]
    uint8_t q_i;
    uint16_t pcm_first;  // decoded samples of the current packet not yet fed to the upsampler
    uint16_t pcm_left;
    uint32_t gap_left;   // 32 kHz samples of silence still to emit
    uint32_t total;      // the exact length of the utterance, 32 kHz samples
    uint32_t produced;   // 32 kHz samples handed out so far
    vp_clip_t clip;
    vp_packet_iter_t iter;
    vc_trim_t trim;
    vr_up_t up;
    int16_t q[2];
    int16_t pcm[VC_FRAME_SAMPLES];
} vr_t;

// Starts an utterance: checks every item's clip exists, sums the length, initialises the codec
// state (caller-owned, at least vc_state_bytes(), malloc-aligned). The pack must stay as it is
// until the utterance ends. 0, or < 0 (VR_ERR_*) with nothing started.
int vr_begin(vr_t *r, const vp_pack_t *pack, const vr_item_t *items, size_t n, void *codec_state,
             size_t codec_bytes);

// Up to max (> 0) samples of 32 kHz mono into out. Returns the count written, 0 when the
// utterance is finished, or < 0: the utterance is cancelled (samples already written by the same
// call are returned first; the next call returns the error) and every later call is < 0.
// Pulls of any size give the same samples.
int32_t vr_pull(vr_t *r, int16_t *out, size_t max);

// The exact number of samples (32 kHz) the utterance produces: 2 x (sum of valid_samples +
// 16 kHz gap samples). Set by vr_begin; for the 10 s check and traces.
static inline uint32_t vr_total_samples_32k(const vr_t *r)
{
    return r->total;
}

#ifdef __cplusplus
}
#endif

#endif  // VOICE_RENDER_H
