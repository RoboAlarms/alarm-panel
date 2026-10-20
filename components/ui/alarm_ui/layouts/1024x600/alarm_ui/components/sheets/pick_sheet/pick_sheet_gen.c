/**
 * @file pick_sheet_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "pick_sheet_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * pick_sheet_create(lv_obj_t * parent)
{
    LV_TRACE_OBJ_CREATE("begin");


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_0(parent, lv_pct(100), lv_pct(100));
        alarm_ui_run_29(box_0, &style_backdrop);
        lv_obj_t * pick_card = alarm_ui_run_0(box_0, 600, LV_SIZE_CONTENT);
        lv_obj_set_style_max_height(pick_card, PICK_MAX_H, 0);
        alarm_ui_run_34(pick_card);
        lv_obj_t * box_1 = alarm_ui_run_0(pick_card, lv_pct(100), LV_SIZE_CONTENT);
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

        lv_obj_t * pick_close = alarm_ui_run_0(box_1, 44, 44);
        alarm_ui_run_60(pick_close, LV_OBJ_FLAG_CLICKABLE, true, 10);
        alarm_ui_run_22(pick_close, RADIUS_FULL, COLOR_RAISED, (255 * 100 / 100));
        alarm_ui_run_10(pick_close, ui_on_sheet_cancel, "pick_close");
        lv_obj_t * lv_label_2 = alarm_ui_run_11(pick_close, "\xEE\x86\xB2");
        alarm_ui_run_4(lv_label_2, icons_20, COLOR_TEXT_2);

        lv_obj_t * pick_list = alarm_ui_run_0(pick_card, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_13(pick_list);
        alarm_ui_run_1(pick_list, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_49(pick_list, SPACE_SM, "pick_list");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
