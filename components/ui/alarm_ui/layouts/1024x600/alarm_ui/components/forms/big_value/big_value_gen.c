/**
 * @file big_value_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "big_value_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * big_value_create(lv_obj_t * parent, const char * label, const char * value)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_big_value;

    static bool style_inited = false;

    if (!style_inited) {
        /*Init all styles*/
        lv_style_init(&style_big_value);

        alarm_ui_run_39(&style_big_value);
        alarm_ui_run_6(&style_big_value, COLOR_SURFACE, (255 * 100 / 100));
        lv_style_set_radius(&style_big_value, RADIUS_MD);
        lv_style_set_pad_all(&style_big_value, SPACE_LG);
        alarm_ui_run_17(&style_big_value, LV_FLEX_FLOW_COLUMN);
        lv_style_set_pad_row(&style_big_value, SPACE_XS);

        alarm_ui_style_track(&style_big_value, ALARM_UI_STYLE_BIG_VALUE_BIG_VALUE);

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_25(parent, &style_big_value);
        lv_obj_t * label_label = alarm_ui_run_2(box_0, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(label_label, label);
        alarm_ui_run_4(label_label, font_body, COLOR_TEXT_2);
        lv_obj_set_style_text_align(label_label, LV_TEXT_ALIGN_CENTER, 0);

        lv_obj_t * value_label = lv_label_create(box_0);
        lv_label_set_text(value_label, value);
        lv_obj_set_style_text_font(value_label, font_headline, 0);
        lv_obj_set_style_text_letter_space(value_label, 6, 0);
        lv_obj_set_name(value_label, "value_label");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
