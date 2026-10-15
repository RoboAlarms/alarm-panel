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

        alarm_ui_run_25(&style_text_card, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_6(&style_text_card, RADIUS_XL);
        lv_style_set_pad_left(&style_text_card, SPACE_MD);
        lv_style_set_pad_right(&style_text_card, SPACE_MD);
        lv_style_set_pad_top(&style_text_card, SPACE_SM);
        lv_style_set_pad_bottom(&style_text_card, SPACE_SM);
        lv_style_set_layout(&style_text_card, LV_LAYOUT_FLEX);
        lv_style_set_flex_flow(&style_text_card, LV_FLEX_FLOW_COLUMN);
        lv_style_set_pad_row(&style_text_card, SPACE_SM);
        lv_style_set_width(&style_text_field, lv_pct(100));
        alarm_ui_run_11(&style_text_field, 44, RADIUS_MD, COLOR_RAISED, (255 * 100 / 100));
        lv_style_set_border_width(&style_text_field, 2);
        lv_style_set_border_color(&style_text_field, COLOR_RAISED);
        lv_style_set_pad_left(&style_text_field, SPACE_LG);
        lv_style_set_pad_right(&style_text_field, SPACE_LG);
        lv_style_set_pad_top(&style_text_field, 11);
        alarm_ui_run_38(&style_text_field);
        lv_style_set_border_color(&style_text_field_focused, COLOR_BLUE);
        lv_style_set_border_color(&style_text_cursor, COLOR_BLUE);
        lv_style_set_border_width(&style_text_cursor, 2);
        lv_style_set_border_side(&style_text_cursor, LV_BORDER_SIDE_LEFT);
        lv_style_set_bg_opa(&style_text_cursor, (255 * 0 / 100));
        lv_style_set_text_color(&style_text_placeholder, COLOR_TEXT_3);
        lv_style_set_height(&style_keyboard, 196);
        lv_style_set_bg_opa(&style_keyboard, (255 * 0 / 100));
        lv_style_set_border_width(&style_keyboard, 0);
        lv_style_set_pad_all(&style_keyboard, 0);
        lv_style_set_pad_row(&style_keyboard, 4);
        lv_style_set_pad_column(&style_keyboard, 4);
        lv_style_set_radius(&style_keyboard_key, RADIUS_SM);
        alarm_ui_run_24(&style_keyboard_key, COLOR_RAISED, (255 * 100 / 100));
        lv_style_set_border_width(&style_keyboard_key, 0);
        lv_style_set_shadow_width(&style_keyboard_key, 0);
        alarm_ui_run_38(&style_keyboard_key);
        lv_style_set_bg_color(&style_keyboard_key_pressed, COLOR_PRESSED);
        lv_style_set_bg_color(&style_keyboard_key_special, COLOR_LINE);
        lv_style_set_text_color(&style_keyboard_key_special, COLOR_TEXT_2);
        lv_style_set_bg_color(&style_keyboard_key_enter, COLOR_BLUE);
        lv_style_set_text_color(&style_keyboard_key_enter, COLOR_ON_TONE);

        static const uint16_t style_ids[] = {
            ALARM_UI_STYLE_TEXT_SHEET_TEXT_CARD,
            ALARM_UI_STYLE_TEXT_SHEET_TEXT_FIELD,
            ALARM_UI_STYLE_TEXT_SHEET_TEXT_FIELD_FOCUSED,
            ALARM_UI_STYLE_TEXT_SHEET_TEXT_CURSOR,
            ALARM_UI_STYLE_TEXT_SHEET_TEXT_PLACEHOLDER,
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
        lv_obj_set_style_pad_all(box_0, SPACE_SM, 0);
        lv_obj_add_style(box_0, &style_backdrop, 0);
        lv_obj_t * text_card = box_create(box_0);
        lv_obj_set_align(text_card, LV_ALIGN_BOTTOM_MID);
        lv_obj_add_style(text_card, &style_text_card, 0);
        lv_obj_set_name(text_card, "text_card");
        lv_obj_t * box_1 = alarm_ui_run_0(text_card, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(box_1, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(box_1);
        lv_obj_set_style_pad_column(box_1, SPACE_SM, 0);
        lv_obj_t * box_2 = alarm_ui_run_9(box_1, LV_SIZE_CONTENT);
        alarm_ui_run_1(box_2, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box_2, 0, 0);
        lv_obj_t * lv_label_0 = alarm_ui_run_2(box_2, LV_LABEL_LONG_MODE_DOTS);
        alarm_ui_run_8(lv_label_0, font_label, &subject_sheet_title);

        lv_obj_t * lv_label_1 = alarm_ui_run_2(box_2, LV_LABEL_LONG_MODE_DOTS);
        alarm_ui_run_4(lv_label_1, font_caption, COLOR_TEXT_2);
        lv_label_bind_text(lv_label_1, &subject_sheet_note, NULL);

        lv_obj_t * sheet_cancel = alarm_ui_run_14(box_1);
        lv_obj_set_ext_click_area(sheet_cancel, 4);
        alarm_ui_run_37(sheet_cancel, &style_button, &style_button_pressed, LV_STATE_PRESSED);
        alarm_ui_run_13(sheet_cancel, ui_on_sheet_cancel, "sheet_cancel");
        alarm_ui_run_31(sheet_cancel, alarm_ui_tr("Cancel"));

        lv_obj_t * sheet_done = alarm_ui_run_14(box_1);
        lv_obj_set_ext_click_area(sheet_done, 4);
        alarm_ui_run_37(sheet_done, &style_button, &style_button_primary, 0);
        lv_obj_add_style(sheet_done, &style_button_tone_pressed, LV_STATE_PRESSED);
        alarm_ui_run_13(sheet_done, ui_on_sheet_done, "sheet_done");
        lv_obj_t * lv_label_3 = lv_label_create(sheet_done);
        lv_label_bind_text(lv_label_3, &subject_sheet_ok, NULL);

        lv_obj_t * sheet_text = lv_textarea_create(text_card);
        lv_textarea_set_one_line(sheet_text, true);
        lv_textarea_set_placeholder_text(sheet_text, "");
        lv_textarea_set_password_show_time(sheet_text, 1200);
        lv_obj_set_scrollbar_mode(sheet_text, LV_SCROLLBAR_MODE_OFF);
        alarm_ui_run_17(sheet_text, &style_text_field, 0, &style_text_field_focused, LV_STATE_FOCUSED);
        alarm_ui_run_17(sheet_text, &style_text_cursor, LV_PART_CURSOR, &style_text_placeholder, LV_PART_TEXTAREA_PLACEHOLDER);
        lv_obj_set_name(sheet_text, "sheet_text");

        lv_obj_t * sheet_kb = lv_keyboard_create(text_card);
        lv_obj_set_width(sheet_kb, lv_pct(100));
        lv_keyboard_set_textarea(sheet_kb, sheet_text);
        alarm_ui_run_17(sheet_kb, &style_keyboard, 0, &style_keyboard_key, LV_PART_ITEMS);
        alarm_ui_run_17(sheet_kb, &style_keyboard_key_pressed, LV_PART_ITEMS | LV_STATE_PRESSED, &style_keyboard_key_special, LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_name(sheet_kb, "sheet_kb");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
