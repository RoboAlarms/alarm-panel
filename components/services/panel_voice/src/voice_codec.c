// voice_codec - the panel voice's Opus adapter (see include/voice_codec.h; D08, D10).
#include "voice_codec.h"

#include "opus.h"

size_t vc_state_bytes(void)
{
    int size = opus_decoder_get_size(1);
    if (size <= 0 || (unsigned)size > VC_MAX_STATE_BYTES) {
        return 0;
    }
    return (size_t)size;
}

int vc_init(void *state, size_t bytes)
{
    size_t need = vc_state_bytes();
    if (state == NULL || need == 0 || bytes < need) {
        return VC_ERR_ARG;
    }
    if (opus_decoder_init((OpusDecoder *)state, VC_SAMPLE_RATE, 1) != OPUS_OK) {
        return VC_ERR_STATE;
    }
    return 0;
}

int vc_reset(void *state)
{
    if (state == NULL) {
        return VC_ERR_ARG;
    }
    if (opus_decoder_ctl((OpusDecoder *)state, OPUS_RESET_STATE) != OPUS_OK) {
        return VC_ERR_STATE;
    }
    return 0;
}

int vc_packet_check(const uint8_t *pkt, size_t len)
{
    if (pkt == NULL || len == 0 || len > VC_MAX_PACKET_BYTES) {
        return VC_ERR_ARG;
    }
    if (opus_packet_get_nb_samples(pkt, (opus_int32)len, VC_SAMPLE_RATE) != VC_FRAME_SAMPLES) {
        return VC_ERR_PACKET;
    }
    return 0;
}

int vc_decode_20ms(void *state, const uint8_t *pkt, size_t len, int16_t out[VC_FRAME_SAMPLES])
{
    if (state == NULL || out == NULL) {
        return VC_ERR_ARG;
    }
    int rc = vc_packet_check(pkt, len);
    if (rc != 0) {
        return rc;
    }
    // decode_fec 0; frame_size exactly one frame, so libopus can never write past out.
    int n = opus_decode((OpusDecoder *)state, pkt, (opus_int32)len, out, VC_FRAME_SAMPLES, 0);
    if (n != VC_FRAME_SAMPLES) {
        return VC_ERR_DECODE;
    }
    return n;
}

void vc_trim_begin(vc_trim_t *trim, uint32_t pre_skip, uint32_t valid_samples)
{
    if (trim == NULL) {
        return;
    }
    trim->skip_left = pre_skip;
    trim->valid_left = valid_samples;
}

void vc_trim_take(vc_trim_t *trim, size_t n, size_t *first, size_t *count)
{
    size_t skip = 0;
    size_t take = 0;
    if (trim != NULL) {
        skip = trim->skip_left < n ? trim->skip_left : n;
        trim->skip_left -= (uint32_t)skip;
        size_t rest = n - skip;
        take = trim->valid_left < rest ? trim->valid_left : rest;
        trim->valid_left -= (uint32_t)take;
    }
    if (first != NULL) {
        *first = skip;
    }
    if (count != NULL) {
        *count = take;
    }
}

int vc_trim_done(const vc_trim_t *trim)
{
    return trim == NULL || trim->valid_left == 0;
}
