/* The UI's backend contract (ADR-012, docs/features/33 step 5). This file includes nothing but the
 * contract and Unity and builds with the portable components' warning set: if the header ever
 * needs LVGL, ESP-IDF or a service, this stops compiling on the PC. */
#include "ui_backend/ui_backend.h"
#include "ui_backend/ui_light.h"
#include "ui_backend/ui_why_words.h"
#include "unity.h"

#include <string.h>

void setUp(void);
void tearDown(void);
void setUp(void) {}
void tearDown(void) {}

/* The defaults a keypad starts with (docs/features/07): partition 1, the region's clock (12 h in
 * the US), US English with the language's region, the screen saver after a minute at a quarter
 * brightness, key clicks on, no setup wizard (docs/features/37 I18N-015). */
static void test_default_options(void)
{
    ui_options_t opt;
    memset(&opt, 0xA5, sizeof(opt));
    ui_options_defaults(&opt);
    TEST_ASSERT_EQUAL_UINT8(1, opt.partition);
    TEST_ASSERT_EQUAL_UINT8(0, opt.clock_mode);
    TEST_ASSERT_EQUAL_STRING("en-US", opt.lang);
    TEST_ASSERT_EQUAL_STRING("", opt.region);
    TEST_ASSERT_EQUAL_UINT8(80, opt.brightness);
    TEST_ASSERT_EQUAL_UINT8(25, opt.saver_brightness);
    TEST_ASSERT_EQUAL_UINT32(60000u, opt.saver_after_ms);
    TEST_ASSERT_EQUAL_UINT8(70, opt.volume);
    TEST_ASSERT_TRUE(opt.key_click);
    TEST_ASSERT_FALSE(opt.run_setup);
    /* UI-043: the screen never goes dark by itself and night hours are off until someone asks;
     * ready for a bedroom at 22:00-07:00, a fifth of the brightness, dark after a minute. */
    TEST_ASSERT_EQUAL_UINT32(0u, opt.off_after_ms);
    TEST_ASSERT_FALSE(opt.night_on);
    TEST_ASSERT_EQUAL_UINT16(22u * 60u, opt.night_start);
    TEST_ASSERT_EQUAL_UINT16(7u * 60u, opt.night_end);
    TEST_ASSERT_EQUAL_UINT8(20, opt.night_brightness);
    TEST_ASSERT_EQUAL_UINT32(60000u, opt.night_off_after_ms);
    /* UI-010 (BL-132): Dark without high contrast, so a panel updated keeps the look it had */
    TEST_ASSERT_EQUAL_UINT8(UI_APPEARANCE_DARK, opt.appearance);
    TEST_ASSERT_FALSE(opt.high_contrast);
    /* docs/features/08 voice-first-build.md D06: announcements on, as modern panels, the two kinds on */
    TEST_ASSERT_TRUE(opt.voice_enabled);
    TEST_ASSERT_TRUE(opt.voice_status);
    TEST_ASSERT_TRUE(opt.voice_chime);
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_THEME_DARK, ui_light_theme(&opt, 12 * 60));
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_THEME_DARK, ui_light_theme(&opt, -1));
}

/* UI-010, UI-043 (BL-132): the palette the appearance asks for. Dark and Light whatever the time;
 * Automatic dark during the night hours and light by day, switching only at the night's two ends;
 * light with the night hours off or the clock not set; High contrast with either. */
