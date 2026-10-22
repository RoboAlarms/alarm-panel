/**
 * @file screen_wizard_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "screen_wizard_gen.h"
#include "../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * screen_wizard_create(void)
{
    LV_TRACE_OBJ_CREATE("begin");

    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        if (screen_wizard == NULL) screen_wizard = lv_obj_create(NULL);
        lv_obj_t * lv_obj_0 = screen_wizard;
        lv_obj_set_flag(lv_obj_0, LV_OBJ_FLAG_SCROLLABLE, false);

        lv_obj_add_style(lv_obj_0, &style_screen, 0);
        lv_obj_t * wiz_row = alarm_ui_run_0(lv_obj_0, lv_pct(100), lv_pct(100));
        alarm_ui_run_1(wiz_row, LV_FLEX_FLOW_ROW);
        lv_obj_t * wiz_side = alarm_ui_run_0(wiz_row, 164, lv_pct(100));
        alarm_ui_run_36(wiz_side, COLOR_RAIL, (255 * 100 / 100));
        lv_obj_set_style_border_side(wiz_side, LV_BORDER_SIDE_RIGHT, 0);
        alarm_ui_run_39(wiz_side, COLOR_LINE);
        lv_obj_set_style_pad_all(wiz_side, SPACE_XXL, 0);
        alarm_ui_run_1(wiz_side, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(wiz_side, SPACE_MD, 0);
        alarm_ui_run_25(wiz_side, &subject_wiz_pad, 1, "wiz_side");
        lv_obj_t * wiz_steps = alarm_ui_run_0(wiz_side, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        alarm_ui_run_1(wiz_steps, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(wiz_steps, SPACE_SM, 0);
        alarm_ui_run_16(wiz_steps, 1);

        alarm_ui_run_16(wiz_steps, 2);

        alarm_ui_run_16(wiz_steps, 3);

        alarm_ui_run_16(wiz_steps, 4);

        alarm_ui_run_16(wiz_steps, 5);

        lv_obj_t * wiz_disk = alarm_ui_run_0(wiz_side, 64, 64);
        lv_obj_set_style_margin_top(wiz_disk, SPACE_MD, 0);
        lv_obj_set_style_radius(wiz_disk, RADIUS_FULL, 0);
        alarm_ui_run_36(wiz_disk, COLOR_BLUE, OPA_TINT);
        lv_obj_set_name(wiz_disk, "wiz_disk");
        lv_obj_t * wiz_icon = alarm_ui_run_15(wiz_disk);
        alarm_ui_run_4(wiz_icon, icons_40, COLOR_BLUE);
        lv_label_bind_text(wiz_icon, &subject_wiz_icon, NULL);

        lv_obj_t * wiz_title = alarm_ui_run_2(wiz_side, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_margin_top(wiz_title, SPACE_XS, 0);
        alarm_ui_run_8(wiz_title, font_title, &subject_wiz_title);

        lv_obj_t * wiz_note = alarm_ui_run_2(wiz_side, LV_LABEL_LONG_MODE_WRAP);
        alarm_ui_run_4(wiz_note, font_caption, COLOR_TEXT_2);
        lv_label_bind_text(wiz_note, &subject_wiz_note, NULL);

        lv_obj_t * wiz_main = alarm_ui_run_9(wiz_row, lv_pct(100));
        lv_obj_set_style_pad_all(wiz_main, SPACE_MD, 0);
        alarm_ui_run_1(wiz_main, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(wiz_main, SPACE_MD, 0);
        lv_obj_t * wiz_body = alarm_ui_run_7(wiz_main);
        alarm_ui_run_1(wiz_body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(wiz_body, SPACE_SM, 0);
        alarm_ui_run_25(wiz_body, &subject_wiz_pad, 1, "wiz_body");

        lv_obj_t * wiz_pad_area = alarm_ui_run_33(wiz_main, lv_pct(100));
        lv_obj_bind_flag_if_eq(wiz_pad_area, &subject_wiz_pad, LV_OBJ_FLAG_HIDDEN, 0);
        lv_obj_t * wiz_pad = code_pad_create(wiz_pad_area, 1, 180);
        alarm_ui_run_22(wiz_pad, lv_pct(100), lv_pct(100));
        lv_obj_set_name(wiz_pad, "wiz_pad");

        lv_obj_t * wiz_footer = alarm_ui_run_0(wiz_main, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(wiz_footer, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(wiz_footer);
        alarm_ui_run_48(wiz_footer, SPACE_MD, &subject_wiz_pad, 1);
        lv_obj_t * wiz_exit = alarm_ui_run_14(wiz_footer);
        alarm_ui_run_37(wiz_exit, &style_button, &style_button_pressed, LV_STATE_PRESSED);
        lv_obj_bind_flag_if_eq(wiz_exit, &subject_wiz_exit, LV_OBJ_FLAG_HIDDEN, 0);
        alarm_ui_run_13(wiz_exit, ui_on_wizard_exit, "wiz_exit");
        lv_obj_t * lv_label_0 = lv_label_create(wiz_exit);
        alarm_ui_run_18(lv_label_0, "\xEE\x84\x8E");

        alarm_ui_run_31(wiz_exit, alarm_ui_tr("Leave"));

        lv_obj_t * wiz_back = alarm_ui_run_0(wiz_footer, 44, 44);
        alarm_ui_run_20(wiz_back, LV_OBJ_FLAG_CLICKABLE, 4);
        lv_obj_set_style_radius(wiz_back, RADIUS_FULL, 0);
        alarm_ui_run_36(wiz_back, COLOR_RAISED, (255 * 100 / 100));
        lv_obj_add_style(wiz_back, &style_button_pressed, LV_STATE_PRESSED);
        lv_obj_bind_flag_if_eq(wiz_back, &subject_wiz_back, LV_OBJ_FLAG_HIDDEN, 0);
        alarm_ui_run_13(wiz_back, ui_on_wizard_back, "wiz_back");
        lv_obj_t * lv_label_2 = alarm_ui_run_15(wiz_back);
        alarm_ui_run_18(lv_label_2, "\xEE\x81\xAE");

        lv_obj_t * wiz_spinner = lv_spinner_create(wiz_footer);
        alarm_ui_run_22(wiz_spinner, 28, 28);
        alarm_ui_run_35(wiz_spinner);

        lv_obj_t * box_5 = alarm_ui_run_0(wiz_footer, 8, 8);
        lv_obj_set_flex_grow(box_5, 1);

        lv_obj_t * wiz_next = alarm_ui_run_14(wiz_footer);
        alarm_ui_run_37(wiz_next, &style_button, &style_button_primary, 0);
        alarm_ui_run_49(wiz_next, &style_button_tone_pressed, LV_STATE_PRESSED, &style_disabled, &subject_wiz_next_on, 0);
        alarm_ui_run_13(wiz_next, ui_on_wizard_next, "wiz_next");
        lv_obj_t * lv_label_3 = lv_label_create(wiz_next);
        lv_label_bind_text(lv_label_3, &subject_wiz_next, NULL);

        lv_obj_t * lv_label_4 = lv_label_create(wiz_next);
        alarm_ui_run_18(lv_label_4, "\xEE\x81\xAF");

        lv_obj_t * wiz_text_sheet = text_sheet_create(lv_obj_0);
        lv_obj_bind_flag_if_not_eq(wiz_text_sheet, &subject_sheet, LV_OBJ_FLAG_HIDDEN, 1);

        lv_obj_t * wiz_pick_sheet = pick_sheet_create(lv_obj_0);
        lv_obj_bind_flag_if_not_eq(wiz_pick_sheet, &subject_sheet, LV_OBJ_FLAG_HIDDEN, 2);

        lv_obj_t * wiz_confirm_dialog = confirm_dialog_create(lv_obj_0);
        lv_obj_bind_flag_if_not_eq(wiz_confirm_dialog, &subject_sheet, LV_OBJ_FLAG_HIDDEN, 3);

        lv_obj_t * wiz_toast = alarm_ui_run_0(lv_obj_0, LV_SIZE_CONTENT, 40);
        alarm_ui_run_43(wiz_toast, LV_ALIGN_TOP_MID, 82, 8);
        lv_obj_set_style_max_width(wiz_toast, 300, 0);
        alarm_ui_run_10(wiz_toast, COLOR_PRESSED, (255 * 100 / 100), RADIUS_FULL);
        alarm_ui_run_39(wiz_toast, lv_color_hex(0x3A4658));
        alarm_ui_run_30(wiz_toast, SPACE_XL, SPACE_XL);
        alarm_ui_run_1(wiz_toast, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(wiz_toast);
        lv_obj_bind_flag_if_eq(wiz_toast, &subject_toast_visible, LV_OBJ_FLAG_HIDDEN, 0);
        lv_obj_t * lv_label_5 = lv_label_create(wiz_toast);
        lv_obj_set_height(lv_label_5, 20);
        lv_label_set_long_mode(lv_label_5, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_max_width(lv_label_5, 276, 0);
        alarm_ui_run_8(lv_label_5, font_label, &subject_toast);

        the_root = lv_obj_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
