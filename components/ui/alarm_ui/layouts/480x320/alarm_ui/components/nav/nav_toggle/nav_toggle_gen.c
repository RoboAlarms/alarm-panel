/**
 * @file nav_toggle_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "nav_toggle_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * nav_toggle_create(lv_obj_t * parent, lv_subject_t * icon, const char * text)
{
    LV_TRACE_OBJ_CREATE("begin");


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_34(parent);
        alarm_ui_run_37(box_0, &style_nav_item, &style_nav_item_pressed, LV_STATE_PRESSED);
        lv_obj_t * lv_label_0 = lv_label_create(box_0);
        lv_obj_set_style_text_font(lv_label_0, icons_20, 0);
        lv_label_bind_text(lv_label_0, icon, NULL);

        lv_obj_t * lv_label_1 = lv_label_create(box_0);
        lv_label_set_text(lv_label_1, text);
        lv_obj_set_style_text_font(lv_label_1, font_caption, 0);

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