static void test_appearance_theme(void)
{
    ui_options_t opt;
    ui_options_defaults(&opt);
    opt.night_on = true; /* 22:00 to 07:00 */
    opt.appearance = UI_APPEARANCE_LIGHT;
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_THEME_LIGHT, ui_light_theme(&opt, 23 * 60));
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_THEME_LIGHT, ui_light_theme(&opt, 12 * 60));
    opt.appearance = UI_APPEARANCE_DARK;
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_THEME_DARK, ui_light_theme(&opt, 12 * 60));
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_THEME_DARK, ui_light_theme(&opt, -1));

    /* Automatic: once at each end of the night, never back and forth (UI-020) */
    opt.appearance = UI_APPEARANCE_AUTO;
    int changes = 0;
    ui_light_theme_t was = ui_light_theme(&opt, 0);
    for (int m = 1; m < 2 * 1440; m++) {
        const ui_light_theme_t now = ui_light_theme(&opt, m);
        changes += now != was ? 1 : 0;
        was = now;
    }
    TEST_ASSERT_EQUAL_INT(4, changes); /* two days: dusk and dawn each day */
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_THEME_LIGHT, ui_light_theme(&opt, 22 * 60 - 1));
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_THEME_DARK, ui_light_theme(&opt, 22 * 60));
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_THEME_DARK, ui_light_theme(&opt, 7 * 60 - 1));
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_THEME_LIGHT, ui_light_theme(&opt, 7 * 60));
    /* the clock not set: the panel can't tell night from day, so light */
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_THEME_LIGHT, ui_light_theme(&opt, -1));
    /* night hours off: says so on the screen and stays light, day and night */
    opt.night_on = false;
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_THEME_LIGHT, ui_light_theme(&opt, 23 * 60));
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_THEME_LIGHT, ui_light_theme(&opt, 3 * 60));
    /* night hours whose start is their end: never night, so light */
    opt.night_on = true;
    opt.night_end = opt.night_start;
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_THEME_LIGHT, ui_light_theme(&opt, 23 * 60));
    opt.night_end = 7u * 60u;

    /* High contrast picks the pair's high-contrast palette, with each appearance */
    opt.high_contrast = true;
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_THEME_CONTRAST, ui_light_theme(&opt, 23 * 60));
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_THEME_CONTRAST_LIGHT, ui_light_theme(&opt, 12 * 60));
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_THEME_CONTRAST_LIGHT, ui_light_theme(&opt, -1));
    opt.appearance = UI_APPEARANCE_DARK;
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_THEME_CONTRAST, ui_light_theme(&opt, 12 * 60));
    opt.appearance = UI_APPEARANCE_LIGHT;
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_THEME_CONTRAST_LIGHT, ui_light_theme(&opt, 23 * 60));

    /* an appearance from newer firmware (or a damaged value) reads as Dark */
    opt.appearance = 3;
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_THEME_CONTRAST, ui_light_theme(&opt, 12 * 60));
    opt.appearance = 255;
    opt.high_contrast = false;
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_THEME_DARK, ui_light_theme(&opt, 12 * 60));
}

/* UI-009, UI-043: dimmed after the screen saver's time, dark after the screen-off time, both from
 * the last touch; the first wins. */
static void test_light_dims_then_goes_dark(void)
{
    ui_options_t opt;
    ui_options_defaults(&opt);
    /* the defaults: the saver after a minute, never dark (as before UI-043) */
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_AWAKE, ui_light_stage(&opt, 59999u, false));
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_SAVER, ui_light_stage(&opt, 60000u, false));
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_SAVER, ui_light_stage(&opt, 86400000u, false));
    /* dim after a minute, dark after ten */
    opt.off_after_ms = 600000u;
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_SAVER, ui_light_stage(&opt, 599999u, false));
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_OFF, ui_light_stage(&opt, 600000u, false));
    TEST_ASSERT_EQUAL_UINT8(80, ui_light_percent(&opt, UI_LIGHT_AWAKE, false, false));
    TEST_ASSERT_EQUAL_UINT8(25, ui_light_percent(&opt, UI_LIGHT_SAVER, false, false));
    TEST_ASSERT_EQUAL_UINT8(0, ui_light_percent(&opt, UI_LIGHT_OFF, false, false));
    /* dark sooner than the saver: straight to dark, no photo first */
    opt.off_after_ms = 30000u;
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_AWAKE, ui_light_stage(&opt, 29999u, false));
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_OFF, ui_light_stage(&opt, 30000u, false));
    /* no saver, only dark */
    opt.saver_after_ms = 0u;
    opt.off_after_ms = 120000u;
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_AWAKE, ui_light_stage(&opt, 119999u, false));
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_OFF, ui_light_stage(&opt, 120000u, false));
    /* neither: always lit */
    opt.off_after_ms = 0u;
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_AWAKE, ui_light_stage(&opt, 86400000u, false));
}

