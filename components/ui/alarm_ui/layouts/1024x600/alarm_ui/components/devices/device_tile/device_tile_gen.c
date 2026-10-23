/**
 * @file device_tile_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "device_tile_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * device_tile_create(lv_obj_t * parent, const char * glyph, const char * title, const char * detail)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_device_tile;
    static lv_style_t style_device_tile_pressed;

    static bool style_inited = false;

    if (!style_inited) {
        /*Init all styles*/
        lv_style_init(&style_device_tile);
        lv_style_init(&style_device_tile_pressed);

        lv_style_set_width(&style_device_tile, DEVICE_CELL_W);
        lv_style_set_height(&style_device_tile, DEVICE_CELL_H);
        alarm_ui_run_6(&style_device_tile, COLOR_SURFACE, (255 * 100 / 100));
        lv_style_set_radius(&style_device_tile, RADIUS_LG);
        alarm_ui_run_53(&style_device_tile, SPACE_LG, SPACE_LG, SPACE_MD, SPACE_MD);
        lv_style_set_flex_main_place(&style_device_tile, LV_FLEX_ALIGN_SPACE_BETWEEN);
        lv_style_set_bg_color(&style_device_tile_pressed, COLOR_RAISED);

        alarm_ui_style_track(&style_device_tile, ALARM_UI_STYLE_DEVICE_TILE_DEVICE_TILE);
        alarm_ui_style_track(&style_device_tile_pressed, ALARM_UI_STYLE_DEVICE_TILE_DEVICE_TILE_PRESSED);

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_21(parent);
        alarm_ui_run_16(box_0, &style_device_tile, 0, &style_device_tile_pressed, LV_STATE_PRESSED);
        lv_obj_t * top = alarm_ui_run_0(box_0, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(top, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(top);
        lv_obj_set_style_pad_column(top, SPACE_MD, 0);
        lv_obj_set_style_pad_right(top, 40, 0);
        lv_obj_set_name(top, "top");
        lv_obj_t * chip = icon_chip_create(top, glyph, 48, icons_28, COLOR_TEXT_3);
        lv_obj_set_name(chip, "chip");

        lv_obj_t * words = alarm_ui_run_8(top);
        alarm_ui_run_1(words, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_name(words, "words");
        lv_obj_t * title_label = lv_label_create(words);
        alarm_ui_run_32(title_label, lv_pct(100), DEVICE_NAME_H);
        lv_label_set_long_mode(title_label, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(title_label, title);
        lv_obj_set_style_text_font(title_label, font_label, 0);
        lv_obj_set_style_text_line_space(title_label, 0, 0);
        lv_obj_set_name(title_label, "title_label");

        lv_obj_t * detail_label = lv_label_create(words);
        alarm_ui_run_32(detail_label, lv_pct(100), DEVICE_STATE_H);
        lv_label_set_long_mode(detail_label, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(detail_label, detail);
        lv_obj_set_style_text_font(detail_label, font_caption, 0);
        lv_obj_set_style_text_line_space(detail_label, 0, 0);
        alarm_ui_run_48(detail_label, "detail_label");

        lv_obj_t * more = alarm_ui_run_0(box_0, 64, 72);
        alarm_ui_run_56(more, LV_ALIGN_TOP_RIGHT, 16, -12);
        lv_obj_set_flag(more, LV_OBJ_FLAG_FLOATING, true);
        lv_obj_set_flag(more, LV_OBJ_FLAG_CLICKABLE, true);
        lv_obj_set_name(more, "more");
        lv_obj_t * lv_label_0 = alarm_ui_run_11(more, "\xEE\x81\xAF");
        alarm_ui_run_4(lv_label_0, icons_20, COLOR_TEXT_3);

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
