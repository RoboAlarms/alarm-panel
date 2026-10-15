/**
 * @file row_stepper_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "row_stepper_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * row_stepper_create(lv_obj_t * parent, const char * title, const char * detail, const char * value)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_row_stepper;
    static lv_style_t style_step_button;
    static lv_style_t style_step_button_pressed;

    static bool style_inited = false;

    if (!style_inited) {
        static lv_style_t * const style_list[] = {
            &style_row_stepper,
            &style_step_button,
            &style_step_button_pressed,
        };

        /*Init all styles*/
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            lv_style_init(style_list[i]);
        }

        alarm_ui_run_36(&style_row_stepper);
        lv_style_set_min_height(&style_row_stepper, 68);
        alarm_ui_run_6(&style_row_stepper, COLOR_SURFACE, (255 * 100 / 100));
        lv_style_set_radius(&style_row_stepper, RADIUS_MD);
        alarm_ui_run_7(&style_row_stepper, SPACE_LG, SPACE_MD, SPACE_MD, SPACE_MD, SPACE_MD);
        alarm_ui_run_19(&style_step_button, 48, 48, RADIUS_FULL);
        alarm_ui_run_6(&style_step_button, COLOR_RAISED, (255 * 100 / 100));
        lv_style_set_text_color(&style_step_button, COLOR_TEXT);
        lv_style_set_bg_color(&style_step_button_pressed, COLOR_PRESSED);

        static const uint16_t style_ids[] = {
            ALARM_UI_STYLE_ROW_STEPPER_ROW_STEPPER,
            ALARM_UI_STYLE_ROW_STEPPER_STEP_BUTTON,
            ALARM_UI_STYLE_ROW_STEPPER_STEP_BUTTON_PRESSED,
        };
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            alarm_ui_style_track(style_list[i], style_ids[i]);
        }

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_25(parent, &style_row_stepper);
        lv_obj_t * box_1 = alarm_ui_run_8(box_0);
        alarm_ui_run_1(box_1, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box_1, 2, 0);
        lv_obj_t * title_label = alarm_ui_run_2(box_1, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(title_label, title);
        alarm_ui_run_35(title_label, font_label, "title_label");

        lv_obj_t * detail_label = alarm_ui_run_2(box_1, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(detail_label, detail);
        alarm_ui_run_4(detail_label, font_caption, COLOR_TEXT_2);
        lv_obj_set_name(detail_label, "detail_label");

        lv_obj_t * minus = alarm_ui_run_20(box_0);
        lv_obj_set_ext_click_area(minus, 8);
        alarm_ui_run_15(minus, &style_step_button, 0, &style_step_button_pressed, LV_STATE_PRESSED);
        lv_obj_set_name(minus, "minus");
        lv_obj_t * lv_label_0 = alarm_ui_run_11(minus, "\xEE\x84\x9C");
        lv_obj_set_style_text_font(lv_label_0, icons_20, 0);

        lv_obj_t * value_label = lv_label_create(box_0);
        lv_obj_set_width(value_label, 112);
        lv_label_set_text(value_label, value);
        alarm_ui_run_54(value_label, LV_TEXT_ALIGN_CENTER, "value_label");

        lv_obj_t * plus = alarm_ui_run_20(box_0);
        lv_obj_set_ext_click_area(plus, 8);
        alarm_ui_run_15(plus, &style_step_button, 0, &style_step_button_pressed, LV_STATE_PRESSED);
        lv_obj_set_name(plus, "plus");
        lv_obj_t * lv_label_1 = alarm_ui_run_11(plus, "\xEE\x84\xBD");
        lv_obj_set_style_text_font(lv_label_1, icons_20, 0);

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
