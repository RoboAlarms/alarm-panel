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
        lv_obj_t * wiz_side = alarm_ui_run_0(lv_obj_0, 400, lv_pct(100));
        lv_obj_set_style_bg_color(wiz_side, COLOR_RAIL, 0);
        lv_obj_set_style_bg_opa(wiz_side, (255 * 100 / 100), 0);
        lv_obj_set_style_border_side(wiz_side, LV_BORDER_SIDE_RIGHT, 0);
        alarm_ui_run_42(wiz_side, COLOR_LINE);
        lv_obj_set_style_pad_all(wiz_side, 48, 0);
        alarm_ui_run_1(wiz_side, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_48(wiz_side, SPACE_LG, "wiz_side");
        lv_obj_t * wiz_steps = alarm_ui_run_0(wiz_side, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        alarm_ui_run_1(wiz_steps, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(wiz_steps, SPACE_SM, 0);
        lv_obj_t * box_0 = box_create(wiz_steps);
        alarm_ui_run_30(box_0, &style_step, 0, &style_step_on, LV_STATE_CHECKED);
        lv_obj_bind_state_if_ge(box_0, &subject_wiz_step, LV_STATE_CHECKED, 1);

        lv_obj_t * box_1 = box_create(wiz_steps);
        alarm_ui_run_30(box_1, &style_step, 0, &style_step_on, LV_STATE_CHECKED);
        lv_obj_bind_state_if_ge(box_1, &subject_wiz_step, LV_STATE_CHECKED, 2);

        lv_obj_t * box_2 = box_create(wiz_steps);
        alarm_ui_run_30(box_2, &style_step, 0, &style_step_on, LV_STATE_CHECKED);
        lv_obj_bind_state_if_ge(box_2, &subject_wiz_step, LV_STATE_CHECKED, 3);

        lv_obj_t * box_3 = box_create(wiz_steps);
        alarm_ui_run_30(box_3, &style_step, 0, &style_step_on, LV_STATE_CHECKED);
        lv_obj_bind_state_if_ge(box_3, &subject_wiz_step, LV_STATE_CHECKED, 4);

        lv_obj_t * box_4 = box_create(wiz_steps);
        alarm_ui_run_30(box_4, &style_step, 0, &style_step_on, LV_STATE_CHECKED);
        lv_obj_bind_state_if_ge(box_4, &subject_wiz_step, LV_STATE_CHECKED, 5);

        lv_obj_t * wiz_disk = alarm_ui_run_0(wiz_side, 168, 168);
        lv_obj_set_style_margin_top(wiz_disk, SPACE_XXL, 0);
        alarm_ui_run_22(wiz_disk, RADIUS_FULL, COLOR_BLUE, OPA_TINT);
        lv_obj_set_name(wiz_disk, "wiz_disk");
        lv_obj_t * wiz_icon = alarm_ui_run_33(wiz_disk);
        alarm_ui_run_4(wiz_icon, icons_96, COLOR_BLUE);
        lv_label_bind_text(wiz_icon, &subject_wiz_icon, NULL);

        lv_obj_t * wiz_title = alarm_ui_run_2(wiz_side, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_margin_top(wiz_title, SPACE_MD, 0);
        alarm_ui_run_12(wiz_title, font_headline, &subject_wiz_title);

        lv_obj_t * wiz_note = alarm_ui_run_2(wiz_side, LV_LABEL_LONG_MODE_WRAP);
        alarm_ui_run_29(wiz_note, COLOR_TEXT_2, &subject_wiz_note);

        lv_obj_t * box_5 = alarm_ui_run_0(wiz_side, 8, 8);
        lv_obj_set_flex_grow(box_5, 1);

        lv_obj_t * wiz_exit = alarm_ui_run_0(wiz_side, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flag(wiz_exit, LV_OBJ_FLAG_CLICKABLE, true);
        alarm_ui_run_1(wiz_exit, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(wiz_exit);
        lv_obj_set_style_pad_column(wiz_exit, SPACE_SM, 0);
        lv_obj_set_style_pad_ver(wiz_exit, SPACE_SM, 0);
        lv_obj_set_style_text_color(wiz_exit, COLOR_TEXT_2, 0);
        lv_obj_set_style_text_font(wiz_exit, font_label, 0);
        lv_obj_bind_flag_if_eq(wiz_exit, &subject_wiz_exit, LV_OBJ_FLAG_HIDDEN, 0);
        alarm_ui_run_9(wiz_exit, ui_on_wizard_exit, "wiz_exit");
        lv_obj_t * lv_label_0 = alarm_ui_run_24(wiz_exit, "\xEE\x84\x8E");
        lv_obj_set_style_text_font(lv_label_0, icons_20, 0);

        alarm_ui_run_16(wiz_exit, alarm_ui_tr("Leave setup"));

        lv_obj_t * wiz_main = box_create(lv_obj_0);
        lv_obj_set_x(wiz_main, 400);
        alarm_ui_run_31(wiz_main, 624, lv_pct(100));
        lv_obj_set_style_pad_all(wiz_main, SPACE_XL, 0);
        alarm_ui_run_1(wiz_main, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(wiz_main, SPACE_LG, 0);
        lv_obj_t * wiz_body_area = alarm_ui_run_27(wiz_main);
        lv_obj_set_flag(wiz_body_area, LV_OBJ_FLAG_SCROLLABLE, false);
        lv_obj_set_style_pad_all(wiz_body_area, 0, 0);
        lv_obj_set_style_bg_opa(wiz_body_area, (255 * 0 / 100), 0);
        lv_obj_set_style_border_width(wiz_body_area, 0, 0);
        lv_obj_bind_flag_if_eq(wiz_body_area, &subject_wiz_pad, LV_OBJ_FLAG_HIDDEN, 1);
        lv_obj_t * wiz_body = alarm_ui_run_0(wiz_body_area, lv_pct(100), lv_pct(100));
        alarm_ui_run_13(wiz_body);
        alarm_ui_run_1(wiz_body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(wiz_body, SPACE_SM, 0);
        alarm_ui_run_39(wiz_body, &subject_wiz_pad, 1, "wiz_body");

        lv_obj_t * wiz_shade = alarm_ui_run_0(wiz_body_area, lv_pct(100), 48);
        lv_obj_set_align(wiz_shade, LV_ALIGN_BOTTOM_MID);
        lv_obj_set_flag(wiz_shade, LV_OBJ_FLAG_FLOATING, true);
        lv_obj_set_flag(wiz_shade, LV_OBJ_FLAG_CLICKABLE, false);
        lv_obj_set_flag(wiz_shade, LV_OBJ_FLAG_SCROLLABLE, false);
        lv_obj_set_style_border_width(wiz_shade, 0, 0);
        alarm_ui_run_22(wiz_shade, 0, COLOR_BG, (255 * 100 / 100));
        alarm_ui_run_66(wiz_shade, COLOR_BG, LV_GRAD_DIR_VER, (255 * 0 / 100), (255 * 100 / 100), 255);
        lv_obj_set_name(wiz_shade, "wiz_shade");

        lv_obj_t * wiz_pad_area = alarm_ui_run_27(wiz_main);
        alarm_ui_run_1(wiz_pad_area, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_3(wiz_pad_area);
        lv_obj_bind_flag_if_eq(wiz_pad_area, &subject_wiz_pad, LV_OBJ_FLAG_HIDDEN, 0);
        lv_obj_t * wiz_pad = code_pad_create(wiz_pad_area, 1, 300);
        alarm_ui_run_31(wiz_pad, 428, lv_pct(100));
        lv_obj_set_name(wiz_pad, "wiz_pad");

        lv_obj_t * wiz_footer = alarm_ui_run_0(wiz_main, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(wiz_footer, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_flex_main_place(wiz_footer, LV_FLEX_ALIGN_END, 0);
        alarm_ui_run_3(wiz_footer);
        lv_obj_set_style_pad_column(wiz_footer, SPACE_MD, 0);
        lv_obj_set_style_pad_row(wiz_footer, SPACE_SM, 0);
        lv_obj_bind_flag_if_eq(wiz_footer, &subject_wiz_pad, LV_OBJ_FLAG_HIDDEN, 1);
        lv_obj_t * wiz_back = alarm_ui_run_21(wiz_footer, LV_SIZE_CONTENT, LV_OBJ_FLAG_CLICKABLE);
        alarm_ui_run_30(wiz_back, &style_button, 0, &style_button_pressed, LV_STATE_PRESSED);
        lv_obj_bind_flag_if_eq(wiz_back, &subject_wiz_back, LV_OBJ_FLAG_HIDDEN, 0);
        alarm_ui_run_9(wiz_back, ui_on_wizard_back, "wiz_back");
        lv_obj_t * lv_label_2 = alarm_ui_run_24(wiz_back, "\xEE\x81\xAE");
        lv_obj_set_style_text_font(lv_label_2, icons_20, 0);

        alarm_ui_run_16(wiz_back, alarm_ui_tr("Back"));

        lv_obj_t * wiz_spinner = lv_spinner_create(wiz_footer);
        alarm_ui_run_31(wiz_spinner, 32, 32);
        alarm_ui_run_49(wiz_spinner);

        lv_obj_t * box_6 = alarm_ui_run_0(wiz_footer, 8, 8);
        lv_obj_set_flex_grow(box_6, 1);

        lv_obj_t * wiz_next = alarm_ui_run_21(wiz_footer, LV_SIZE_CONTENT, LV_OBJ_FLAG_CLICKABLE);
        alarm_ui_run_30(wiz_next, &style_button, 0, &style_button_primary, 0);
        alarm_ui_run_52(wiz_next, &style_button_tone_pressed, LV_STATE_PRESSED, &style_disabled, &subject_wiz_next_on, 0);
        alarm_ui_run_9(wiz_next, ui_on_wizard_next, "wiz_next");
        alarm_ui_run_58(wiz_next, &subject_wiz_next);

        lv_obj_t * lv_label_5 = alarm_ui_run_24(wiz_next, "\xEE\x81\xAF");
        lv_obj_set_style_text_font(lv_label_5, icons_20, 0);

        lv_obj_t * wiz_text_sheet = text_sheet_create(lv_obj_0);
        lv_obj_bind_flag_if_not_eq(wiz_text_sheet, &subject_sheet, LV_OBJ_FLAG_HIDDEN, 1);

        lv_obj_t * wiz_pick_sheet = pick_sheet_create(lv_obj_0);
        lv_obj_bind_flag_if_not_eq(wiz_pick_sheet, &subject_sheet, LV_OBJ_FLAG_HIDDEN, 2);

        lv_obj_t * wiz_confirm_dialog = confirm_dialog_create(lv_obj_0);
        lv_obj_bind_flag_if_not_eq(wiz_confirm_dialog, &subject_sheet, LV_OBJ_FLAG_HIDDEN, 3);

        lv_obj_t * wiz_toast = alarm_ui_run_0(lv_obj_0, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(wiz_toast, 52, 0);
        alarm_ui_run_51(wiz_toast, LV_ALIGN_BOTTOM_MID, -312, -28);
        alarm_ui_run_10(wiz_toast, COLOR_PRESSED, 0, (255 * 100 / 100), RADIUS_FULL);
        alarm_ui_run_42(wiz_toast, COLOR_TOAST_LINE);
        alarm_ui_run_28(wiz_toast, SPACE_XL, SPACE_XL);
        lv_obj_set_style_pad_top(wiz_toast, 12, 0);
        lv_obj_set_style_pad_bottom(wiz_toast, 12, 0);
        alarm_ui_run_1(wiz_toast, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(wiz_toast);
        lv_obj_bind_flag_if_eq(wiz_toast, &subject_toast_visible, LV_OBJ_FLAG_HIDDEN, 0);
        lv_obj_t * lv_label_6 = alarm_ui_run_63(wiz_toast, 320);
        lv_obj_set_style_text_align(lv_label_6, LV_TEXT_ALIGN_CENTER, 0);
        alarm_ui_run_12(lv_label_6, font_label, &subject_toast);

        the_root = lv_obj_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
