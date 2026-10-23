/**
 * @file device_key_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "device_key_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * device_key_create(lv_obj_t * parent, const char * glyph, const char * text)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_device_key;
    static lv_style_t style_device_key_pressed;

    static bool style_inited = false;

    if (!style_inited) {
        /*Init all styles*/
        lv_style_init(&style_device_key);
        lv_style_init(&style_device_key_pressed);

        alarm_ui_run_65(&style_device_key, lv_pct(100), RADIUS_MD);
        alarm_ui_run_6(&style_device_key, COLOR_RAISED, (255 * 100 / 100));
        alarm_ui_run_17(&style_device_key, LV_FLEX_FLOW_ROW);
        lv_style_set_pad_column(&style_device_key, SPACE_XS);
        lv_style_set_text_font(&style_device_key, font_label);
        lv_style_set_bg_color(&style_device_key_pressed, COLOR_PRESSED);

        alarm_ui_style_track(&style_device_key, ALARM_UI_STYLE_DEVICE_KEY_DEVICE_KEY);
        alarm_ui_style_track(&style_device_key_pressed, ALARM_UI_STYLE_DEVICE_KEY_DEVICE_KEY_PRESSED);

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = box_create(parent);
        lv_obj_set_flex_grow(box_0, 1);
        lv_obj_set_flag(box_0, LV_OBJ_FLAG_CLICKABLE, true);
        alarm_ui_run_16(box_0, &style_device_key, 0, &style_device_key_pressed, LV_STATE_PRESSED);
        lv_obj_t * icon = lv_label_create(box_0);
        lv_label_set_text(icon, glyph);
        alarm_ui_run_37(icon, icons_20, "icon");

        lv_obj_t * text_label = lv_label_create(box_0);
        lv_label_set_text(text_label, text);
        lv_label_set_long_mode(text_label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_max_width(text_label, lv_pct(100), 0);
        lv_obj_set_name(text_label, "text_label");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
