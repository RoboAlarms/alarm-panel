/**
 * @file alarm_ui.c
 *
 * Custom (not generated) part of the LVGL Pro project: the event callbacks named in the XML
 * and the switch between the appearances of themes.xml.
 * Keep this file free of platform code so the LVGL Pro Editor can compile it for its preview.
 */

#include "alarm_ui.h"

static alarm_ui_action_cb_t s_action_cb;

void alarm_ui_init(const char * asset_path)
{
    alarm_ui_init_gen(asset_path);
}

void alarm_ui_set_action_cb(alarm_ui_action_cb_t cb)
{
    s_action_cb = cb;
}

/* ---- Appearance (docs/features/07 UI-010, BL-132) --------------------------------------------
 * The palettes, the style registry and what a theme adds to the styles are generated from
 * themes.xml (alarm_ui_gen.c); the switch is here, so the editor's preview has it too. Every
 * palette value stands for one colour name across all the palettes, and is never a literal colour
 * of the XML (tools/lvpro_gen.py): a colour already set anywhere is found by its value.
 * ALARM_UI_THEMES is lvpro_gen.py's: an LVGL Pro Editor export has no theme tables, and
 * builds without the switch until the script runs again. */

#ifdef ALARM_UI_THEMES

static uint32_t s_theme;

static const lv_style_prop_t COLOR_PROPS[] = {
    LV_STYLE_BG_COLOR, LV_STYLE_BG_GRAD_COLOR, LV_STYLE_BG_IMAGE_RECOLOR, LV_STYLE_BORDER_COLOR,
    LV_STYLE_OUTLINE_COLOR, LV_STYLE_SHADOW_COLOR, LV_STYLE_IMAGE_RECOLOR, LV_STYLE_LINE_COLOR,
    LV_STYLE_ARC_COLOR, LV_STYLE_TEXT_COLOR, LV_STYLE_RECOLOR, LV_STYLE_TEXT_OUTLINE_STROKE_COLOR,
    LV_STYLE_DROP_SHADOW_COLOR,
};

uint32_t alarm_ui_theme_get(void)
{
    return s_theme;
}

lv_color_t alarm_ui_theme_color(lv_color_t c, const lv_color_t * to)
{
    for(uint32_t t = 0; t < ALARM_UI_THEME_COUNT; t++) {
        for(uint32_t i = 0; i < ALARM_UI_COLOR_COUNT; i++) {
            if(lv_color_eq(alarm_ui_palettes[t][i], c)) {
                return to[i];
            }
        }
    }
    return c; /* not a palette colour (a photo's shade, a QR code's white): it stays */
}

static bool recolor_style(lv_style_t * style, const lv_color_t * to)
{
    bool changed = false;
    for(size_t i = 0; i < sizeof(COLOR_PROPS) / sizeof(COLOR_PROPS[0]); i++) {
        lv_style_value_t v;
        if(lv_style_get_prop(style, COLOR_PROPS[i], &v) != LV_STYLE_RES_FOUND) {
            continue;
        }
        const lv_color_t c = alarm_ui_theme_color(v.color, to);
        if(!lv_color_eq(c, v.color)) {
            lv_style_value_t nv = {.color = c};
            lv_style_set_prop(style, COLOR_PROPS[i], nv);
            changed = true;
        }
    }
    return changed;
}

void alarm_ui_theme_recolor_tree(lv_obj_t * obj, const lv_color_t * to)
{
    bool changed = false;
    for(uint32_t i = 0; i < obj->style_cnt; i++) {
        if(obj->styles[i].is_local && !obj->styles[i].is_trans) {
            changed |= recolor_style((lv_style_t *)obj->styles[i].style, to);
        }
    }
    if(changed) {
        lv_obj_invalidate(obj);
    }
    const uint32_t n = lv_obj_get_child_count(obj);
    for(uint32_t i = 0; i < n; i++) {
        alarm_ui_theme_recolor_tree(lv_obj_get_child(obj, (int32_t)i), to);
    }
}

static lv_style_value_t override_value(uint8_t kind, int32_t v)
{
    lv_style_value_t out = {.num = v};
    if(kind == ALARM_UI_VALUE_COLOR) {
        out.color = alarm_ui_palette[(uint32_t)v];
    }
    else if(kind == ALARM_UI_VALUE_HEX) {
        out.color = lv_color_hex((uint32_t)v);
    }
    return out;
}

static void override_apply(const alarm_ui_style_override_t * o, lv_style_t * style, bool on)
{
    if(on) {
        lv_style_set_prop(style, o->prop, override_value(o->kind, o->value));
    }
    else if(o->base_kind == ALARM_UI_VALUE_NONE) {
        lv_style_remove_prop(style, o->prop);
    }
    else {
        lv_style_set_prop(style, o->prop, override_value(o->base_kind, o->base));
    }
}

void alarm_ui_style_track(lv_style_t * style, uint32_t id)
{
    if(id >= ALARM_UI_STYLE_COUNT) {
        return;
    }
    alarm_ui_styles[id] = style;
    for(uint32_t i = 0; i < alarm_ui_style_override_count; i++) {
        const alarm_ui_style_override_t * o = &alarm_ui_style_overrides[i];
        if(o->style == id && (((uint32_t)o->themes >> s_theme) & 1u)) {
            override_apply(o, style, true);
        }
    }
}

