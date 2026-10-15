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
        lv_obj_set_flag(box_0, LV_OBJ_FLAG_CLICKABLE, true);
        lv_obj_add_style(box_0, &style_backdrop, 0);
        lv_obj_t * dialog_card = alarm_ui_run_0(box_0, 420, LV_SIZE_CONTENT);
        lv_obj_set_align(dialog_card, LV_ALIGN_CENTER);
        lv_obj_add_style(dialog_card, &style_sheet_card, 0);
        lv_obj_set_name(dialog_card, "dialog_card");
        lv_obj_t * lv_label_0 = alarm_ui_run_2(dialog_card, LV_LABEL_LONG_MODE_WRAP);
        alarm_ui_run_8(lv_label_0, font_title, &subject_sheet_title);

        lv_obj_t * lv_label_1 = alarm_ui_run_2(dialog_card, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_color(lv_label_1, COLOR_TEXT_2, 0);
        lv_label_bind_text(lv_label_1, &subject_sheet_note, NULL);

        lv_obj_t * box_1 = alarm_ui_run_0(dialog_card, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_margin_top(box_1, SPACE_SM, 0);
        alarm_ui_run_1(box_1, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_main_place(box_1, LV_FLEX_ALIGN_END, 0);
        lv_obj_set_style_pad_column(box_1, SPACE_MD, 0);
        lv_obj_t * dialog_cancel = alarm_ui_run_14(box_1);
        alarm_ui_run_37(dialog_cancel, &style_button, &style_button_pressed, LV_STATE_PRESSED);
        alarm_ui_run_13(dialog_cancel, ui_on_sheet_cancel, "dialog_cancel");
        lv_obj_t * lv_label_2 = lv_label_create(dialog_cancel);
        lv_label_bind_text(lv_label_2, &subject_sheet_cancel, NULL);

        lv_obj_t * dialog_ok = alarm_ui_run_14(box_1);
        alarm_ui_run_37(dialog_ok, &style_button, &style_button_danger, 0);
        lv_obj_add_style(dialog_ok, &style_button_tone_pressed, LV_STATE_PRESSED);
        alarm_ui_run_13(dialog_ok, ui_on_sheet_done, "dialog_ok");
        lv_obj_t * lv_label_3 = lv_label_create(dialog_ok);
        lv_label_bind_text(lv_label_3, &subject_sheet_ok, NULL);

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
