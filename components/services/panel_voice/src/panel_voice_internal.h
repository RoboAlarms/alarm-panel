// panel_voice_internal.h - what the service console (voice_console.c) reads of panel_voice.c.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "panel_voice.h"

// A Preview of raw clip IDs (`voice say`): status and walk clips too, no recipe check; the
// policy still checks the pack has them, the 10 s ceiling, volume, power saving and alarms.
panel_voice_preview_t pv_preview_raw(const uint16_t *ids, size_t n);

// "ok", "unavailable", "bad_words", "too_long", "quiet".
const char *pv_preview_name(panel_voice_preview_t r);

const char *pv_state_name(panel_voice_state_t s);
