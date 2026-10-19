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
        alarm_ui_run_66(box_0, LV_FLEX_ALIGN_END, SPACE_SM);
        lv_obj_t * box_1 = box_create(box_0);
        lv_obj_set_width(box_1, roll_w);
        lv_obj_set_height(box_1, LV_SIZE_CONTENT);
        alarm_ui_run_1(box_1, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_57(box_1, SPACE_XS);
        lv_obj_t * lv_label_0 = alarm_ui_run_14(box_1, alarm_ui_tr_c("set the clock", "Hour"));
        alarm_ui_run_4(lv_label_0, font_caption, COLOR_TEXT_2);

        alarm_ui_run_36(box_1, lv_pct(100), "roll_hour");

        lv_obj_t * box_2 = box_create(box_0);
        lv_obj_set_width(box_2, roll_w);
        lv_obj_set_height(box_2, LV_SIZE_CONTENT);
        alarm_ui_run_1(box_2, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_57(box_2, SPACE_XS);
        lv_obj_t * lv_label_1 = alarm_ui_run_14(box_2, alarm_ui_tr_c("set the clock", "Minute"));
        alarm_ui_run_4(lv_label_1, font_caption, COLOR_TEXT_2);

        alarm_ui_run_36(box_2, lv_pct(100), "roll_minute");

        lv_obj_t * ampm_col = box_create(box_0);
        lv_obj_set_width(ampm_col, ampm_w);
        lv_obj_set_height(ampm_col, LV_SIZE_CONTENT);
        lv_obj_set_name(ampm_col, "ampm_col");
        alarm_ui_run_36(ampm_col, lv_pct(100), "roll_ampm");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
