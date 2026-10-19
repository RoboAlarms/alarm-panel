/**
 * @file list_row_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "list_row_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * list_row_create(lv_obj_t * parent, const char * glyph, lv_color_t color, const char * title, const char * detail, const char * value, const char * trail)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_list_row;
    static lv_style_t style_list_row_pressed;
    static lv_style_t style_list_row_level;
    static lv_style_t style_list_row_level_indicator;

    static bool style_inited = false;

    if (!style_inited) {
        static lv_style_t * const style_list[] = {
            &style_list_row,
            &style_list_row_pressed,
            &style_list_row_level,
            &style_list_row_level_indicator,
        };

        /*Init all styles*/
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            lv_style_init(style_list[i]);
        }

        alarm_ui_run_39(&style_list_row);
        lv_style_set_min_height(&style_list_row, 72);
        alarm_ui_run_6(&style_list_row, COLOR_SURFACE, (255 * 100 / 100));
        lv_style_set_radius(&style_list_row, RADIUS_MD);
        alarm_ui_run_7(&style_list_row, SPACE_MD, SPACE_LG, SPACE_SM, SPACE_SM, SPACE_MD);
        lv_style_set_bg_color(&style_list_row_pressed, COLOR_RAISED);
        alarm_ui_run_19(&style_list_row_level, lv_pct(100), 8, RADIUS_FULL);
        alarm_ui_run_6(&style_list_row_level, COLOR_PRESSED, (255 * 100 / 100));
        lv_style_set_margin_top(&style_list_row_level, SPACE_SM);
        alarm_ui_run_63(&style_list_row_level_indicator, RADIUS_FULL, (255 * 100 / 100));

        static const uint16_t style_ids[] = {
            ALARM_UI_STYLE_LIST_ROW_LIST_ROW,
            ALARM_UI_STYLE_LIST_ROW_LIST_ROW_PRESSED,
            ALARM_UI_STYLE_LIST_ROW_LIST_ROW_LEVEL,
            ALARM_UI_STYLE_LIST_ROW_LIST_ROW_LEVEL_INDICATOR,
        };
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            alarm_ui_style_track(style_list[i], style_ids[i]);
        }

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_20(parent);
        alarm_ui_run_16(box_0, &style_list_row, 0, &style_list_row_pressed, LV_STATE_PRESSED);
        lv_obj_t * chip = icon_chip_create(box_0, glyph, 44, icons_20, color);
        lv_obj_set_name(chip, "chip");

        lv_obj_t * box_1 = alarm_ui_run_8(box_0);
        lv_obj_set_style_min_height(box_1, 56, 0);
        alarm_ui_run_1(box_1, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_59(box_1);
        lv_obj_t * title_label = alarm_ui_run_2(box_1, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(title_label, title);
        alarm_ui_run_37(title_label, font_label, "title_label");

        lv_obj_t * detail_label = alarm_ui_run_2(box_1, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(detail_label, detail);
        alarm_ui_run_4(detail_label, font_caption, COLOR_TEXT_2);
        lv_obj_set_name(detail_label, "detail_label");

        lv_obj_t * level = lv_bar_create(box_1);
        lv_bar_set_min_value(level, 0);
        lv_bar_set_max_value(level, 100);
        lv_bar_set_value(level, 0, LV_ANIM_OFF);
        lv_obj_set_flag(level, LV_OBJ_FLAG_CLICKABLE, false);
        alarm_ui_run_16(level, &style_list_row_level, 0, &style_list_row_level_indicator, LV_PART_INDICATOR);
        lv_obj_set_name(level, "level");

        lv_obj_t * value_label = lv_label_create(box_0);
        lv_label_set_text(value_label, value);
        alarm_ui_run_4(value_label, font_label, COLOR_TEXT_2);
        lv_obj_set_name(value_label, "value_label");

        lv_obj_t * trail_label = lv_label_create(box_0);
        lv_label_set_text(trail_label, trail);
        alarm_ui_run_4(trail_label, icons_20, COLOR_TEXT_3);
        lv_obj_set_name(trail_label, "trail_label");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
