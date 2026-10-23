/**
 * @file device_level_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "device_level_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * device_level_create(lv_obj_t * parent, int32_t value)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_device_level;
    static lv_style_t style_device_level_fill;
    static lv_style_t style_device_level_edge;

    static bool style_inited = false;

    if (!style_inited) {
        static lv_style_t * const style_list[] = {
            &style_device_level,
            &style_device_level_fill,
            &style_device_level_edge,
        };

        /*Init all styles*/
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            lv_style_init(style_list[i]);
        }

        lv_style_set_radius(&style_device_level, RADIUS_FULL);
        alarm_ui_run_6(&style_device_level, COLOR_RAISED, (255 * 100 / 100));
        lv_style_set_radius(&style_device_level_fill, RADIUS_FULL);
        alarm_ui_run_6(&style_device_level_fill, COLOR_BLUE, (255 * 30 / 100));
        lv_style_set_radius(&style_device_level_edge, 2);
        alarm_ui_run_6(&style_device_level_edge, COLOR_TEXT, (255 * 100 / 100));
        lv_style_set_pad_left(&style_device_level_edge, -22);
        lv_style_set_pad_right(&style_device_level_edge, -22);
        lv_style_set_pad_top(&style_device_level_edge, -12);
        lv_style_set_pad_bottom(&style_device_level_edge, -12);

        static const uint16_t style_ids[] = {
            ALARM_UI_STYLE_DEVICE_LEVEL_DEVICE_LEVEL,
            ALARM_UI_STYLE_DEVICE_LEVEL_DEVICE_LEVEL_FILL,
            ALARM_UI_STYLE_DEVICE_LEVEL_DEVICE_LEVEL_EDGE,
        };
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            alarm_ui_style_track(style_list[i], style_ids[i]);
        }

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_0(parent, lv_pct(100), 48);
        lv_obj_t * level = lv_slider_create(box_0);
        alarm_ui_run_32(level, lv_pct(100), lv_pct(100));
        lv_slider_set_min_value(level, 0);
        lv_slider_set_max_value(level, 100);
        lv_slider_set_value(level, value, LV_ANIM_OFF);
        alarm_ui_run_16(level, &style_device_level, 0, &style_device_level_fill, LV_PART_INDICATOR);
        alarm_ui_run_68(level, &style_device_level_edge, LV_PART_KNOB, "level");

        lv_obj_t * lv_label_0 = lv_label_create(box_0);
        lv_obj_set_align(lv_label_0, LV_ALIGN_LEFT_MID);
        lv_obj_set_x(lv_label_0, SPACE_LG);
        lv_label_set_text(lv_label_0, "\xEE\x85\xB8");
        alarm_ui_run_4(lv_label_0, icons_20, COLOR_TEXT_2);

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
