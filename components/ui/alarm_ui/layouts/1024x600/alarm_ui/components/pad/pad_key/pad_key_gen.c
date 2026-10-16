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
    static lv_style_t style_pad_key_short;
    static lv_style_t style_pad_key_disabled;

    static bool style_inited = false;

    if (!style_inited) {
        static lv_style_t * const style_list[] = {
            &style_pad_key,
            &style_pad_key_pressed,
            &style_pad_key_short,
            &style_pad_key_disabled,
        };

        /*Init all styles*/
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            lv_style_init(style_list[i]);
        }

        alarm_ui_run_19(&style_pad_key, 120, 60, RADIUS_MD);
        alarm_ui_run_6(&style_pad_key, COLOR_RAISED, (255 * 100 / 100));
        lv_style_set_text_color(&style_pad_key, COLOR_TEXT);
        lv_style_set_bg_color(&style_pad_key_pressed, COLOR_PRESSED);
        lv_style_set_transform_width(&style_pad_key_pressed, -3);
        lv_style_set_transform_height(&style_pad_key_pressed, -3);
        lv_style_set_height(&style_pad_key_short, 48);
        lv_style_set_text_color(&style_pad_key_disabled, COLOR_TEXT_3);

        static const uint16_t style_ids[] = {
            ALARM_UI_STYLE_PAD_KEY_PAD_KEY,
            ALARM_UI_STYLE_PAD_KEY_PAD_KEY_PRESSED,
            ALARM_UI_STYLE_PAD_KEY_PAD_KEY_SHORT,
            ALARM_UI_STYLE_PAD_KEY_PAD_KEY_DISABLED,
        };
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            alarm_ui_style_track(style_list[i], style_ids[i]);
        }

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_20(parent);
        alarm_ui_run_16(box_0, &style_pad_key, 0, &style_pad_key_pressed, LV_STATE_PRESSED);
        alarm_ui_run_16(box_0, &style_pad_key_short, LV_STATE_USER_2, &style_pad_key_disabled, LV_STATE_DISABLED);
        lv_obj_t * lv_label_0 = alarm_ui_run_33(box_0);
        lv_label_set_text(lv_label_0, text);
        lv_obj_set_style_text_font(lv_label_0, font, 0);

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
