/*
 * h264_sps.c - rewrites an H.264 sequence parameter set for low delay (see h264_sps.h).
 *
 * The syntax is ITU-T H.264 7.3.2.1.1 (seq_parameter_set_data) and E.1.1 (vui_parameters).
 */
#include "h264_sps/h264_sps.h"

#include <string.h>

/* The longest SPS this handles: esp_h264's is about 20 bytes. */
#define SPS_MAX 96u

/* ---- bits --------------------------------------------------------------------------------- */

typedef struct {
    const uint8_t *p;
    size_t len;  /* bytes */
    size_t pos;  /* bits */
    bool failed; /* read past the end, or a value too big to be real */
} reader_t;

static uint32_t read_bits(reader_t *r, unsigned n)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++) {
        if (r->pos >= r->len * 8u) {
            r->failed = true;
            return 0;
        }
        /* unsigned before the shift: a byte promoted to int would make older GCC warn */
        const unsigned bit = ((unsigned)r->p[r->pos / 8u] >> (7u - (unsigned)(r->pos % 8u))) & 1u;
        v = (v << 1) | bit;
        r->pos++;
    }
    return v;
}

static uint32_t read_ue(reader_t *r)
{
    unsigned zeros = 0;
    while (!r->failed && read_bits(r, 1) == 0u) {
        if (++zeros > 31u) {
            r->failed = true;
            return 0;
        }
    }
    if (r->failed) {
        return 0;
    }
    return (uint32_t)((1ull << zeros) - 1u + read_bits(r, zeros));
}

typedef struct {
    uint8_t *p;
    size_t cap; /* bytes */
    size_t pos; /* bits */
    bool failed;
} writer_t;

static void write_bits(writer_t *w, uint32_t v, unsigned n)
{
    for (unsigned i = n; i-- > 0u;) {
        if (w->pos >= w->cap * 8u) {
            w->failed = true;
            return;
        }
        const uint8_t mask = (uint8_t)(0x80u >> (w->pos % 8u));
        if ((v >> i) & 1u) {
            w->p[w->pos / 8u] = (uint8_t)(w->p[w->pos / 8u] | mask);
        } else {
            w->p[w->pos / 8u] = (uint8_t)(w->p[w->pos / 8u] & (uint8_t)~mask);
        }
        w->pos++;
    }
}

static void write_ue(writer_t *w, uint32_t v)
{
    const uint64_t x = (uint64_t)v + 1u;
    unsigned bits = 0;
    while ((x >> bits) > 1u) {
        bits++;
    }
    write_bits(w, 0, bits);
    write_bits(w, (uint32_t)x, bits + 1u);
}

/* ---- NAL units ------------------------------------------------------------------------------ */

/* The next start code at or after from: its first byte (the zero_byte of a four-byte one
 * included) in *at and the NAL unit's first byte in *nal. false: none. */
static bool next_start(const uint8_t *p, size_t len, size_t from, size_t *at, size_t *nal)
{
    for (size_t i = from; i + 2u < len; i++) {
        if (p[i] == 0u && p[i + 1u] == 0u && p[i + 2u] == 1u) {
            *at = (i > from && p[i - 1u] == 0u) ? i - 1u : i;
            *nal = i + 3u;
            return true;
        }
    }
    return false;
}

/* Emulation-prevention bytes out (EBSP -> RBSP). */
static size_t unescape(const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    size_t n = 0;
    unsigned zeros = 0;
    for (size_t i = 0; i < len; i++) {
        if (zeros >= 2u && in[i] == 3u) {
            zeros = 0;
            continue;
        }
        if (n >= cap) {
            return 0;
        }
        out[n++] = in[i];
        zeros = in[i] == 0u ? zeros + 1u : 0u;
    }
    return n;
}

/* ...and back in (RBSP -> EBSP): no 00 00 0x (x <= 3) may appear inside a NAL unit. */
static size_t escape(const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    size_t n = 0;
    unsigned zeros = 0;
    for (size_t i = 0; i < len; i++) {
        if (zeros >= 2u && in[i] <= 3u) {
            if (n >= cap) {
                return 0;
            }
            out[n++] = 3u;
            zeros = 0;
        }
        if (n >= cap) {
            return 0;
        }
        out[n++] = in[i];
        zeros = in[i] == 0u ? zeros + 1u : 0u;
    }
    return n;
}

