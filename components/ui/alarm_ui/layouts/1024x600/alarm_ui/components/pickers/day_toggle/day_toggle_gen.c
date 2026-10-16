/**
 * @file day_toggle_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "day_toggle_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * day_toggle_create(lv_obj_t * parent)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_day_toggle;
    static lv_style_t style_day_toggle_pressed;
    static lv_style_t style_day_toggle_on;

    static bool style_inited = false;

    if (!style_inited) {
        static lv_style_t * const style_list[] = {
            &style_day_toggle,
            &style_day_toggle_pressed,
            &style_day_toggle_on,
        };

        /*Init all styles*/
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            lv_style_init(style_list[i]);
        }

        alarm_ui_run_19(&style_day_toggle, 64, 64, RADIUS_FULL);
        alarm_ui_run_6(&style_day_toggle, COLOR_RAISED, (255 * 100 / 100));
        alarm_ui_run_45(&style_day_toggle, font_caption, COLOR_TEXT_2);
        lv_style_set_bg_color(&style_day_toggle_pressed, COLOR_PRESSED);
        lv_style_set_bg_color(&style_day_toggle_on, COLOR_BLUE);
        lv_style_set_text_color(&style_day_toggle_on, COLOR_ON_TONE);

        static const uint16_t style_ids[] = {
            ALARM_UI_STYLE_DAY_TOGGLE_DAY_TOGGLE,
            ALARM_UI_STYLE_DAY_TOGGLE_DAY_TOGGLE_PRESSED,
            ALARM_UI_STYLE_DAY_TOGGLE_DAY_TOGGLE_ON,
        };
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            alarm_ui_style_track(style_list[i], style_ids[i]);
        }

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_20(parent);
        alarm_ui_run_59(box_0, LV_OBJ_FLAG_CHECKABLE, true, 4);
        alarm_ui_run_16(box_0, &style_day_toggle, 0, &style_day_toggle_pressed, LV_STATE_PRESSED);
        lv_obj_add_style(box_0, &style_day_toggle_on, LV_STATE_CHECKED);
        lv_obj_t * day_name = alarm_ui_run_11(box_0, "");
        lv_obj_set_name(day_name, "day_name");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
