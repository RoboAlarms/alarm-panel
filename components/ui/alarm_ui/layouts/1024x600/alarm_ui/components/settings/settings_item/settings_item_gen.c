/**
 * @file settings_item_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "settings_item_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * settings_item_create(lv_obj_t * parent, const char * glyph, const char * title)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_settings_item;
    static lv_style_t style_settings_item_pressed;

    static bool style_inited = false;

    if (!style_inited) {
        /*Init all styles*/
        lv_style_init(&style_settings_item);
        lv_style_init(&style_settings_item_pressed);

        alarm_ui_run_20(&style_settings_item, lv_pct(100), 52, RADIUS_LG);
        alarm_ui_run_15(&style_settings_item, SPACE_MD, SPACE_MD);
        lv_style_set_pad_column(&style_settings_item, SPACE_MD);
        lv_style_set_text_color(&style_settings_item, COLOR_TEXT_2);
        alarm_ui_run_6(&style_settings_item_pressed, COLOR_PRESSED, (255 * 100 / 100));

        alarm_ui_style_track(&style_settings_item, ALARM_UI_STYLE_SETTINGS_ITEM_SETTINGS_ITEM);
        alarm_ui_style_track(&style_settings_item_pressed, ALARM_UI_STYLE_SETTINGS_ITEM_SETTINGS_ITEM_PRESSED);

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_21(parent);
        alarm_ui_run_16(box_0, &style_settings_item, 0, &style_settings_item_pressed, LV_STATE_PRESSED);
        lv_obj_t * lv_label_0 = lv_label_create(box_0);
        lv_obj_set_width(lv_label_0, 28);
        lv_label_set_text(lv_label_0, glyph);
        lv_obj_set_style_text_font(lv_label_0, icons_20, 0);
        lv_obj_set_style_text_align(lv_label_0, LV_TEXT_ALIGN_CENTER, 0);

        lv_obj_t * lv_label_1 = alarm_ui_run_44(box_0, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(lv_label_1, title);
        alarm_ui_run_4(lv_label_1, font_label, COLOR_TEXT);

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
