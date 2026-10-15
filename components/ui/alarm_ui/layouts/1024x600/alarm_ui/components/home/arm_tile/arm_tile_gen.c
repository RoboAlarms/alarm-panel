/**
 * @file arm_tile_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "arm_tile_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * arm_tile_create(lv_obj_t * parent, const char * glyph, const char * title, const char * subtitle, lv_color_t color)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_arm_tile;
    static lv_style_t style_arm_tile_pressed;

    static bool style_inited = false;

    if (!style_inited) {
        /*Init all styles*/
        lv_style_init(&style_arm_tile);
        lv_style_init(&style_arm_tile_pressed);

        alarm_ui_run_6(&style_arm_tile, COLOR_SURFACE, (255 * 100 / 100));
        alarm_ui_run_23(&style_arm_tile, RADIUS_LG);
        alarm_ui_run_14(&style_arm_tile, SPACE_XL, SPACE_XL);
        lv_style_set_pad_column(&style_arm_tile, SPACE_XL);
        lv_style_set_bg_color(&style_arm_tile_pressed, COLOR_RAISED);
        lv_style_set_transform_width(&style_arm_tile_pressed, -4);
        lv_style_set_transform_height(&style_arm_tile_pressed, -4);

        alarm_ui_style_track(&style_arm_tile, ALARM_UI_STYLE_ARM_TILE_ARM_TILE);
        alarm_ui_style_track(&style_arm_tile_pressed, ALARM_UI_STYLE_ARM_TILE_ARM_TILE_PRESSED);

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_20(parent);
        alarm_ui_run_15(box_0, &style_arm_tile, 0, &style_arm_tile_pressed, LV_STATE_PRESSED);
        icon_chip_create(box_0, glyph, 76, icons_40, color);

        lv_obj_t * box_1 = alarm_ui_run_8(box_0);
        alarm_ui_run_1(box_1, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box_1, 4, 0);
        lv_obj_t * lv_label_0 = lv_label_create(box_1);
        lv_label_set_text(lv_label_0, title);
        lv_obj_set_style_text_font(lv_label_0, font_title, 0);

        lv_obj_t * lv_label_1 = alarm_ui_run_2(box_1, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(lv_label_1, subtitle);
        lv_obj_set_style_text_color(lv_label_1, COLOR_TEXT_2, 0);

        lv_obj_t * lv_label_2 = alarm_ui_run_24(box_0, "\xEE\x81\xAF");
        alarm_ui_run_4(lv_label_2, icons_28, COLOR_TEXT_3);

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
