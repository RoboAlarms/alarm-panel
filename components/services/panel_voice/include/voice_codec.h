// voice_codec - the panel voice's Opus adapter (docs/features/08 voice-first-build.md D10).
//
// Plain C11 over libopus (esphome/micro-opus 0.4.1): no ESP-IDF calls, no heap, no globals, so
// the PC host tests, simulator and preview compile this same file. The caller owns the decoder
// state (vc_state_bytes() bytes, PSRAM on the panel) and every buffer.
//
// One clip of a voice pack (D08) is decoded like this:
//
//     vc_reset(state);                               // every clip starts from a reset state
//     vc_trim_t trim;
//     vc_trim_begin(&trim, pre_skip, valid_samples); // from the clip's index entry
//     for each packet:
//         int n = vc_decode_20ms(state, pkt, len, pcm); // 320 samples, or < 0: cancel the clip
//         size_t first, count;
//         vc_trim_take(&trim, (size_t)n, &first, &count);
//         play pcm[first .. first + count)
//
// The trim drops the first pre_skip samples (the encoder's delay), passes exactly
// valid_samples, and drops everything after them (the final frame's padding).
#ifndef VOICE_CODEC_H
#define VOICE_CODEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VC_SAMPLE_RATE 16000
#define VC_FRAME_SAMPLES 320       // 20 ms at 16 kHz, mono: every packet of a voice pack
#define VC_MAX_PACKET_BYTES 1275   // the largest Opus packet (RFC 6716), D08's packet limit
#define VC_MAX_STATE_BYTES 65536u  // a bigger decoder state is "voice unavailable" (D10)

// Errors: negative, never a sample count.
#define VC_ERR_ARG -1      // a NULL pointer, an empty or oversized packet, too small a state
#define VC_ERR_PACKET -2   // not an Opus packet of exactly 320 samples at 16 kHz
#define VC_ERR_DECODE -3   // libopus refused the packet (the clip is cancelled)
#define VC_ERR_STATE -4    // the decoder state could not be initialised or reset

// The bytes the caller provides for one mono decoder (opus_decoder_get_size(1)); 0 if libopus
// reports none or more than VC_MAX_STATE_BYTES, which makes the voice unavailable.
size_t vc_state_bytes(void);

// Initialise caller-owned state for 16 kHz mono. bytes must be at least vc_state_bytes();
// the state must be aligned for any type (malloc / heap_caps_malloc alignment). 0 or < 0.
int vc_init(void *state, size_t bytes);

// Back to a fresh decoder, as at the start of a clip (D08). 0 or < 0.
int vc_reset(void *state);

// Whether pkt is one Opus packet describing exactly VC_FRAME_SAMPLES samples at 16 kHz:
// 0 if so, else VC_ERR_ARG or VC_ERR_PACKET. Reads only the packet's table of contents; the
// pack verifier (D08) and vc_decode_20ms both use it.
int vc_packet_check(const uint8_t *pkt, size_t len);

// Decode one 20 ms packet into out[VC_FRAME_SAMPLES]. Returns VC_FRAME_SAMPLES, or < 0 with
// out untouched on VC_ERR_ARG/VC_ERR_PACKET (the state is unchanged then too). After
// VC_ERR_DECODE the clip must be abandoned and the state reset.
int vc_decode_20ms(void *state, const uint8_t *pkt, size_t len, int16_t out[VC_FRAME_SAMPLES]);

// D08's trim of one clip's decoded samples.
typedef struct {
    uint32_t skip_left;   // pre-skip samples still to drop
    uint32_t valid_left;  // valid samples still to pass
} vc_trim_t;

// Start a clip's trim from its index entry (pre_skip <= 320, valid_samples 1..160000; the pack
// verifier has checked both).
void vc_trim_begin(vc_trim_t *trim, uint32_t pre_skip, uint32_t valid_samples);

// For a frame of n decoded samples, the part to play: *first and *count (count may be 0).
void vc_trim_take(vc_trim_t *trim, size_t n, size_t *first, size_t *count);

// Whether the clip has passed all its valid samples.
int vc_trim_done(const vc_trim_t *trim);

#ifdef __cplusplus
}
#endif

#endif  // VOICE_CODEC_H
