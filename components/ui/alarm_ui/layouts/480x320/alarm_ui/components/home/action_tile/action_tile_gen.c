/**
 * @file action_tile_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "action_tile_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * action_tile_create(lv_obj_t * parent, const char * glyph, lv_subject_t * title, lv_subject_t * subtitle, lv_color_t color)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_action_tile;
    static lv_style_t style_action_tile_pressed;

    static bool style_inited = false;

    if (!style_inited) {
        /*Init all styles*/
        lv_style_init(&style_action_tile);
        lv_style_init(&style_action_tile_pressed);

        alarm_ui_run_6(&style_action_tile, RADIUS_XL);
        lv_style_set_pad_all(&style_action_tile, SPACE_XL);
        alarm_ui_run_19(&style_action_tile, LV_FLEX_FLOW_COLUMN);
        lv_style_set_pad_row(&style_action_tile, SPACE_MD);
        lv_style_set_bg_color(&style_action_tile_pressed, COLOR_RAISED);
        lv_style_set_transform_width(&style_action_tile_pressed, -3);
        lv_style_set_transform_height(&style_action_tile_pressed, -3);

        alarm_ui_style_track(&style_action_tile, ALARM_UI_STYLE_ACTION_TILE_ACTION_TILE);
        alarm_ui_style_track(&style_action_tile_pressed, ALARM_UI_STYLE_ACTION_TILE_ACTION_TILE_PRESSED);

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_34(parent);
        alarm_ui_run_17(box_0, &style_action_tile, 0, &style_action_tile_pressed, LV_STATE_PRESSED);
        lv_obj_t * icon_chip_0 = icon_chip_create(box_0, glyph, 88, icons_40, color);
        lv_obj_set_style_margin_bottom(icon_chip_0, SPACE_XS, 0);

        lv_obj_t * lv_label_0 = alarm_ui_run_2(box_0, LV_LABEL_LONG_MODE_WRAP);
        alarm_ui_run_45(lv_label_0, font_headline, LV_TEXT_ALIGN_CENTER);
        lv_label_bind_text(lv_label_0, title, NULL);

        lv_obj_t * lv_label_1 = alarm_ui_run_2(box_0, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(lv_label_1, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(lv_label_1, COLOR_TEXT_2, 0);
        lv_label_bind_text(lv_label_1, subtitle, NULL);

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