void alarm_ui_theme_set(uint32_t theme)
{
    if(theme >= ALARM_UI_THEME_COUNT) {
        theme = 0;
    }
    const uint32_t old = s_theme;
    s_theme = theme;
    alarm_ui_palette = alarm_ui_palettes[theme];
    for(uint32_t i = 0; i < ALARM_UI_STYLE_COUNT; i++) {
        if(alarm_ui_styles[i] != NULL) {
            recolor_style(alarm_ui_styles[i], alarm_ui_palette);
        }
    }
    /* What the old theme added goes first, then the new one's: a property two themes change
     * differently ends as the new theme's. */
    for(uint32_t pass = 0; pass < 2u; pass++) {
        for(uint32_t i = 0; i < alarm_ui_style_override_count; i++) {
            const alarm_ui_style_override_t * o = &alarm_ui_style_overrides[i];
            lv_style_t * style = o->style < ALARM_UI_STYLE_COUNT ? alarm_ui_styles[o->style] : NULL;
            const bool was = (((uint32_t)o->themes >> old) & 1u) != 0u;
            const bool now = (((uint32_t)o->themes >> theme) & 1u) != 0u;
            if(style == NULL) {
                continue;
            }
            if(pass == 0u && was && !now) {
                override_apply(o, style, false);
            }
            else if(pass == 1u && now) {
                override_apply(o, style, true);
            }
        }
    }
    for(uint32_t i = 0; i < alarm_ui_color_subject_count; i++) {
        lv_subject_t * s = alarm_ui_color_subjects[i];
        if(s->type != LV_SUBJECT_TYPE_COLOR) {
            continue; /* before alarm_ui_init(): it starts from the palette in force */
        }
        lv_subject_set_color(s, alarm_ui_theme_color(lv_subject_get_color(s), alarm_ui_palette));
    }
    lv_obj_report_style_change(NULL);
}

#endif /*ALARM_UI_THEMES*/

static void act(alarm_ui_action_t action, lv_event_t * e)
{
    if(s_action_cb != NULL) {
        s_action_cb(action, e);
    }
}

void ui_on_arm(lv_event_t * e)
{
    act(ALARM_UI_ACT_ARM, e);
}

void ui_on_disarm(lv_event_t * e)
{
    act(ALARM_UI_ACT_DISARM, e);
}

void ui_on_pad_key(lv_event_t * e)
{
    act(ALARM_UI_ACT_PAD_KEY, e);
}

void ui_on_pad_close(lv_event_t * e)
{
    act(ALARM_UI_ACT_PAD_CLOSE, e);
}

void ui_on_silence(lv_event_t * e)
{
    act(ALARM_UI_ACT_SILENCE, e);
}

void ui_on_clear_memory(lv_event_t * e)
{
    act(ALARM_UI_ACT_CLEAR_MEMORY, e);
}

void ui_on_end_walk_test(lv_event_t * e)
{
    act(ALARM_UI_ACT_END_WALK_TEST, e);
}

void ui_on_zone_bypass(lv_event_t * e)
{
    act(ALARM_UI_ACT_ZONE_BYPASS, e);
}

void ui_on_ack_troubles(lv_event_t * e)
{
    act(ALARM_UI_ACT_ACK_TROUBLES, e);
}

void ui_on_troubles_page(lv_event_t * e)
{
    act(ALARM_UI_ACT_TROUBLES_PAGE, e);
}

void ui_on_panic(lv_event_t * e)
{
    act(ALARM_UI_ACT_PANIC, e);
}

void ui_on_chime(lv_event_t * e)
{
    act(ALARM_UI_ACT_CHIME, e);
}

void ui_on_change_code(lv_event_t * e)
{
    act(ALARM_UI_ACT_CHANGE_CODE, e);
}

void ui_on_settings(lv_event_t * e)
{
    act(ALARM_UI_ACT_SETTINGS, e);
}

void ui_on_wake(lv_event_t * e)
{
    act(ALARM_UI_ACT_WAKE, e);
}

void ui_on_settings_section(lv_event_t * e)
{
    act(ALARM_UI_ACT_SETTINGS_SECTION, e);
}

void ui_on_page_back(lv_event_t * e)
{
    act(ALARM_UI_ACT_PAGE_BACK, e);
}

void ui_on_sheet_done(lv_event_t * e)
{
    act(ALARM_UI_ACT_SHEET_DONE, e);
}

void ui_on_sheet_cancel(lv_event_t * e)
{
    act(ALARM_UI_ACT_SHEET_CANCEL, e);
}

void ui_on_sheet_eye(lv_event_t * e)
{
    act(ALARM_UI_ACT_SHEET_EYE, e);
}

void ui_on_program_exit(lv_event_t * e)
{
    act(ALARM_UI_ACT_PROGRAM_EXIT, e);
}

void ui_on_wizard_next(lv_event_t * e)
{
    act(ALARM_UI_ACT_WIZARD_NEXT, e);
}

void ui_on_wizard_back(lv_event_t * e)
{
    act(ALARM_UI_ACT_WIZARD_BACK, e);
}

void ui_on_wizard_exit(lv_event_t * e)
{
    act(ALARM_UI_ACT_WIZARD_EXIT, e);
}

void ui_on_remote_banner(lv_event_t * e)
{
    act(ALARM_UI_ACT_REMOTE_BANNER, e);
}
