/**
 * @file confirm_dialog_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "confirm_dialog_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * confirm_dialog_create(lv_obj_t * parent)
{
    LV_TRACE_OBJ_CREATE("begin");


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_0(parent, lv_pct(100), lv_pct(100));
        alarm_ui_run_32(box_0, &style_backdrop);
        lv_obj_t * dialog_card = alarm_ui_run_0(box_0, 560, LV_SIZE_CONTENT);
        alarm_ui_run_34(dialog_card);
        lv_obj_set_name(dialog_card, "dialog_card");
        lv_obj_t * lv_label_0 = alarm_ui_run_2(dialog_card, LV_LABEL_LONG_MODE_WRAP);
        alarm_ui_run_12(lv_label_0, font_title, &subject_sheet_title);

        lv_obj_t * lv_label_1 = alarm_ui_run_2(dialog_card, LV_LABEL_LONG_MODE_WRAP);
        alarm_ui_run_29(lv_label_1, COLOR_TEXT_2, &subject_sheet_note);

        lv_obj_t * box_1 = alarm_ui_run_0(dialog_card, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_margin_top(box_1, SPACE_MD, 0);
        alarm_ui_run_1(box_1, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_main_place(box_1, LV_FLEX_ALIGN_END, 0);
        lv_obj_set_style_pad_column(box_1, SPACE_MD, 0);
        lv_obj_t * dialog_cancel = alarm_ui_run_22(box_1, LV_SIZE_CONTENT, LV_OBJ_FLAG_CLICKABLE);
        alarm_ui_run_54(dialog_cancel, &style_button, 0, &style_button_pressed, LV_STATE_PRESSED);
        alarm_ui_run_10(dialog_cancel, ui_on_sheet_cancel, "dialog_cancel");
        alarm_ui_run_62(dialog_cancel, &subject_sheet_cancel);

        lv_obj_t * dialog_ok = alarm_ui_run_22(box_1, LV_SIZE_CONTENT, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_bind_flag_if_not_eq(dialog_ok, &subject_sheet_tone, LV_OBJ_FLAG_HIDDEN, 0);
        alarm_ui_run_54(dialog_ok, &style_button, 0, &style_button_danger, 0);
        lv_obj_add_style(dialog_ok, &style_button_tone_pressed, LV_STATE_PRESSED);
        alarm_ui_run_10(dialog_ok, ui_on_sheet_done, "dialog_ok");
        alarm_ui_run_62(dialog_ok, &subject_sheet_ok);

        lv_obj_t * dialog_ok_primary = alarm_ui_run_22(box_1, LV_SIZE_CONTENT, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_bind_flag_if_not_eq(dialog_ok_primary, &subject_sheet_tone, LV_OBJ_FLAG_HIDDEN, 1);
        alarm_ui_run_54(dialog_ok_primary, &style_button, 0, &style_button_primary, 0);
        lv_obj_add_style(dialog_ok_primary, &style_button_tone_pressed, LV_STATE_PRESSED);
        alarm_ui_run_10(dialog_ok_primary, ui_on_sheet_done, "dialog_ok_primary");
        alarm_ui_run_62(dialog_ok_primary, &subject_sheet_ok);

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
