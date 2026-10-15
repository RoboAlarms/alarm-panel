/* ui_voice.h - the touchscreen's voice approval, shared by the panel's port (ui_port_esp) and
 * the PC builds' backend (host/ui_preview/host_panel.c) (docs/features/08 voice-first-build.md
 * D01, D07; CORE-CFG-021). Header only and portable, like the rest of ui_backend.
 *
 * Call ui_voice_seen_engine() and ui_voice_approve_engine() on the task that owns the engine
 * (the panel's alarm task), so that reading the zone and approving it are one transaction, and
 * ui_voice_check_pack() with the lease the caller holds until the approval has answered. */
#ifndef UI_BACKEND_UI_VOICE_H
#define UI_BACKEND_UI_VOICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_config.h"
#include "alarm/alarm_engine.h"
#include "ui_backend/ui_backend.h"
#include "voice_pack/voice_pack.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The engine's answer as the UI's: BAD_ARG (words empty or malformed, the zone gone) and
 * UNAVAILABLE (no binding revision) both say "these words can't be approved"; BUSY (a partition
 * not idle) is a refusal. */
static inline uint8_t ui_voice_approve_result(alarm_voice_result_t r)
{
    switch (r) {
    case ALARM_VOICE_OK:
        return UI_VOICE_APPROVE_OK;
    case ALARM_VOICE_STALE:
        return UI_VOICE_APPROVE_STALE;
    case ALARM_VOICE_BUSY:
        return UI_VOICE_APPROVE_REFUSED;
    case ALARM_VOICE_BAD_ARG:
    case ALARM_VOICE_UNAVAILABLE:
    default:
        return UI_VOICE_APPROVE_UNAVAILABLE;
    }
}

/* ui_backend_t.voice_seen against an engine, on the task that owns it: the configuration's
 * generation and the zone's stored binding revision (pack_revision 0: the caller's lease). */
static inline bool ui_voice_seen_engine(const alarm_engine_t *e, uint16_t zone,
                                        ui_voice_seen_t *out)
{
    if (out == NULL) {
        return false;
    }
    out->generation = 0u;
    out->binding_revision = 0u;
    out->pack_revision = 0u;
    const alarm_config_t *cfg = alarm_engine_config(e);
    if (cfg == NULL || zone == 0u || zone > ALARM_MAX_ZONES) {
        return false;
    }
    out->generation = alarm_engine_config_generation(e);
    out->binding_revision = cfg->zones[zone - 1u].voice_binding_revision;
    return true;
}

/* The pack half of an approval, on the caller's task with the lease it holds through the whole
 * transaction: the lease must be the pack the editor displayed (else STALE: a replaced pack, even
 * with the same word IDs) and must say the words (vp_check_recipe: each known and selectable, at
 * most VP_RECIPE_MAX, the whole within 10 s; else UNAVAILABLE). No lease: UNAVAILABLE. */
static inline uint8_t ui_voice_check_pack(const vp_pack_t *lease, const ui_voice_seen_t *seen,
                                          const uint16_t *ids, size_t n)
{
    if (lease == NULL || seen == NULL || ids == NULL || lease->pack_revision == 0u) {
        return UI_VOICE_APPROVE_UNAVAILABLE;
    }
    if (lease->pack_revision != seen->pack_revision) {
        return UI_VOICE_APPROVE_STALE;
    }
    vp_recipe_check_t chk;
    if (!vp_check_recipe(lease, ids, n, &chk) || chk.verdict != VP_RECIPE_OK) {
        return UI_VOICE_APPROVE_UNAVAILABLE;
    }
    return UI_VOICE_APPROVE_OK;
}

/* The engine half, on the task that owns the engine, after the save (saved true: this caller's
 * set_config of the same transaction succeeded) or alone (saved false). The identity is the
 * caller's, never the engine's current one:
 * - saved: the generation must be seen->generation + 1 - the save moved it by exactly one, so
 *   nothing else changed the configuration since Done, before or after it - and the approval is
 *   against the binding that save produced (the installer's own rename or move, D03);
 * - alone: the generation must be seen->generation and the binding seen->binding_revision.
 * The stored words must equal ids[0..n) (n 1..ALARM_ZONE_WORDS, the rest zeros), and
 * pack_revision (the caller's lease, checked by ui_voice_check_pack) seen->pack_revision. */
