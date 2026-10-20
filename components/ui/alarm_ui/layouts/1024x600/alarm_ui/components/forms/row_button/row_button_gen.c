/**
 * @file row_button_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "row_button_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * row_button_create(lv_obj_t * parent, const char * glyph, const char * text, lv_color_t color)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_row_button;
    static lv_style_t style_row_button_pressed;

    static bool style_inited = false;

    if (!style_inited) {
        /*Init all styles*/
        lv_style_init(&style_row_button);
        lv_style_init(&style_row_button_pressed);

        alarm_ui_run_20(&style_row_button, lv_pct(100), 60, RADIUS_MD);
        lv_style_set_bg_opa(&style_row_button, (255 * 14 / 100));
        lv_style_set_border_width(&style_row_button, 1);
        alarm_ui_run_17(&style_row_button, LV_FLEX_FLOW_ROW);
        lv_style_set_pad_column(&style_row_button, SPACE_SM);
        alarm_ui_run_45(&style_row_button, font_label, COLOR_TEXT);
        lv_style_set_bg_opa(&style_row_button_pressed, (255 * 30 / 100));

        alarm_ui_style_track(&style_row_button, ALARM_UI_STYLE_ROW_BUTTON_ROW_BUTTON);
        alarm_ui_style_track(&style_row_button_pressed, ALARM_UI_STYLE_ROW_BUTTON_ROW_BUTTON_PRESSED);

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_21(parent);
        lv_obj_set_style_bg_color(box_0, color, 0);
        lv_obj_set_style_border_color(box_0, color, 0);
        alarm_ui_run_16(box_0, &style_row_button, 0, &style_row_button_pressed, LV_STATE_PRESSED);
        lv_obj_t * lv_label_0 = lv_label_create(box_0);
        lv_label_set_text(lv_label_0, glyph);
        lv_obj_set_style_text_font(lv_label_0, icons_20, 0);
        lv_obj_set_style_text_color(lv_label_0, color, 0);

        lv_obj_t * text_label = lv_label_create(box_0);
        lv_label_set_text(text_label, text);
        lv_obj_set_name(text_label, "text_label");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
