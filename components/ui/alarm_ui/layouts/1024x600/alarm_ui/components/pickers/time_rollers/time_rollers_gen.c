/**
 * @file time_rollers_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "time_rollers_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * time_rollers_create(lv_obj_t * parent, int32_t roll_w, int32_t ampm_w)
{
    LV_TRACE_OBJ_CREATE("begin");


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_0(parent, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        alarm_ui_run_1(box_0, LV_FLEX_FLOW_ROW);
        alarm_ui_run_62(box_0, LV_FLEX_ALIGN_END, SPACE_SM);
        lv_obj_t * box_1 = box_create(box_0);
        lv_obj_set_width(box_1, roll_w);
        lv_obj_set_height(box_1, LV_SIZE_CONTENT);
        alarm_ui_run_1(box_1, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_53(box_1, SPACE_XS);
        lv_obj_t * lv_label_0 = alarm_ui_run_16(box_1, alarm_ui_tr_c("set the clock", "Hour"));
        alarm_ui_run_4(lv_label_0, font_caption, COLOR_TEXT_2);

        lv_obj_t * roll_hour = lv_roller_create(box_1);
        lv_obj_set_width(roll_hour, lv_pct(100));
        alarm_ui_run_30(roll_hour, &style_roller, 0, &style_roller_selected, LV_PART_SELECTED);
        lv_obj_set_name(roll_hour, "roll_hour");

        lv_obj_t * box_2 = box_create(box_0);
        lv_obj_set_width(box_2, roll_w);
        lv_obj_set_height(box_2, LV_SIZE_CONTENT);
        alarm_ui_run_1(box_2, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_53(box_2, SPACE_XS);
        lv_obj_t * lv_label_1 = alarm_ui_run_16(box_2, alarm_ui_tr_c("set the clock", "Minute"));
        alarm_ui_run_4(lv_label_1, font_caption, COLOR_TEXT_2);

        lv_obj_t * roll_minute = lv_roller_create(box_2);
        lv_obj_set_width(roll_minute, lv_pct(100));
        alarm_ui_run_30(roll_minute, &style_roller, 0, &style_roller_selected, LV_PART_SELECTED);
        lv_obj_set_name(roll_minute, "roll_minute");

        lv_obj_t * ampm_col = box_create(box_0);
        lv_obj_set_width(ampm_col, ampm_w);
        lv_obj_set_height(ampm_col, LV_SIZE_CONTENT);
        lv_obj_set_name(ampm_col, "ampm_col");
        lv_obj_t * roll_ampm = lv_roller_create(ampm_col);
        lv_obj_set_width(roll_ampm, lv_pct(100));
        alarm_ui_run_30(roll_ampm, &style_roller, 0, &style_roller_selected, LV_PART_SELECTED);
        lv_obj_set_name(roll_ampm, "roll_ampm");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