/* ---- the rewrite ---------------------------------------------------------------------------- */

static bool high_profile(uint32_t profile)
{
    switch (profile) {
    case 100: case 110: case 122: case 244: case 44: case 83:
    case 86: case 118: case 128: case 138: case 139: case 134: case 135: return true;
    default: return false;
    }
}

/* rbsp: the SPS after its NAL header byte. Writes the new RBSP to w. */
static bool rewrite(const uint8_t *rbsp, size_t len, writer_t *w)
{
    reader_t r = {.p = rbsp, .len = len};
    const uint32_t profile = read_bits(&r, 8);
    (void)read_bits(&r, 16); /* constraint flags, level */
    (void)read_ue(&r);       /* seq_parameter_set_id */
    if (high_profile(profile)) {
        if (read_ue(&r) == 3u) { /* chroma_format_idc */
            (void)read_bits(&r, 1);
        }
        (void)read_ue(&r); /* bit depths */
        (void)read_ue(&r);
        (void)read_bits(&r, 1);
        if (read_bits(&r, 1)) {
            return false; /* scaling matrices: not from this encoder */
        }
    }
    (void)read_ue(&r); /* log2_max_frame_num_minus4 */
    const uint32_t poc_type = read_ue(&r);
    if (poc_type == 0u) {
        (void)read_ue(&r);
    } else if (poc_type == 1u) {
        (void)read_bits(&r, 1);
        (void)read_ue(&r); /* two se(v): read as ue, only skipped */
        (void)read_ue(&r);
        const uint32_t cycle = read_ue(&r);
        if (cycle > 255u) {
            return false;
        }
        for (uint32_t i = 0; i < cycle && !r.failed; i++) {
            (void)read_ue(&r);
        }
    } else if (poc_type != 2u) {
        return false;
    }
    const uint32_t refs = read_ue(&r); /* max_num_ref_frames */
    (void)read_bits(&r, 1);            /* gaps_in_frame_num_value_allowed_flag */
    (void)read_ue(&r);                 /* width and height in macroblocks */
    (void)read_ue(&r);
    if (!read_bits(&r, 1)) { /* frame_mbs_only_flag */
        (void)read_bits(&r, 1);
    }
    (void)read_bits(&r, 1); /* direct_8x8_inference_flag */
    if (read_bits(&r, 1)) { /* frame_cropping_flag */
        for (unsigned i = 0; i < 4u; i++) {
            (void)read_ue(&r);
        }
    }
    if (r.failed || refs > 16u) {
        return false;
    }
    const size_t keep = r.pos; /* everything before vui_parameters_present_flag stays */

    /* The old VUI: only its timing is worth keeping. */
    bool timing = false, fixed = false;
    uint32_t units = 0, scale = 0;
    if (read_bits(&r, 1)) {
        if (read_bits(&r, 1) && read_bits(&r, 8) == 255u) { /* aspect ratio, extended SAR */
            (void)read_bits(&r, 32);
        }
        if (read_bits(&r, 1)) { /* overscan */
            (void)read_bits(&r, 1);
        }
        if (read_bits(&r, 1)) { /* video signal type */
            (void)read_bits(&r, 4);
            if (read_bits(&r, 1)) {
                (void)read_bits(&r, 24);
            }
        }
        if (read_bits(&r, 1)) { /* chroma location */
            (void)read_ue(&r);
            (void)read_ue(&r);
        }
        timing = read_bits(&r, 1) != 0u;
        if (timing) {
            units = read_bits(&r, 32);
            scale = read_bits(&r, 32);
            fixed = read_bits(&r, 1) != 0u;
        }
        if (read_bits(&r, 1) || read_bits(&r, 1)) {
            return false; /* HRD parameters: not from this encoder */
        }
    }
    if (r.failed) {
        return false;
    }

    /* The new SPS: the same up to the VUI... */
    reader_t copy = {.p = rbsp, .len = len};
    for (size_t i = 0; i < keep; i++) {
        write_bits(w, read_bits(&copy, 1), 1);
    }
    /* ...and a VUI of our own. */
    write_bits(w, 1, 1); /* vui_parameters_present_flag */
    write_bits(w, 0, 1); /* aspect_ratio_info_present_flag: square pixels */
    write_bits(w, 0, 1); /* overscan_info_present_flag */
    write_bits(w, 1, 1); /* video_signal_type_present_flag */
    write_bits(w, 5, 3); /* video_format: unspecified */
    write_bits(w, 0, 1); /* video_full_range_flag: limited, as the PPA writes it */
    write_bits(w, 1, 1); /* colour_description_present_flag */
    write_bits(w, 6, 8); /* colour_primaries: SMPTE 170M (BT.601) */
    write_bits(w, 6, 8); /* transfer_characteristics */
    write_bits(w, 6, 8); /* matrix_coefficients */
    write_bits(w, 0, 1); /* chroma_loc_info_present_flag */
    write_bits(w, timing ? 1u : 0u, 1);
    if (timing) {
        write_bits(w, units, 32);
        write_bits(w, scale, 32);
        write_bits(w, fixed ? 1u : 0u, 1);
    }
    write_bits(w, 0, 1); /* nal_hrd_parameters_present_flag */
    write_bits(w, 0, 1); /* vcl_hrd_parameters_present_flag */
    write_bits(w, 0, 1); /* pic_struct_present_flag */
    write_bits(w, 1, 1); /* bitstream_restriction_flag */
    write_bits(w, 1, 1); /* motion_vectors_over_pic_boundaries_flag */
    write_ue(w, 2);      /* max_bytes_per_pic_denom (the default) */
    write_ue(w, 1);      /* max_bits_per_mb_denom (the default) */
    write_ue(w, 16);     /* log2_max_mv_length_horizontal */
    write_ue(w, 16);     /* log2_max_mv_length_vertical */
    write_ue(w, 0);      /* max_num_reorder_frames: pictures come in display order */
    write_ue(w, refs > 0u ? refs : 1u); /* max_dec_frame_buffering: show each one at once */
    write_bits(w, 1, 1); /* rbsp_stop_one_bit */
    while (w->pos % 8u != 0u) {
        write_bits(w, 0, 1);
    }
    return !w->failed;
}

