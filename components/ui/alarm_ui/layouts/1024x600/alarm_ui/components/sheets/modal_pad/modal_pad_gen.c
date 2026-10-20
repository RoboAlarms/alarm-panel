/**
 * @file modal_pad_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "modal_pad_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * modal_pad_create(lv_obj_t * parent)
{
    LV_TRACE_OBJ_CREATE("begin");


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_0(parent, lv_pct(100), lv_pct(100));
        alarm_ui_run_29(box_0, &style_backdrop);
        lv_obj_t * modal_pad_card = code_pad_create(box_0, 0, 352);
        alarm_ui_run_32(modal_pad_card, 480, LV_SIZE_CONTENT);
        lv_obj_set_align(modal_pad_card, LV_ALIGN_CENTER);
        lv_obj_set_name(modal_pad_card, "modal_pad_card");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
