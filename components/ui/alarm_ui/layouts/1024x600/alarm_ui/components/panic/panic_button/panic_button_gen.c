/**
 * @file panic_button_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "panic_button_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * panic_button_create(lv_obj_t * parent, const char * glyph, const char * text, lv_color_t color)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_panic_ring;
    static lv_style_t style_panic_ring_indicator;
    static lv_style_t style_panic_button;
    static lv_style_t style_panic_button_pressed;

    static bool style_inited = false;

    if (!style_inited) {
        static lv_style_t * const style_list[] = {
            &style_panic_ring,
            &style_panic_ring_indicator,
            &style_panic_button,
            &style_panic_button_pressed,
        };

        /*Init all styles*/
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            lv_style_init(style_list[i]);
        }

        lv_style_set_arc_width(&style_panic_ring, 10);
        lv_style_set_arc_opa(&style_panic_ring, (255 * 0 / 100));
        lv_style_set_arc_width(&style_panic_ring_indicator, 10);
        lv_style_set_arc_rounded(&style_panic_ring_indicator, true);
        alarm_ui_run_20(&style_panic_button, 176, 176, RADIUS_FULL);
        lv_style_set_bg_opa(&style_panic_button, (255 * 16 / 100));
        lv_style_set_border_width(&style_panic_button, 2);
        lv_style_set_border_opa(&style_panic_button, (255 * 50 / 100));
        lv_style_set_bg_opa(&style_panic_button_pressed, (255 * 40 / 100));
        lv_style_set_transform_width(&style_panic_button_pressed, -6);
        lv_style_set_transform_height(&style_panic_button_pressed, -6);

        static const uint16_t style_ids[] = {
            ALARM_UI_STYLE_PANIC_BUTTON_PANIC_RING,
            ALARM_UI_STYLE_PANIC_BUTTON_PANIC_RING_INDICATOR,
            ALARM_UI_STYLE_PANIC_BUTTON_PANIC_BUTTON,
            ALARM_UI_STYLE_PANIC_BUTTON_PANIC_BUTTON_PRESSED,
        };
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            alarm_ui_style_track(style_list[i], style_ids[i]);
        }

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_0(parent, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        alarm_ui_run_1(box_0, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_3(box_0);
        lv_obj_set_style_pad_row(box_0, SPACE_LG, 0);
        lv_obj_t * ring_box = alarm_ui_run_0(box_0, 212, 212);
        lv_obj_set_flag(ring_box, LV_OBJ_FLAG_EVENT_BUBBLE, true);
        lv_obj_t * progress = lv_arc_create(ring_box);
        alarm_ui_run_32(progress, lv_pct(100), lv_pct(100));
        lv_arc_set_bg_start_angle(progress, 0);
        lv_arc_set_bg_end_angle(progress, 360);
        lv_arc_set_rotation(progress, 270);
        lv_arc_set_min_value(progress, 0);
        lv_arc_set_max_value(progress, 100);
        lv_arc_set_value(progress, 0);
        lv_obj_set_flag(progress, LV_OBJ_FLAG_CLICKABLE, false);
        lv_obj_set_style_arc_color(progress, color, LV_PART_INDICATOR);
        alarm_ui_run_16(progress, &style_panic_ring, 0, &style_panic_ring_indicator, LV_PART_INDICATOR);
        alarm_ui_run_69(progress, &style_knob_off, LV_PART_KNOB, "progress");

        lv_obj_t * button = box_create(ring_box);
        lv_obj_set_align(button, LV_ALIGN_CENTER);
        lv_obj_set_flag(button, LV_OBJ_FLAG_CLICKABLE, true);
        lv_obj_set_flag(button, LV_OBJ_FLAG_EVENT_BUBBLE, true);
        lv_obj_set_flag(button, LV_OBJ_FLAG_PRESS_LOCK, true);
        lv_obj_set_style_bg_color(button, color, 0);
        lv_obj_set_style_border_color(button, color, 0);
        alarm_ui_run_16(button, &style_panic_button, 0, &style_panic_button_pressed, LV_STATE_PRESSED);
        lv_obj_set_name(button, "button");
        lv_obj_t * lv_label_0 = alarm_ui_run_33(button);
        lv_label_set_text(lv_label_0, glyph);
        lv_obj_set_style_text_font(lv_label_0, icons_96, 0);
        lv_obj_set_style_text_color(lv_label_0, color, 0);

        lv_obj_t * lv_label_1 = lv_label_create(box_0);
        lv_label_set_text(lv_label_1, text);
        lv_obj_set_style_text_font(lv_label_1, font_title, 0);

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