size_t h264_sps_low_delay(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap)
{
    if (in == NULL || out == NULL || out_cap < in_len + H264_SPS_GROWTH) {
        return 0;
    }
    size_t at = 0, nal = 0, from = 0;
    while (next_start(in, in_len, from, &at, &nal)) {
        size_t next_at = in_len, next_nal = 0;
        if (!next_start(in, in_len, nal, &next_at, &next_nal)) {
            next_at = in_len;
        }
        if (nal < in_len && (in[nal] & 0x1Fu) == 7u) {
            uint8_t rbsp[SPS_MAX], built[SPS_MAX], escaped[SPS_MAX * 3u / 2u];
            const size_t rlen = unescape(in + nal + 1u, next_at - nal - 1u, rbsp, sizeof(rbsp));
            memset(built, 0, sizeof(built));
            writer_t w = {.p = built, .cap = sizeof(built)};
            if (rlen == 0u || !rewrite(rbsp, rlen, &w)) {
                return 0;
            }
            const size_t elen = escape(built, w.pos / 8u, escaped, sizeof(escaped));
            const size_t total = at + 4u + 1u + elen + (in_len - next_at);
            if (elen == 0u || total > out_cap) {
                return 0;
            }
            size_t n = 0;
            memcpy(out, in, at);
            n += at;
            static const uint8_t start[4] = {0, 0, 0, 1};
            memcpy(out + n, start, sizeof(start));
            n += sizeof(start);
            out[n++] = in[nal]; /* the NAL header: forbidden bit, nal_ref_idc, type 7 */
            memcpy(out + n, escaped, elen);
            n += elen;
            memcpy(out + n, in + next_at, in_len - next_at);
            return n + (in_len - next_at);
        }
        from = nal;
    }
    return 0;
}

bool h264_has_idr(const uint8_t *au, size_t len)
{
    size_t at = 0, nal = 0, from = 0;
    while (au != NULL && next_start(au, len, from, &at, &nal)) {
        if (nal < len && (au[nal] & 0x1Fu) == 5u) {
            return true;
        }
        from = nal;
    }
    return false;
}