/* UI-043: night hours, wrapping past midnight; the start is in, the end is out. */
static void test_light_night_hours(void)
{
    ui_options_t opt;
    ui_options_defaults(&opt);
    TEST_ASSERT_FALSE(ui_light_night(&opt, 23 * 60)); /* off by default */
    opt.night_on = true;
    TEST_ASSERT_FALSE(ui_light_night(&opt, 22 * 60 - 1));
    TEST_ASSERT_TRUE(ui_light_night(&opt, 22 * 60));
    TEST_ASSERT_TRUE(ui_light_night(&opt, 23 * 60 + 59));
    TEST_ASSERT_TRUE(ui_light_night(&opt, 0));
    TEST_ASSERT_TRUE(ui_light_night(&opt, 7 * 60 - 1));
    TEST_ASSERT_FALSE(ui_light_night(&opt, 7 * 60));
    TEST_ASSERT_FALSE(ui_light_night(&opt, 12 * 60));
    TEST_ASSERT_FALSE(ui_light_night(&opt, -1)); /* the clock is not set: never night */
    /* hours within a day (a nap room, or a night-shift worker's day) */
    opt.night_start = 9u * 60u;
    opt.night_end = 17u * 60u + 30u;
    TEST_ASSERT_FALSE(ui_light_night(&opt, 9 * 60 - 1));
    TEST_ASSERT_TRUE(ui_light_night(&opt, 9 * 60));
    TEST_ASSERT_TRUE(ui_light_night(&opt, 17 * 60 + 29));
    TEST_ASSERT_FALSE(ui_light_night(&opt, 17 * 60 + 30));
    TEST_ASSERT_FALSE(ui_light_night(&opt, 23 * 60));
    /* the same start and end: never */
    opt.night_end = opt.night_start;
    TEST_ASSERT_FALSE(ui_light_night(&opt, 9 * 60));
}

/* UI-043: at night the screen is dimmer and goes dark after the night's own time; an alarm lights
 * it as by day. */
static void test_light_at_night(void)
{
    ui_options_t opt;
    ui_options_defaults(&opt);
    opt.night_on = true;
    opt.off_after_ms = 0u; /* by day it never goes dark */
    /* the night's time wins over the saver at the same minute: straight to dark */
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_AWAKE, ui_light_stage(&opt, 59999u, true));
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_OFF, ui_light_stage(&opt, 60000u, true));
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_SAVER, ui_light_stage(&opt, 60000u, false));
    /* a longer night time: the saver first, then dark */
    opt.night_off_after_ms = 300000u;
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_SAVER, ui_light_stage(&opt, 60000u, true));
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_OFF, ui_light_stage(&opt, 300000u, true));
    /* never dark at night: only dimmer */
    opt.night_off_after_ms = 0u;
    TEST_ASSERT_EQUAL_INT(UI_LIGHT_SAVER, ui_light_stage(&opt, 86400000u, true));
    /* brightness: the night's when awake, never above it on the saver, the day's in an alarm */
    TEST_ASSERT_EQUAL_UINT8(20, ui_light_percent(&opt, UI_LIGHT_AWAKE, true, false));
    TEST_ASSERT_EQUAL_UINT8(80, ui_light_percent(&opt, UI_LIGHT_AWAKE, true, true));
    TEST_ASSERT_EQUAL_UINT8(80, ui_light_percent(&opt, UI_LIGHT_AWAKE, false, true));
    TEST_ASSERT_EQUAL_UINT8(20, ui_light_percent(&opt, UI_LIGHT_SAVER, true, false));
    opt.saver_brightness = 10;
    TEST_ASSERT_EQUAL_UINT8(10, ui_light_percent(&opt, UI_LIGHT_SAVER, true, false));
    TEST_ASSERT_EQUAL_UINT8(0, ui_light_percent(&opt, UI_LIGHT_OFF, true, true));
}

