/**
 * @file row_slider_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "row_slider_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * row_slider_create(lv_obj_t * parent, const char * glyph, const char * title, const char * value)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_row_slider;
    static lv_style_t style_slider;
    static lv_style_t style_slider_indicator;
    static lv_style_t style_slider_knob;

    static bool style_inited = false;

    if (!style_inited) {
        static lv_style_t * const style_list[] = {
            &style_row_slider,
            &style_slider,
            &style_slider_indicator,
            &style_slider_knob,
        };

        /*Init all styles*/
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            lv_style_init(style_list[i]);
        }

        alarm_ui_run_25(&style_row_slider, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_21(&style_row_slider, SPACE_LG, SPACE_XL, LV_FLEX_FLOW_COLUMN);
        lv_style_set_pad_row(&style_row_slider, SPACE_LG);
        alarm_ui_run_11(&style_slider, 8, RADIUS_FULL, COLOR_PRESSED, (255 * 100 / 100));
        lv_style_set_radius(&style_slider_indicator, RADIUS_FULL);
        alarm_ui_run_24(&style_slider_indicator, COLOR_BLUE, (255 * 100 / 100));
        lv_style_set_radius(&style_slider_knob, RADIUS_FULL);
        alarm_ui_run_24(&style_slider_knob, COLOR_TEXT, (255 * 100 / 100));
        lv_style_set_pad_all(&style_slider_knob, 10);

        static const uint16_t style_ids[] = {
            ALARM_UI_STYLE_ROW_SLIDER_ROW_SLIDER,
            ALARM_UI_STYLE_ROW_SLIDER_SLIDER,
            ALARM_UI_STYLE_ROW_SLIDER_SLIDER_INDICATOR,
            ALARM_UI_STYLE_ROW_SLIDER_SLIDER_KNOB,
        };
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            alarm_ui_style_track(style_list[i], style_ids[i]);
        }

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_32(parent, &style_row_slider);
        lv_obj_t * box_1 = alarm_ui_run_0(box_0, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(box_1, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(box_1);
        lv_obj_set_style_pad_column(box_1, SPACE_MD, 0);
        lv_obj_t * lv_label_0 = lv_label_create(box_1);
        lv_label_set_text(lv_label_0, glyph);
        alarm_ui_run_4(lv_label_0, icons_20, COLOR_TEXT_2);

        lv_obj_t * lv_label_1 = lv_label_create(box_1);
        lv_obj_set_flex_grow(lv_label_1, 1);
        lv_label_set_text(lv_label_1, title);
        lv_obj_set_style_text_font(lv_label_1, font_label, 0);

        lv_obj_t * value_label = lv_label_create(box_1);
        lv_label_set_text(value_label, value);
        lv_obj_set_style_text_color(value_label, COLOR_TEXT_2, 0);
        lv_obj_set_name(value_label, "value_label");

        lv_obj_t * slider = lv_slider_create(box_0);
        lv_obj_set_width(slider, lv_pct(100));
        lv_slider_set_min_value(slider, 0);
        lv_slider_set_max_value(slider, 100);
        lv_slider_set_value(slider, 50, LV_ANIM_OFF);
        lv_obj_set_ext_click_area(slider, 20);
        lv_obj_set_style_margin_left(slider, 12, 0);
        lv_obj_set_style_margin_right(slider, 12, 0);
        alarm_ui_run_17(slider, &style_slider, 0, &style_slider_indicator, LV_PART_INDICATOR);
        lv_obj_add_style(slider, &style_slider_knob, LV_PART_KNOB);
        lv_obj_set_name(slider, "slider");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
