/**
 * @file section_title_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "section_title_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * section_title_create(lv_obj_t * parent, const char * text)
{
    LV_TRACE_OBJ_CREATE("begin");


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_0(parent, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_top(box_0, SPACE_MD, 0);
        lv_obj_set_style_pad_left(box_0, SPACE_XS, 0);
        lv_obj_t * text_label = lv_label_create(box_0);
        lv_label_set_text(text_label, text);
        alarm_ui_run_4(text_label, font_caption, COLOR_TEXT_3);
        lv_obj_set_name(text_label, "text_label");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
