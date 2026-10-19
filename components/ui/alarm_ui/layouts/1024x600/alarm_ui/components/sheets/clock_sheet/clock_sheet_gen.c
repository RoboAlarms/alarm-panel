/**
 * @file clock_sheet_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "clock_sheet_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * clock_sheet_create(lv_obj_t * parent)
{
    LV_TRACE_OBJ_CREATE("begin");


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_0(parent, lv_pct(100), lv_pct(100));
        alarm_ui_run_32(box_0, &style_backdrop);
        lv_obj_set_name_static(box_0, "clock_sheet_#");
        lv_obj_t * clock_card = alarm_ui_run_0(box_0, 880, LV_SIZE_CONTENT);
        alarm_ui_run_34(clock_card);
        lv_obj_t * box_1 = alarm_ui_run_0(clock_card, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(box_1, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(box_1);
        lv_obj_set_style_pad_column(box_1, SPACE_MD, 0);
        lv_obj_t * box_2 = alarm_ui_run_8(box_1);
        alarm_ui_run_1(box_2, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box_2, 2, 0);
        lv_obj_t * lv_label_0 = alarm_ui_run_2(box_2, LV_LABEL_LONG_MODE_DOTS);
        alarm_ui_run_73(lv_label_0, alarm_ui_tr("Set the date and time"));

        lv_obj_t * lv_label_1 = alarm_ui_run_2(box_2, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(lv_label_1, alarm_ui_tr("The internet corrects it once the panel is online"));
        lv_obj_set_style_text_color(lv_label_1, COLOR_TEXT_2, 0);

        lv_obj_t * clock_cancel = alarm_ui_run_5(box_1, &style_button_pressed, LV_STATE_PRESSED);
        alarm_ui_run_10(clock_cancel, ui_on_sheet_cancel, "clock_cancel");
        alarm_ui_run_14(clock_cancel, alarm_ui_tr("Cancel"));

        lv_obj_t * clock_ok = alarm_ui_run_5(box_1, &style_button_primary, 0);
        lv_obj_add_style(clock_ok, &style_button_tone_pressed, LV_STATE_PRESSED);
        alarm_ui_run_10(clock_ok, ui_on_sheet_done, "clock_ok");
        alarm_ui_run_14(clock_ok, alarm_ui_tr_c("set the clock", "Set"));

        lv_obj_t * clock_sheet_picker = clock_picker_create(clock_card, CAL_W, CAL_CELL_H, ROLLER_W, ROLLER_AMPM_W, SPACE_XXL, SPACE_LG);
        lv_obj_set_name(clock_sheet_picker, "clock_sheet_picker");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
