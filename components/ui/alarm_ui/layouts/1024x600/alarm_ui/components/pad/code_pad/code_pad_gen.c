/**
 * @file code_pad_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "code_pad_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * code_pad_create(lv_obj_t * parent, int32_t top_grow, int32_t words_max)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_pad;
    static lv_style_t style_pad_icon;
    static lv_style_t style_pad_hint_under_dots;
    static lv_style_t style_pad_words_alone;
    static lv_style_t style_pad_dot;
    static lv_style_t style_pad_close;
    static lv_style_t style_pad_close_pressed;
    static lv_style_t style_pad_keys;
    static lv_style_t style_pad_enter;
    static lv_style_t style_pad_silence;
    static lv_style_t style_pad_silence_pressed;

    static bool style_inited = false;

    if (!style_inited) {
        static lv_style_t * const style_list[] = {
            &style_pad,
            &style_pad_icon,
            &style_pad_hint_under_dots,
            &style_pad_words_alone,
            &style_pad_dot,
            &style_pad_close,
            &style_pad_close_pressed,
            &style_pad_keys,
            &style_pad_enter,
            &style_pad_silence,
            &style_pad_silence_pressed,
        };

        /*Init all styles*/
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            lv_style_init(style_list[i]);
        }

        alarm_ui_run_6(&style_pad, COLOR_SURFACE, (255 * 100 / 100));
        alarm_ui_run_24(&style_pad, RADIUS_XL);
        lv_style_set_pad_all(&style_pad, SPACE_XL);
        lv_style_set_pad_top(&style_pad, SPACE_MD);
        alarm_ui_run_17(&style_pad, LV_FLEX_FLOW_COLUMN);
        lv_style_set_pad_row(&style_pad, SPACE_MD);
        alarm_ui_run_19(&style_pad_icon, 64, 64, RADIUS_FULL);
        lv_style_set_bg_opa(&style_pad_icon, (255 * 22 / 100));
        lv_style_set_text_opa(&style_pad_hint_under_dots, 0);
        lv_style_set_flex_cross_place(&style_pad_words_alone, LV_FLEX_ALIGN_CENTER);
        lv_style_set_text_align(&style_pad_words_alone, LV_TEXT_ALIGN_CENTER);
        alarm_ui_run_19(&style_pad_dot, 14, 14, RADIUS_FULL);
        alarm_ui_run_6(&style_pad_dot, COLOR_TEXT, (255 * 100 / 100));
        alarm_ui_run_19(&style_pad_close, 36, 36, RADIUS_FULL);
        alarm_ui_run_6(&style_pad_close, COLOR_RAISED, (255 * 100 / 100));
        lv_style_set_text_color(&style_pad_close, COLOR_TEXT_2);
        lv_style_set_bg_color(&style_pad_close_pressed, COLOR_PRESSED);
        lv_style_set_layout(&style_pad_keys, LV_LAYOUT_FLEX);
        lv_style_set_flex_flow(&style_pad_keys, LV_FLEX_FLOW_ROW_WRAP);
        lv_style_set_flex_main_place(&style_pad_keys, LV_FLEX_ALIGN_CENTER);
        lv_style_set_pad_row(&style_pad_keys, 8);
        lv_style_set_pad_column(&style_pad_keys, 16);
        lv_style_set_text_color(&style_pad_enter, COLOR_ON_TONE);
        alarm_ui_run_64(&style_pad_silence, 44, RADIUS_FULL);
        alarm_ui_run_6(&style_pad_silence, COLOR_RED, (255 * 18 / 100));
        alarm_ui_run_65(&style_pad_silence, 1, COLOR_RED);
        alarm_ui_run_15(&style_pad_silence, SPACE_XL, SPACE_XL);
        lv_style_set_pad_column(&style_pad_silence, SPACE_SM);
        alarm_ui_run_45(&style_pad_silence, font_label, COLOR_TEXT);
        lv_style_set_bg_opa(&style_pad_silence_pressed, (255 * 40 / 100));

        static const uint16_t style_ids[] = {
            ALARM_UI_STYLE_CODE_PAD_PAD,
            ALARM_UI_STYLE_CODE_PAD_PAD_ICON,
            ALARM_UI_STYLE_CODE_PAD_PAD_HINT_UNDER_DOTS,
            ALARM_UI_STYLE_CODE_PAD_PAD_WORDS_ALONE,
            ALARM_UI_STYLE_CODE_PAD_PAD_DOT,
            ALARM_UI_STYLE_CODE_PAD_PAD_CLOSE,
            ALARM_UI_STYLE_CODE_PAD_PAD_CLOSE_PRESSED,
            ALARM_UI_STYLE_CODE_PAD_PAD_KEYS,
            ALARM_UI_STYLE_CODE_PAD_PAD_ENTER,
            ALARM_UI_STYLE_CODE_PAD_PAD_SILENCE,
            ALARM_UI_STYLE_CODE_PAD_PAD_SILENCE_PRESSED,
        };
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            alarm_ui_style_track(style_list[i], style_ids[i]);
        }

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_25(parent, &style_pad);
        lv_obj_t * pad_close = box_create(box_0);
        alarm_ui_run_56(pad_close, LV_ALIGN_TOP_RIGHT, 14, -2);
        lv_obj_set_flag(pad_close, LV_OBJ_FLAG_FLOATING, true);
        alarm_ui_run_60(pad_close, LV_OBJ_FLAG_CLICKABLE, true, 16);
        alarm_ui_run_16(pad_close, &style_pad_close, 0, &style_pad_close_pressed, LV_STATE_PRESSED);
        lv_obj_bind_flag_if_eq(pad_close, &subject_pad_close_on, LV_OBJ_FLAG_HIDDEN, 0);
        alarm_ui_run_10(pad_close, ui_on_pad_close, "pad_close");
        lv_obj_t * lv_label_0 = alarm_ui_run_11(pad_close, "\xEE\x86\xB2");
        lv_obj_set_style_text_font(lv_label_0, icons_20, 0);

        lv_obj_t * pad_top = alarm_ui_run_0(box_0, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(pad_top, top_grow);
        alarm_ui_run_70(pad_top, SPACE_MD, SPACE_MD);
        alarm_ui_run_1(pad_top, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_flex_main_place(pad_top, LV_FLEX_ALIGN_CENTER, 0);
        alarm_ui_run_3(pad_top);
        lv_obj_set_style_pad_row(pad_top, SPACE_MD, 0);
        lv_obj_t * pad_header = alarm_ui_run_0(pad_top, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(pad_header, 64, 0);
        alarm_ui_run_1(pad_header, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_main_place(pad_header, LV_FLEX_ALIGN_CENTER, 0);
        alarm_ui_run_3(pad_header);
        lv_obj_t * pad_lead = alarm_ui_run_0(pad_header, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        alarm_ui_run_1(pad_lead, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(pad_lead);
        lv_obj_set_style_pad_column(pad_lead, SPACE_LG, 0);
        lv_obj_t * pad_icon = alarm_ui_run_25(pad_lead, &style_pad_icon);
        lv_obj_bind_style_prop(pad_icon, LV_STYLE_BG_COLOR, 0, &subject_pad_color);
        alarm_ui_run_43(pad_icon, &subject_pad_glyph_on, 0, "pad_icon");
        lv_obj_t * lv_label_1 = alarm_ui_run_33(pad_icon);
        alarm_ui_run_12(lv_label_1, icons_28, &subject_pad_glyph);
        lv_obj_bind_style_prop(lv_label_1, LV_STYLE_TEXT_COLOR, 0, &subject_pad_color);

        lv_obj_t * pad_words = alarm_ui_run_0(pad_lead, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        alarm_ui_run_1(pad_words, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_flex_cross_place(pad_words, LV_FLEX_ALIGN_START, 0);
        lv_obj_set_style_flex_track_place(pad_words, LV_FLEX_ALIGN_START, 0);
        lv_obj_set_style_pad_row(pad_words, 2, 0);
        lv_obj_bind_style(pad_words, &style_pad_words_alone, 0, &subject_pad_glyph_on, 0);
        lv_obj_t * pad_title = lv_label_create(pad_words);
        lv_label_set_long_mode(pad_title, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_max_width(pad_title, words_max, 0);
        alarm_ui_run_12(pad_title, font_title, &subject_pad_title);
        lv_obj_bind_style_prop(pad_title, LV_STYLE_TEXT_COLOR, 0, &subject_pad_color);

        lv_obj_t * pad_hint = lv_label_create(pad_words);
        lv_label_set_long_mode(pad_hint, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_max_width(pad_hint, words_max, 0);
        lv_label_bind_text(pad_hint, &subject_pad_hint, NULL);
        alarm_ui_run_52(pad_hint, &style_text_2, 0, &style_text_error, &subject_pad_error, 1);
        lv_obj_bind_style(pad_hint, &style_pad_hint_under_dots, 0, &subject_pad_dots_on, 1);
        lv_obj_set_name(pad_hint, "pad_hint");

        lv_obj_t * pad_dots = box_create(pad_words);
        lv_obj_set_align(pad_dots, LV_ALIGN_BOTTOM_LEFT);
        lv_obj_set_flag(pad_dots, LV_OBJ_FLAG_FLOATING, true);
        alarm_ui_run_31(pad_dots, LV_SIZE_CONTENT, 24);
        alarm_ui_run_1(pad_dots, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(pad_dots);
        lv_obj_set_style_pad_column(pad_dots, 12, 0);
        lv_obj_bind_flag_if_eq(pad_dots, &subject_pad_dots_on, LV_OBJ_FLAG_HIDDEN, 0);
        lv_obj_t * box_1 = alarm_ui_run_25(pad_dots, &style_pad_dot);
        lv_obj_bind_flag_if_lt(box_1, &subject_pad_digits, LV_OBJ_FLAG_HIDDEN, 1);

        lv_obj_t * box_2 = alarm_ui_run_25(pad_dots, &style_pad_dot);
        lv_obj_bind_flag_if_lt(box_2, &subject_pad_digits, LV_OBJ_FLAG_HIDDEN, 2);

        lv_obj_t * box_3 = alarm_ui_run_25(pad_dots, &style_pad_dot);
        lv_obj_bind_flag_if_lt(box_3, &subject_pad_digits, LV_OBJ_FLAG_HIDDEN, 3);

        lv_obj_t * box_4 = alarm_ui_run_25(pad_dots, &style_pad_dot);
        lv_obj_bind_flag_if_lt(box_4, &subject_pad_digits, LV_OBJ_FLAG_HIDDEN, 4);

        lv_obj_t * box_5 = alarm_ui_run_25(pad_dots, &style_pad_dot);
        lv_obj_bind_flag_if_lt(box_5, &subject_pad_digits, LV_OBJ_FLAG_HIDDEN, 5);

        lv_obj_t * box_6 = alarm_ui_run_25(pad_dots, &style_pad_dot);
        lv_obj_bind_flag_if_lt(box_6, &subject_pad_digits, LV_OBJ_FLAG_HIDDEN, 6);

        lv_obj_t * box_7 = alarm_ui_run_25(pad_dots, &style_pad_dot);
        lv_obj_bind_flag_if_lt(box_7, &subject_pad_digits, LV_OBJ_FLAG_HIDDEN, 7);

        lv_obj_t * box_8 = alarm_ui_run_25(pad_dots, &style_pad_dot);
        lv_obj_bind_flag_if_lt(box_8, &subject_pad_digits, LV_OBJ_FLAG_HIDDEN, 8);

        lv_obj_t * box_9 = alarm_ui_run_25(pad_dots, &style_pad_dot);
        lv_obj_bind_flag_if_lt(box_9, &subject_pad_digits, LV_OBJ_FLAG_HIDDEN, 9);

        lv_obj_t * box_10 = alarm_ui_run_25(pad_dots, &style_pad_dot);
        lv_obj_bind_flag_if_lt(box_10, &subject_pad_digits, LV_OBJ_FLAG_HIDDEN, 10);

        lv_obj_t * pad_note = alarm_ui_run_2(pad_top, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(pad_note, LV_TEXT_ALIGN_CENTER, 0);
        alarm_ui_run_4(pad_note, font_caption, COLOR_TEXT_3);
        lv_label_bind_text(pad_note, &subject_pad_note, NULL);
        lv_obj_bind_flag_if_eq(pad_note, &subject_pad_note_on, LV_OBJ_FLAG_HIDDEN, 0);

        lv_obj_t * pad_options = alarm_ui_run_0(box_0, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(pad_options, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_flex_main_place(pad_options, LV_FLEX_ALIGN_CENTER, 0);
        lv_obj_set_style_flex_track_place(pad_options, LV_FLEX_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_column(pad_options, SPACE_SM, 0);
        lv_obj_set_style_pad_row(pad_options, SPACE_SM, 0);
        alarm_ui_run_43(pad_options, &subject_pad_options, 0, "pad_options");
        chip_create(pad_options, alarm_ui_tr_c("arming option, no entry time", "No entry delay"), &subject_opt_instant);

        lv_obj_t * chip_1 = chip_create(pad_options, alarm_ui_tr("Silent exit"), &subject_opt_silent);
        lv_obj_bind_flag_if_eq(chip_1, &subject_opt_silent_avail, LV_OBJ_FLAG_HIDDEN, 0);

        lv_obj_t * chip_2 = chip_create(pad_options, alarm_ui_tr("Bypass open"), &subject_opt_bypass);
        lv_obj_bind_flag_if_eq(chip_2, &subject_opt_bypass_avail, LV_OBJ_FLAG_HIDDEN, 0);

        lv_obj_t * pad_silence = alarm_ui_run_22(box_0, LV_SIZE_CONTENT, LV_OBJ_FLAG_CLICKABLE);
        alarm_ui_run_16(pad_silence, &style_pad_silence, 0, &style_pad_silence_pressed, LV_STATE_PRESSED);
        lv_obj_bind_flag_if_eq(pad_silence, &subject_pad_silence, LV_OBJ_FLAG_HIDDEN, 0);
        lv_obj_add_event_cb(pad_silence, ui_on_silence, LV_EVENT_CLICKED, NULL);
        lv_obj_t * lv_label_2 = alarm_ui_run_23(pad_silence, "\xEE\x81\x9A");
        alarm_ui_run_4(lv_label_2, icons_20, COLOR_RED);

        alarm_ui_run_14(pad_silence, alarm_ui_tr("Silence"));

        lv_obj_t * pad_keys = alarm_ui_run_0(box_0, 392, LV_SIZE_CONTENT);
        alarm_ui_run_67(pad_keys, &style_pad_keys, 0, "pad_keys");
        lv_obj_t * pad_key_0 = pad_key_create(pad_keys, "1", font_keypad);
        lv_obj_add_event_cb(pad_key_0, ui_on_pad_key, LV_EVENT_CLICKED, (void *)"1");

        lv_obj_t * pad_key_1 = pad_key_create(pad_keys, "2", font_keypad);
        lv_obj_add_event_cb(pad_key_1, ui_on_pad_key, LV_EVENT_CLICKED, (void *)"2");

        lv_obj_t * pad_key_2 = pad_key_create(pad_keys, "3", font_keypad);
        lv_obj_add_event_cb(pad_key_2, ui_on_pad_key, LV_EVENT_CLICKED, (void *)"3");

        lv_obj_t * pad_key_3 = pad_key_create(pad_keys, "4", font_keypad);
        lv_obj_add_event_cb(pad_key_3, ui_on_pad_key, LV_EVENT_CLICKED, (void *)"4");

        lv_obj_t * pad_key_4 = pad_key_create(pad_keys, "5", font_keypad);
        lv_obj_add_event_cb(pad_key_4, ui_on_pad_key, LV_EVENT_CLICKED, (void *)"5");

        lv_obj_t * pad_key_5 = pad_key_create(pad_keys, "6", font_keypad);
        lv_obj_add_event_cb(pad_key_5, ui_on_pad_key, LV_EVENT_CLICKED, (void *)"6");

        lv_obj_t * pad_key_6 = pad_key_create(pad_keys, "7", font_keypad);
        lv_obj_add_event_cb(pad_key_6, ui_on_pad_key, LV_EVENT_CLICKED, (void *)"7");

        lv_obj_t * pad_key_7 = pad_key_create(pad_keys, "8", font_keypad);
        lv_obj_add_event_cb(pad_key_7, ui_on_pad_key, LV_EVENT_CLICKED, (void *)"8");

        lv_obj_t * pad_key_8 = pad_key_create(pad_keys, "9", font_keypad);
        lv_obj_add_event_cb(pad_key_8, ui_on_pad_key, LV_EVENT_CLICKED, (void *)"9");

        lv_obj_t * pad_key_9 = pad_key_create(pad_keys, "\xEE\x82\xAE", icons_28);
        lv_obj_add_event_cb(pad_key_9, ui_on_pad_key, LV_EVENT_CLICKED, (void *)"del");

        lv_obj_t * pad_key_10 = pad_key_create(pad_keys, "0", font_keypad);
        lv_obj_add_event_cb(pad_key_10, ui_on_pad_key, LV_EVENT_CLICKED, (void *)"0");

        lv_obj_t * pad_enter = pad_key_create(pad_keys, "\xEE\x81\xAC", icons_28);
        lv_obj_add_style(pad_enter, &style_pad_enter, 0);
        lv_obj_bind_style_prop(pad_enter, LV_STYLE_BG_COLOR, 0, &subject_pad_color);
        lv_obj_add_event_cb(pad_enter, ui_on_pad_key, LV_EVENT_CLICKED, (void *)"ok");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
