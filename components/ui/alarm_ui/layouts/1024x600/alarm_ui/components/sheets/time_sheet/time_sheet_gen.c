/**
 * @file time_sheet_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "time_sheet_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * time_sheet_create(lv_obj_t * parent)
{
    LV_TRACE_OBJ_CREATE("begin");


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_0(parent, lv_pct(100), lv_pct(100));
        alarm_ui_run_29(box_0, &style_backdrop);
        lv_obj_set_name_static(box_0, "time_sheet_#");
        lv_obj_t * time_card = alarm_ui_run_0(box_0, 880, LV_SIZE_CONTENT);
        alarm_ui_run_34(time_card);
        lv_obj_t * box_1 = alarm_ui_run_0(time_card, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(box_1, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(box_1);
        lv_obj_set_style_pad_column(box_1, SPACE_MD, 0);
        lv_obj_t * box_2 = alarm_ui_run_8(box_1);
        alarm_ui_run_1(box_2, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box_2, 2, 0);
        lv_obj_t * lv_label_0 = alarm_ui_run_2(box_2, LV_LABEL_LONG_MODE_DOTS);
        alarm_ui_run_12(lv_label_0, font_title, &subject_sheet_title);

        lv_obj_t * lv_label_1 = alarm_ui_run_2(box_2, LV_LABEL_LONG_MODE_WRAP);
        alarm_ui_run_30(lv_label_1, COLOR_TEXT_2, &subject_sheet_note);

        lv_obj_t * time_cancel = alarm_ui_run_5(box_1, &style_button_pressed, LV_STATE_PRESSED);
        lv_obj_add_event_cb(time_cancel, ui_on_sheet_cancel, LV_EVENT_CLICKED, NULL);
        alarm_ui_run_14(time_cancel, alarm_ui_tr("Cancel"));

        lv_obj_t * time_ok = alarm_ui_run_5(box_1, &style_button_primary, 0);
        alarm_ui_run_54(time_ok, &style_button_tone_pressed, LV_STATE_PRESSED, &style_disabled, LV_STATE_DISABLED);
        alarm_ui_run_10(time_ok, ui_on_sheet_done, "time_ok");
        alarm_ui_run_14(time_ok, alarm_ui_tr("Done"));

        lv_obj_t * box_3 = alarm_ui_run_0(time_card, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(box_3, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_main_place(box_3, LV_FLEX_ALIGN_CENTER, 0);
        alarm_ui_run_3(box_3);
        lv_obj_set_style_pad_column(box_3, SPACE_XXL, 0);
        lv_obj_set_style_pad_top(box_3, SPACE_SM, 0);
        alarm_ui_run_69(box_3, "time_at");

        lv_obj_t * box_4 = alarm_ui_run_0(box_3, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        alarm_ui_run_1(box_4, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box_4, SPACE_SM, 0);
        lv_obj_t * lv_label_4 = alarm_ui_run_14(box_4, alarm_ui_tr_c("the days an automation runs on", "Days"));
        alarm_ui_run_4(lv_label_4, font_caption, COLOR_TEXT_2);

        lv_obj_t * box_5 = alarm_ui_run_0(box_4, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        alarm_ui_run_1(box_5, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(box_5, SPACE_SM, 0);
        alarm_ui_run_72(box_5, "day_1");

        alarm_ui_run_72(box_5, "day_2");

        alarm_ui_run_72(box_5, "day_3");

        alarm_ui_run_72(box_5, "day_4");

        alarm_ui_run_72(box_5, "day_5");

        alarm_ui_run_72(box_5, "day_6");

        alarm_ui_run_72(box_5, "day_7");

        lv_obj_t * box_6 = alarm_ui_run_0(time_card, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_19(box_6);
        lv_obj_t * time_result = alarm_ui_run_2(box_6, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(time_result, "");
        alarm_ui_run_37(time_result, font_label, "time_result");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
