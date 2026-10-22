/**
 * @file cal_day_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "cal_day_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * cal_day_create(lv_obj_t * parent)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_cal_day;
    static lv_style_t style_cal_day_pressed;
    static lv_style_t style_cal_day_today;
    static lv_style_t style_cal_day_chosen;

    static bool style_inited = false;

    if (!style_inited) {
        static lv_style_t * const style_list[] = {
            &style_cal_day,
            &style_cal_day_pressed,
            &style_cal_day_today,
            &style_cal_day_chosen,
        };

        /*Init all styles*/
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            lv_style_init(style_list[i]);
        }

        lv_style_set_height(&style_cal_day, lv_pct(100));
        lv_style_set_flex_grow(&style_cal_day, 1);
        lv_style_set_radius(&style_cal_day, RADIUS_FULL);
        alarm_ui_run_6(&style_cal_day, COLOR_PRESSED, (255 * 0 / 100));
        alarm_ui_run_45(&style_cal_day, font_label, COLOR_TEXT);
        lv_style_set_bg_opa(&style_cal_day_pressed, (255 * 100 / 100));
        alarm_ui_run_66(&style_cal_day_today, 2, COLOR_LINE);
        alarm_ui_run_6(&style_cal_day_chosen, COLOR_BLUE, (255 * 100 / 100));
        lv_style_set_text_color(&style_cal_day_chosen, COLOR_ON_TONE);

        static const uint16_t style_ids[] = {
            ALARM_UI_STYLE_CAL_DAY_CAL_DAY,
            ALARM_UI_STYLE_CAL_DAY_CAL_DAY_PRESSED,
            ALARM_UI_STYLE_CAL_DAY_CAL_DAY_TODAY,
            ALARM_UI_STYLE_CAL_DAY_CAL_DAY_CHOSEN,
        };
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            alarm_ui_style_track(style_list[i], style_ids[i]);
        }

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_21(parent);
        alarm_ui_run_16(box_0, &style_cal_day, 0, &style_cal_day_pressed, LV_STATE_PRESSED);
        alarm_ui_run_16(box_0, &style_cal_day_today, LV_STATE_USER_1, &style_cal_day_chosen, LV_STATE_CHECKED);
        lv_obj_t * day_label = alarm_ui_run_11(box_0, "");
        lv_obj_set_name(day_label, "day_label");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
