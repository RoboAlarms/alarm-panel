/**
 * @file zone_card_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "zone_card_gen.h"
#include "../../../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * zone_card_create(lv_obj_t * parent, const char * glyph, const char * title, const char * detail)
{
    LV_TRACE_OBJ_CREATE("begin");

    static lv_style_t style_zone_card;
    static lv_style_t style_zone_bypass;
    static lv_style_t style_zone_bypass_pressed;

    static bool style_inited = false;

    if (!style_inited) {
        static lv_style_t * const style_list[] = {
            &style_zone_card,
            &style_zone_bypass,
            &style_zone_bypass_pressed,
        };

        /*Init all styles*/
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            lv_style_init(style_list[i]);
        }

        lv_style_set_width(&style_zone_card, 428);
        lv_style_set_height(&style_zone_card, LV_SIZE_CONTENT);
        lv_style_set_min_height(&style_zone_card, 88);
        alarm_ui_run_6(&style_zone_card, COLOR_SURFACE, (255 * 100 / 100));
        alarm_ui_run_24(&style_zone_card, RADIUS_LG);
        alarm_ui_run_7(&style_zone_card, SPACE_LG, SPACE_LG, SPACE_SM, SPACE_SM, SPACE_LG);
        alarm_ui_run_64(&style_zone_bypass, 44, RADIUS_FULL);
        alarm_ui_run_6(&style_zone_bypass, COLOR_RAISED, (255 * 100 / 100));
        alarm_ui_run_15(&style_zone_bypass, SPACE_LG, SPACE_LG);
        lv_style_set_text_font(&style_zone_bypass, font_label);
        lv_style_set_bg_color(&style_zone_bypass_pressed, COLOR_PRESSED);

        static const uint16_t style_ids[] = {
            ALARM_UI_STYLE_ZONE_CARD_ZONE_CARD,
            ALARM_UI_STYLE_ZONE_CARD_ZONE_BYPASS,
            ALARM_UI_STYLE_ZONE_CARD_ZONE_BYPASS_PRESSED,
        };
        for (uint32_t i = 0; i < sizeof(style_list) / sizeof(style_list[0]); i++) {
            alarm_ui_style_track(style_list[i], style_ids[i]);
        }

        style_inited = true;
    }


    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        lv_obj_t * box_0 = alarm_ui_run_25(parent, &style_zone_card);
        lv_obj_t * chip = icon_chip_create(box_0, glyph, 56, icons_28, COLOR_GREEN);
        lv_obj_set_name(chip, "chip");

        lv_obj_t * box_1 = alarm_ui_run_8(box_0);
        lv_obj_set_style_min_height(box_1, 70, 0);
        alarm_ui_run_1(box_1, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_59(box_1);
        lv_obj_t * title_label = alarm_ui_run_2(box_1, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_max_height(title_label, 52, 0);
        lv_label_set_text(title_label, title);
        alarm_ui_run_37(title_label, font_label, "title_label");

        lv_obj_t * detail_label = alarm_ui_run_2(box_1, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(detail_label, detail);
        alarm_ui_run_47(detail_label, "detail_label");

        lv_obj_t * bypass = alarm_ui_run_18(box_0, LV_SIZE_CONTENT, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(bypass, 8);
        alarm_ui_run_16(bypass, &style_zone_bypass, 0, &style_zone_bypass_pressed, LV_STATE_PRESSED);
        alarm_ui_run_10(bypass, ui_on_zone_bypass, "bypass");
        lv_obj_t * bypass_label = alarm_ui_run_14(bypass, alarm_ui_tr("Bypass"));
        lv_obj_set_name(bypass_label, "bypass_label");

        the_root = box_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
