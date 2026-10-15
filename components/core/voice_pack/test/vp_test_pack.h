/*
 * vp_test_pack.h - a small RVOC format 1 writer for tests that need words the frozen fixture lacks
 * (never edit voice_pack_fixture_v1.h). vpt_build() writes a valid pack: the given selectable clips
 * (ascending IDs) plus the 16 status and walk clips, each clip made of 1-byte CELT packets.
 */
#ifndef VP_TEST_PACK_H
#define VP_TEST_PACK_H

#include <string.h>

#include "alarm_proto/proto_link.h"
#include "voice_pack/voice_pack.h"

typedef struct {
    uint16_t id;
    uint8_t category;
    const char *text;
    const char *key;
    uint32_t samples;   /* valid samples; 0 = 640 */
} vpt_clip_t;

static uint8_t vpt_buf[VP_PAYLOAD_MAX];
static uint32_t vpt_len;

static void vpt_w16(uint32_t o, uint32_t v)
{
    vpt_buf[o] = (uint8_t)(v & 0xFFu);
    vpt_buf[o + 1u] = (uint8_t)((v >> 8) & 0xFFu);
}
static void vpt_w32(uint32_t o, uint32_t v)
{
    vpt_w16(o, v & 0xFFFFu);
    vpt_w16(o + 2u, (v >> 16) & 0xFFFFu);
}
static uint32_t vpt_up4(uint32_t v) { return (v + 3u) & ~3u; }

static const char *const vpt_status_keys[VP_STATUS_COUNT] = {
    "status.arming", "status.armed", "status.away", "status.home", "status.night",
    "status.exit_now", "status.no_entry_delay", "status.disarmed", "status.entry", "status.not_ready"};
static const char *const vpt_walk_keys[VP_WALK_COUNT] = {
    "walk.test", "walk.started", "walk.ended", "walk.ends_soon", "walk.all_tested", "walk.some_untested"};

/* Builds the pack into vpt_buf/vpt_len and opens it. */
static vp_err_t vpt_build(const vpt_clip_t *sel, unsigned nsel, vp_pack_t *out)
{
    vpt_clip_t all[VP_CLIPS_MAX];
    unsigned n = 0;
    for (unsigned i = 0; i < nsel; i++) {
        all[n++] = sel[i];
    }
    for (unsigned i = 0; i < VP_STATUS_COUNT; i++) {
        vpt_clip_t c = {(uint16_t)(VP_STATUS_FIRST + i), (uint8_t)VP_CAT_STATUS, "Status", vpt_status_keys[i], 0};
        all[n++] = c;
    }
    for (unsigned i = 0; i < VP_WALK_COUNT; i++) {
        vpt_clip_t c = {(uint16_t)(VP_WALK_FIRST + i), (uint8_t)VP_CAT_WALK, "Walk", vpt_walk_keys[i], 0};
        all[n++] = c;
    }
    memset(vpt_buf, 0, sizeof vpt_buf);
    uint32_t index_end = VP_HEADER_SIZE + n * VP_INDEX_ENTRY_SIZE;
    uint32_t strings_off = vpt_up4(index_end);
    uint32_t sp = strings_off;
    uint32_t text_off[VP_CLIPS_MAX], key_off[VP_CLIPS_MAX];
    for (unsigned i = 0; i < n; i++) {
        text_off[i] = sp;
        memcpy(vpt_buf + sp, all[i].text, strlen(all[i].text) + 1u);
        sp += (uint32_t)strlen(all[i].text) + 1u;
        key_off[i] = sp;
        memcpy(vpt_buf + sp, all[i].key, strlen(all[i].key) + 1u);
        sp += (uint32_t)strlen(all[i].key) + 1u;
    }
    uint32_t strings_len = sp - strings_off;
    uint32_t audio_off = vpt_up4(sp);
    uint32_t ap = audio_off;
    for (unsigned i = 0; i < n; i++) {
        uint32_t samples = all[i].samples ? all[i].samples : 640u;
        uint32_t packets = (samples + VP_FRAME_SAMPLES - 1u) / VP_FRAME_SAMPLES;
        uint32_t dlen = packets * 3u;
        ap = vpt_up4(ap);
        uint8_t *e = vpt_buf + VP_HEADER_SIZE + i * VP_INDEX_ENTRY_SIZE;
        vpt_w16(VP_HEADER_SIZE + i * VP_INDEX_ENTRY_SIZE, all[i].id);
        e[2] = all[i].category;
        vpt_w32(VP_HEADER_SIZE + i * VP_INDEX_ENTRY_SIZE + 4u, text_off[i]);
        vpt_w32(VP_HEADER_SIZE + i * VP_INDEX_ENTRY_SIZE + 8u, key_off[i]);
        vpt_w32(VP_HEADER_SIZE + i * VP_INDEX_ENTRY_SIZE + 12u, ap);
        vpt_w32(VP_HEADER_SIZE + i * VP_INDEX_ENTRY_SIZE + 16u, dlen);
        vpt_w32(VP_HEADER_SIZE + i * VP_INDEX_ENTRY_SIZE + 20u, samples);
        vpt_w16(VP_HEADER_SIZE + i * VP_INDEX_ENTRY_SIZE + 24u, packets);
        for (uint32_t k = 0; k < packets; k++) {
            vpt_buf[ap + k * 3u] = 1;
            vpt_buf[ap + k * 3u + 2u] = 0xF8; /* CELT FB 20 ms, code 0 */
        }
        proto_sha256(vpt_buf + ap, dlen, e + 28);
        ap += dlen;
    }
    vpt_len = ap;
    memcpy(vpt_buf, VP_MAGIC, 4);
    vpt_w16(4, VP_FORMAT);
    vpt_w16(6, VP_HEADER_SIZE);
    vpt_w32(8, vpt_len);
    vpt_w32(12, 1);
    vpt_w32(16, VP_VOCABULARY_SCHEMA);
    memcpy(vpt_buf + 24, "en-US", 5);
    memcpy(vpt_buf + 40, "test-voice", 10);
    vpt_w32(72, VP_SAMPLE_RATE);
    vpt_w16(76, VP_CODEC_OPUS);
    vpt_buf[78] = VP_CHANNELS;
    vpt_buf[79] = VP_FRAME_MS;
    vpt_w16(80, n);
    vpt_w16(82, VP_INDEX_ENTRY_SIZE);
    vpt_w32(84, VP_HEADER_SIZE);
    vpt_w32(88, strings_off);
    vpt_w32(92, strings_len);
    vpt_w32(96, audio_off);
    vpt_w32(100, vpt_len - audio_off);
    vpt_w16(104, VP_WORD_GAP_MS);
    vpt_w16(106, VP_PART_GAP_MS);
    vpt_w32(108, VP_NOMINAL_BITRATE);
    return vp_open(vpt_buf, vpt_len, out, NULL);
}

#endif /* VP_TEST_PACK_H */
