/**
 * @file device_stepper_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "device_stepper_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * device_stepper_create(lv_obj_t * parent, const char * label, const char * value)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_device_step;
    static lv_style_t style_device_step_pressed;

    static bool style_inited = false;

    if (!style_inited) {
        /*Init all styles*/
        lv_style_init(&style_device_step);
        lv_style_init(&style_device_step_pressed);

        alarm_ui_run_19(&style_device_step, 48, 48, RADIUS_FULL);
        alarm_ui_run_6(&style_device_step, COLOR_RAISED, (255 * 100 / 100));
        lv_style_set_text_color(&style_device_step, COLOR_TEXT);
        lv_style_set_bg_color(&style_device_step_pressed, COLOR_PRESSED);

        alarm_ui_style_track(&style_device_step, ALARM_UI_STYLE_DEVICE_STEPPER_DEVICE_STEP);
        alarm_ui_style_track(&style_device_step_pressed, ALARM_UI_STYLE_DEVICE_STEPPER_DEVICE_STEP_PRESSED);

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_0(parent, lv_pct(100), 48);
        alarm_ui_run_1(box_0, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(box_0);
        lv_obj_set_style_pad_column(box_0, SPACE_MD, 0);
        lv_obj_t * minus = alarm_ui_run_20(box_0);
        lv_obj_set_ext_click_area(minus, 8);
        alarm_ui_run_15(minus, &style_device_step, 0, &style_device_step_pressed, LV_STATE_PRESSED);
        lv_obj_set_name(minus, "minus");
        lv_obj_t * lv_label_0 = alarm_ui_run_11(minus, "\xEE\x84\x9C");
        lv_obj_set_style_text_font(lv_label_0, icons_20, 0);

        lv_obj_t * box_1 = alarm_ui_run_8(box_0);
        alarm_ui_run_1(box_1, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_main_place(box_1, LV_FLEX_ALIGN_CENTER, 0);
        alarm_ui_run_3(box_1);
        lv_obj_set_style_pad_column(box_1, SPACE_SM, 0);
        lv_obj_t * what_label = lv_label_create(box_1);
        lv_label_set_text(what_label, label);
        alarm_ui_run_45(what_label, "what_label");

        lv_obj_t * value_label = lv_label_create(box_1);
        lv_label_set_text(value_label, value);
        alarm_ui_run_35(value_label, font_title, "value_label");

        lv_obj_t * plus = alarm_ui_run_20(box_0);
        lv_obj_set_ext_click_area(plus, 8);
        alarm_ui_run_15(plus, &style_device_step, 0, &style_device_step_pressed, LV_STATE_PRESSED);
        lv_obj_set_name(plus, "plus");
        lv_obj_t * lv_label_1 = alarm_ui_run_11(plus, "\xEE\x84\xBD");
        lv_obj_set_style_text_font(lv_label_1, icons_20, 0);

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
