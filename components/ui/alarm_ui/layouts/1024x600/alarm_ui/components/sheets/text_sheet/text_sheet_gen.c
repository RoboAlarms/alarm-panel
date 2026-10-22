/**
 * @file text_sheet_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "text_sheet_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * text_sheet_create(lv_obj_t * parent)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_text_card;
    static lv_style_t style_text_field;
    static lv_style_t style_text_field_focused;
    static lv_style_t style_text_cursor;
    static lv_style_t style_text_placeholder;
    static lv_style_t style_text_field_eye;
    static lv_style_t style_text_eye;
    static lv_style_t style_text_eye_pressed;
    static lv_style_t style_keyboard;
    static lv_style_t style_keyboard_key;
    static lv_style_t style_keyboard_key_pressed;
    static lv_style_t style_keyboard_key_special;
    static lv_style_t style_keyboard_key_enter;

    static bool style_inited = false;

    if (!style_inited) {
        static lv_style_t * const style_list[] = {
            &style_text_card,
            &style_text_field,
            &style_text_field_focused,
            &style_text_cursor,
            &style_text_placeholder,
            &style_text_field_eye,
            &style_text_eye,
            &style_text_eye_pressed,
            &style_keyboard,
            &style_keyboard_key,
            &style_keyboard_key_pressed,
            &style_keyboard_key_special,
            &style_keyboard_key_enter,
        };

        /*Init all styles*/
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            lv_style_init(style_list[i]);
        }

        alarm_ui_run_39(&style_text_card);
        alarm_ui_run_6(&style_text_card, COLOR_SURFACE, (255 * 100 / 100));
        alarm_ui_run_24(&style_text_card, RADIUS_XL);
        alarm_ui_run_53(&style_text_card, SPACE_XL, SPACE_XL, SPACE_LG, SPACE_LG);
        lv_style_set_pad_row(&style_text_card, SPACE_MD);
        lv_style_set_width(&style_text_field, lv_pct(100));
        lv_style_set_radius(&style_text_field, RADIUS_MD);
        alarm_ui_run_6(&style_text_field, COLOR_RAISED, (255 * 100 / 100));
        alarm_ui_run_66(&style_text_field, 2, COLOR_RAISED);
        lv_style_set_pad_left(&style_text_field, SPACE_LG);
        lv_style_set_pad_right(&style_text_field, SPACE_LG);
        lv_style_set_pad_top(&style_text_field, 15);
        alarm_ui_run_45(&style_text_field, font_title, COLOR_TEXT);
        lv_style_set_border_color(&style_text_field_focused, COLOR_BLUE);
        lv_style_set_border_color(&style_text_cursor, COLOR_BLUE);
        lv_style_set_border_width(&style_text_cursor, 2);
        lv_style_set_border_side(&style_text_cursor, LV_BORDER_SIDE_LEFT);
        lv_style_set_bg_opa(&style_text_cursor, (255 * 0 / 100));
        lv_style_set_text_color(&style_text_placeholder, COLOR_TEXT_3);
        lv_style_set_pad_right(&style_text_field_eye, 56);
        lv_style_set_radius(&style_text_eye, RADIUS_FULL);
        lv_style_set_text_color(&style_text_eye, COLOR_TEXT_2);
        alarm_ui_run_6(&style_text_eye_pressed, COLOR_PRESSED, (255 * 100 / 100));
        lv_style_set_height(&style_keyboard, 264);
        lv_style_set_bg_opa(&style_keyboard, (255 * 0 / 100));
        lv_style_set_border_width(&style_keyboard, 0);
        lv_style_set_pad_all(&style_keyboard, 0);
        lv_style_set_pad_row(&style_keyboard, 8);
        lv_style_set_pad_column(&style_keyboard, 8);
        lv_style_set_radius(&style_keyboard_key, RADIUS_MD);
        alarm_ui_run_6(&style_keyboard_key, COLOR_RAISED, (255 * 100 / 100));
        lv_style_set_border_width(&style_keyboard_key, 0);
        lv_style_set_shadow_width(&style_keyboard_key, 0);
        alarm_ui_run_45(&style_keyboard_key, font_label, COLOR_TEXT);
        lv_style_set_bg_color(&style_keyboard_key_pressed, COLOR_PRESSED);
        lv_style_set_bg_color(&style_keyboard_key_special, COLOR_KEY_SPECIAL);
        lv_style_set_text_color(&style_keyboard_key_special, COLOR_TEXT_2);
        lv_style_set_bg_color(&style_keyboard_key_enter, COLOR_BLUE);
        lv_style_set_text_color(&style_keyboard_key_enter, COLOR_ON_TONE);

        static const uint16_t style_ids[] = {
            ALARM_UI_STYLE_TEXT_SHEET_TEXT_CARD,
            ALARM_UI_STYLE_TEXT_SHEET_TEXT_FIELD,
            ALARM_UI_STYLE_TEXT_SHEET_TEXT_FIELD_FOCUSED,
            ALARM_UI_STYLE_TEXT_SHEET_TEXT_CURSOR,
            ALARM_UI_STYLE_TEXT_SHEET_TEXT_PLACEHOLDER,
            ALARM_UI_STYLE_TEXT_SHEET_TEXT_FIELD_EYE,
            ALARM_UI_STYLE_TEXT_SHEET_TEXT_EYE,
            ALARM_UI_STYLE_TEXT_SHEET_TEXT_EYE_PRESSED,
            ALARM_UI_STYLE_TEXT_SHEET_KEYBOARD,
            ALARM_UI_STYLE_TEXT_SHEET_KEYBOARD_KEY,
            ALARM_UI_STYLE_TEXT_SHEET_KEYBOARD_KEY_PRESSED,
            ALARM_UI_STYLE_TEXT_SHEET_KEYBOARD_KEY_SPECIAL,
            ALARM_UI_STYLE_TEXT_SHEET_KEYBOARD_KEY_ENTER,
        };
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            alarm_ui_style_track(style_list[i], style_ids[i]);
        }

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_0(parent, lv_pct(100), lv_pct(100));
        lv_obj_set_flag(box_0, LV_OBJ_FLAG_CLICKABLE, true);
        lv_obj_set_style_pad_all(box_0, SPACE_MD, 0);
        lv_obj_add_style(box_0, &style_backdrop, 0);
        lv_obj_t * text_card = box_create(box_0);
        lv_obj_set_align(text_card, LV_ALIGN_BOTTOM_MID);
        alarm_ui_run_68(text_card, &style_text_card, 0, "text_card");
        lv_obj_t * box_1 = alarm_ui_run_0(text_card, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(box_1, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(box_1);
        lv_obj_set_style_pad_column(box_1, SPACE_MD, 0);
        lv_obj_t * box_2 = alarm_ui_run_8(box_1);
        alarm_ui_run_1(box_2, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box_2, 2, 0);
        lv_obj_t * lv_label_0 = alarm_ui_run_2(box_2, LV_LABEL_LONG_MODE_DOTS);
        alarm_ui_run_12(lv_label_0, font_title, &subject_sheet_title);

        lv_obj_t * lv_label_1 = alarm_ui_run_2(box_2, LV_LABEL_LONG_MODE_DOTS);
        alarm_ui_run_30(lv_label_1, COLOR_TEXT_2, &subject_sheet_note);

        lv_obj_t * sheet_cancel = alarm_ui_run_5(box_1, &style_button_pressed, LV_STATE_PRESSED);
        alarm_ui_run_10(sheet_cancel, ui_on_sheet_cancel, "sheet_cancel");
        alarm_ui_run_14(sheet_cancel, alarm_ui_tr("Cancel"));

        lv_obj_t * sheet_done = alarm_ui_run_5(box_1, &style_button_primary, 0);
        lv_obj_add_style(sheet_done, &style_button_tone_pressed, LV_STATE_PRESSED);
        alarm_ui_run_10(sheet_done, ui_on_sheet_done, "sheet_done");
        alarm_ui_run_63(sheet_done, &subject_sheet_ok);

        lv_obj_t * box_3 = alarm_ui_run_0(text_card, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_t * sheet_text = lv_textarea_create(box_3);
        lv_textarea_set_one_line(sheet_text, true);
        lv_textarea_set_placeholder_text(sheet_text, "");
        lv_textarea_set_password_show_time(sheet_text, 1200);
        lv_obj_set_scrollbar_mode(sheet_text, LV_SCROLLBAR_MODE_OFF);
        alarm_ui_run_16(sheet_text, &style_text_field, 0, &style_text_field_focused, LV_STATE_FOCUSED);
        alarm_ui_run_16(sheet_text, &style_text_cursor, LV_PART_CURSOR, &style_text_placeholder, LV_PART_TEXTAREA_PLACEHOLDER);
        lv_obj_bind_style(sheet_text, &style_text_field_eye, 0, &subject_sheet_eye, 1);
        lv_obj_set_name(sheet_text, "sheet_text");

        lv_obj_t * sheet_eye = alarm_ui_run_0(box_3, 44, 44);
        lv_obj_set_align(sheet_eye, LV_ALIGN_RIGHT_MID);
        lv_obj_set_x(sheet_eye, -4);
        lv_obj_set_flag(sheet_eye, LV_OBJ_FLAG_CLICKABLE, true);
        alarm_ui_run_60(sheet_eye, LV_OBJ_FLAG_CLICK_FOCUSABLE, false, 6);
        alarm_ui_run_16(sheet_eye, &style_text_eye, 0, &style_text_eye_pressed, LV_STATE_PRESSED);
        lv_obj_bind_flag_if_eq(sheet_eye, &subject_sheet_eye, LV_OBJ_FLAG_HIDDEN, 0);
        alarm_ui_run_10(sheet_eye, ui_on_sheet_eye, "sheet_eye");
        lv_obj_t * lv_label_4 = alarm_ui_run_11(sheet_eye, "\xEE\x82\xBA");
        lv_obj_set_style_text_font(lv_label_4, icons_20, 0);
        lv_obj_bind_flag_if_eq(lv_label_4, &subject_sheet_shown, LV_OBJ_FLAG_HIDDEN, 1);

        lv_obj_t * lv_label_5 = alarm_ui_run_11(sheet_eye, "\xEE\x82\xBB");
        lv_obj_set_style_text_font(lv_label_5, icons_20, 0);
        lv_obj_bind_flag_if_eq(lv_label_5, &subject_sheet_shown, LV_OBJ_FLAG_HIDDEN, 0);

        lv_obj_t * sheet_kb = lv_keyboard_create(text_card);
        lv_obj_set_width(sheet_kb, lv_pct(100));
        lv_keyboard_set_textarea(sheet_kb, sheet_text);
        alarm_ui_run_16(sheet_kb, &style_keyboard, 0, &style_keyboard_key, LV_PART_ITEMS);
        alarm_ui_run_16(sheet_kb, &style_keyboard_key_pressed, LV_PART_ITEMS | LV_STATE_PRESSED, &style_keyboard_key_special, LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_name(sheet_kb, "sheet_kb");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
