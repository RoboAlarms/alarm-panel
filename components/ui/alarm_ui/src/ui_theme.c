/*
 * ui_theme.c - the panel's appearance: Dark, Light, High contrast and High contrast, light
 * (docs/features/07 UI-010, BL-132).
 *
 * The palettes and what a theme adds to the styles come from the layout's themes.xml
 * (tools/lvpro_gen.py); the layout's alarm_ui.c switches them (alarm_ui_theme_set: the palette
 * COLOR_* read, every style the layout made, the colour subjects). Here the switch reaches the
 * live screens in one step - nothing fades (UI-020): every colour an object holds of its own (an
 * XML style_*_color, a tone C set) is found by its value and given the new palette's colour of
 * the same name, and what C keeps as a colour is worked out again. Nothing is rebuilt, so the
 * view, the page stack and an open sheet stay as they are. The screen saver keeps its theme's
 * saver= palette (a dark photo in every appearance). Nothing is stored here.
 */

#include "ui_internal.h"

static const lv_color_t *saver_palette(void)
{
    return alarm_ui_palettes[alarm_ui_theme_saver[alarm_ui_theme_get()]];
}

/* A colour subject (the state's tone) wrote the current palette's colour on the screen saver:
 * give it the saver's. Added after the screens' own bindings, so it runs after them. */
static void saver_tone_changed(lv_observer_t *o, lv_subject_t *s)
{
    (void)o;
    (void)s;
    if (S.saver != NULL && saver_palette() != alarm_ui_palette) {
        alarm_ui_theme_recolor_tree(S.saver, saver_palette());
    }
}

static uint32_t layout_theme(ui_theme_t t)
{
    switch (t) {
    case UI_THEME_LIGHT: return ALARM_UI_THEME_LIGHT;
    case UI_THEME_CONTRAST: return ALARM_UI_THEME_CONTRAST;
    case UI_THEME_CONTRAST_LIGHT: return ALARM_UI_THEME_CONTRAST_LIGHT;
    case UI_THEME_DARK:
    default: return ALARM_UI_THEME_DARK;
    }
}

void ui_theme_watch_saver(void)
{
    for (uint32_t i = 0; i < alarm_ui_color_subject_count; i++) {
        lv_subject_add_observer(alarm_ui_color_subjects[i], saver_tone_changed, NULL);
    }
}

void ui_theme_apply(ui_theme_t t)
{
    alarm_ui_theme_set(layout_theme(t));
    if (S.main == NULL) {
        return; /* before ui_app_start(): the screens are made in this theme */
    }
    lv_obj_t *const roots[] = {S.main, S.wizard, lv_layer_top(), lv_layer_sys(), lv_layer_bottom()};
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        if (roots[i] != NULL) {
            alarm_ui_theme_recolor_tree(roots[i], alarm_ui_palette);
        }
    }
    if (S.saver != NULL) {
        alarm_ui_theme_recolor_tree(S.saver, saver_palette());
    }
    lv_obj_report_style_change(NULL);
    ui_app_theme_changed();
    /* What C keeps as a colour (the hero's tone, a list's tones) is worked out again, and the page
     * that is open is drawn again unless a sheet is over it (its rows keep their place). */
    if (S.cfg != NULL) {
        ui_refresh(true);
        if (S.depth > 0u && lv_subject_get_int(&subject_view) == VIEW_SETTINGS &&
            lv_subject_get_int(&subject_sheet) == SHEET_NONE) {
            ui_page_rebuild();
        }
    }
}

ui_theme_t ui_theme_current(void)
{
    switch (alarm_ui_theme_get()) {
    case ALARM_UI_THEME_LIGHT: return UI_THEME_LIGHT;
    case ALARM_UI_THEME_CONTRAST: return UI_THEME_CONTRAST;
    case ALARM_UI_THEME_CONTRAST_LIGHT: return UI_THEME_CONTRAST_LIGHT;
    default: return UI_THEME_DARK;
    }
}
