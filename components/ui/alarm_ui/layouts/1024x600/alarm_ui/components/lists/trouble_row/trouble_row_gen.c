/**
 * @file trouble_row_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "trouble_row_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * trouble_row_create(lv_obj_t * parent, const char * glyph, const char * title, const char * detail)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_trouble_row;

    static bool style_inited = false;

    if (!style_inited) {
        /*Init all styles*/
        lv_style_init(&style_trouble_row);

        alarm_ui_run_39(&style_trouble_row);
        lv_style_set_min_height(&style_trouble_row, 80);
        alarm_ui_run_6(&style_trouble_row, COLOR_SURFACE, (255 * 100 / 100));
        alarm_ui_run_24(&style_trouble_row, RADIUS_MD);
        alarm_ui_run_7(&style_trouble_row, SPACE_LG, SPACE_XL, SPACE_SM, SPACE_SM, SPACE_LG);

        alarm_ui_style_track(&style_trouble_row, ALARM_UI_STYLE_TROUBLE_ROW_TROUBLE_ROW);

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_25(parent, &style_trouble_row);
        lv_obj_t * chip = icon_chip_create(box_0, glyph, 52, icons_28, COLOR_AMBER);
        lv_obj_set_name(chip, "chip");

        lv_obj_t * box_1 = alarm_ui_run_8(box_0);
        lv_obj_set_style_min_height(box_1, 62, 0);
        alarm_ui_run_1(box_1, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_59(box_1);
        lv_obj_t * title_label = alarm_ui_run_2(box_1, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(title_label, title);
        alarm_ui_run_37(title_label, font_label, "title_label");

        lv_obj_t * detail_label = alarm_ui_run_2(box_1, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(detail_label, detail);
        alarm_ui_run_48(detail_label, "detail_label");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
