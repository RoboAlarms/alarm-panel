/**
 * @file chip_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "chip_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * chip_create(lv_obj_t * parent, const char * text, lv_subject_t * subject)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_chip;
    static lv_style_t style_chip_checked;

    static bool style_inited = false;

    if (!style_inited) {
        /*Init all styles*/
        lv_style_init(&style_chip);
        lv_style_init(&style_chip_checked);

        lv_style_set_height(&style_chip, 40);
        alarm_ui_run_24(&style_chip, RADIUS_FULL);
        alarm_ui_run_15(&style_chip, SPACE_LG, SPACE_LG);
        alarm_ui_run_45(&style_chip, font_label, COLOR_TEXT_2);
        alarm_ui_run_6(&style_chip_checked, COLOR_BLUE, (255 * 18 / 100));
        lv_style_set_border_color(&style_chip_checked, COLOR_BLUE);
        lv_style_set_text_color(&style_chip_checked, COLOR_TEXT);

        alarm_ui_style_track(&style_chip, ALARM_UI_STYLE_CHIP_CHIP);
        alarm_ui_style_track(&style_chip_checked, ALARM_UI_STYLE_CHIP_CHIP_CHECKED);

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_22(parent, LV_SIZE_CONTENT, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flag(box_0, LV_OBJ_FLAG_CHECKABLE, true);
        lv_obj_bind_checked(box_0, subject);
        alarm_ui_run_16(box_0, &style_chip, 0, &style_chip_checked, LV_STATE_CHECKED);
        lv_obj_set_name_static(box_0, "chip_#");
        lv_obj_t * lv_label_0 = lv_label_create(box_0);
        lv_label_set_text(lv_label_0, text);

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
