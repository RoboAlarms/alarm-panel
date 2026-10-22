/**
 * @file screen_saver_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "screen_saver_gen.h"
#include "../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * screen_saver_create(void)
{
    LV_TRACE_OBJ_CREATE("begin");

    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        if (screen_saver == NULL) screen_saver = lv_obj_create(NULL);
        lv_obj_t * lv_obj_0 = screen_saver;
        lv_obj_set_flag(lv_obj_0, LV_OBJ_FLAG_SCROLLABLE, false);
        lv_obj_set_style_text_color(lv_obj_0, COLOR_TEXT, 0);
        lv_obj_set_name_static(lv_obj_0, "screen_saver_#");

        lv_obj_add_style(lv_obj_0, &style_saver, 0);
        lv_obj_add_event_cb(lv_obj_0, ui_on_wake, LV_EVENT_CLICKED, NULL);
        lv_obj_t * saver_bg = lv_image_create(lv_obj_0);
        alarm_ui_run_32(saver_bg, lv_pct(100), lv_pct(100));
        lv_obj_set_name(saver_bg, "saver_bg");

        lv_obj_t * saver_shade = alarm_ui_run_0(lv_obj_0, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_color(saver_shade, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(saver_shade, (255 * 100 / 100), 0);
        alarm_ui_run_71(saver_shade, lv_color_hex(0x000000), LV_GRAD_DIR_HOR, (255 * 82 / 100), (255 * 0 / 100), 150);

        lv_obj_t * saver_clock = box_create(lv_obj_0);
        lv_obj_set_x(saver_clock, 56);
        lv_obj_set_y(saver_clock, 40);
        alarm_ui_run_32(saver_clock, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        alarm_ui_run_1(saver_clock, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(saver_clock, 0, 0);
        lv_obj_t * box_0 = alarm_ui_run_0(saver_clock, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        alarm_ui_run_1(box_0, LV_FLEX_FLOW_ROW);
        alarm_ui_run_67(box_0, LV_FLEX_ALIGN_END, SPACE_SM);
        lv_obj_t * saver_time = lv_label_create(box_0);
        alarm_ui_run_4(saver_time, font_digits, COLOR_TEXT);
        lv_label_bind_text(saver_time, &subject_time, NULL);

        lv_obj_t * lv_label_0 = lv_label_create(box_0);
        alarm_ui_run_4(lv_label_0, font_title, lv_color_hex(0xCBD5E1));
        lv_obj_set_style_pad_bottom(lv_label_0, 14, 0);
        lv_label_bind_text(lv_label_0, &subject_ampm, NULL);

        lv_obj_t * saver_date = lv_label_create(saver_clock);
        alarm_ui_run_4(saver_date, font_title, lv_color_hex(0xCBD5E1));
        lv_label_bind_text(saver_date, &subject_date, NULL);

        lv_obj_t * saver_state = box_create(lv_obj_0);
        alarm_ui_run_56(saver_state, LV_ALIGN_BOTTOM_LEFT, 48, -48);
        alarm_ui_run_32(saver_state, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(saver_state, 560, 0);
        alarm_ui_run_9(saver_state, COLOR_BG, 0, (255 * 74 / 100), RADIUS_XL);
        alarm_ui_run_40(saver_state, lv_color_hex(0xFFFFFF));
        lv_obj_set_style_border_opa(saver_state, (255 * 14 / 100), 0);
        lv_obj_set_style_pad_all(saver_state, SPACE_XL, 0);
        lv_obj_set_style_pad_right(saver_state, SPACE_XXL, 0);
        alarm_ui_run_1(saver_state, LV_FLEX_FLOW_ROW);
        alarm_ui_run_67(saver_state, LV_FLEX_ALIGN_CENTER, SPACE_XL);
        lv_obj_t * saver_disk = alarm_ui_run_0(saver_state, 84, 84);
        lv_obj_set_style_radius(saver_disk, RADIUS_FULL, 0);
        lv_obj_set_style_bg_opa(saver_disk, (255 * 22 / 100), 0);
        lv_obj_bind_style_prop(saver_disk, LV_STYLE_BG_COLOR, 0, &subject_tone_color);
        lv_obj_t * saver_icon = alarm_ui_run_33(saver_disk);
        alarm_ui_run_12(saver_icon, icons_40, &subject_hero_icon);
        lv_obj_bind_style_prop(saver_icon, LV_STYLE_TEXT_COLOR, 0, &subject_tone_color);

        lv_obj_t * saver_words = alarm_ui_run_0(saver_state, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(saver_words, 400, 0);
        alarm_ui_run_1(saver_words, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(saver_words, 2, 0);
        lv_obj_t * saver_headline = lv_label_create(saver_words);
        alarm_ui_run_12(saver_headline, font_headline, &subject_headline);
        lv_obj_bind_style_prop(saver_headline, LV_STYLE_TEXT_COLOR, 0, &subject_tone_color);

        lv_obj_t * saver_detail = alarm_ui_run_55(saver_words, 400);
        alarm_ui_run_4(saver_detail, font_label, COLOR_TEXT);
        lv_label_bind_text(saver_detail, &subject_detail, NULL);

        lv_obj_t * saver_note = alarm_ui_run_55(saver_words, 400);
        alarm_ui_run_4(saver_note, font_body, lv_color_hex(0xCBD5E1));
        lv_label_bind_text(saver_note, &subject_hero_note, NULL);

        lv_obj_t * saver_trouble = alarm_ui_run_0(saver_words, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_margin_top(saver_trouble, SPACE_SM, 0);
        alarm_ui_run_1(saver_trouble, LV_FLEX_FLOW_ROW);
        alarm_ui_run_67(saver_trouble, LV_FLEX_ALIGN_CENTER, SPACE_SM);
        lv_obj_set_style_text_color(saver_trouble, COLOR_AMBER, 0);
        lv_obj_bind_flag_if_eq(saver_trouble, &subject_trouble_count, LV_OBJ_FLAG_HIDDEN, 0);
        lv_obj_t * lv_label_1 = alarm_ui_run_23(saver_trouble, "\xEE\x86\x93");
        lv_obj_set_style_text_font(lv_label_1, icons_20, 0);

        lv_obj_t * lv_label_2 = lv_label_create(saver_trouble);
        alarm_ui_run_12(lv_label_2, font_label, &subject_trouble_text);

        the_root = lv_obj_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
