/**
 * @file window_sheet_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "window_sheet_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * window_sheet_create(lv_obj_t * parent)
{
    LV_TRACE_OBJ_CREATE("begin");


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_0(parent, lv_pct(100), lv_pct(100));
        alarm_ui_run_32(box_0, &style_backdrop);
        lv_obj_set_name_static(box_0, "window_sheet_#");
        lv_obj_t * window_card = alarm_ui_run_0(box_0, 760, LV_SIZE_CONTENT);
        alarm_ui_run_34(window_card);
        lv_obj_t * box_1 = alarm_ui_run_0(window_card, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(box_1, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(box_1);
        lv_obj_set_style_pad_column(box_1, SPACE_MD, 0);
        lv_obj_t * lv_label_0 = alarm_ui_run_44(box_1, LV_LABEL_LONG_MODE_DOTS);
        alarm_ui_run_12(lv_label_0, font_title, &subject_sheet_title);

        lv_obj_t * window_cancel = alarm_ui_run_5(box_1, &style_button_pressed, LV_STATE_PRESSED);
        lv_obj_add_event_cb(window_cancel, ui_on_sheet_cancel, LV_EVENT_CLICKED, NULL);
        alarm_ui_run_14(window_cancel, alarm_ui_tr("Cancel"));

        lv_obj_t * window_ok = alarm_ui_run_5(box_1, &style_button_primary, 0);
        alarm_ui_run_54(window_ok, &style_button_tone_pressed, LV_STATE_PRESSED, &style_disabled, LV_STATE_DISABLED);
        alarm_ui_run_10(window_ok, ui_on_sheet_done, "window_ok");
        alarm_ui_run_14(window_ok, alarm_ui_tr("Done"));

        lv_obj_t * box_2 = alarm_ui_run_0(window_card, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(box_2, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_main_place(box_2, LV_FLEX_ALIGN_SPACE_AROUND, 0);
        lv_obj_set_style_pad_top(box_2, SPACE_SM, 0);
        lv_obj_t * box_3 = alarm_ui_run_0(box_2, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        alarm_ui_run_1(box_3, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_57(box_3, SPACE_SM);
        lv_obj_t * lv_label_3 = alarm_ui_run_14(box_3, alarm_ui_tr_c("a window of time", "From"));
        lv_obj_set_style_text_font(lv_label_3, font_label, 0);

        alarm_ui_run_68(box_3, "window_from");

        lv_obj_t * box_4 = alarm_ui_run_0(box_2, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        alarm_ui_run_1(box_4, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_57(box_4, SPACE_SM);
        lv_obj_t * lv_label_4 = alarm_ui_run_14(box_4, alarm_ui_tr_c("a window of time", "To"));
        lv_obj_set_style_text_font(lv_label_4, font_label, 0);

        alarm_ui_run_68(box_4, "window_to");

        lv_obj_t * box_5 = alarm_ui_run_0(window_card, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_18(box_5);
        alarm_ui_run_1(box_5, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(box_5);
        lv_obj_set_style_pad_column(box_5, SPACE_LG, 0);
        lv_obj_t * window_result = alarm_ui_run_44(box_5, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(window_result, "");
        alarm_ui_run_37(window_result, font_label, "window_result");

        lv_obj_t * window_any = alarm_ui_run_5(box_5, &style_button_pressed, LV_STATE_PRESSED);
        lv_obj_set_name(window_any, "window_any");
        alarm_ui_run_14(window_any, alarm_ui_tr_c("a window of time", "Any time"));

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
