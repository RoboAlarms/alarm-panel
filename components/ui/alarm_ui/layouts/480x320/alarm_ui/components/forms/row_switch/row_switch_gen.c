/**
 * @file row_switch_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "row_switch_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * row_switch_create(lv_obj_t * parent, const char * title, const char * detail)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_row_setting;
    static lv_style_t style_switch;
    static lv_style_t style_switch_indicator;
    static lv_style_t style_switch_on;
    static lv_style_t style_switch_knob;

    static bool style_inited = false;

    if (!style_inited) {
        static lv_style_t * const style_list[] = {
            &style_row_setting,
            &style_switch,
            &style_switch_indicator,
            &style_switch_on,
            &style_switch_knob,
        };

        /*Init all styles*/
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            lv_style_init(style_list[i]);
        }

        alarm_ui_run_26(&style_row_setting, lv_pct(100), LV_SIZE_CONTENT);
        lv_style_set_min_height(&style_row_setting, 60);
        alarm_ui_run_21(&style_row_setting, SPACE_LG, SPACE_MD, LV_FLEX_FLOW_ROW);
        lv_style_set_flex_cross_place(&style_row_setting, LV_FLEX_ALIGN_CENTER);
        lv_style_set_flex_track_place(&style_row_setting, LV_FLEX_ALIGN_CENTER);
        lv_style_set_pad_column(&style_row_setting, SPACE_LG);
        lv_style_set_width(&style_switch, 56);
        alarm_ui_run_11(&style_switch, 32, RADIUS_FULL, COLOR_PRESSED, (255 * 100 / 100));
        alarm_ui_run_42(&style_switch_indicator, RADIUS_FULL, (255 * 0 / 100));
        alarm_ui_run_24(&style_switch_on, COLOR_GREEN, (255 * 100 / 100));
        lv_style_set_radius(&style_switch_knob, RADIUS_FULL);
        alarm_ui_run_24(&style_switch_knob, COLOR_TEXT, (255 * 100 / 100));
        lv_style_set_pad_all(&style_switch_knob, -4);

        static const uint16_t style_ids[] = {
            ALARM_UI_STYLE_ROW_SWITCH_ROW_SETTING,
            ALARM_UI_STYLE_ROW_SWITCH_SWITCH,
            ALARM_UI_STYLE_ROW_SWITCH_SWITCH_INDICATOR,
            ALARM_UI_STYLE_ROW_SWITCH_SWITCH_ON,
            ALARM_UI_STYLE_ROW_SWITCH_SWITCH_KNOB,
        };
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            alarm_ui_style_track(style_list[i], style_ids[i]);
        }

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_32(parent, &style_row_setting);
        lv_obj_t * box_1 = alarm_ui_run_9(box_0, LV_SIZE_CONTENT);
        alarm_ui_run_1(box_1, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box_1, 0, 0);
        lv_obj_t * title_label = alarm_ui_run_2(box_1, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(title_label, title);
        alarm_ui_run_29(title_label);

        lv_obj_t * detail_label = alarm_ui_run_2(box_1, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(detail_label, detail);
        alarm_ui_run_4(detail_label, font_caption, COLOR_TEXT_2);
        lv_obj_set_name(detail_label, "detail_label");

        lv_obj_t * toggle = lv_switch_create(box_0);
        lv_obj_set_ext_click_area(toggle, 12);
        alarm_ui_run_17(toggle, &style_switch, 0, &style_switch_indicator, LV_PART_INDICATOR);
        alarm_ui_run_17(toggle, &style_switch_on, LV_PART_INDICATOR | LV_STATE_CHECKED, &style_switch_knob, LV_PART_KNOB);
        lv_obj_set_name(toggle, "toggle");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
