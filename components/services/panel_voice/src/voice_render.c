// voice_render - see include/voice_render.h (docs/features/08 voice-first-build.md D11).
#include "voice_render.h"

#include <string.h>

void vr_up_reset(vr_up_t *up)
{
    up->prev = 0;
    up->has_prev = 0;
}

size_t vr_up_push(vr_up_t *up, int16_t x, int16_t out[2])
{
    size_t n = 0;
    if (up->has_prev) {
        int32_t mean = ((int32_t)up->prev + (int32_t)x) / 2;  // truncates toward zero
        out[0] = up->prev;
        out[1] = (int16_t)mean;
        n = 2;
    }
    up->prev = x;
    up->has_prev = 1;
    return n;
}

size_t vr_up_flush(vr_up_t *up, int16_t out[2])
{
    size_t n = 0;
    if (up->has_prev) {
        out[0] = up->prev;
        out[1] = up->prev;
        n = 2;
    }
    vr_up_reset(up);
    return n;
}

int vr_begin(vr_t *r, const vp_pack_t *pack, const vr_item_t *items, size_t n, void *codec_state,
             size_t codec_bytes)
{
    if (r == NULL || pack == NULL || items == NULL || codec_state == NULL || n == 0u ||
        n > VR_ITEMS_MAX) {
        return VR_ERR_ARG;
    }
    uint64_t total = 0;
    for (size_t i = 0; i < n; i++) {
        vp_clip_t c;
        if (!vp_find(pack, items[i].clip_id, &c)) {
            return VR_ERR_CLIP;
        }
        total += 2u * (uint64_t)c.valid_samples;
        if (i > 0u) {
            total += (uint64_t)items[i].gap_before_ms * VR_SAMPLES_PER_MS;
        }
        if (total > VR_TOTAL_MAX) {
            return VR_ERR_TOO_LONG;
        }
    }
    if (codec_bytes < vc_state_bytes() || vc_init(codec_state, codec_bytes) < 0) {
        return VR_ERR_STATE;
    }
    memset(r, 0, sizeof *r);
    r->pack = pack;
    r->codec = codec_state;
    for (size_t i = 0; i < n; i++) {
        r->items[i] = items[i];
    }
    r->items[0].gap_before_ms = 0;
    r->count = (uint8_t)n;
    r->total = (uint32_t)total;
    r->phase = VR_PHASE_NEXT;
    return 0;
}

// Fills the output queue with the next samples; 0 = queued, 1 = utterance finished, < 0 = error.
static int vr_refill(vr_t *r)
{
    for (;;) {
        switch (r->phase) {
        case VR_PHASE_NEXT: {
            if (r->next_item >= r->count) {
                r->phase = VR_PHASE_DONE;
                return 1;
            }
            const vr_item_t *it = &r->items[r->next_item++];
            if (!vp_find(r->pack, it->clip_id, &r->clip) || vc_reset(r->codec) < 0) {
                return VR_ERR_DECODE;
            }
            vp_packets_begin(&r->clip, &r->iter);
            vc_trim_begin(&r->trim, r->clip.pre_skip, r->clip.valid_samples);
            vr_up_reset(&r->up);
            r->pcm_left = 0;
            r->gap_left = (uint32_t)it->gap_before_ms * VR_SAMPLES_PER_MS;
            r->phase = r->gap_left > 0u ? VR_PHASE_GAP : VR_PHASE_CLIP;
            break;
        }
        case VR_PHASE_GAP:
            r->q[0] = 0;
            r->q[1] = 0;
            r->q_i = 0;
            r->q_n = 2;
            r->gap_left -= 2u;  // gaps are whole milliseconds: always even
            if (r->gap_left == 0u) {
                r->phase = VR_PHASE_CLIP;
            }
            return 0;
        case VR_PHASE_CLIP: {
            while (r->pcm_left == 0u) {
                if (vc_trim_done(&r->trim)) {
                    r->q_n = (uint8_t)vr_up_flush(&r->up, r->q);
                    r->q_i = 0;
                    r->phase = VR_PHASE_NEXT;
                    if (r->q_n > 0u) {
                        return 0;
                    }
                    break;
                }
                const uint8_t *pkt;
                uint16_t len;
                if (!vp_packet_next(&r->iter, &pkt, &len)) {
                    return VR_ERR_DECODE;  // out of packets before valid_samples
                }
                int got = vc_decode_20ms(r->codec, pkt, len, r->pcm);
                if (got < 0) {
                    return VR_ERR_DECODE;
                }
                size_t first, count;
                vc_trim_take(&r->trim, (size_t)got, &first, &count);
                r->pcm_first = (uint16_t)first;
                r->pcm_left = (uint16_t)count;
            }
            if (r->phase != VR_PHASE_CLIP) {
                break;
            }
            int16_t x = r->pcm[r->pcm_first++];
            r->pcm_left--;
            size_t n = vr_up_push(&r->up, x, r->q);
            if (n > 0u) {
                r->q_n = (uint8_t)n;
                r->q_i = 0;
                return 0;
            }
            break;
        }
        default:
            return 1;
        }
    }
}

int32_t vr_pull(vr_t *r, int16_t *out, size_t max)
{
    if (r == NULL || out == NULL || max == 0u) {
        return VR_ERR_ARG;
    }
    if (r->phase == VR_PHASE_FAILED) {
        return VR_ERR_CANCELLED;
    }
    size_t w = 0;
    while (w < max) {
        if (r->q_i < r->q_n) {
            out[w++] = r->q[r->q_i++];
            continue;
        }
        if (r->phase == VR_PHASE_DONE) {
            break;
        }
        int rc = vr_refill(r);
        if (rc < 0) {
            r->phase = VR_PHASE_FAILED;
            r->q_n = r->q_i = 0;
            return w > 0u ? (int32_t)w : rc;
        }
        if (rc > 0) {
            break;
        }
    }
    r->produced += (uint32_t)w;
    return (int32_t)w;
}
