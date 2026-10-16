/**
 * @file arm_strip_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "arm_strip_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * arm_strip_create(lv_obj_t * parent)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_arm_strip;

    static bool style_inited = false;

    if (!style_inited) {
        /*Init all styles*/
        lv_style_init(&style_arm_strip);

        alarm_ui_run_6(&style_arm_strip, COLOR_SURFACE, (255 * 100 / 100));
        alarm_ui_run_24(&style_arm_strip, RADIUS_LG);
        lv_style_set_pad_top(&style_arm_strip, SPACE_MD);
        lv_style_set_pad_bottom(&style_arm_strip, SPACE_MD);
        lv_style_set_pad_left(&style_arm_strip, SPACE_LG);
        lv_style_set_pad_right(&style_arm_strip, SPACE_LG);
        lv_style_set_layout(&style_arm_strip, LV_LAYOUT_FLEX);
        lv_style_set_flex_flow(&style_arm_strip, LV_FLEX_FLOW_COLUMN);
        lv_style_set_pad_row(&style_arm_strip, SPACE_MD);

        alarm_ui_style_track(&style_arm_strip, ALARM_UI_STYLE_ARM_STRIP_ARM_STRIP);

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_0(parent, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_add_style(box_0, &style_arm_strip, 0);
        lv_obj_set_name_static(box_0, "arm_strip_#");
        lv_obj_t * box_1 = alarm_ui_run_0(box_0, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(box_1, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(box_1);
        lv_obj_set_style_pad_column(box_1, SPACE_MD, 0);
        lv_obj_t * arm_strip_chip = alarm_ui_run_0(box_1, 48, 48);
        lv_obj_set_style_radius(arm_strip_chip, RADIUS_FULL, 0);
        lv_obj_set_style_bg_opa(arm_strip_chip, OPA_TINT, 0);
        lv_obj_bind_style_prop(arm_strip_chip, LV_STYLE_BG_COLOR, 0, &subject_arm_strip_color);
        lv_obj_t * lv_label_0 = alarm_ui_run_33(arm_strip_chip);
        alarm_ui_run_12(lv_label_0, icons_28, &subject_arm_strip_glyph);
        lv_obj_bind_style_prop(lv_label_0, LV_STYLE_TEXT_COLOR, 0, &subject_arm_strip_color);

        lv_obj_t * box_2 = alarm_ui_run_8(box_1);
        alarm_ui_run_1(box_2, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box_2, 2, 0);
        lv_obj_t * arm_strip_title = alarm_ui_run_2(box_2, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_max_height(arm_strip_title, 33, 0);
        alarm_ui_run_12(arm_strip_title, font_title, &subject_arm_strip_title);

        lv_obj_t * arm_strip_detail = alarm_ui_run_2(box_2, LV_LABEL_LONG_MODE_WRAP);
        alarm_ui_run_29(arm_strip_detail, COLOR_TEXT_2, &subject_arm_strip_detail);

        lv_obj_t * arm_strip_note = alarm_ui_run_2(box_2, LV_LABEL_LONG_MODE_WRAP);
        alarm_ui_run_29(arm_strip_note, COLOR_AMBER, &subject_arm_strip_note);
        alarm_ui_run_42(arm_strip_note, &subject_arm_strip_note_on, 0, "arm_strip_note");

        lv_obj_t * arm_strip_put_off = alarm_ui_run_22(box_0, lv_pct(100), LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_flex_main_place(arm_strip_put_off, LV_FLEX_ALIGN_CENTER, 0);
        lv_obj_bind_flag_if_not_eq(arm_strip_put_off, &subject_arm_strip, LV_OBJ_FLAG_HIDDEN, 1);
        alarm_ui_run_54(arm_strip_put_off, &style_button, 0, &style_button_pressed, LV_STATE_PRESSED);
        lv_obj_set_name(arm_strip_put_off, "arm_strip_put_off");
        lv_obj_t * lv_label_1 = alarm_ui_run_23(arm_strip_put_off, "\xEE\x82\x87");
        lv_obj_set_style_text_font(lv_label_1, icons_20, 0);

        alarm_ui_run_14(arm_strip_put_off, alarm_ui_tr_c("Home, the arming warning's button", "Put off"));

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
