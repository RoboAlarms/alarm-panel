/**
 * @file arm_levels_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "arm_levels_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * arm_levels_create(lv_obj_t * parent)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_arm_level;
    static lv_style_t style_arm_level_pressed;
    static lv_style_t style_arm_level_away;
    static lv_style_t style_arm_level_stay;
    static lv_style_t style_arm_level_night;

    static bool style_inited = false;

    if (!style_inited) {
        static lv_style_t * const style_list[] = {
            &style_arm_level,
            &style_arm_level_pressed,
            &style_arm_level_away,
            &style_arm_level_stay,
            &style_arm_level_night,
        };

        /*Init all styles*/
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            lv_style_init(style_list[i]);
        }

        lv_style_set_flex_grow(&style_arm_level, 1);
        alarm_ui_run_63(&style_arm_level, lv_pct(100), RADIUS_MD);
        alarm_ui_run_6(&style_arm_level, COLOR_RAISED, (255 * 100 / 100));
        lv_style_set_text_color(&style_arm_level, COLOR_TEXT);
        alarm_ui_run_17(&style_arm_level, LV_FLEX_FLOW_ROW);
        lv_style_set_pad_column(&style_arm_level, SPACE_SM);
        lv_style_set_bg_color(&style_arm_level_pressed, COLOR_PRESSED);
        alarm_ui_run_6(&style_arm_level_away, COLOR_ROSE, (255 * 20 / 100));
        alarm_ui_run_64(&style_arm_level_away, 2, COLOR_ROSE);
        lv_style_set_text_color(&style_arm_level_away, COLOR_ROSE);
        alarm_ui_run_6(&style_arm_level_stay, COLOR_BLUE, (255 * 20 / 100));
        alarm_ui_run_64(&style_arm_level_stay, 2, COLOR_BLUE);
        lv_style_set_text_color(&style_arm_level_stay, COLOR_BLUE);
        alarm_ui_run_6(&style_arm_level_night, COLOR_VIOLET, (255 * 20 / 100));
        alarm_ui_run_64(&style_arm_level_night, 2, COLOR_VIOLET);
        lv_style_set_text_color(&style_arm_level_night, COLOR_VIOLET);

        static const uint16_t style_ids[] = {
            ALARM_UI_STYLE_ARM_LEVELS_ARM_LEVEL,
            ALARM_UI_STYLE_ARM_LEVELS_ARM_LEVEL_PRESSED,
            ALARM_UI_STYLE_ARM_LEVELS_ARM_LEVEL_AWAY,
            ALARM_UI_STYLE_ARM_LEVELS_ARM_LEVEL_STAY,
            ALARM_UI_STYLE_ARM_LEVELS_ARM_LEVEL_NIGHT,
        };
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            alarm_ui_style_track(style_list[i], style_ids[i]);
        }

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_0(parent, lv_pct(100), 64);
        alarm_ui_run_1(box_0, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(box_0, SPACE_SM, 0);
        lv_obj_t * level_away = alarm_ui_run_20(box_0);
        alarm_ui_run_16(level_away, &style_arm_level, 0, &style_arm_level_pressed, LV_STATE_PRESSED);
        alarm_ui_run_67(level_away, &style_arm_level_away, LV_STATE_CHECKED, "level_away");
        lv_obj_t * lv_label_0 = alarm_ui_run_23(level_away, "\xEE\x84\x8E");
        lv_obj_set_style_text_font(lv_label_0, icons_20, 0);

        lv_obj_t * lv_label_1 = alarm_ui_run_14(level_away, alarm_ui_tr_c("arming mode", "Away"));
        lv_obj_set_style_text_font(lv_label_1, font_label, 0);

        lv_obj_t * level_stay = alarm_ui_run_20(box_0);
        alarm_ui_run_16(level_stay, &style_arm_level, 0, &style_arm_level_pressed, LV_STATE_PRESSED);
        alarm_ui_run_67(level_stay, &style_arm_level_stay, LV_STATE_CHECKED, "level_stay");
        lv_obj_t * lv_label_2 = alarm_ui_run_23(level_stay, "\xEE\x83\xB5");
        lv_obj_set_style_text_font(lv_label_2, icons_20, 0);

        lv_obj_t * lv_label_3 = alarm_ui_run_14(level_stay, alarm_ui_tr_c("arming mode", "Stay"));
        lv_obj_set_style_text_font(lv_label_3, font_label, 0);

        lv_obj_t * level_night = alarm_ui_run_20(box_0);
        alarm_ui_run_16(level_night, &style_arm_level, 0, &style_arm_level_pressed, LV_STATE_PRESSED);
        alarm_ui_run_67(level_night, &style_arm_level_night, LV_STATE_CHECKED, "level_night");
        lv_obj_t * lv_label_4 = alarm_ui_run_23(level_night, "\xEE\x84\x9E");
        lv_obj_set_style_text_font(lv_label_4, icons_20, 0);

        lv_obj_t * lv_label_5 = alarm_ui_run_14(level_night, alarm_ui_tr_c("arming mode", "Night"));
        lv_obj_set_style_text_font(lv_label_5, font_label, 0);

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
