/**
 * @file hold_button_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "hold_button_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * hold_button_create(lv_obj_t * parent, const char * text, lv_color_t color)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_hold_button;
    static lv_style_t style_hold_fill;
    static lv_style_t style_hold_fill_indicator;

    static bool style_inited = false;

    if (!style_inited) {
        static lv_style_t * const style_list[] = {
            &style_hold_button,
            &style_hold_fill,
            &style_hold_fill_indicator,
        };

        /*Init all styles*/
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            lv_style_init(style_list[i]);
        }

        alarm_ui_run_26(&style_hold_button, lv_pct(100), 56);
        alarm_ui_run_42(&style_hold_button, RADIUS_MD, (255 * 14 / 100));
        lv_style_set_border_width(&style_hold_button, 1);
        lv_style_set_clip_corner(&style_hold_button, true);
        alarm_ui_run_42(&style_hold_fill, 0, (255 * 0 / 100));
        alarm_ui_run_42(&style_hold_fill_indicator, 0, (255 * 45 / 100));

        static const uint16_t style_ids[] = {
            ALARM_UI_STYLE_HOLD_BUTTON_HOLD_BUTTON,
            ALARM_UI_STYLE_HOLD_BUTTON_HOLD_FILL,
            ALARM_UI_STYLE_HOLD_BUTTON_HOLD_FILL_INDICATOR,
        };
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            alarm_ui_style_track(style_list[i], style_ids[i]);
        }

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_34(parent);
        lv_obj_set_style_bg_color(box_0, color, 0);
        lv_obj_set_style_border_color(box_0, color, 0);
        lv_obj_add_style(box_0, &style_hold_button, 0);
        lv_obj_t * fill = lv_bar_create(box_0);
        alarm_ui_run_22(fill, lv_pct(100), lv_pct(100));
        lv_bar_set_min_value(fill, 0);
        lv_bar_set_max_value(fill, 100);
        lv_bar_set_value(fill, 0, LV_ANIM_OFF);
        lv_obj_set_flag(fill, LV_OBJ_FLAG_CLICKABLE, false);
        lv_obj_set_style_bg_color(fill, color, LV_PART_INDICATOR);
        alarm_ui_run_17(fill, &style_hold_fill, 0, &style_hold_fill_indicator, LV_PART_INDICATOR);
        lv_obj_set_name(fill, "fill");

        lv_obj_t * lv_label_0 = alarm_ui_run_15(box_0);
        lv_label_set_text(lv_label_0, text);
        lv_obj_set_style_text_font(lv_label_0, font_label, 0);

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
