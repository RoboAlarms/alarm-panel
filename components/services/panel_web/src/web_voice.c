/* web_voice.c - see web_voice.h. The proposal it builds: docs/features/31-Browser-Panel-Manager/
 * mockup/proposals/BL-202-voice.js (approved); the rules: docs/features/08 voice-first-build.md. */
#include "web_voice.h"

#include <stdio.h>
#include <string.h>

#include "lang_packs.h"
#include "panel_prefs.h"
#include "web_rules.h"

cJSON *web_voice_get(void)
{
    panel_voice_status_t st;
    panel_voice_status(&st);
    if (st.state == PANEL_VOICE_OFF)
        return NULL;
    const bool installed = st.state != PANEL_VOICE_NONE;
    const bool ready = st.state == PANEL_VOICE_READY && panel_voice_ready();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "installed", installed);
    cJSON_AddBoolToObject(o, "ready", ready);
    /* Readiness, not presence (D06): an installed pack that is not ready keeps its language and
     * version, and says why Announcements can't be turned on. */
    cJSON_AddStringToObject(o, "locale", installed ? st.locale : "");
    cJSON_AddStringToObject(o, "name", installed ? st.voice_id : ""); /* the pack's own: data */
    cJSON_AddNumberToObject(o, "version", installed ? st.pack_revision : 0);
    cJSON_AddBoolToObject(o, "draft", installed && st.draft);
    cJSON_AddStringToObject(o, "reason", ready ? "" : installed ? "unavailable" : "no_pack");
    cJSON_AddNumberToObject(o, "free", lang_packs_free_bytes()); /* the shared pack store's */
    return o;
}

const char *web_voice_refusal(void)
{
    if (panel_voice_ready())
        return NULL;
    panel_voice_status_t st;
    panel_voice_status(&st);
    return st.state == PANEL_VOICE_NONE ? "no_pack" : "unavailable";
}

const char *web_voice_answer(panel_voice_install_t r, const char **status)
{
    switch (r) {
    case PANEL_VOICE_INSTALL_OK:
        *status = "200 OK";
        return "ok";
    case PANEL_VOICE_INSTALL_TOO_BIG:
        *status = "400 Bad Request";
        return "too_big";
    case PANEL_VOICE_INSTALL_BAD_SIGNATURE:
    case PANEL_VOICE_INSTALL_BAD_PACK: /* signed but not a valid pack: the same words */
        *status = "400 Bad Request";
        return "bad_signature";
    case PANEL_VOICE_INSTALL_LOCKED: /* also when the gate closed mid-install */
        *status = "409 Conflict";
        return "armed";
    case PANEL_VOICE_INSTALL_BUSY:
        *status = "409 Conflict";
        return "busy";
    /* The language upload's answers for the same failures (lang_result_name). */
    case PANEL_VOICE_INSTALL_NO_SPACE:
        *status = "400 Bad Request";
        return "no_space";
    case PANEL_VOICE_INSTALL_FAILED:
        *status = "503 Service Unavailable";
        return "flash";
    case PANEL_VOICE_INSTALL_UNAVAILABLE:
    default:
        *status = "400 Bad Request";
        return "off";
    }
}

void web_voice_zones_begin(web_voice_zones_t *v)
{
    panel_voice_status_t st;
    panel_voice_status(&st);
    panel_prefs_t prefs;
    panel_prefs_load(&prefs);
    *v = (web_voice_zones_t){.on = st.state != PANEL_VOICE_OFF,
                             .pack = panel_voice_pack_acquire(),
                             .installed = st.state != PANEL_VOICE_NONE && st.state != PANEL_VOICE_OFF,
                             .pack_revision = st.pack_revision,
                             .enabled = prefs.voice_enabled};
    if (v->pack)
        v->pack_revision = v->pack->pack_revision; /* the lease's own, should one be installed meanwhile */
}

void web_voice_zones_end(web_voice_zones_t *v)
{
    if (v->pack)
        panel_voice_pack_release(v->pack);
    v->pack = NULL;
}

bool web_voice_zone(const web_voice_zones_t *v, const alarm_zone_config_t *zc, cJSON *zone)
{
    if (!v->on)
        return true;
    cJSON *s = cJSON_AddObjectToObject(zone, "spoken");
    cJSON *ids = cJSON_AddArrayToObject(s, "words");
    bool ok = s != NULL && ids != NULL;
    const unsigned n = alarm_zone_word_count(zc);
    char summary[ALARM_ZONE_WORDS * (VP_TEXT_MAX + 1u) + 1u] = "";
    size_t used = 0;
    bool unknown = false;
    for (unsigned i = 0; i < n; i++) {
        const uint16_t id = alarm_zone_word(zc, i);
        ok &= web_attach(ids, NULL, cJSON_CreateNumber(id));
        vp_clip_t clip;
        if (!v->pack)
            continue; /* the words' texts are the pack's: none without one */
        if (!vp_find(v->pack, id, &clip) || !vp_category_selectable(clip.category)) {
            unknown = true;
            continue;
        }
        const int w = snprintf(summary + used, sizeof(summary) - used, "%s%s", used ? " " : "", clip.text);
        if (w > 0 && (size_t)w < sizeof(summary) - used)
            used += (size_t)w;
    }
    ok &= cJSON_AddStringToObject(s, "summary", summary) != NULL;
    /* D01, D07: approval holds while it names the zone's current binding and the active pack. */
    const bool approved = zc->voice_approved_revision != 0 && zc->voice_binding_revision != 0 &&
                          zc->voice_approved_revision == zc->voice_binding_revision &&
                          (v->pack_revision == 0 || zc->voice_pack_revision == v->pack_revision) && !unknown;
    const char *state = n == 0                ? "empty"
                        : !approved           ? "review"
                        : !v->installed       ? "missing"
                        : !v->pack            ? "unavailable"
                        : !v->enabled         ? "off"
                                              : "ready";
    ok &= cJSON_AddStringToObject(s, "state", state) != NULL;
    ok &= cJSON_AddNumberToObject(s, "binding_rev", zc->voice_binding_revision) != NULL;
    ok &= cJSON_AddNumberToObject(s, "approved_rev", zc->voice_approved_revision) != NULL;
    ok &= cJSON_AddNumberToObject(s, "pack_rev", zc->voice_pack_revision) != NULL;
    return ok;
}
