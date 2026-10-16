/**
 * @file device_sheet_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "device_sheet_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * device_sheet_create(lv_obj_t * parent)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_dsheet_switch;
    static lv_style_t style_dsheet_switch_indicator;
    static lv_style_t style_dsheet_switch_on;
    static lv_style_t style_dsheet_switch_knob;

    static bool style_inited = false;

    if (!style_inited) {
        static lv_style_t * const style_list[] = {
            &style_dsheet_switch,
            &style_dsheet_switch_indicator,
            &style_dsheet_switch_on,
            &style_dsheet_switch_knob,
        };

        /*Init all styles*/
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            lv_style_init(style_list[i]);
        }

        alarm_ui_run_19(&style_dsheet_switch, 64, 36, RADIUS_FULL);
        alarm_ui_run_6(&style_dsheet_switch, COLOR_PRESSED, (255 * 100 / 100));
        alarm_ui_run_62(&style_dsheet_switch_indicator, RADIUS_FULL, (255 * 0 / 100));
        alarm_ui_run_6(&style_dsheet_switch_on, COLOR_GREEN, (255 * 100 / 100));
        lv_style_set_radius(&style_dsheet_switch_knob, RADIUS_FULL);
        alarm_ui_run_6(&style_dsheet_switch_knob, COLOR_TEXT, (255 * 100 / 100));
        lv_style_set_pad_all(&style_dsheet_switch_knob, -4);

        static const uint16_t style_ids[] = {
            ALARM_UI_STYLE_DEVICE_SHEET_DSHEET_SWITCH,
            ALARM_UI_STYLE_DEVICE_SHEET_DSHEET_SWITCH_INDICATOR,
            ALARM_UI_STYLE_DEVICE_SHEET_DSHEET_SWITCH_ON,
            ALARM_UI_STYLE_DEVICE_SHEET_DSHEET_SWITCH_KNOB,
        };
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            alarm_ui_style_track(style_list[i], style_ids[i]);
        }

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_0(parent, lv_pct(100), lv_pct(100));
        alarm_ui_run_32(box_0, &style_backdrop);
        lv_obj_set_name_static(box_0, "device_sheet_#");
        lv_obj_t * device_card = alarm_ui_run_0(box_0, 600, LV_SIZE_CONTENT);
        lv_obj_set_style_max_height(device_card, PICK_MAX_H, 0);
        alarm_ui_run_34(device_card);
        lv_obj_set_name(device_card, "device_card");
        lv_obj_t * box_1 = alarm_ui_run_0(device_card, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(box_1, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_cross_place(box_1, LV_FLEX_ALIGN_START, 0);
        lv_obj_set_style_flex_track_place(box_1, LV_FLEX_ALIGN_START, 0);
        lv_obj_set_style_pad_column(box_1, SPACE_MD, 0);
        lv_obj_t * sheet_chip = icon_chip_create(box_1, "\xEE\x87\x82", 48, icons_28, COLOR_TEXT_3);
        lv_obj_set_name(sheet_chip, "sheet_chip");

        lv_obj_t * box_2 = alarm_ui_run_8(box_1);
        alarm_ui_run_1(box_2, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box_2, 2, 0);
        lv_obj_t * lv_label_0 = alarm_ui_run_2(box_2, LV_LABEL_LONG_MODE_WRAP);
        alarm_ui_run_12(lv_label_0, font_title, &subject_sheet_title);

        lv_obj_t * sheet_state = alarm_ui_run_2(box_2, LV_LABEL_LONG_MODE_WRAP);
        alarm_ui_run_29(sheet_state, COLOR_TEXT_2, &subject_sheet_note);
        lv_obj_set_name(sheet_state, "sheet_state");

        lv_obj_t * sheet_switch = lv_switch_create(box_1);
        lv_obj_set_ext_click_area(sheet_switch, 12);
        lv_obj_set_style_margin_top(sheet_switch, 6, 0);
        alarm_ui_run_16(sheet_switch, &style_dsheet_switch, 0, &style_dsheet_switch_indicator, LV_PART_INDICATOR);
        alarm_ui_run_16(sheet_switch, &style_dsheet_switch_on, LV_PART_INDICATOR | LV_STATE_CHECKED, &style_dsheet_switch_knob, LV_PART_KNOB);
        lv_obj_set_name(sheet_switch, "sheet_switch");

        lv_obj_t * device_close = alarm_ui_run_0(box_1, 44, 44);
        alarm_ui_run_59(device_close, LV_OBJ_FLAG_CLICKABLE, true, 10);
        lv_obj_set_style_margin_top(device_close, 2, 0);
        alarm_ui_run_21(device_close, RADIUS_FULL, COLOR_RAISED, (255 * 100 / 100));
        alarm_ui_run_10(device_close, ui_on_sheet_cancel, "device_close");
        lv_obj_t * lv_label_1 = alarm_ui_run_11(device_close, "\xEE\x86\xB2");
        alarm_ui_run_4(lv_label_1, icons_20, COLOR_TEXT_2);

        lv_obj_t * device_body = alarm_ui_run_0(device_card, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_13(device_body);
        alarm_ui_run_1(device_body, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_49(device_body, SPACE_MD, "device_body");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