/* An empty backend is a valid one: every optional entry is a NULL function pointer, which the
 * UI reads as "hide what depends on it". */
static void test_an_empty_backend_is_all_null(void)
{
    ui_backend_t be;
    memset(&be, 0, sizeof(be));
    TEST_ASSERT_NULL(be.ctx);
    TEST_ASSERT_NULL(be.get_snapshot);
    TEST_ASSERT_NULL(be.wifi_info);
    TEST_ASSERT_NULL(be.mqtt_get);
    TEST_ASSERT_NULL(be.halink_status);
    TEST_ASSERT_NULL(be.notify_status);
    TEST_ASSERT_NULL(be.storage_sd);
    TEST_ASSERT_NULL(be.fw_status);
    TEST_ASSERT_NULL(be.factory_reset);
    TEST_ASSERT_NULL(be.zw_status); /* no Z-Wave: every Z-Wave row and page hidden */
    TEST_ASSERT_NULL(be.zw_devices);
    TEST_ASSERT_NULL(be.zw_device);
    TEST_ASSERT_NULL(be.zw_request);
    TEST_ASSERT_NULL(be.zw_progress);
    TEST_ASSERT_NULL(be.link_info); /* no panel link: no Keypads section, no role step */
    TEST_ASSERT_NULL(be.link_pair_status);
    TEST_ASSERT_NULL(be.link_keypads);
    TEST_ASSERT_NULL(be.link_request);
}

/* The panel link's pages' contract (docs/features/11): a zeroed info is the alarm panel itself,
 * a zeroed pairing is idle, request kinds start at 1, and the pairing's failure numbers are the
 * protocol's (plink_fail_t). */
static void test_link_contract(void)
{
    ui_link_info_t info;
    memset(&info, 0, sizeof(info));
    TEST_ASSERT_EQUAL_INT(UI_LINK_ROLE_MASTER, info.role);
    ui_link_pair_t pair;
    memset(&pair, 0, sizeof(pair));
    TEST_ASSERT_EQUAL_INT(UI_LINK_PAIR_IDLE, pair.state);
    TEST_ASSERT_EQUAL_INT(UI_LINK_FAIL_NONE, pair.fail);
    TEST_ASSERT_TRUE(UI_LINK_REQ_PAIR_OPEN != 0);
    TEST_ASSERT_EQUAL_INT(0, UI_LINK_REQ_OK);
    TEST_ASSERT_EQUAL_INT(13, UI_LINK_FAIL_VERSION);
    TEST_ASSERT_TRUE(sizeof(pair.code) >= 7u);
}

/* The Z-Wave pages' contract (docs/features/36 screens.md §12): an idle operation is all zeros, a
 * request's kinds start at 1 (a zeroed request asks for nothing), and the classes are the Z-Wave
 * key bits a device asks for. */
