/* voice_smoke - the PC builds' voice backend (docs/features/08 voice-first-build.md D03, D06, D07,
 * D09; BL-202 U13a): ui_backend_t's five voice hooks over the real en-US pack (packed at build
 * time) and the host engine. Windowless; exits non-zero when a check fails, 77 (skipped) when the
 * build has no pack (no Python 3).
 *
 * D09: voice_info READY with the pack's identity; a lease taken and given back. D03: Preview
 * checks the words (empty, over 8, a status clip, an unknown ID: BAD_WORDS) and traces what it
 * would say. D07, CORE-CFG-021: approval only for the stored words (else STALE), with the pack's
 * revision, never while the voice isn't ready (UNAVAILABLE); a zone's words changed after approval
 * lose it. ALARM_PREVIEW_VOICE's states, through host_panel_voice_pretend: NONE hides the pack. */
#include <stdio.h>
#include <string.h>

#include "host_panel.h"
#include "ui_app.h"
#include "voice_pack/voice_pack.h"

static int s_checks, s_failed;
static ui_backend_t B;

static void check(bool ok, const char *what)
{
    s_checks++;
    if (!ok) {
        s_failed++;
        printf("FAIL: %s\n", what);
    }
}

/* D07: Done's identity now (the configuration's generation, the zone's binding, the pack's
 * revision), as the editor captures it. */
static ui_voice_seen_t voice_seen_now(const ui_backend_t *b, uint16_t zone)
{
    ui_voice_seen_t seen = {0};
    ui_voice_info_t info;
    b->voice_info(NULL, &info);
    if (!b->voice_seen(NULL, zone, &seen)) {
        seen.generation = UINT32_MAX; /* nothing to approve against */
    }
    seen.pack_revision = info.pack_revision;
    return seen;
}

/* D07: approve words already stored (Done on a review: no save). */
static uint8_t approve_stored(const ui_backend_t *b, uint16_t zone, const uint16_t *ids, size_t n)
{
    const ui_voice_seen_t seen = voice_seen_now(b, zone);
    alarm_result_t saved;
    return b->voice_apply_approve(NULL, NULL, zone, &seen, ids, n, &saved);
}

/* Store words on a zone through the backend's own configuration write. */
static bool store_words(uint16_t zone, const uint16_t *ids, unsigned n)
{
    static alarm_config_t cfg;
    if (!B.get_config(NULL, &cfg)) {
        return false;
    }
    for (unsigned i = 0; i < ALARM_ZONE_WORDS; i++) {
        alarm_zone_set_word(&cfg.zones[zone - 1u], i, i < n ? ids[i] : 0u);
    }
    return B.set_config(NULL, &cfg) == ALARM_OK;
}

static bool approved(uint16_t zone, uint32_t *pack_revision)
{
    static alarm_config_t cfg;
    B.get_config(NULL, &cfg);
    *pack_revision = cfg.zones[zone - 1u].voice_pack_revision;
    return alarm_zone_voice_approved(&cfg.zones[zone - 1u]);
}

