/**
 * @file segment_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "segment_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * segment_create(lv_obj_t * parent, const char * text)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_segment;
    static lv_style_t style_segment_pressed;

    static bool style_inited = false;

    if (!style_inited) {
        /*Init all styles*/
        lv_style_init(&style_segment);
        lv_style_init(&style_segment_pressed);

        alarm_ui_run_64(&style_segment, 44, RADIUS_MD);
        alarm_ui_run_15(&style_segment, SPACE_LG, SPACE_LG);
        alarm_ui_run_45(&style_segment, font_label, COLOR_TEXT_2);
        alarm_ui_run_6(&style_segment_pressed, COLOR_PRESSED, (255 * 100 / 100));

        alarm_ui_style_track(&style_segment, ALARM_UI_STYLE_SEGMENT_SEGMENT);
        alarm_ui_style_track(&style_segment_pressed, ALARM_UI_STYLE_SEGMENT_SEGMENT_PRESSED);

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_18(parent, LV_SIZE_CONTENT, LV_OBJ_FLAG_CLICKABLE);
        alarm_ui_run_16(box_0, &style_segment, 0, &style_segment_pressed, LV_STATE_PRESSED);
        lv_obj_t * lv_label_0 = lv_label_create(box_0);
        lv_label_set_text(lv_label_0, text);

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
