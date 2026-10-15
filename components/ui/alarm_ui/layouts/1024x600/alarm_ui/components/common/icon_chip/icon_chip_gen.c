/**
 * @file icon_chip_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "icon_chip_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * icon_chip_create(lv_obj_t * parent, const char * glyph, int32_t size, const lv_font_t * font, lv_color_t color)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_icon_chip;

    static bool style_inited = false;

    if (!style_inited) {
        /*Init all styles*/
        lv_style_init(&style_icon_chip);

        alarm_ui_run_59(&style_icon_chip, RADIUS_FULL, OPA_TINT);

        alarm_ui_style_track(&style_icon_chip, ALARM_UI_STYLE_ICON_CHIP_ICON_CHIP);

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = box_create(parent);
        lv_obj_set_width(box_0, size);
        lv_obj_set_height(box_0, size);
        lv_obj_set_style_bg_color(box_0, color, 0);
        lv_obj_add_style(box_0, &style_icon_chip, 0);
        lv_obj_t * icon = alarm_ui_run_33(box_0);
        lv_label_set_text(icon, glyph);
        lv_obj_set_style_text_font(icon, font, 0);
        lv_obj_set_style_text_color(icon, color, 0);
        lv_obj_set_name(icon, "icon");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
