/**
 * @file month_calendar_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "month_calendar_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * month_calendar_create(lv_obj_t * parent, int32_t cal_w, int32_t day_h)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_cal_round;
    static lv_style_t style_cal_week;
    static lv_style_t style_cal_row;

    static bool style_inited = false;

    if (!style_inited) {
        static lv_style_t * const style_list[] = {
            &style_cal_round,
            &style_cal_week,
            &style_cal_row,
        };

        /*Init all styles*/
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            lv_style_init(style_list[i]);
        }

        alarm_ui_run_19(&style_cal_round, 44, 44, RADIUS_FULL);
        alarm_ui_run_6(&style_cal_round, COLOR_RAISED, (255 * 100 / 100));
        alarm_ui_run_45(&style_cal_round, icons_20, COLOR_TEXT);
        lv_style_set_flex_grow(&style_cal_week, 1);
        alarm_ui_run_45(&style_cal_week, font_caption, COLOR_TEXT_3);
        lv_style_set_text_align(&style_cal_week, LV_TEXT_ALIGN_CENTER);
        lv_style_set_width(&style_cal_row, lv_pct(100));
        lv_style_set_layout(&style_cal_row, LV_LAYOUT_FLEX);
        lv_style_set_flex_flow(&style_cal_row, LV_FLEX_FLOW_ROW);
        lv_style_set_pad_column(&style_cal_row, SPACE_XS);

        static const uint16_t style_ids[] = {
            ALARM_UI_STYLE_MONTH_CALENDAR_CAL_ROUND,
            ALARM_UI_STYLE_MONTH_CALENDAR_CAL_WEEK,
            ALARM_UI_STYLE_MONTH_CALENDAR_CAL_ROW,
        };
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            alarm_ui_style_track(style_list[i], style_ids[i]);
        }

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = box_create(parent);
        lv_obj_set_width(box_0, cal_w);
        lv_obj_set_height(box_0, LV_SIZE_CONTENT);
        alarm_ui_run_1(box_0, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box_0, SPACE_SM, 0);
        lv_obj_t * cal_head = alarm_ui_run_0(box_0, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(cal_head, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(cal_head);
        lv_obj_set_style_pad_column(cal_head, SPACE_SM, 0);
        lv_obj_t * cal_prev = alarm_ui_run_20(cal_head);
        lv_obj_set_ext_click_area(cal_prev, 6);
        lv_obj_add_style(cal_prev, &style_cal_round, 0);
        lv_obj_add_style(cal_prev, &style_button_pressed, LV_STATE_PRESSED);
        lv_obj_set_name(cal_prev, "cal_prev");
        alarm_ui_run_11(cal_prev, "\xEE\x81\xAE");

        lv_obj_t * cal_title = box_create(cal_head);
        lv_obj_set_flex_grow(cal_title, 1);
        lv_obj_set_height(cal_title, 44);
        lv_obj_set_flag(cal_title, LV_OBJ_FLAG_CLICKABLE, true);
        alarm_ui_run_21(cal_title, RADIUS_FULL, COLOR_PRESSED, (255 * 0 / 100));
        lv_obj_set_style_bg_opa(cal_title, (255 * 100 / 100), LV_STATE_PRESSED);
        alarm_ui_run_1(cal_title, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_main_place(cal_title, LV_FLEX_ALIGN_CENTER, 0);
        alarm_ui_run_3(cal_title);
        alarm_ui_run_48(cal_title, SPACE_XS, "cal_title");
        lv_obj_t * cal_month = lv_label_create(cal_title);
        lv_label_set_long_mode(cal_month, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(cal_month, "");
        lv_obj_set_style_max_width(cal_month, lv_pct(85), 0);
        alarm_ui_run_37(cal_month, font_label, "cal_month");

        lv_obj_t * lv_label_1 = alarm_ui_run_23(cal_title, "\xEE\x81\xAD");
        alarm_ui_run_4(lv_label_1, icons_20, COLOR_TEXT_3);

        lv_obj_t * cal_next = alarm_ui_run_20(cal_head);
        lv_obj_set_ext_click_area(cal_next, 6);
        lv_obj_add_style(cal_next, &style_cal_round, 0);
        lv_obj_add_style(cal_next, &style_button_pressed, LV_STATE_PRESSED);
        lv_obj_set_name(cal_next, "cal_next");
        alarm_ui_run_11(cal_next, "\xEE\x81\xAF");

        lv_obj_t * cal_body = alarm_ui_run_0(box_0, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(cal_body, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_49(cal_body, SPACE_XS, "cal_body");
        lv_obj_t * cal_week = alarm_ui_run_0(cal_body, lv_pct(100), 24);
        alarm_ui_run_1(cal_week, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(cal_week);
        alarm_ui_run_48(cal_week, SPACE_XS, "cal_week");
        lv_obj_t * lv_label_3 = alarm_ui_run_23(cal_week, "");
        lv_obj_add_style(lv_label_3, &style_cal_week, 0);

        lv_obj_t * lv_label_4 = alarm_ui_run_23(cal_week, "");
        lv_obj_add_style(lv_label_4, &style_cal_week, 0);

        lv_obj_t * lv_label_5 = alarm_ui_run_23(cal_week, "");
        lv_obj_add_style(lv_label_5, &style_cal_week, 0);

        lv_obj_t * lv_label_6 = alarm_ui_run_23(cal_week, "");
        lv_obj_add_style(lv_label_6, &style_cal_week, 0);

        lv_obj_t * lv_label_7 = alarm_ui_run_23(cal_week, "");
        lv_obj_add_style(lv_label_7, &style_cal_week, 0);

        lv_obj_t * lv_label_8 = alarm_ui_run_23(cal_week, "");
        lv_obj_add_style(lv_label_8, &style_cal_week, 0);

        lv_obj_t * lv_label_9 = alarm_ui_run_23(cal_week, "");
        lv_obj_add_style(lv_label_9, &style_cal_week, 0);

        lv_obj_t * cal_days = alarm_ui_run_0(cal_body, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(cal_days, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_49(cal_days, SPACE_XS, "cal_days");
        lv_obj_t * box_1 = box_create(cal_days);
        lv_obj_set_height(box_1, day_h);
        lv_obj_add_style(box_1, &style_cal_row, 0);

        lv_obj_t * box_2 = box_create(cal_days);
        lv_obj_set_height(box_2, day_h);
        lv_obj_add_style(box_2, &style_cal_row, 0);

        lv_obj_t * box_3 = box_create(cal_days);
        lv_obj_set_height(box_3, day_h);
        lv_obj_add_style(box_3, &style_cal_row, 0);

        lv_obj_t * box_4 = box_create(cal_days);
        lv_obj_set_height(box_4, day_h);
        lv_obj_add_style(box_4, &style_cal_row, 0);

        lv_obj_t * box_5 = box_create(cal_days);
        lv_obj_set_height(box_5, day_h);
        lv_obj_add_style(box_5, &style_cal_row, 0);

        lv_obj_t * box_6 = box_create(cal_days);
        lv_obj_set_height(box_6, day_h);
        lv_obj_add_style(box_6, &style_cal_row, 0);

        lv_obj_t * cal_pick = alarm_ui_run_30(cal_body);
        lv_obj_set_flag(cal_pick, LV_OBJ_FLAG_HIDDEN, true);
        alarm_ui_run_1(cal_pick, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_main_place(cal_pick, LV_FLEX_ALIGN_CENTER, 0);
        alarm_ui_run_3(cal_pick);
        alarm_ui_run_48(cal_pick, SPACE_SM, "cal_pick");
        lv_obj_t * roll_month = lv_roller_create(cal_pick);
        lv_obj_set_flex_grow(roll_month, 1);
        alarm_ui_run_54(roll_month, &style_roller, 0, &style_roller_selected, LV_PART_SELECTED);
        lv_obj_set_name(roll_month, "roll_month");

        alarm_ui_run_36(cal_pick, 112, "roll_year");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
