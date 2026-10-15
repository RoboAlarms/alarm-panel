/*
 * panel_prefs.c - display, sound and language preferences ("ui") and the setup state ("sys") in
 * NVS.
 */
#include "panel_prefs.h"

#include <string.h>

#include "i18n/i18n.h"
#include "i18n/i18n_date.h"
#include "nvs.h"

void panel_prefs_defaults(panel_prefs_t *out)
{
    out->brightness = 80;
    out->saver_brightness = 25; /* the screen saver shows a picture */
    out->saver_after_ms = 60000u;
    out->off_after_ms = 0u;
    out->night_on = false;
    out->night_start = 22u * 60u;
    out->night_end = 7u * 60u;
    out->night_brightness = 20;
    out->night_off_after_ms = 60000u;
    out->clock_mode = I18N_CLOCK_AUTO;
    out->volume = 70;
    out->chime_volume = 70; /* AUD-032 */
    out->voice_volume = 70;
    out->key_click = true;
    strcpy(out->lang, "en-US");
    out->region[0] = '\0';
    out->appearance = 0; /* Dark: the look before the choice (UI_APPEARANCE_DARK) */
    out->high_contrast = false;
    out->voice_enabled = true; /* D06: on, as on modern panels (decided 2026-10-12) */
    out->voice_status = true;
    out->voice_chime = true;
}

static uint8_t get_u8(nvs_handle_t h, const char *key, uint8_t def, uint8_t lo, uint8_t hi)
{
    uint8_t v = def;
    if (nvs_get_u8(h, key, &v) != ESP_OK || v < lo || v > hi) {
        v = def;
    }
    return v;
}

static uint16_t get_minute(nvs_handle_t h, const char *key, uint16_t def)
{
    uint16_t v = def;
    if (nvs_get_u16(h, key, &v) != ESP_OK || v > 1439u) {
        v = def;
    }
    return v;
}

/* An idle time: 0 (never) or at least 10 s, at most a day; anything else is the default. */
static uint32_t get_idle_ms(nvs_handle_t h, const char *key, uint32_t def)
{
    uint32_t ms = def;
    if (nvs_get_u32(h, key, &ms) != ESP_OK || (ms != 0u && (ms < 10000u || ms > 86400000u))) {
        ms = def;
    }
    return ms;
}

void panel_prefs_load(panel_prefs_t *out)
{
    panel_prefs_defaults(out);
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    out->brightness = get_u8(h, "bright", out->brightness, 10, 100);
    out->saver_brightness = get_u8(h, "saver_bright", out->saver_brightness, 0, 50);
    /* The clock choice (docs/features/37 I18N-016). Firmware before it kept only "clock24":
     * a stored 24 h stays 24 h, anything else becomes the region's (12 h in the US). */
    uint8_t mode = 0;
    if (nvs_get_u8(h, "clockmode", &mode) == ESP_OK && mode <= I18N_CLOCK_24H) {
        out->clock_mode = mode;
    } else {
        out->clock_mode = get_u8(h, "clock24", 0, 0, 1) != 0u ? I18N_CLOCK_24H : I18N_CLOCK_AUTO;
    }
    /* A stored language is kept whenever it is a valid tag, even when this firmware has no
     * catalog for it: its language pack may come later (I18N-042). Nothing or garbage is
     * English. A region this firmware doesn't know is the language's (I18N-015). */
    size_t len = sizeof(out->lang);
    if (nvs_get_str(h, "lang", out->lang, &len) != ESP_OK || !i18n_tag_valid(out->lang)) {
        strcpy(out->lang, "en-US");
    }
    len = sizeof(out->region);
    if (nvs_get_str(h, "region", out->region, &len) != ESP_OK ||
        (out->region[0] != '\0' && i18n_region(out->region) == NULL)) {
        out->region[0] = '\0';
    }
    out->volume = get_u8(h, "volume", out->volume, 0, 100);
    /* AUD-032: a panel updated from a firmware with one volume has neither key, and keeps
     * sounding as it did - the chime and the voice start at that volume. */
    out->chime_volume = get_u8(h, "chime_vol", out->volume, 0, 100);
    out->voice_volume = get_u8(h, "voice_vol", out->volume, 0, 100);
    out->key_click = get_u8(h, "click", 1, 0, 1) != 0u;
    uint32_t ms = out->saver_after_ms;
    if (nvs_get_u32(h, "saver_ms", &ms) == ESP_OK && (ms == 0u || ms >= 10000u)) {
        out->saver_after_ms = ms;
    }
    out->off_after_ms = get_idle_ms(h, "off_ms", out->off_after_ms);
    out->night_on = get_u8(h, "night", 0, 0, 1) != 0u;
    out->night_start = get_minute(h, "night_from", out->night_start);
    out->night_end = get_minute(h, "night_to", out->night_end);
    out->night_brightness = get_u8(h, "night_bright", out->night_brightness, 10, 100);
    out->night_off_after_ms = get_idle_ms(h, "night_off_ms", out->night_off_after_ms);
    /* The appearance (UI-010, BL-132): missing in a store from before it, or a value this
     * firmware doesn't know (0 Dark, 1 Light, 2 Automatic), reads as the default, Dark. */
    out->appearance = get_u8(h, "appearance", out->appearance, 0, 2);
    out->high_contrast = get_u8(h, "contrast", 0, 0, 1) != 0u;
    /* Announcements (D06): missing in an older store, so the defaults above apply. */
    out->voice_enabled = get_u8(h, "voice", 1, 0, 1) != 0u;
    out->voice_status = get_u8(h, "voice_st", 1, 0, 1) != 0u;
    out->voice_chime = get_u8(h, "voice_ch", 1, 0, 1) != 0u;
    nvs_close(h);
}

bool panel_prefs_lang_stored(void)
{
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = 0;
    const bool stored = nvs_get_str(h, "lang", NULL, &len) == ESP_OK;
    nvs_close(h);
    return stored;
}

esp_err_t panel_prefs_save(const panel_prefs_t *p)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("ui", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, "bright", p->brightness);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "saver_bright", p->saver_brightness);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(h, "saver_ms", p->saver_after_ms);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(h, "off_ms", p->off_after_ms);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "night", p->night_on ? 1u : 0u);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(h, "night_from", p->night_start);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(h, "night_to", p->night_end);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "night_bright", p->night_brightness);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(h, "night_off_ms", p->night_off_after_ms);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "clockmode", p->clock_mode);
    }
    if (err == ESP_OK) {
        /* Still written, as the clock it comes to: a rollback to firmware without the choice
         * keeps showing the same clock (I18N-015). */
        const bool h24 = i18n_clock_24h(p->clock_mode, i18n_region_for(p->region, p->lang));
        err = nvs_set_u8(h, "clock24", h24 ? 1u : 0u);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "lang", p->lang);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "region", p->region);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "volume", p->volume);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "chime_vol", p->chime_volume > 100u ? 100u : p->chime_volume);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "voice_vol", p->voice_volume > 100u ? 100u : p->voice_volume);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "click", p->key_click ? 1u : 0u);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "appearance", p->appearance);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "contrast", p->high_contrast ? 1u : 0u);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "voice", p->voice_enabled ? 1u : 0u);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "voice_st", p->voice_status ? 1u : 0u);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "voice_ch", p->voice_chime ? 1u : 0u);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool panel_setup_done(void)
{
    nvs_handle_t h;
    uint8_t done = 0;
    if (nvs_open("sys", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "setup", &done);
        nvs_close(h);
    }
    return done != 0u;
}

esp_err_t panel_setup_set_done(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("sys", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, "setup", 1);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
