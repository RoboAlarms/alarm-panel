/* web_voice - the Panel Manager's parts of the voice (docs/features/08 voice-first-build.md D06,
 * D14; docs/features/31 "Announcements"): GET /api/v1/voices, the answers of PUT /api/v1/voices,
 * and each zone's read-only spoken name in zone-status. The four Announcements switches are in
 * config/display (web_config.c). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "alarm/alarm_config.h"
#include "cJSON.h"
#include "panel_voice.h"

/* {installed, ready, locale, name, version, draft, reason, free}; NULL when the voice is not in
 * this firmware (PANEL_VOICE_OFF: the route answers 404 and the page shows no card). */
cJSON *web_voice_get(void);

/* Why Announcements can't be turned on now: NULL when they can (panel_voice_ready()), else
 * "no_pack" (nothing installed) or "unavailable" (installed but not usable, or not in this build). */
const char *web_voice_refusal(void);

/* An install's answer: the result word the page knows (the proposal's: ok, too_big,
 * bad_signature, armed, busy; the language upload's for the same failures: no_space, flash, off)
 * and its HTTP status. */
const char *web_voice_answer(panel_voice_install_t r, const char **status);

/* What a zone's spoken name is, for the zones the viewer sees: a lease on the active pack for
 * the display texts, the pack's revision and whether announcements are on. Begin, add each
 * zone's, end (releases the lease; same task). */
typedef struct {
    bool on;               /* the voice is in this firmware: without it, no "spoken" at all */
    const vp_pack_t *pack; /* NULL: not READY */
    bool installed;
    uint32_t pack_revision;
    bool enabled;
} web_voice_zones_t;
void web_voice_zones_begin(web_voice_zones_t *v);
/* Adds (when on) "spoken": {words, summary, state, binding_rev, approved_rev, pack_rev} to `zone`. State:
 * "empty" (no words), "review" (the approval doesn't match the binding or the active pack, D01,
 * D07), "missing" (no pack), "unavailable" (a pack that is not ready), "off" (announcements off),
 * "ready". Computed, never a field a write takes. False: part of it could not be built (no
 * memory): the caller sends no partial answer. */
bool web_voice_zone(const web_voice_zones_t *v, const alarm_zone_config_t *zc, cJSON *zone);
void web_voice_zones_end(web_voice_zones_t *v);
