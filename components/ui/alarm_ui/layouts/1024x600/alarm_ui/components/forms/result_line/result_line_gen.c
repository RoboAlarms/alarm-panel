/**
 * @file result_line_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "result_line_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * result_line_create(lv_obj_t * parent, const char * text)
{
    LV_TRACE_OBJ_CREATE("begin");


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_0(parent, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_19(box_0);
        lv_obj_t * result_label = alarm_ui_run_2(box_0, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(result_label, text);
        alarm_ui_run_37(result_label, font_label, "result_label");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
