/*
 * h264_sps.h - the remote keypad's H.264 stream, made to show at once in a browser
 * (docs/features/31, "Remote keypad").
 *
 * The P4's hardware encoder (esp_h264) writes a sequence parameter set whose VUI carries the
 * frame rate only. Without bitstream_restriction a decoder must allow for frames arriving out
 * of order and may hold back a whole picture buffer before showing the first one (Chromium's
 * hardware decoders: about seven pictures, a quarter of a second or more). A keypad whose
 * screen lags behind its buttons is what was to be got rid of, so the SPS of every key
 * frame is rewritten before it leaves the panel: no reordering, one picture buffered, and the
 * colour description of what the P4's PPA produces (BT.601, limited range) so the browser does
 * not guess BT.709.
 *
 * Portable C11: no heap, no globals. Every buffer is the caller's.
 */
#ifndef H264_SPS_H
#define H264_SPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Room h264_sps_low_delay() may need beyond the access unit it rewrites. */
#define H264_SPS_GROWTH 48u

/* Copies the Annex-B access unit in/in_len to out with its sequence parameter set rewritten as
 * above; everything else (PPS, slices, start codes) is copied byte for byte. out_cap must be at
 * least in_len + H264_SPS_GROWTH. Returns the length written, or 0 when the access unit has no
 * SPS, or one this does not understand (scaling matrices, HRD parameters, a damaged one): the
 * caller then sends in unchanged, which still decodes, only later. in and out must not overlap. */
size_t h264_sps_low_delay(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap);

/* Does the access unit hold an IDR picture (NAL unit type 5)? */
bool h264_has_idr(const uint8_t *au, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* H264_SPS_H */
