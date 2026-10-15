/**
 * @file pad_key_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "pad_key_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * pad_key_create(lv_obj_t * parent, const char * text, const lv_font_t * font)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_pad_key;
    static lv_style_t style_pad_key_pressed;

    static bool style_inited = false;

    if (!style_inited) {
        /*Init all styles*/
        lv_style_init(&style_pad_key);
        lv_style_init(&style_pad_key_pressed);

        lv_style_set_width(&style_pad_key, KEY_W);
        alarm_ui_run_11(&style_pad_key, KEY_H, RADIUS_MD, COLOR_RAISED, (255 * 100 / 100));
        lv_style_set_text_color(&style_pad_key, COLOR_TEXT);
        lv_style_set_bg_color(&style_pad_key_pressed, COLOR_PRESSED);
        lv_style_set_transform_width(&style_pad_key_pressed, -2);
        lv_style_set_transform_height(&style_pad_key_pressed, -2);

        alarm_ui_style_track(&style_pad_key, ALARM_UI_STYLE_PAD_KEY_PAD_KEY);
        alarm_ui_style_track(&style_pad_key_pressed, ALARM_UI_STYLE_PAD_KEY_PAD_KEY_PRESSED);

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_34(parent);
        alarm_ui_run_17(box_0, &style_pad_key, 0, &style_pad_key_pressed, LV_STATE_PRESSED);
        lv_obj_t * lv_label_0 = alarm_ui_run_15(box_0);
        lv_label_set_text(lv_label_0, text);
        lv_obj_set_style_text_font(lv_label_0, font, 0);

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
