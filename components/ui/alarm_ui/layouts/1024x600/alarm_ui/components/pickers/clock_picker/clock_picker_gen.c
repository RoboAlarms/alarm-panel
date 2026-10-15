/**
 * @file clock_picker_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "clock_picker_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * clock_picker_create(lv_obj_t * parent, int32_t cal_w, int32_t day_h, int32_t roll_w, int32_t ampm_w, int32_t gap, int32_t roll_y)
{
    LV_TRACE_OBJ_CREATE("begin");


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_0(parent, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(box_0, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box_0, SPACE_MD, 0);
        lv_obj_t * box_1 = alarm_ui_run_0(box_0, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(box_1, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_main_place(box_1, LV_FLEX_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_column(box_1, gap, 0);
        lv_obj_t * clock_cal = month_calendar_create(box_1, cal_w, day_h);
        lv_obj_set_name(clock_cal, "clock_cal");

        lv_obj_t * box_2 = alarm_ui_run_0(box_1, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_top(box_2, roll_y, 0);
        lv_obj_t * clock_time = time_rollers_create(box_2, roll_w, ampm_w);
        lv_obj_set_name(clock_time, "clock_time");

        lv_obj_t * box_3 = alarm_ui_run_0(box_0, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_18(box_3);
        alarm_ui_run_1(box_3, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(box_3);
        lv_obj_set_style_pad_column(box_3, SPACE_LG, 0);
        lv_obj_t * lv_label_0 = alarm_ui_run_16(box_3, alarm_ui_tr("The clock will say"));
        lv_obj_set_style_text_color(lv_label_0, COLOR_TEXT_2, 0);

        lv_obj_t * clock_result = alarm_ui_run_41(box_3, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(clock_result, "");
        alarm_ui_run_54(clock_result, LV_TEXT_ALIGN_RIGHT, "clock_result");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
