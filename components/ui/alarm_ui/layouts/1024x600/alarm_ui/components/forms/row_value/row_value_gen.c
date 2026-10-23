/**
 * @file row_value_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "row_value_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * row_value_create(lv_obj_t * parent, const char * title, const char * value)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_row_value;

    static bool style_inited = false;

    if (!style_inited) {
        /*Init all styles*/
        lv_style_init(&style_row_value);

        alarm_ui_run_39(&style_row_value);
        lv_style_set_min_height(&style_row_value, 52);
        lv_style_set_border_side(&style_row_value, LV_BORDER_SIDE_BOTTOM);
        alarm_ui_run_66(&style_row_value, 1, COLOR_LINE);
        alarm_ui_run_7(&style_row_value, SPACE_XS, SPACE_XS, 12, 13, SPACE_LG);

        alarm_ui_style_track(&style_row_value, ALARM_UI_STYLE_ROW_VALUE_ROW_VALUE);

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_25(parent, &style_row_value);
        lv_obj_t * title_label = lv_label_create(box_0);
        lv_label_set_text(title_label, title);
        alarm_ui_run_48(title_label, "title_label");

        lv_obj_t * value_label = alarm_ui_run_44(box_0, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(value_label, value);
        alarm_ui_run_58(value_label, LV_TEXT_ALIGN_RIGHT, "value_label");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
