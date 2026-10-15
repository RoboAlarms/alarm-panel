/**
 * @file view_header_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "view_header_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * view_header_create(lv_obj_t * parent, const char * title, lv_subject_t * note)
{
    LV_TRACE_OBJ_CREATE("begin");


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_0(parent, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(box_0, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box_0, 2, 0);
        lv_obj_t * lv_label_0 = lv_label_create(box_0);
        lv_label_set_text(lv_label_0, title);
        lv_obj_set_width(lv_label_0, lv_pct(100));
        lv_label_set_long_mode(lv_label_0, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_font(lv_label_0, font_title, 0);

        lv_obj_t * lv_label_1 = alarm_ui_run_2(box_0, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_color(lv_label_1, COLOR_TEXT_2, 0);
        lv_label_bind_text(lv_label_1, note, NULL);

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