static void test_zwave_contract(void)
{
    ui_zw_progress_t p;
    memset(&p, 0, sizeof(p));
    TEST_ASSERT_EQUAL_INT(UI_ZW_OP_NONE, p.op);
    TEST_ASSERT_EQUAL_INT(UI_ZW_STEP_IDLE, p.step);
    TEST_ASSERT_EQUAL_INT(UI_ZW_RES_NONE, p.result);
    TEST_ASSERT_TRUE(UI_ZW_REQ_ADD != 0);
    TEST_ASSERT_EQUAL_HEX8(0x01u, UI_ZW_KEY_S2_UNAUTH);
    TEST_ASSERT_EQUAL_HEX8(0x02u, UI_ZW_KEY_S2_AUTH);
    TEST_ASSERT_EQUAL_HEX8(0x04u, UI_ZW_KEY_S2_ACCESS);
    TEST_ASSERT_EQUAL_HEX8(0x80u, UI_ZW_KEY_S0);
    ui_zw_device_t d;
    memset(&d, 0, sizeof(d));
    TEST_ASSERT_EQUAL_INT(UI_ZW_SEC_UNKNOWN, d.security);
    TEST_ASSERT_EQUAL_INT(UI_ZW_DEV_READY, d.state);
    TEST_ASSERT_EQUAL_INT(UI_ZW_RADIO_NOT_CONNECTED, 0);
    TEST_ASSERT_TRUE(UI_ZW_DSK_LEN >= 8u * 5u + 7u + 1u); /* 8 groups of 5, dashes, the end */
}

/* docs/features/37 I18N-001: a service's answer in the reader's language - one marked by itself,
 * one that carries a number (the pattern translated, the number kept), anything else as it came;
 * English needs no catalog and reads exactly what the service wrote. */
static void test_answers_in_the_readers_language(void)
{
    const char *texts[3] = {"Pushover a r\xC3\xA9pondu {0}", "Serveur de mise \xC3\xA0 jour : {0}",
                            "Cl\xC3\xA9 inconnue"};
    i18n_entry_t entries[3] = {{i18n_hash("why", "Pushover answered {0}"), 0u},
                               {i18n_hash("update", "The update server answered {0}"), 0u},
                               {i18n_hash("why", "Unknown key"), 0u}};
    char strings[128];
    size_t used = 0;
    for (int i = 0; i < 3; i++) {
        entries[i].text = (uint32_t)used;
        memcpy(strings + used, texts[i], strlen(texts[i]) + 1u);
        used += strlen(texts[i]) + 1u;
    }
    for (int i = 0; i < 3; i++) { /* the catalog's order: by hash */
        for (int j = i + 1; j < 3; j++) {
            if (entries[j].hash < entries[i].hash) {
                const i18n_entry_t e = entries[i];
                entries[i] = entries[j];
                entries[j] = e;
            }
        }
    }
    const i18n_catalog_t fr = {"fr", (uint8_t)I18N_PLURALS_FR, 1, 3u, entries, strings};
    char out[96];
    char said[64];
    I18N_FORMAT(said, sizeof(said), UI_WHY_PUSHOVER_ANSWERED, 429);
    TEST_ASSERT_EQUAL_STRING("Pushover answered 429", said); /* what the service writes */
    ui_why_line(&fr, "why", said, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Pushover a r\xC3\xA9pondu 429", out);
    ui_why_line(NULL, "why", said, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Pushover answered 429", out);
    ui_why_line(&fr, "update", "The update server answered 404", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Serveur de mise \xC3\xA0 jour : 404", out);
    ui_why_line(&fr, "why", "Unknown key", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Cl\xC3\xA9 inconnue", out);
    /* not a number, the wrong context, or a service's own words: as they came */
    ui_why_line(&fr, "why", "Pushover answered soon", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Pushover answered soon", out);
    ui_why_line(&fr, "update", "Pushover answered 429", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Pushover answered 429", out);
    ui_why_line(&fr, "why", "quota exceeded", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("quota exceeded", out);
    ui_why_line(&fr, "why", NULL, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_default_options);
    RUN_TEST(test_light_dims_then_goes_dark);
    RUN_TEST(test_light_night_hours);
    RUN_TEST(test_light_at_night);
    RUN_TEST(test_appearance_theme);
    RUN_TEST(test_an_empty_backend_is_all_null);
    RUN_TEST(test_zwave_contract);
    RUN_TEST(test_link_contract);
    RUN_TEST(test_answers_in_the_readers_language);
    return UNITY_END();
}
