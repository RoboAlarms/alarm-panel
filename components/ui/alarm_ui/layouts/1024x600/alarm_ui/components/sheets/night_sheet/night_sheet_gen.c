/**
 * @file night_sheet_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "night_sheet_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * night_sheet_create(lv_obj_t * parent)
{
    LV_TRACE_OBJ_CREATE("begin");


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_0(parent, lv_pct(100), lv_pct(100));
        alarm_ui_run_32(box_0);
        lv_obj_set_name_static(box_0, "night_sheet_#");
        lv_obj_t * night_card = alarm_ui_run_0(box_0, 760, LV_SIZE_CONTENT);
        alarm_ui_run_34(night_card);
        lv_obj_t * box_1 = alarm_ui_run_0(night_card, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(box_1, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(box_1);
        lv_obj_set_style_pad_column(box_1, SPACE_MD, 0);
        lv_obj_t * box_2 = alarm_ui_run_8(box_1);
        alarm_ui_run_1(box_2, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box_2, 2, 0);
        lv_obj_t * lv_label_0 = alarm_ui_run_2(box_2, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(lv_label_0, alarm_ui_tr("Night hours"));
        lv_obj_set_style_text_font(lv_label_0, font_title, 0);

        lv_obj_t * lv_label_1 = alarm_ui_run_2(box_2, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(lv_label_1, alarm_ui_tr("From when to when, every night"));
        lv_obj_set_style_text_color(lv_label_1, COLOR_TEXT_2, 0);

        lv_obj_t * night_cancel = alarm_ui_run_5(box_1, &style_button_pressed, LV_STATE_PRESSED);
        alarm_ui_run_9(night_cancel, ui_on_sheet_cancel, "night_cancel");
        alarm_ui_run_16(night_cancel, alarm_ui_tr("Cancel"));

        lv_obj_t * night_ok = alarm_ui_run_5(box_1, &style_button_primary, 0);
        lv_obj_add_style(night_ok, &style_button_tone_pressed, LV_STATE_PRESSED);
        alarm_ui_run_9(night_ok, ui_on_sheet_done, "night_ok");
        alarm_ui_run_16(night_ok, alarm_ui_tr("Done"));

        lv_obj_t * box_3 = alarm_ui_run_0(night_card, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(box_3, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_main_place(box_3, LV_FLEX_ALIGN_SPACE_AROUND, 0);
        lv_obj_set_style_pad_top(box_3, SPACE_SM, 0);
        lv_obj_t * box_4 = alarm_ui_run_0(box_3, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        alarm_ui_run_1(box_4, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_53(box_4, SPACE_SM);
        lv_obj_t * lv_label_4 = alarm_ui_run_16(box_4, alarm_ui_tr_c("night hours", "Starts"));
        lv_obj_set_style_text_font(lv_label_4, font_label, 0);

        alarm_ui_run_65(box_4, "night_start");

        lv_obj_t * box_5 = alarm_ui_run_0(box_3, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        alarm_ui_run_1(box_5, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_53(box_5, SPACE_SM);
        lv_obj_t * lv_label_5 = alarm_ui_run_16(box_5, alarm_ui_tr_c("night hours", "Ends"));
        lv_obj_set_style_text_font(lv_label_5, font_label, 0);

        alarm_ui_run_65(box_5, "night_end");

        lv_obj_t * box_6 = alarm_ui_run_0(night_card, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_18(box_6);
        alarm_ui_run_1(box_6, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(box_6);
        lv_obj_set_style_pad_column(box_6, SPACE_LG, 0);
        lv_obj_t * lv_label_6 = alarm_ui_run_16(box_6, alarm_ui_tr("Night lasts"));
        lv_obj_set_style_text_color(lv_label_6, COLOR_TEXT_2, 0);

        lv_obj_t * night_result = alarm_ui_run_41(box_6, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(night_result, "");
        alarm_ui_run_54(night_result, LV_TEXT_ALIGN_RIGHT, "night_result");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