static inline uint8_t ui_voice_approve_engine(alarm_engine_t *e, uint16_t zone,
                                              const ui_voice_seen_t *seen, bool saved,
                                              const uint16_t *ids, size_t n,
                                              uint32_t pack_revision, alarm_ms_t now)
{
    if (pack_revision == 0u || seen == NULL) {
        return UI_VOICE_APPROVE_UNAVAILABLE;
    }
    if (e == NULL || ids == NULL || n == 0u || n > ALARM_ZONE_WORDS) {
        return UI_VOICE_APPROVE_UNAVAILABLE;
    }
    const alarm_config_t *cfg = alarm_engine_config(e);
    if (cfg == NULL || zone == 0u || zone > ALARM_MAX_ZONES) {
        return UI_VOICE_APPROVE_UNAVAILABLE;
    }
    if (pack_revision != seen->pack_revision) {
        return UI_VOICE_APPROVE_STALE; /* not the pack whose words the editor showed */
    }
    const uint32_t generation = saved ? seen->generation + 1u : seen->generation;
    if (alarm_engine_config_generation(e) != generation) {
        return UI_VOICE_APPROVE_STALE; /* someone else changed the configuration since Done */
    }
    const alarm_zone_config_t *z = &cfg->zones[zone - 1u];
    const uint32_t binding = saved ? z->voice_binding_revision : seen->binding_revision;
    if (z->voice_binding_revision != binding) {
        return UI_VOICE_APPROVE_STALE;
    }
    uint16_t words[ALARM_ZONE_WORDS];
    for (unsigned i = 0; i < ALARM_ZONE_WORDS; i++) {
        words[i] = i < n ? ids[i] : 0u;
        if (alarm_zone_word(z, i) != words[i]) {
            return UI_VOICE_APPROVE_STALE; /* the stored words aren't the ones approved */
        }
    }
    return ui_voice_approve_result(
        alarm_engine_voice_approve(e, zone, generation, binding, words, pack_revision, now));
}

/* A save the transaction applies: the backend's own set_config path. */
typedef alarm_result_t (*ui_voice_save_fn)(void *ctx, const alarm_config_t *cfg);

/* ui_backend_t.voice_apply_approve as one transaction, for a backend that runs its save on the
 * engine's task itself (the PC builds): the pack half with the caller's lease, the identity
 * checked before the save, the save (always applied, its result in *saved), then the engine
 * half. The panel's port does the same in its steps (ui_port_esp.c). */
static inline uint8_t ui_voice_apply_approve(alarm_engine_t *e, const vp_pack_t *lease,
                                             const alarm_config_t *cfg, uint16_t zone,
                                             const ui_voice_seen_t *seen, const uint16_t *ids,
                                             size_t n, ui_voice_save_fn save, void *save_ctx,
                                             alarm_result_t *saved, alarm_ms_t now)
{
    if (saved != NULL) {
        *saved = ALARM_OK;
    }
    uint8_t v = ui_voice_check_pack(lease, seen, ids, n);
    if (v == UI_VOICE_APPROVE_OK) {
        ui_voice_seen_t now_seen;
        if (!ui_voice_seen_engine(e, zone, &now_seen)) {
            v = UI_VOICE_APPROVE_UNAVAILABLE;
        } else if (now_seen.generation != seen->generation ||
                   now_seen.binding_revision != seen->binding_revision) {
            v = UI_VOICE_APPROVE_STALE; /* the save still applies; the approval doesn't */
        }
    }
    if (cfg != NULL) {
        const alarm_result_t r = save != NULL ? save(save_ctx, cfg) : ALARM_ERR_UNAVAILABLE;
        if (saved != NULL) {
            *saved = r;
        }
        if (r != ALARM_OK && v == UI_VOICE_APPROVE_OK) {
            v = UI_VOICE_APPROVE_REFUSED; /* nothing saved: nothing to approve */
        }
    }
    if (v != UI_VOICE_APPROVE_OK) {
        return v;
    }
    return ui_voice_approve_engine(e, zone, seen, cfg != NULL, ids, n, lease->pack_revision, now);
}

#ifdef __cplusplus
}
#endif

#endif /* UI_BACKEND_UI_VOICE_H */
