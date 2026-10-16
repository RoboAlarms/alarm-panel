/**
 * @file action_pair_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "action_pair_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * action_pair_create(lv_obj_t * parent)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_action_pair;

    static bool style_inited = false;

    if (!style_inited) {
        /*Init all styles*/
        lv_style_init(&style_action_pair);

        alarm_ui_run_39(&style_action_pair);
        lv_style_set_layout(&style_action_pair, LV_LAYOUT_FLEX);
        lv_style_set_flex_flow(&style_action_pair, LV_FLEX_FLOW_ROW);
        lv_style_set_flex_cross_place(&style_action_pair, LV_FLEX_ALIGN_START);
        lv_style_set_flex_track_place(&style_action_pair, LV_FLEX_ALIGN_START);
        lv_style_set_pad_column(&style_action_pair, SPACE_MD);

        alarm_ui_style_track(&style_action_pair, ALARM_UI_STYLE_ACTION_PAIR_ACTION_PAIR);

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_25(parent, &style_action_pair);
        lv_obj_t * left = alarm_ui_run_8(box_0);
        lv_obj_set_name(left, "left");

        lv_obj_t * right = alarm_ui_run_8(box_0);
        lv_obj_set_name(right, "right");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
