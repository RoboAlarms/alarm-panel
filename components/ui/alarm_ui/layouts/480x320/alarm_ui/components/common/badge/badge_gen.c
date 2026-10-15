/**
 * @file badge_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "badge_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * badge_create(lv_obj_t * parent, lv_subject_t * subject, lv_color_t color)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_badge;

    static bool style_inited = false;

    if (!style_inited) {
        /*Init all styles*/
        lv_style_init(&style_badge);

        lv_style_set_height(&style_badge, 18);
        lv_style_set_min_width(&style_badge, 18);
        alarm_ui_run_42(&style_badge, RADIUS_FULL, (255 * 100 / 100));
        lv_style_set_pad_left(&style_badge, 5);
        lv_style_set_pad_right(&style_badge, 5);
        alarm_ui_run_19(&style_badge, LV_FLEX_FLOW_ROW);
        lv_style_set_text_font(&style_badge, font_caption);
        lv_style_set_text_color(&style_badge, COLOR_ON_TONE);

        alarm_ui_style_track(&style_badge, ALARM_UI_STYLE_BADGE_BADGE);

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = box_create(parent);
        lv_obj_set_width(box_0, LV_SIZE_CONTENT);
        lv_obj_set_flag(box_0, LV_OBJ_FLAG_FLOATING, true);
        alarm_ui_run_43(box_0, LV_ALIGN_TOP_RIGHT, -2, 1);
        lv_obj_set_style_bg_color(box_0, color, 0);
        lv_obj_add_style(box_0, &style_badge, 0);
        lv_obj_bind_flag_if_eq(box_0, subject, LV_OBJ_FLAG_HIDDEN, 0);
        lv_obj_t * lv_label_0 = lv_label_create(box_0);
        lv_label_bind_text(lv_label_0, subject, "%d");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
