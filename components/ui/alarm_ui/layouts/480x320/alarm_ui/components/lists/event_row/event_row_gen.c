/**
 * @file event_row_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "event_row_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * event_row_create(lv_obj_t * parent, const char * glyph, const char * title, const char * detail, const char * time)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_event_row;

    static bool style_inited = false;

    if (!style_inited) {
        /*Init all styles*/
        lv_style_init(&style_event_row);

        alarm_ui_run_26(&style_event_row, lv_pct(100), 56);
        alarm_ui_run_24(&style_event_row, COLOR_SURFACE, (255 * 100 / 100));
        lv_style_set_radius(&style_event_row, RADIUS_MD);
        alarm_ui_run_5(&style_event_row, SPACE_MD, SPACE_LG);
        lv_style_set_pad_column(&style_event_row, SPACE_MD);

        alarm_ui_style_track(&style_event_row, ALARM_UI_STYLE_EVENT_ROW_EVENT_ROW);

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_32(parent, &style_event_row);
        lv_obj_t * chip = icon_chip_create(box_0, glyph, 36, icons_20, COLOR_TEXT_2);
        lv_obj_set_name(chip, "chip");

        lv_obj_t * box_1 = alarm_ui_run_9(box_0, LV_SIZE_CONTENT);
        alarm_ui_run_1(box_1, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box_1, 0, 0);
        lv_obj_t * title_label = alarm_ui_run_2(box_1, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(title_label, title);
        alarm_ui_run_29(title_label);

        lv_obj_t * detail_label = alarm_ui_run_2(box_1, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(detail_label, detail);
        alarm_ui_run_4(detail_label, font_caption, COLOR_TEXT_2);
        lv_obj_set_name(detail_label, "detail_label");

        lv_obj_t * time_label = lv_label_create(box_0);
        lv_label_set_text(time_label, time);
        alarm_ui_run_4(time_label, font_caption, COLOR_TEXT_2);

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
