/**
 * @file hero_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "hero_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * hero_create(lv_obj_t * parent)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_hero;
    static lv_style_t style_ring_track;
    static lv_style_t style_ring_indicator;
    static lv_style_t style_disk;
    static lv_style_t style_hero_alarm;

    static bool style_inited = false;

    if (!style_inited) {
        static lv_style_t * const style_list[] = {
            &style_hero,
            &style_ring_track,
            &style_ring_indicator,
            &style_disk,
            &style_hero_alarm,
        };

        /*Init all styles*/
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            lv_style_init(style_list[i]);
        }

        alarm_ui_run_6(&style_hero, COLOR_SURFACE, (255 * 100 / 100));
        alarm_ui_run_23(&style_hero, RADIUS_XL);
        lv_style_set_pad_all(&style_hero, SPACE_XL);
        alarm_ui_run_17(&style_hero, LV_FLEX_FLOW_COLUMN);
        lv_style_set_pad_row(&style_hero, SPACE_SM);
        lv_style_set_arc_width(&style_ring_track, 12);
        lv_style_set_arc_color(&style_ring_track, COLOR_TEXT);
        lv_style_set_arc_opa(&style_ring_track, (255 * 8 / 100));
        lv_style_set_arc_width(&style_ring_indicator, 12);
        lv_style_set_arc_rounded(&style_ring_indicator, true);
        alarm_ui_run_59(&style_disk, RADIUS_FULL, (255 * 12 / 100));
        lv_style_set_shadow_width(&style_disk, 72);
        lv_style_set_shadow_opa(&style_disk, (255 * 30 / 100));
        lv_style_set_bg_color(&style_hero_alarm, COLOR_ALARM_CARD);
        lv_style_set_border_color(&style_hero_alarm, COLOR_RED);
        lv_style_set_border_width(&style_hero_alarm, 2);

        static const uint16_t style_ids[] = {
            ALARM_UI_STYLE_HERO_HERO,
            ALARM_UI_STYLE_HERO_RING_TRACK,
            ALARM_UI_STYLE_HERO_RING_INDICATOR,
            ALARM_UI_STYLE_HERO_DISK,
            ALARM_UI_STYLE_HERO_HERO_ALARM,
        };
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            alarm_ui_style_track(style_list[i], style_ids[i]);
        }

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_25(parent, &style_hero);
        lv_obj_bind_style(box_0, &style_hero_alarm, 0, &subject_alarm_active, 1);
        lv_obj_t * ring_box = alarm_ui_run_0(box_0, 236, 236);
        lv_obj_set_style_margin_bottom(ring_box, SPACE_MD, 0);
        lv_obj_t * ring = lv_arc_create(ring_box);
        alarm_ui_run_31(ring, lv_pct(100), lv_pct(100));
        lv_arc_set_bg_start_angle(ring, 0);
        lv_arc_set_bg_end_angle(ring, 360);
        lv_arc_set_rotation(ring, 270);
        lv_arc_set_min_value(ring, 0);
        lv_arc_set_max_value(ring, 1000);
        lv_arc_bind_value(ring, &subject_countdown_pct);
        lv_obj_set_flag(ring, LV_OBJ_FLAG_CLICKABLE, false);
        alarm_ui_run_15(ring, &style_ring_track, 0, &style_ring_indicator, LV_PART_INDICATOR);
        lv_obj_add_style(ring, &style_knob_off, LV_PART_KNOB);
        lv_obj_bind_style_prop(ring, LV_STYLE_ARC_COLOR, LV_PART_INDICATOR, &subject_tone_color);
        lv_obj_set_name(ring, "ring");

        lv_obj_t * disk = alarm_ui_run_0(ring_box, 176, 176);
        lv_obj_set_align(disk, LV_ALIGN_CENTER);
        lv_obj_add_style(disk, &style_disk, 0);
        lv_obj_bind_style_prop(disk, LV_STYLE_BG_COLOR, 0, &subject_tone_color);
        lv_obj_bind_style_prop(disk, LV_STYLE_SHADOW_COLOR, 0, &subject_tone_color);
        lv_obj_set_name(disk, "disk");
        lv_obj_t * hero_icon = alarm_ui_run_33(disk);
        alarm_ui_run_12(hero_icon, icons_96, &subject_hero_icon);
        lv_obj_bind_style_prop(hero_icon, LV_STYLE_TEXT_COLOR, 0, &subject_tone_color);
        lv_obj_bind_flag_if_gt(hero_icon, &subject_countdown, LV_OBJ_FLAG_HIDDEN, 0);

        lv_obj_t * hero_count = alarm_ui_run_33(disk);
        lv_obj_set_style_text_font(hero_count, font_digits, 0);
        lv_label_bind_text(hero_count, &subject_countdown, "%d");
        lv_obj_bind_flag_if_eq(hero_count, &subject_countdown, LV_OBJ_FLAG_HIDDEN, 0);

        lv_obj_t * hero_headline = lv_label_create(box_0);
        alarm_ui_run_12(hero_headline, font_headline, &subject_headline);

        lv_obj_t * hero_detail = alarm_ui_run_2(box_0, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(hero_detail, LV_TEXT_ALIGN_CENTER, 0);
        alarm_ui_run_29(hero_detail, COLOR_TEXT_2, &subject_detail);

        lv_obj_t * hero_note = alarm_ui_run_2(box_0, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(hero_note, LV_TEXT_ALIGN_CENTER, 0);
        alarm_ui_run_4(hero_note, font_caption, COLOR_TEXT_3);
        lv_label_bind_text(hero_note, &subject_hero_note, NULL);

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
