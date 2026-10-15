/**
 * @file form_sheet_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "form_sheet_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * form_sheet_create(lv_obj_t * parent)
{
    LV_TRACE_OBJ_CREATE("begin");


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_0(parent, lv_pct(100), lv_pct(100));
        alarm_ui_run_32(box_0);
        lv_obj_set_name_static(box_0, "form_sheet_#");
        lv_obj_t * form_card = alarm_ui_run_0(box_0, 760, LV_SIZE_CONTENT);
        lv_obj_set_style_max_height(form_card, 584, 0);
        alarm_ui_run_34(form_card);
        lv_obj_t * box_1 = alarm_ui_run_0(form_card, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(box_1, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(box_1);
        lv_obj_set_style_pad_column(box_1, SPACE_MD, 0);
        lv_obj_t * box_2 = alarm_ui_run_8(box_1);
        alarm_ui_run_1(box_2, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box_2, 2, 0);
        lv_obj_t * lv_label_0 = alarm_ui_run_2(box_2, LV_LABEL_LONG_MODE_DOTS);
        alarm_ui_run_12(lv_label_0, font_title, &subject_sheet_title);

        lv_obj_t * form_note = alarm_ui_run_2(box_2, LV_LABEL_LONG_MODE_WRAP);
        alarm_ui_run_29(form_note, COLOR_TEXT_2, &subject_sheet_note);
        lv_obj_set_name(form_note, "form_note");

        lv_obj_t * form_cancel = alarm_ui_run_5(box_1, &style_button_pressed, LV_STATE_PRESSED);
        alarm_ui_run_9(form_cancel, ui_on_sheet_cancel, "form_cancel");
        alarm_ui_run_16(form_cancel, alarm_ui_tr("Cancel"));

        lv_obj_t * form_ok = alarm_ui_run_5(box_1, &style_button_primary, 0);
        lv_obj_add_style(form_ok, &style_button_tone_pressed, LV_STATE_PRESSED);
        alarm_ui_run_9(form_ok, ui_on_sheet_done, "form_ok");
        alarm_ui_run_16(form_ok, alarm_ui_tr("Done"));

        lv_obj_t * form_body = alarm_ui_run_0(form_card, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_13(form_body);
        alarm_ui_run_1(form_body, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_48(form_body, SPACE_SM, "form_body");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