int main(void)
{
    host_panel_init(1789857660000LL);
    host_panel_backend(&B);
    check(B.voice_info && B.voice_pack_acquire && B.voice_pack_release && B.voice_preview &&
              B.voice_seen && B.voice_apply_approve,
          "the six voice hooks are set");

    ui_voice_info_t info;
    B.voice_info(NULL, &info);
    if (info.state == UI_VOICE_NONE) {
        printf("voice_smoke: no voice pack in this build: skipped\n");
        return 77;
    }
    check(info.state == UI_VOICE_READY, "READY");
    check(strcmp(info.locale, "en-US") == 0, "the pack's locale");
    check(info.voice_name[0] != '\0' && info.pack_revision != 0u, "voice and revision");

    /* D09: a lease on the pack's index; two selectable words and a status clip from it */
    const vp_pack_t *pack = B.voice_pack_acquire(NULL);
    check(pack != NULL && host_panel_voice_leases() == 1u, "a lease");
    if (pack == NULL) {
        return 1;
    }
    check(pack->clip_count == 237u + pack->ending_count &&
              (pack->ending_count == 0u || pack->ending_count == pack->selectable_count),
          "237 clips, and an ending for every word or none (AUD-031)");
    uint16_t cursor = 0;
    vp_clip_t clip;
    uint16_t w[3] = {0};
    for (unsigned i = 0; i < 2u && vp_next_in_category(pack, 0, &cursor, &clip); i++) {
        w[i] = clip.id;
    }
    cursor = 0;
    const uint16_t status_id =
        vp_next_in_category(pack, VP_CAT_STATUS, &cursor, &clip) ? clip.id : 0u;
    check(w[0] != 0u && w[1] != 0u && status_id != 0u, "words from the pack");
    w[2] = w[0];

    /* D03: Preview */
    check(B.voice_preview(NULL, w, 2) == UI_VOICE_PREVIEW_OK, "Preview of two words");
    check(B.voice_preview(NULL, w, 0) == UI_VOICE_PREVIEW_BAD_WORDS, "Preview of nothing");
    const uint16_t nine[9] = {w[0], w[0], w[0], w[0], w[0], w[0], w[0], w[0], w[0]};
    check(B.voice_preview(NULL, nine, 9) == UI_VOICE_PREVIEW_BAD_WORDS, "Preview of nine");
    check(B.voice_preview(NULL, &status_id, 1) == UI_VOICE_PREVIEW_BAD_WORDS,
          "Preview of a status clip");
    const uint16_t unknown = 0x7FFEu;
    check(B.voice_preview(NULL, &unknown, 1) == UI_VOICE_PREVIEW_BAD_WORDS,
          "Preview of an unknown word");

    /* D07, CORE-CFG-021: approval of the stored words only */
    uint32_t rev = 0;
    check(store_words(1, w, 2), "words stored on zone 1");
    check(!approved(1, &rev), "not approved by the write");
    check(approve_stored(&B, 1, w, 3) == UI_VOICE_APPROVE_STALE, "other words: STALE");
    check(approve_stored(&B, 1, &status_id, 1) == UI_VOICE_APPROVE_UNAVAILABLE,
          "words the voice can't say: UNAVAILABLE");
    const uint16_t unknown_word[2] = {w[0], 0x7FFEu};
    check(approve_stored(&B, 1, unknown_word, 2) == UI_VOICE_APPROVE_UNAVAILABLE,
          "a word the pack doesn't have: UNAVAILABLE");
    {
        ui_voice_seen_t replaced = voice_seen_now(&B, 1);
        replaced.pack_revision++; /* the editor showed another pack's words */
        alarm_result_t saved;
        check(B.voice_apply_approve(NULL, NULL, 1, &replaced, w, 2, &saved) ==
                  UI_VOICE_APPROVE_STALE,
              "another pack's revision: STALE");
        ui_voice_seen_t moved = voice_seen_now(&B, 1);
        moved.generation--; /* the configuration changed since Done */
        check(B.voice_apply_approve(NULL, NULL, 1, &moved, w, 2, &saved) == UI_VOICE_APPROVE_STALE,
              "a moved generation: STALE");
    }
    check(!approved(1, &rev), "nothing approved by the refusals");
    check(approve_stored(&B, 1, w, 2) == UI_VOICE_APPROVE_OK, "the stored words: OK");
    check(approved(1, &rev) && rev == info.pack_revision, "approved with the pack's revision");
    check(store_words(1, w, 3), "words changed");
    check(!approved(1, &rev), "a change loses approval");

    /* ALARM_PREVIEW_VOICE's states: nothing works while the voice isn't ready */
    host_panel_voice_pretend(UI_VOICE_UNAVAILABLE);
    B.voice_info(NULL, &info);
    check(info.state == UI_VOICE_UNAVAILABLE, "pretend UNAVAILABLE");
    check(B.voice_pack_acquire(NULL) == NULL, "no lease while UNAVAILABLE");
    check(B.voice_preview(NULL, w, 3) == UI_VOICE_PREVIEW_UNAVAILABLE, "no Preview");
    check(approve_stored(&B, 1, w, 3) == UI_VOICE_APPROVE_UNAVAILABLE, "no approval");
    host_panel_voice_pretend(UI_VOICE_NONE);
    B.voice_info(NULL, &info);
    check(info.state == UI_VOICE_NONE && info.locale[0] == '\0' && info.pack_revision == 0u,
          "pretend NONE: no pack");
    host_panel_voice_pretend(-1);
    B.voice_pack_release(NULL, pack);
    check(host_panel_voice_leases() == 0u, "the lease given back");

    printf("voice_smoke: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed == 0 ? 0 : 1;
}
