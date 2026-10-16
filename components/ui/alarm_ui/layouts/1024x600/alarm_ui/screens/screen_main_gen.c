/**
 * @file screen_main_gen.c
 * @brief Template source file for LVGL objects
 */

/*********************
 *      INCLUDES
 *********************/

#include "screen_main_gen.h"
#include "../alarm_ui.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * screen_main_create(void)
{
    LV_TRACE_OBJ_CREATE("begin");

    lv_obj_t * the_root = NULL;

    #if ALARM_UI_CHECK_COMPILE_TARGET(ALARM_UI_TARGET_ALL)
    if (alarm_ui_check_target(ALARM_UI_TARGET_ALL)) {
        if (screen_main == NULL) screen_main = lv_obj_create(NULL);
        lv_obj_t * lv_obj_0 = screen_main;
        lv_obj_set_flag(lv_obj_0, LV_OBJ_FLAG_SCROLLABLE, false);

        alarm_ui_run_52(lv_obj_0, &style_screen, 0, &style_screen_alarm, &subject_alarm_active, 1);
        lv_obj_t * rail = alarm_ui_run_0(lv_obj_0, RAIL_W, lv_pct(100));
        lv_obj_set_style_bg_color(rail, COLOR_RAIL, 0);
        lv_obj_set_style_bg_opa(rail, (255 * 100 / 100), 0);
        lv_obj_set_style_border_side(rail, LV_BORDER_SIDE_RIGHT, 0);
        alarm_ui_run_40(rail, COLOR_LINE);
        alarm_ui_run_1(rail, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_3(rail);
        alarm_ui_run_70(rail, SPACE_LG, SPACE_LG);
        alarm_ui_run_49(rail, SPACE_SM, "rail");
        lv_obj_t * rail_logo = alarm_ui_run_0(rail, 56, 56);
        lv_obj_set_style_margin_bottom(rail_logo, SPACE_SM, 0);
        lv_obj_set_name(rail_logo, "rail_logo");
        lv_obj_t * rail_brand = lv_image_create(rail_logo);
        lv_obj_set_align(rail_brand, LV_ALIGN_CENTER);
        alarm_ui_run_31(rail_brand, 44, 44);
        lv_obj_set_style_image_recolor_opa(rail_brand, (255 * 100 / 100), 0);
        lv_obj_bind_style_prop(rail_brand, LV_STYLE_IMAGE_RECOLOR, 0, &subject_tone_color);
        lv_obj_set_name(rail_brand, "rail_brand");

        lv_obj_t * nav_home = nav_item_create(rail, "\xEE\x85\x98", alarm_ui_tr_c("navigation, the arming screen", "Security"));
        lv_obj_bind_style(nav_home, &style_nav_active, 0, &subject_view, 0);
        alarm_ui_run_51(nav_home, 0, "nav_home");

        lv_obj_t * nav_zones = nav_item_create(rail, "\xEE\x83\xBF", alarm_ui_tr("Zones"));
        lv_obj_bind_style(nav_zones, &style_nav_active, 0, &subject_view, 1);
        alarm_ui_run_51(nav_zones, 1, "nav_zones");
        badge_create(nav_zones, &subject_open_count, COLOR_AMBER);

        lv_obj_t * nav_devices = box_create(rail);
        lv_obj_set_height(nav_devices, 86);
        alarm_ui_run_32(nav_devices, &style_nav_item);
        alarm_ui_run_52(nav_devices, &style_nav_item_pressed, LV_STATE_PRESSED, &style_nav_active, &subject_view, 6);
        lv_obj_bind_flag_if_eq(nav_devices, &subject_devices_avail, LV_OBJ_FLAG_HIDDEN, 0);
        alarm_ui_run_51(nav_devices, 6, "nav_devices");
        lv_obj_t * lv_label_0 = alarm_ui_run_23(nav_devices, "\xEE\x98\xBC");
        lv_obj_set_style_text_font(lv_label_0, icons_28, 0);

        lv_obj_t * lv_label_1 = alarm_ui_run_14(nav_devices, alarm_ui_tr_c("navigation, the devices screen", "Smart Home"));
        lv_obj_set_width(lv_label_1, 72);
        alarm_ui_run_35(lv_label_1, 36);
        alarm_ui_run_61(lv_label_1);

        lv_obj_t * nav_activity = nav_item_create(rail, "\xEE\x87\xB5", alarm_ui_tr("Activity"));
        lv_obj_bind_style(nav_activity, &style_nav_active, 0, &subject_view, 2);
        alarm_ui_run_51(nav_activity, 2, "nav_activity");

        lv_obj_t * rail_fill = box_create(rail);
        lv_obj_set_width(rail_fill, 8);
        lv_obj_set_flex_grow(rail_fill, 1);

        lv_obj_t * nav_panic = nav_item_create(rail, "\xEE\x8B\xAF", alarm_ui_tr("Panic"));
        lv_obj_set_style_text_color(nav_panic, COLOR_RED, 0);
        lv_obj_bind_style(nav_panic, &style_nav_active, 0, &subject_view, 4);
        lv_obj_bind_flag_if_eq(nav_panic, &subject_link_home, LV_OBJ_FLAG_HIDDEN, 3);
        alarm_ui_run_51(nav_panic, 4, "nav_panic");

        lv_obj_t * bar = box_create(lv_obj_0);
        lv_obj_set_x(bar, RAIL_W);
        alarm_ui_run_31(bar, CONTENT_W, BAR_H);
        alarm_ui_run_26(bar, SPACE_XL, SPACE_XL);
        alarm_ui_run_1(bar, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(bar);
        alarm_ui_run_48(bar, SPACE_LG, "bar");
        lv_obj_t * bar_date = lv_label_create(bar);
        alarm_ui_run_35(bar_date, 26);
        alarm_ui_run_4(bar_date, font_label, COLOR_TEXT_2);
        lv_label_bind_text(bar_date, &subject_date, NULL);
        lv_obj_bind_flag_if_not_eq(bar_date, &subject_bar_banner, LV_OBJ_FLAG_HIDDEN, 0);

        lv_obj_t * program_banner = alarm_ui_run_0(bar, LV_SIZE_CONTENT, 40);
        lv_obj_set_flag(program_banner, LV_OBJ_FLAG_CLICKABLE, true);
        alarm_ui_run_9(program_banner, COLOR_BLUE, 0, (255 * 16 / 100), RADIUS_FULL);
        alarm_ui_run_26(program_banner, SPACE_LG, SPACE_LG);
        alarm_ui_run_1(program_banner, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(program_banner);
        alarm_ui_run_39(program_banner, SPACE_SM, &subject_bar_banner, 2);
        alarm_ui_run_10(program_banner, ui_on_program_exit, "program_banner");
        lv_obj_t * lv_label_2 = alarm_ui_run_23(program_banner, "\xEE\x86\xB1");
        alarm_ui_run_4(lv_label_2, icons_20, COLOR_BLUE);

        lv_obj_t * lv_label_3 = alarm_ui_run_14(program_banner, alarm_ui_tr("Installer \xC2\xB7 Arming off \xC2\xB7 Tap to leave"));
        alarm_ui_run_35(lv_label_3, 26);
        alarm_ui_run_4(lv_label_3, font_label, COLOR_BLUE);

        lv_obj_t * codes_banner = alarm_ui_run_0(bar, LV_SIZE_CONTENT, 40);
        lv_obj_set_flag(codes_banner, LV_OBJ_FLAG_CLICKABLE, true);
        alarm_ui_run_9(codes_banner, COLOR_AMBER, 0, (255 * 16 / 100), RADIUS_FULL);
        alarm_ui_run_26(codes_banner, SPACE_LG, SPACE_LG);
        alarm_ui_run_1(codes_banner, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(codes_banner);
        alarm_ui_run_39(codes_banner, SPACE_SM, &subject_bar_banner, 1);
        alarm_ui_run_10(codes_banner, ui_on_change_code, "codes_banner");
        lv_obj_t * lv_label_4 = alarm_ui_run_23(codes_banner, "\xEE\x92\xA3");
        alarm_ui_run_4(lv_label_4, icons_20, COLOR_AMBER);

        lv_obj_t * lv_label_5 = alarm_ui_run_14(codes_banner, alarm_ui_tr("Factory codes \xC2\xB7 Tap to change"));
        alarm_ui_run_35(lv_label_5, 26);
        alarm_ui_run_4(lv_label_5, font_label, COLOR_AMBER);

        lv_obj_t * codes_banner_link = alarm_ui_run_0(bar, LV_SIZE_CONTENT, 40);
        alarm_ui_run_9(codes_banner_link, COLOR_AMBER, 0, (255 * 16 / 100), RADIUS_FULL);
        alarm_ui_run_26(codes_banner_link, SPACE_LG, SPACE_LG);
        alarm_ui_run_1(codes_banner_link, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(codes_banner_link);
        alarm_ui_run_39(codes_banner_link, SPACE_SM, &subject_bar_banner, 8);
        lv_obj_t * lv_label_6 = alarm_ui_run_23(codes_banner_link, "\xEE\x92\xA3");
        alarm_ui_run_4(lv_label_6, icons_20, COLOR_AMBER);

        lv_obj_t * lv_label_7 = alarm_ui_run_14(codes_banner_link, alarm_ui_tr("Factory codes \xC2\xB7 Change them on the alarm panel"));
        alarm_ui_run_35(lv_label_7, 26);
        alarm_ui_run_4(lv_label_7, font_label, COLOR_AMBER);

        lv_obj_t * walk_banner = alarm_ui_run_0(bar, LV_SIZE_CONTENT, 40);
        lv_obj_set_flag(walk_banner, LV_OBJ_FLAG_CLICKABLE, true);
        alarm_ui_run_9(walk_banner, COLOR_BLUE, 0, (255 * 16 / 100), RADIUS_FULL);
        alarm_ui_run_26(walk_banner, SPACE_LG, SPACE_LG);
        alarm_ui_run_1(walk_banner, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(walk_banner);
        alarm_ui_run_39(walk_banner, SPACE_SM, &subject_bar_banner, 4);
        lv_obj_add_event_cb(walk_banner, ui_on_end_walk_test, LV_EVENT_CLICKED, (void *)"banner");
        lv_obj_set_name(walk_banner, "walk_banner");
        lv_obj_t * lv_label_8 = alarm_ui_run_23(walk_banner, "\xEE\x8E\xB9");
        alarm_ui_run_4(lv_label_8, icons_20, COLOR_BLUE);

        lv_obj_t * lv_label_9 = lv_label_create(walk_banner);
        alarm_ui_run_35(lv_label_9, 26);
        alarm_ui_run_4(lv_label_9, font_label, COLOR_BLUE);
        lv_label_bind_text(lv_label_9, &subject_walk_banner, NULL);

        lv_obj_t * arm_banner = alarm_ui_run_0(bar, LV_SIZE_CONTENT, 40);
        lv_obj_set_flag(arm_banner, LV_OBJ_FLAG_CLICKABLE, true);
        alarm_ui_run_9(arm_banner, COLOR_AMBER, 0, (255 * 16 / 100), RADIUS_FULL);
        alarm_ui_run_26(arm_banner, SPACE_LG, SPACE_LG);
        alarm_ui_run_1(arm_banner, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(arm_banner);
        alarm_ui_run_39(arm_banner, SPACE_SM, &subject_bar_banner, 5);
        alarm_ui_run_51(arm_banner, 0, "arm_banner");
        lv_obj_t * lv_label_10 = alarm_ui_run_23(arm_banner, "\xEE\x82\x87");
        alarm_ui_run_4(lv_label_10, icons_20, COLOR_AMBER);

        lv_obj_t * lv_label_11 = lv_label_create(arm_banner);
        alarm_ui_run_35(lv_label_11, 26);
        alarm_ui_run_4(lv_label_11, font_label, COLOR_AMBER);
        lv_label_bind_text(lv_label_11, &subject_arm_banner, NULL);

        lv_obj_t * update_banner = alarm_ui_run_0(bar, LV_SIZE_CONTENT, 40);
        lv_obj_set_flag(update_banner, LV_OBJ_FLAG_CLICKABLE, true);
        alarm_ui_run_9(update_banner, COLOR_BLUE, 0, (255 * 16 / 100), RADIUS_FULL);
        alarm_ui_run_26(update_banner, SPACE_LG, SPACE_LG);
        alarm_ui_run_1(update_banner, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(update_banner);
        alarm_ui_run_39(update_banner, SPACE_SM, &subject_bar_banner, 6);
        lv_obj_add_event_cb(update_banner, ui_on_end_walk_test, LV_EVENT_CLICKED, (void *)"update");
        lv_obj_t * lv_label_12 = lv_label_create(update_banner);
        alarm_ui_run_4(lv_label_12, icons_20, COLOR_BLUE);
        lv_label_bind_text(lv_label_12, &subject_update_banner_glyph, NULL);

        lv_obj_t * lv_label_13 = lv_label_create(update_banner);
        alarm_ui_run_35(lv_label_13, 26);
        alarm_ui_run_4(lv_label_13, font_label, COLOR_BLUE);
        lv_label_bind_text(lv_label_13, &subject_update_banner, NULL);

        lv_obj_t * update_banner_back = alarm_ui_run_0(bar, LV_SIZE_CONTENT, 40);
        lv_obj_set_flag(update_banner_back, LV_OBJ_FLAG_CLICKABLE, true);
        alarm_ui_run_9(update_banner_back, COLOR_AMBER, 0, (255 * 16 / 100), RADIUS_FULL);
        alarm_ui_run_26(update_banner_back, SPACE_LG, SPACE_LG);
        alarm_ui_run_1(update_banner_back, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(update_banner_back);
        alarm_ui_run_39(update_banner_back, SPACE_SM, &subject_bar_banner, 7);
        lv_obj_t * lv_label_14 = alarm_ui_run_23(update_banner_back, "\xEE\x86\x93");
        alarm_ui_run_4(lv_label_14, icons_20, COLOR_AMBER);

        lv_obj_t * lv_label_15 = lv_label_create(update_banner_back);
        alarm_ui_run_35(lv_label_15, 26);
        alarm_ui_run_4(lv_label_15, font_label, COLOR_AMBER);
        lv_label_bind_text(lv_label_15, &subject_update_banner, NULL);

        lv_obj_t * remote_banner = alarm_ui_run_0(bar, LV_SIZE_CONTENT, 40);
        lv_obj_set_flag(remote_banner, LV_OBJ_FLAG_CLICKABLE, true);
        alarm_ui_run_9(remote_banner, COLOR_RED, 0, (255 * 100 / 100), RADIUS_FULL);
        alarm_ui_run_26(remote_banner, SPACE_LG, SPACE_LG);
        alarm_ui_run_1(remote_banner, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(remote_banner);
        alarm_ui_run_39(remote_banner, SPACE_SM, &subject_bar_banner, 3);
        alarm_ui_run_10(remote_banner, ui_on_remote_banner, "remote_banner");
        lv_obj_t * lv_label_16 = alarm_ui_run_23(remote_banner, "\xEE\x83\xA8");
        alarm_ui_run_4(lv_label_16, icons_20, COLOR_ON_TONE);

        lv_obj_t * lv_label_17 = lv_label_create(remote_banner);
        alarm_ui_run_35(lv_label_17, 26);
        alarm_ui_run_4(lv_label_17, font_label, COLOR_ON_TONE);
        lv_label_bind_text(lv_label_17, &subject_remote_banner, NULL);

        lv_obj_t * bar_fill = alarm_ui_run_0(bar, 8, 8);
        lv_obj_set_flex_grow(bar_fill, 1);

        lv_obj_t * bar_trouble = alarm_ui_run_22(bar, LV_SIZE_CONTENT, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(bar_trouble, 6);
        alarm_ui_run_52(bar_trouble, &style_bar_pill, 0, &style_bar_pill_active, &subject_view, 3);
        lv_obj_bind_flag_if_eq(bar_trouble, &subject_trouble_count, LV_OBJ_FLAG_HIDDEN, 0);
        alarm_ui_run_51(bar_trouble, 3, "bar_trouble");
        lv_obj_t * lv_label_18 = alarm_ui_run_23(bar_trouble, "\xEE\x86\x93");
        lv_obj_set_style_text_font(lv_label_18, icons_20, 0);

        lv_obj_t * lv_label_19 = lv_label_create(bar_trouble);
        lv_obj_set_style_text_font(lv_label_19, font_label, 0);
        lv_label_bind_text(lv_label_19, &subject_trouble_count, "%d");

        lv_obj_t * bar_wifi = lv_label_create(bar);
        alarm_ui_run_12(bar_wifi, icons_20, &subject_wifi_icon);
        alarm_ui_run_52(bar_wifi, &style_text_2, 0, &style_text_warn, &subject_wifi_ok, 0);

        lv_obj_t * bar_battery = alarm_ui_run_0(bar, LV_SIZE_CONTENT, 24);
        alarm_ui_run_1(bar_battery, LV_FLEX_FLOW_ROW);
        alarm_ui_run_66(bar_battery, LV_FLEX_ALIGN_CENTER, 4);
        alarm_ui_run_52(bar_battery, &style_text_2, 0, &style_text_warn, &subject_battery_warn, 1);
        lv_obj_bind_style(bar_battery, &style_text_error, 0, &subject_battery_critical, 1);
        lv_obj_t * box_0 = alarm_ui_run_0(bar_battery, 24, 24);
        lv_obj_t * bar_battery_icon = alarm_ui_run_33(box_0);
        alarm_ui_run_12(bar_battery_icon, icons_20, &subject_battery_icon);
        lv_obj_set_name(bar_battery_icon, "bar_battery_icon");

        lv_obj_t * bar_battery_strike = box_create(box_0);
        lv_obj_set_x(bar_battery_strike, 3);
        lv_obj_set_y(bar_battery_strike, 3);
        alarm_ui_run_31(bar_battery_strike, 24, 2);
        lv_obj_set_style_bg_color(bar_battery_strike, COLOR_RED, 0);
        lv_obj_set_style_bg_opa(bar_battery_strike, 255, 0);
        lv_obj_set_style_transform_rotation(bar_battery_strike, 450, 0);
        lv_obj_set_style_transform_pivot_x(bar_battery_strike, 0, 0);
        lv_obj_set_style_transform_pivot_y(bar_battery_strike, 0, 0);
        alarm_ui_run_42(bar_battery_strike, &subject_battery_critical, 0, "bar_battery_strike");

        lv_obj_t * bar_battery_text = lv_label_create(bar_battery);
        alarm_ui_run_12(bar_battery_text, font_caption, &subject_battery_text);
        lv_obj_set_name(bar_battery_text, "bar_battery_text");

        lv_obj_t * bar_clock = alarm_ui_run_0(bar, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        alarm_ui_run_1(bar_clock, LV_FLEX_FLOW_ROW);
        alarm_ui_run_66(bar_clock, LV_FLEX_ALIGN_END, 4);
        lv_obj_t * bar_time = lv_label_create(bar_clock);
        alarm_ui_run_12(bar_time, font_title, &subject_time);

        lv_obj_t * bar_ampm = lv_label_create(bar_clock);
        alarm_ui_run_4(bar_ampm, font_caption, COLOR_TEXT_2);
        lv_obj_set_style_pad_bottom(bar_ampm, 3, 0);
        lv_label_bind_text(bar_ampm, &subject_ampm, NULL);

        lv_obj_t * bar_chime = alarm_ui_run_0(bar, 44, 44);
        alarm_ui_run_60(bar_chime, LV_OBJ_FLAG_CLICKABLE, true, 8);
        alarm_ui_run_52(bar_chime, &style_bar_round, 0, &style_nav_on, &subject_chime, 1);
        lv_obj_bind_style(bar_chime, &style_disabled, 0, &subject_locked, 1);
        lv_obj_add_style(bar_chime, &style_disabled, LV_STATE_DISABLED);
        lv_obj_bind_state_if_ge(bar_chime, &subject_link_home, LV_STATE_DISABLED, 1);
        alarm_ui_run_10(bar_chime, ui_on_chime, "bar_chime");
        lv_obj_t * lv_label_20 = alarm_ui_run_33(bar_chime);
        alarm_ui_run_12(lv_label_20, icons_20, &subject_chime_icon);

        lv_obj_t * bar_settings = alarm_ui_run_0(bar, 44, 44);
        alarm_ui_run_60(bar_settings, LV_OBJ_FLAG_CLICKABLE, true, 8);
        alarm_ui_run_52(bar_settings, &style_bar_round, 0, &style_nav_active, &subject_view, 5);
        lv_obj_bind_style(bar_settings, &style_disabled, 0, &subject_locked, 1);
        alarm_ui_run_10(bar_settings, ui_on_settings, "bar_settings");
        lv_obj_t * lv_label_21 = alarm_ui_run_11(bar_settings, "\xEE\x85\x94");
        alarm_ui_run_4(lv_label_21, icons_20, COLOR_TEXT_2);

        lv_obj_t * content = box_create(lv_obj_0);
        lv_obj_set_x(content, RAIL_W);
        lv_obj_set_y(content, BAR_H);
        alarm_ui_run_31(content, CONTENT_W, CONTENT_H);
        alarm_ui_run_26(content, SPACE_XL, SPACE_XL);
        lv_obj_set_style_pad_bottom(content, SPACE_XL, 0);
        lv_obj_t * view_home = alarm_ui_run_0(content, lv_pct(100), lv_pct(100));
        alarm_ui_run_1(view_home, LV_FLEX_FLOW_ROW);
        alarm_ui_run_39(view_home, SPACE_XL, &subject_view, 0);
        lv_obj_t * hero = hero_create(view_home);
        alarm_ui_run_31(hero, PANE_W, lv_pct(100));

        lv_obj_t * pane = alarm_ui_run_0(view_home, PANE_W, lv_pct(100));
        alarm_ui_run_1(pane, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(pane, SPACE_LG, 0);
        lv_obj_t * arm_strip = arm_strip_create(pane);
        alarm_ui_run_42(arm_strip, &subject_arm_strip, 0, "arm_strip");

        lv_obj_t * link_strip = alarm_ui_run_0(pane, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_9(link_strip, COLOR_SURFACE, 0, (255 * 100 / 100), RADIUS_LG);
        alarm_ui_run_40(link_strip, COLOR_LINE);
        alarm_ui_run_70(link_strip, SPACE_MD, SPACE_MD);
        alarm_ui_run_26(link_strip, SPACE_LG, SPACE_LG);
        alarm_ui_run_1(link_strip, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(link_strip);
        alarm_ui_run_39(link_strip, SPACE_MD, &subject_link_home, 2);
        lv_obj_set_name(link_strip, "link_strip");
        lv_obj_t * box_1 = alarm_ui_run_0(link_strip, 48, 48);
        alarm_ui_run_21(box_1, RADIUS_FULL, COLOR_AMBER, OPA_TINT);
        lv_obj_t * lv_label_22 = alarm_ui_run_11(box_1, "\xEE\x86\xAF");
        alarm_ui_run_4(lv_label_22, icons_28, COLOR_AMBER);

        lv_obj_t * box_2 = alarm_ui_run_8(link_strip);
        alarm_ui_run_1(box_2, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box_2, 2, 0);
        lv_obj_t * lv_label_23 = alarm_ui_run_2(box_2, LV_LABEL_LONG_MODE_WRAP);
        alarm_ui_run_73(lv_label_23, alarm_ui_tr("Check the alarm panel"));

        lv_obj_t * lv_label_24 = alarm_ui_run_2(box_2, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(lv_label_24, alarm_ui_tr("Is it on, and on the same Wi-Fi?"));
        lv_obj_set_style_text_color(lv_label_24, COLOR_TEXT_2, 0);

        lv_obj_t * panel_arm = alarm_ui_run_0(pane, lv_pct(100), lv_pct(100));
        lv_obj_set_flex_grow(panel_arm, 1);
        alarm_ui_run_1(panel_arm, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(panel_arm, SPACE_LG, 0);
        alarm_ui_run_43(panel_arm, &subject_panel, 0, "panel_arm");
        lv_obj_t * arm_tile_0 = alarm_ui_run_46(panel_arm, "\xEE\x84\x8E", alarm_ui_tr_c("arming mode", "Away"), alarm_ui_tr_c("arming mode", "Everyone is leaving"), COLOR_ROSE);
        lv_obj_add_event_cb(arm_tile_0, ui_on_arm, LV_EVENT_CLICKED, (void *)"away");

        lv_obj_t * arm_tile_1 = alarm_ui_run_46(panel_arm, "\xEE\x83\xB5", alarm_ui_tr_c("arming mode", "Stay"), alarm_ui_tr_c("arming mode", "Someone is home"), COLOR_BLUE);
        lv_obj_add_event_cb(arm_tile_1, ui_on_arm, LV_EVENT_CLICKED, (void *)"stay");

        lv_obj_t * arm_tile_2 = alarm_ui_run_46(panel_arm, "\xEE\x84\x9E", alarm_ui_tr_c("arming mode", "Night"), alarm_ui_tr_c("arming mode", "Going to bed"), COLOR_VIOLET);
        lv_obj_add_event_cb(arm_tile_2, ui_on_arm, LV_EVENT_CLICKED, (void *)"night");

        lv_obj_t * panel_disarm = alarm_ui_run_0(pane, lv_pct(100), lv_pct(100));
        lv_obj_bind_flag_if_not_eq(panel_disarm, &subject_panel, LV_OBJ_FLAG_HIDDEN, 1);
        lv_obj_t * disarm_tile = action_tile_create(panel_disarm, "\xEE\x84\x8C", &subject_disarm_title, &subject_disarm_note, COLOR_GREEN);
        alarm_ui_run_31(disarm_tile, lv_pct(100), lv_pct(100));
        lv_obj_add_event_cb(disarm_tile, ui_on_disarm, LV_EVENT_CLICKED, NULL);

        lv_obj_t * pad = code_pad_create(pane, 1, 300);
        alarm_ui_run_31(pad, lv_pct(100), lv_pct(100));
        alarm_ui_run_43(pad, &subject_panel, 2, "pad");

        lv_obj_t * panel_memory = alarm_ui_run_0(pane, lv_pct(100), lv_pct(100));
        lv_obj_bind_flag_if_not_eq(panel_memory, &subject_panel, LV_OBJ_FLAG_HIDDEN, 3);
        lv_obj_t * memory_tile = action_tile_create(panel_memory, "\xEE\x88\xA4", &subject_disarm_title, &subject_memory_note, COLOR_AMBER);
        alarm_ui_run_31(memory_tile, lv_pct(100), lv_pct(100));
        alarm_ui_run_10(memory_tile, ui_on_clear_memory, "memory_tile");

        lv_obj_t * panel_walk = alarm_ui_run_0(pane, lv_pct(100), lv_pct(100));
        alarm_ui_run_1(panel_walk, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_72(panel_walk, SPACE_MD, &subject_panel, 4);
        lv_obj_t * walk_from = alarm_ui_run_0(panel_walk, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_9(walk_from, COLOR_BLUE, 0, (255 * 16 / 100), RADIUS_LG);
        lv_obj_set_style_pad_all(walk_from, SPACE_MD, 0);
        alarm_ui_run_1(walk_from, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(walk_from);
        lv_obj_set_style_pad_column(walk_from, SPACE_MD, 0);
        alarm_ui_run_42(walk_from, &subject_walk_from_on, 0, "walk_from");
        lv_obj_t * lv_label_25 = alarm_ui_run_23(walk_from, "\xEE\x83\xA8");
        alarm_ui_run_4(lv_label_25, icons_20, COLOR_BLUE);

        lv_obj_t * walk_from_text = alarm_ui_run_44(walk_from, LV_LABEL_LONG_MODE_WRAP);
        alarm_ui_run_4(walk_from_text, font_label, COLOR_TEXT);
        lv_label_bind_text(walk_from_text, &subject_walk_from, NULL);

        lv_obj_t * walk_card = alarm_ui_run_30(panel_walk);
        alarm_ui_run_9(walk_card, COLOR_SURFACE, 0, (255 * 100 / 100), RADIUS_XL);
        alarm_ui_run_40(walk_card, COLOR_LINE);
        lv_obj_set_style_pad_all(walk_card, SPACE_XL, 0);
        alarm_ui_run_1(walk_card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_flex_main_place(walk_card, LV_FLEX_ALIGN_CENTER, 0);
        alarm_ui_run_3(walk_card);
        lv_obj_set_style_pad_row(walk_card, SPACE_MD, 0);
        icon_chip_create(walk_card, "\xEE\x8E\xB9", 128, icons_96, COLOR_BLUE);

        lv_obj_t * walk_count = alarm_ui_run_2(walk_card, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_font(walk_count, font_headline, 0);
        lv_obj_set_style_text_align(walk_count, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_bind_text(walk_count, &subject_walk_count, NULL);

        lv_obj_t * walk_left = alarm_ui_run_2(walk_card, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(walk_left, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_bind_text(walk_left, &subject_walk_left, NULL);
        alarm_ui_run_52(walk_left, &style_text_2, 0, &style_text_warn, &subject_walk_warn, 1);

        lv_obj_t * walk_zones = row_button_create(panel_walk, "\xEE\x81\xAF", alarm_ui_tr("Show zones"), COLOR_BLUE);
        lv_obj_add_event_cb(walk_zones, ui_on_end_walk_test, LV_EVENT_CLICKED, (void *)"zones");
        lv_obj_set_name(walk_zones, "walk_zones");

        lv_obj_t * box_3 = alarm_ui_run_0(panel_walk, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(box_3, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(box_3, SPACE_MD, 0);
        lv_obj_t * walk_extend = row_button_create(box_3, "\xEE\x82\x87", alarm_ui_tr("Extend"), COLOR_BLUE);
        lv_obj_set_flex_grow(walk_extend, 1);
        lv_obj_add_event_cb(walk_extend, ui_on_end_walk_test, LV_EVENT_CLICKED, (void *)"extend");
        lv_obj_set_name(walk_extend, "walk_extend");

        lv_obj_t * walk_end = row_button_create(box_3, "\xEE\x81\xAC", alarm_ui_tr("End test"), COLOR_GREEN);
        lv_obj_set_flex_grow(walk_end, 1);
        lv_obj_add_event_cb(walk_end, ui_on_end_walk_test, LV_EVENT_CLICKED, (void *)"end");
        lv_obj_set_name(walk_end, "walk_end");

        lv_obj_t * panel_link = alarm_ui_run_0(pane, lv_pct(100), lv_pct(100));
        lv_obj_bind_flag_if_not_eq(panel_link, &subject_panel, LV_OBJ_FLAG_HIDDEN, 5);
        lv_obj_t * pair_tile = action_tile_create(panel_link, "\xEE\x83\xB5", &subject_disarm_title, &subject_disarm_note, COLOR_BLUE);
        alarm_ui_run_31(pair_tile, lv_pct(100), lv_pct(100));
        lv_obj_add_event_cb(pair_tile, ui_on_end_walk_test, LV_EVENT_CLICKED, (void *)"pair");

        lv_obj_t * view_zones = alarm_ui_run_0(content, lv_pct(100), lv_pct(100));
        alarm_ui_run_1(view_zones, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_72(view_zones, SPACE_LG, &subject_view, 1);
        lv_obj_t * box_4 = alarm_ui_run_0(view_zones, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(box_4, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(box_4);
        lv_obj_t * view_header_0 = view_header_create(box_4, alarm_ui_tr("Zones"), &subject_zones_note);
        lv_obj_set_flex_grow(view_header_0, 1);

        lv_obj_t * zone_filters = alarm_ui_run_0(box_4, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        alarm_ui_run_9(zone_filters, COLOR_SURFACE, 0, (255 * 100 / 100), RADIUS_LG);
        lv_obj_set_style_pad_all(zone_filters, 4, 0);
        alarm_ui_run_1(zone_filters, LV_FLEX_FLOW_ROW);
        alarm_ui_run_48(zone_filters, 4, "zone_filters");
        alarm_ui_run_28(zone_filters, alarm_ui_tr_c("zone filter", "All"), &subject_zone_filter, 0, &subject_zone_filter, 0);

        alarm_ui_run_28(zone_filters, alarm_ui_tr_c("zone filter", "Open"), &subject_zone_filter, 1, &subject_zone_filter, 1);

        alarm_ui_run_28(zone_filters, alarm_ui_tr_c("zone filter", "Bypassed"), &subject_zone_filter, 2, &subject_zone_filter, 2);

        alarm_ui_run_28(zone_filters, alarm_ui_tr_c("zone filter", "Trouble"), &subject_zone_filter, 3, &subject_zone_filter, 3);

        lv_obj_t * zone_list = alarm_ui_run_30(view_zones);
        alarm_ui_run_13(zone_list);
        alarm_ui_run_1(zone_list, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_row(zone_list, SPACE_LG, 0);
        lv_obj_set_style_pad_column(zone_list, SPACE_XL, 0);
        alarm_ui_run_42(zone_list, &subject_zones_empty, 1, "zone_list");

        lv_obj_t * zones_none = alarm_ui_run_30(view_zones);
        alarm_ui_run_1(zones_none, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_flex_main_place(zones_none, LV_FLEX_ALIGN_CENTER, 0);
        alarm_ui_run_3(zones_none);
        lv_obj_set_style_pad_row(zones_none, SPACE_MD, 0);
        alarm_ui_run_42(zones_none, &subject_zones_empty, 0, "zones_none");
        icon_chip_create(zones_none, "\xEE\x88\xA6", 112, icons_40, COLOR_GREEN);

        lv_obj_t * lv_label_26 = alarm_ui_run_14(zones_none, alarm_ui_tr("No open zones"));
        alarm_ui_run_74(lv_label_26, 1);

        lv_obj_t * lv_label_27 = alarm_ui_run_14(zones_none, alarm_ui_tr("No bypassed zones"));
        alarm_ui_run_74(lv_label_27, 2);

        lv_obj_t * lv_label_28 = alarm_ui_run_14(zones_none, alarm_ui_tr("No zone troubles"));
        alarm_ui_run_74(lv_label_28, 3);

        lv_obj_t * view_devices = alarm_ui_run_0(content, lv_pct(100), lv_pct(100));
        alarm_ui_run_1(view_devices, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_72(view_devices, SPACE_LG, &subject_view, 6);
        lv_obj_t * box_5 = alarm_ui_run_0(view_devices, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(box_5, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(box_5);
        lv_obj_set_style_pad_column(box_5, SPACE_MD, 0);
        lv_obj_t * device_header = view_header_create(box_5, alarm_ui_tr("Smart Home"), &subject_devices_note);
        lv_obj_set_flex_grow(device_header, 1);
        lv_obj_set_name(device_header, "device_header");

        lv_obj_t * device_arrange_header = view_header_create(box_5, "", &subject_devices_note);
        lv_obj_set_flex_grow(device_arrange_header, 1);
        lv_obj_set_flag(device_arrange_header, LV_OBJ_FLAG_HIDDEN, true);
        lv_obj_set_name(device_arrange_header, "device_arrange_header");

        lv_obj_t * device_areas = alarm_ui_run_0(box_5, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(device_areas, 560, 0);
        lv_obj_set_flag(device_areas, LV_OBJ_FLAG_SCROLLABLE, true);
        lv_obj_set_scrollbar_mode(device_areas, LV_SCROLLBAR_MODE_OFF);
        alarm_ui_run_9(device_areas, COLOR_SURFACE, 0, (255 * 100 / 100), RADIUS_LG);
        lv_obj_set_style_pad_all(device_areas, 4, 0);
        alarm_ui_run_1(device_areas, LV_FLEX_FLOW_ROW);
        alarm_ui_run_48(device_areas, 4, "device_areas");

        lv_obj_t * device_arrange = alarm_ui_run_0(box_5, 44, 44);
        lv_obj_set_flag(device_arrange, LV_OBJ_FLAG_CLICKABLE, true);
        alarm_ui_run_21(device_arrange, RADIUS_FULL, COLOR_RAISED, (255 * 100 / 100));
        lv_obj_set_name(device_arrange, "device_arrange");
        lv_obj_t * lv_label_29 = alarm_ui_run_11(device_arrange, "\xEE\x87\xB9");
        lv_obj_set_style_text_font(lv_label_29, icons_20, 0);

        lv_obj_t * device_arrange_add = alarm_ui_run_22(box_5, LV_SIZE_CONTENT, LV_OBJ_FLAG_HIDDEN);
        alarm_ui_run_32(device_arrange_add, &style_button);
        lv_obj_add_style(device_arrange_add, &style_button_pressed, LV_STATE_PRESSED);
        lv_obj_set_name(device_arrange_add, "device_arrange_add");
        lv_obj_t * lv_label_30 = alarm_ui_run_23(device_arrange_add, "\xEE\x84\xBD");
        lv_obj_set_style_text_font(lv_label_30, icons_20, 0);

        lv_obj_t * lv_label_31 = alarm_ui_run_14(device_arrange_add, alarm_ui_tr("Add"));
        lv_obj_set_style_text_font(lv_label_31, font_label, 0);

        lv_obj_t * device_arrange_prev = alarm_ui_run_0(box_5, 44, 44);
        lv_obj_set_flag(device_arrange_prev, LV_OBJ_FLAG_HIDDEN, true);
        lv_obj_set_flag(device_arrange_prev, LV_OBJ_FLAG_CLICKABLE, true);
        alarm_ui_run_21(device_arrange_prev, RADIUS_FULL, COLOR_RAISED, (255 * 100 / 100));
        lv_obj_set_name(device_arrange_prev, "device_arrange_prev");
        lv_obj_t * lv_label_32 = alarm_ui_run_11(device_arrange_prev, "\xEE\x81\x88");
        lv_obj_set_style_text_font(lv_label_32, icons_20, 0);

        lv_obj_t * device_arrange_next = alarm_ui_run_0(box_5, 44, 44);
        lv_obj_set_flag(device_arrange_next, LV_OBJ_FLAG_HIDDEN, true);
        lv_obj_set_flag(device_arrange_next, LV_OBJ_FLAG_CLICKABLE, true);
        alarm_ui_run_21(device_arrange_next, RADIUS_FULL, COLOR_RAISED, (255 * 100 / 100));
        lv_obj_set_name(device_arrange_next, "device_arrange_next");
        lv_obj_t * lv_label_33 = alarm_ui_run_11(device_arrange_next, "\xEE\x81\x89");
        lv_obj_set_style_text_font(lv_label_33, icons_20, 0);

        lv_obj_t * device_arrange_size = alarm_ui_run_0(box_5, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flag(device_arrange_size, LV_OBJ_FLAG_HIDDEN, true);
        alarm_ui_run_9(device_arrange_size, COLOR_SURFACE, 0, (255 * 100 / 100), RADIUS_LG);
        lv_obj_set_style_pad_all(device_arrange_size, 4, 0);
        alarm_ui_run_1(device_arrange_size, LV_FLEX_FLOW_ROW);
        alarm_ui_run_48(device_arrange_size, 4, "device_arrange_size");

        lv_obj_t * device_arrange_remove = alarm_ui_run_22(box_5, LV_SIZE_CONTENT, LV_OBJ_FLAG_HIDDEN);
        alarm_ui_run_32(device_arrange_remove, &style_button);
        lv_obj_add_style(device_arrange_remove, &style_button_pressed, LV_STATE_PRESSED);
        lv_obj_set_name(device_arrange_remove, "device_arrange_remove");
        lv_obj_t * lv_label_34 = alarm_ui_run_23(device_arrange_remove, "\xEE\x8A\xB0");
        lv_obj_set_style_text_font(lv_label_34, icons_20, 0);

        lv_obj_t * lv_label_35 = alarm_ui_run_14(device_arrange_remove, alarm_ui_tr("Remove"));
        lv_obj_set_style_text_font(lv_label_35, font_label, 0);

        lv_obj_t * device_arrange_done = alarm_ui_run_22(box_5, LV_SIZE_CONTENT, LV_OBJ_FLAG_HIDDEN);
        alarm_ui_run_32(device_arrange_done, &style_button);
        lv_obj_add_style(device_arrange_done, &style_button_primary, 0);
        lv_obj_set_name(device_arrange_done, "device_arrange_done");
        lv_obj_t * lv_label_36 = alarm_ui_run_14(device_arrange_done, alarm_ui_tr("Done"));
        lv_obj_set_style_text_font(lv_label_36, font_label, 0);

        lv_obj_t * device_grid = alarm_ui_run_30(view_devices);
        alarm_ui_run_13(device_grid);
        lv_obj_set_name(device_grid, "device_grid");

        lv_obj_t * view_activity = alarm_ui_run_0(content, lv_pct(100), lv_pct(100));
        alarm_ui_run_1(view_activity, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_72(view_activity, SPACE_LG, &subject_view, 2);
        view_header_create(view_activity, alarm_ui_tr("Activity"), &subject_events_note);

        lv_obj_t * event_filters = alarm_ui_run_0(view_activity, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        alarm_ui_run_9(event_filters, COLOR_SURFACE, 0, (255 * 100 / 100), RADIUS_LG);
        lv_obj_set_style_pad_all(event_filters, 4, 0);
        alarm_ui_run_1(event_filters, LV_FLEX_FLOW_ROW);
        alarm_ui_run_48(event_filters, 4, "event_filters");
        alarm_ui_run_28(event_filters, alarm_ui_tr_c("event log filter", "All"), &subject_event_filter, 0, &subject_event_filter, 0);

        alarm_ui_run_28(event_filters, alarm_ui_tr_c("event log filter", "Alarms"), &subject_event_filter, 1, &subject_event_filter, 1);

        alarm_ui_run_28(event_filters, alarm_ui_tr_c("event log filter", "Troubles"), &subject_event_filter, 2, &subject_event_filter, 2);

        alarm_ui_run_28(event_filters, alarm_ui_tr_c("event log filter", "Arming"), &subject_event_filter, 3, &subject_event_filter, 3);

        alarm_ui_run_28(event_filters, alarm_ui_tr_c("event log filter", "Bypass"), &subject_event_filter, 4, &subject_event_filter, 4);

        alarm_ui_run_28(event_filters, alarm_ui_tr_c("event log filter", "System"), &subject_event_filter, 5, &subject_event_filter, 5);

        lv_obj_t * event_list = alarm_ui_run_30(view_activity);
        alarm_ui_run_13(event_list);
        alarm_ui_run_1(event_list, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_49(event_list, SPACE_SM, "event_list");

        lv_obj_t * view_troubles = alarm_ui_run_0(content, lv_pct(100), lv_pct(100));
        alarm_ui_run_1(view_troubles, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(view_troubles, SPACE_LG, 0);
        alarm_ui_run_43(view_troubles, &subject_view, 3, "view_troubles");
        lv_obj_t * box_6 = alarm_ui_run_0(view_troubles, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(box_6, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(box_6);
        lv_obj_t * view_header_2 = view_header_create(box_6, alarm_ui_tr("Troubles"), &subject_troubles_note);
        lv_obj_set_flex_grow(view_header_2, 1);

        lv_obj_t * ack_button = alarm_ui_run_0(box_6, LV_SIZE_CONTENT, 52);
        lv_obj_set_flag(ack_button, LV_OBJ_FLAG_CLICKABLE, true);
        alarm_ui_run_9(ack_button, COLOR_RAISED, 0, (255 * 100 / 100), RADIUS_FULL);
        alarm_ui_run_26(ack_button, SPACE_XL, SPACE_XL);
        alarm_ui_run_1(ack_button, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(ack_button);
        lv_obj_set_style_pad_column(ack_button, SPACE_SM, 0);
        lv_obj_bind_flag_if_eq(ack_button, &subject_troubles_empty, LV_OBJ_FLAG_HIDDEN, 1);
        lv_obj_bind_style(ack_button, &style_disabled, 0, &subject_locked, 1);
        lv_obj_add_event_cb(ack_button, ui_on_ack_troubles, LV_EVENT_CLICKED, NULL);
        lv_obj_t * lv_label_37 = alarm_ui_run_23(ack_button, "\xEE\x81\xAC");
        lv_obj_set_style_text_font(lv_label_37, icons_20, 0);

        lv_obj_t * lv_label_38 = alarm_ui_run_14(ack_button, alarm_ui_tr("Acknowledge"));
        lv_obj_set_style_text_font(lv_label_38, font_label, 0);

        lv_obj_t * trouble_list = alarm_ui_run_30(view_troubles);
        alarm_ui_run_13(trouble_list);
        alarm_ui_run_1(trouble_list, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_49(trouble_list, SPACE_SM, "trouble_list");

        lv_obj_t * trouble_pager = alarm_ui_run_0(view_troubles, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(trouble_pager, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_main_place(trouble_pager, LV_FLEX_ALIGN_CENTER, 0);
        alarm_ui_run_3(trouble_pager);
        lv_obj_set_style_pad_column(trouble_pager, SPACE_LG, 0);
        lv_obj_bind_flag_if_eq(trouble_pager, &subject_troubles_pager, LV_OBJ_FLAG_HIDDEN, 0);
        lv_obj_t * troubles_back = alarm_ui_run_0(trouble_pager, 64, 52);
        alarm_ui_run_60(troubles_back, LV_OBJ_FLAG_CLICKABLE, true, 8);
        alarm_ui_run_9(troubles_back, COLOR_RAISED, 0, (255 * 100 / 100), RADIUS_FULL);
        lv_obj_add_event_cb(troubles_back, ui_on_troubles_page, LV_EVENT_CLICKED, (void *)"back");
        lv_obj_set_name(troubles_back, "troubles_back");
        lv_obj_t * lv_label_39 = alarm_ui_run_11(troubles_back, "\xEE\x81\xAE");
        lv_obj_set_style_text_font(lv_label_39, icons_20, 0);

        lv_obj_t * troubles_page_label = lv_label_create(trouble_pager);
        alarm_ui_run_4(troubles_page_label, font_label, COLOR_TEXT_2);
        lv_label_bind_text(troubles_page_label, &subject_troubles_page, NULL);

        lv_obj_t * troubles_more = alarm_ui_run_0(trouble_pager, 64, 52);
        alarm_ui_run_60(troubles_more, LV_OBJ_FLAG_CLICKABLE, true, 8);
        alarm_ui_run_9(troubles_more, COLOR_RAISED, 0, (255 * 100 / 100), RADIUS_FULL);
        lv_obj_add_event_cb(troubles_more, ui_on_troubles_page, LV_EVENT_CLICKED, (void *)"more");
        lv_obj_set_name(troubles_more, "troubles_more");
        lv_obj_t * lv_label_40 = alarm_ui_run_11(troubles_more, "\xEE\x81\xAF");
        lv_obj_set_style_text_font(lv_label_40, icons_20, 0);

        lv_obj_t * troubles_none = alarm_ui_run_30(view_troubles);
        alarm_ui_run_1(troubles_none, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_flex_main_place(troubles_none, LV_FLEX_ALIGN_CENTER, 0);
        alarm_ui_run_3(troubles_none);
        lv_obj_set_style_pad_row(troubles_none, SPACE_MD, 0);
        lv_obj_bind_flag_if_eq(troubles_none, &subject_troubles_empty, LV_OBJ_FLAG_HIDDEN, 0);
        icon_chip_create(troubles_none, "\xEE\x88\xA6", 112, icons_40, COLOR_GREEN);

        lv_obj_t * lv_label_41 = alarm_ui_run_14(troubles_none, alarm_ui_tr("No troubles"));
        lv_obj_set_style_text_font(lv_label_41, font_title, 0);

        lv_obj_t * lv_label_42 = alarm_ui_run_14(troubles_none, alarm_ui_tr("Power, network and sensors are all fine"));
        lv_obj_set_style_text_color(lv_label_42, COLOR_TEXT_2, 0);

        lv_obj_t * view_panic = alarm_ui_run_0(content, lv_pct(100), lv_pct(100));
        alarm_ui_run_1(view_panic, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_flex_main_place(view_panic, LV_FLEX_ALIGN_CENTER, 0);
        alarm_ui_run_3(view_panic);
        alarm_ui_run_72(view_panic, SPACE_XXL, &subject_view, 4);
        lv_obj_t * box_7 = alarm_ui_run_0(view_panic, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        alarm_ui_run_1(box_7, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_3(box_7);
        lv_obj_set_style_pad_row(box_7, 4, 0);
        lv_obj_t * lv_label_43 = alarm_ui_run_14(box_7, alarm_ui_tr("Emergency"));
        lv_obj_set_style_text_font(lv_label_43, font_headline, 0);

        lv_obj_t * lv_label_44 = alarm_ui_run_14(box_7, alarm_ui_tr("Hold a button for 2 seconds to set off the alarm on this panel"));
        lv_obj_set_style_text_color(lv_label_44, COLOR_TEXT_2, 0);
        lv_obj_bind_flag_if_eq(lv_label_44, &subject_link_role, LV_OBJ_FLAG_HIDDEN, 1);

        lv_obj_t * lv_label_45 = alarm_ui_run_55(box_7, 860);
        lv_obj_set_style_text_align(lv_label_45, LV_TEXT_ALIGN_CENTER, 0);
        alarm_ui_run_29(lv_label_45, COLOR_TEXT_2, &subject_panic_hint);
        lv_obj_bind_flag_if_eq(lv_label_45, &subject_link_role, LV_OBJ_FLAG_HIDDEN, 0);

        lv_obj_t * panic_report = lv_label_create(box_7);
        alarm_ui_run_4(panic_report, font_caption, COLOR_TEXT_3);
        lv_label_bind_text(panic_report, &subject_panic_report, NULL);
        lv_obj_bind_flag_if_eq(panic_report, &subject_panic_link, LV_OBJ_FLAG_HIDDEN, 1);

        lv_obj_t * lv_label_46 = alarm_ui_run_14(box_7, alarm_ui_tr("No system communication \xC2\xB7 This keypad keeps trying for 1 minute"));
        alarm_ui_run_4(lv_label_46, font_caption, COLOR_AMBER);
        lv_obj_bind_flag_if_eq(lv_label_46, &subject_panic_link, LV_OBJ_FLAG_HIDDEN, 0);

        lv_obj_t * box_8 = alarm_ui_run_0(view_panic, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        alarm_ui_run_1(box_8, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(box_8, 56, 0);
        lv_obj_t * panic_police = panic_button_create(box_8, "\xEE\x8B\xAF", alarm_ui_tr("Police"), COLOR_RED);
        lv_obj_add_event_cb(panic_police, ui_on_panic, LV_EVENT_ALL, (void *)"police");
        lv_obj_set_name(panic_police, "panic_police");

        lv_obj_t * panic_fire = panic_button_create(box_8, "\xEE\x83\x92", alarm_ui_tr("Fire"), COLOR_ORANGE);
        lv_obj_add_event_cb(panic_fire, ui_on_panic, LV_EVENT_ALL, (void *)"fire");

        lv_obj_t * panic_medical = panic_button_create(box_8, "\xEE\x8D\xAE", alarm_ui_tr("Medical"), COLOR_ROSE);
        lv_obj_add_event_cb(panic_medical, ui_on_panic, LV_EVENT_ALL, (void *)"medical");

        lv_obj_t * view_settings = alarm_ui_run_0(content, lv_pct(100), lv_pct(100));
        alarm_ui_run_1(view_settings, LV_FLEX_FLOW_ROW);
        alarm_ui_run_39(view_settings, SPACE_XL, &subject_view, 5);
        lv_obj_t * settings_nav = alarm_ui_run_0(view_settings, 300, lv_pct(100));
        alarm_ui_run_13(settings_nav);
        alarm_ui_run_1(settings_nav, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(settings_nav, SPACE_XS, 0);
        lv_obj_t * set_display = alarm_ui_run_41(settings_nav, "\xEE\x85\xB8", alarm_ui_tr("Screen and sound"), 0);
        lv_obj_add_event_cb(set_display, ui_on_settings_section, LV_EVENT_CLICKED, (void *)"display");
        lv_obj_set_name(set_display, "set_display");

        lv_obj_t * set_users = alarm_ui_run_41(settings_nav, "\xEE\x86\xA4", alarm_ui_tr("Users"), 1);
        lv_obj_add_event_cb(set_users, ui_on_settings_section, LV_EVENT_CLICKED, (void *)"users");
        lv_obj_set_name(set_users, "set_users");

        lv_obj_t * set_wifi = alarm_ui_run_41(settings_nav, "\xEE\x86\xAE", alarm_ui_tr("Wi-Fi and network"), 2);
        lv_obj_add_event_cb(set_wifi, ui_on_settings_section, LV_EVENT_CLICKED, (void *)"wifi");
        lv_obj_set_name(set_wifi, "set_wifi");

        lv_obj_t * set_time = alarm_ui_run_41(settings_nav, "\xEE\x8C\x84", alarm_ui_tr("Time and language"), 3);
        lv_obj_add_event_cb(set_time, ui_on_settings_section, LV_EVENT_CLICKED, (void *)"time");
        lv_obj_set_name(set_time, "set_time");

        lv_obj_t * set_integrations = alarm_ui_run_41(settings_nav, "\xEE\x8A\x9C", alarm_ui_tr("Integrations"), 4);
        lv_obj_add_event_cb(set_integrations, ui_on_settings_section, LV_EVENT_CLICKED, (void *)"integrations");
        lv_obj_set_name(set_integrations, "set_integrations");

        lv_obj_t * set_automations = settings_item_create(settings_nav, "\xEE\x86\xB4", alarm_ui_tr("Automations"));
        lv_obj_bind_flag_if_not_eq(set_automations, &subject_automations_avail, LV_OBJ_FLAG_HIDDEN, 1);
        lv_obj_bind_style(set_automations, &style_nav_active, 0, &subject_settings_section, 9);
        lv_obj_add_event_cb(set_automations, ui_on_settings_section, LV_EVENT_CLICKED, (void *)"automations");
        lv_obj_set_name(set_automations, "set_automations");

        lv_obj_t * set_walk = alarm_ui_run_41(settings_nav, "\xEE\x8E\xB9", alarm_ui_tr("Walk test"), 8);
        lv_obj_add_event_cb(set_walk, ui_on_settings_section, LV_EVENT_CLICKED, (void *)"walk");
        lv_obj_set_name(set_walk, "set_walk");

        lv_obj_t * set_about = alarm_ui_run_41(settings_nav, "\xEE\x83\xB9", alarm_ui_tr("About"), 7);
        lv_obj_add_event_cb(set_about, ui_on_settings_section, LV_EVENT_CLICKED, (void *)"about");
        lv_obj_set_name(set_about, "set_about");
        lv_obj_t * set_about_tag = alarm_ui_run_0(set_about, LV_SIZE_CONTENT, 28);
        alarm_ui_run_21(set_about_tag, RADIUS_FULL, COLOR_VIOLET, (255 * 16 / 100));
        alarm_ui_run_26(set_about_tag, SPACE_SM, SPACE_SM);
        alarm_ui_run_43(set_about_tag, &subject_update_waiting, 1, "set_about_tag");
        lv_obj_t * lv_label_47 = alarm_ui_run_33(set_about_tag);
        lv_label_set_text(lv_label_47, alarm_ui_tr_c("settings list, an update waits", "Update"));
        alarm_ui_run_4(lv_label_47, font_caption, COLOR_VIOLET);

        lv_obj_t * set_about_tag_running = alarm_ui_run_0(set_about, LV_SIZE_CONTENT, 28);
        alarm_ui_run_21(set_about_tag_running, RADIUS_FULL, COLOR_BLUE, (255 * 16 / 100));
        alarm_ui_run_26(set_about_tag_running, SPACE_SM, SPACE_SM);
        alarm_ui_run_43(set_about_tag_running, &subject_update_waiting, 2, "set_about_tag_running");
        lv_obj_t * lv_label_48 = alarm_ui_run_33(set_about_tag_running);
        lv_label_set_text(lv_label_48, alarm_ui_tr_c("settings list, an update runs", "Updating"));
        alarm_ui_run_4(lv_label_48, font_caption, COLOR_BLUE);

        lv_obj_t * set_about_tag_back = alarm_ui_run_0(set_about, LV_SIZE_CONTENT, 28);
        alarm_ui_run_21(set_about_tag_back, RADIUS_FULL, COLOR_AMBER, (255 * 16 / 100));
        alarm_ui_run_26(set_about_tag_back, SPACE_SM, SPACE_SM);
        alarm_ui_run_43(set_about_tag_back, &subject_update_waiting, 3, "set_about_tag_back");
        lv_obj_t * lv_label_49 = alarm_ui_run_33(set_about_tag_back);
        lv_label_set_text(lv_label_49, alarm_ui_tr_c("settings list, an update was undone", "Rolled back"));
        alarm_ui_run_4(lv_label_49, font_caption, COLOR_AMBER);

        lv_obj_t * settings_page = box_create(view_settings);
        lv_obj_set_flex_grow(settings_page, 1);
        lv_obj_set_height(settings_page, lv_pct(100));
        alarm_ui_run_1(settings_page, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_49(settings_page, SPACE_MD, "settings_page");
        lv_obj_t * page_head = alarm_ui_run_0(settings_page, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(page_head, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(page_head);
        lv_obj_set_style_pad_column(page_head, SPACE_MD, 0);
        lv_obj_bind_flag_if_eq(page_head, &subject_settings_pad, LV_OBJ_FLAG_HIDDEN, 1);
        lv_obj_t * page_back = alarm_ui_run_0(page_head, 44, 44);
        alarm_ui_run_60(page_back, LV_OBJ_FLAG_CLICKABLE, true, 10);
        alarm_ui_run_21(page_back, RADIUS_FULL, COLOR_RAISED, (255 * 100 / 100));
        lv_obj_bind_flag_if_eq(page_back, &subject_page_back, LV_OBJ_FLAG_HIDDEN, 0);
        alarm_ui_run_10(page_back, ui_on_page_back, "page_back");
        lv_obj_t * lv_label_50 = alarm_ui_run_11(page_back, "\xEE\x81\xAE");
        lv_obj_set_style_text_font(lv_label_50, icons_20, 0);

        lv_obj_t * box_9 = alarm_ui_run_8(page_head);
        alarm_ui_run_1(box_9, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box_9, 2, 0);
        lv_obj_t * lv_label_51 = alarm_ui_run_2(box_9, LV_LABEL_LONG_MODE_DOTS);
        alarm_ui_run_12(lv_label_51, font_title, &subject_page_title);

        lv_obj_t * page_note = alarm_ui_run_2(box_9, LV_LABEL_LONG_MODE_WRAP);
        alarm_ui_run_29(page_note, COLOR_TEXT_2, &subject_page_note);
        lv_obj_set_name(page_note, "page_note");

        lv_obj_t * page_spinner = lv_spinner_create(page_head);
        alarm_ui_run_31(page_spinner, 32, 32);
        alarm_ui_run_50(page_spinner);

        lv_obj_t * page_body = alarm_ui_run_30(settings_page);
        alarm_ui_run_13(page_body);
        alarm_ui_run_1(page_body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(page_body, SPACE_SM, 0);
        alarm_ui_run_42(page_body, &subject_settings_pad, 1, "page_body");

        lv_obj_t * settings_pad_area = alarm_ui_run_0(settings_page, lv_pct(100), lv_pct(100));
        alarm_ui_run_1(settings_pad_area, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_3(settings_pad_area);
        lv_obj_bind_flag_if_eq(settings_pad_area, &subject_settings_pad, LV_OBJ_FLAG_HIDDEN, 0);
        lv_obj_t * settings_pad = code_pad_create(settings_pad_area, 1, 300);
        alarm_ui_run_31(settings_pad, 428, lv_pct(100));
        lv_obj_set_name(settings_pad, "settings_pad");

        lv_obj_t * text_sheet = text_sheet_create(lv_obj_0);
        lv_obj_bind_flag_if_not_eq(text_sheet, &subject_sheet, LV_OBJ_FLAG_HIDDEN, 1);

        lv_obj_t * pick_sheet = pick_sheet_create(lv_obj_0);
        lv_obj_bind_flag_if_not_eq(pick_sheet, &subject_sheet, LV_OBJ_FLAG_HIDDEN, 2);

        lv_obj_t * confirm_dialog = confirm_dialog_create(lv_obj_0);
        lv_obj_bind_flag_if_not_eq(confirm_dialog, &subject_sheet, LV_OBJ_FLAG_HIDDEN, 3);

        lv_obj_t * clock_sheet = clock_sheet_create(lv_obj_0);
        alarm_ui_run_43(clock_sheet, &subject_sheet, 4, "clock_sheet");

        lv_obj_t * night_sheet = night_sheet_create(lv_obj_0);
        alarm_ui_run_43(night_sheet, &subject_sheet, 5, "night_sheet");

        lv_obj_t * device_sheet = device_sheet_create(lv_obj_0);
        alarm_ui_run_43(device_sheet, &subject_sheet, 6, "device_sheet");

        lv_obj_t * time_sheet = time_sheet_create(lv_obj_0);
        alarm_ui_run_43(time_sheet, &subject_sheet, 7, "time_sheet");

        lv_obj_t * window_sheet = window_sheet_create(lv_obj_0);
        alarm_ui_run_43(window_sheet, &subject_sheet, 8, "window_sheet");

        lv_obj_t * poff_sheet = alarm_ui_run_0(lv_obj_0, lv_pct(100), lv_pct(100));
        lv_obj_set_flag(poff_sheet, LV_OBJ_FLAG_CLICKABLE, true);
        lv_obj_bind_flag_if_not_eq(poff_sheet, &subject_sheet, LV_OBJ_FLAG_HIDDEN, 9);
        lv_obj_add_style(poff_sheet, &style_backdrop, 0);
        lv_obj_t * poff_card = alarm_ui_run_0(poff_sheet, 600, LV_SIZE_CONTENT);
        alarm_ui_run_34(poff_card);
        lv_obj_t * box_10 = alarm_ui_run_0(poff_card, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(box_10, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box_10, 2, 0);
        lv_obj_t * lv_label_52 = alarm_ui_run_2(box_10, LV_LABEL_LONG_MODE_WRAP);
        alarm_ui_run_12(lv_label_52, font_title, &subject_sheet_title);

        lv_obj_t * lv_label_53 = alarm_ui_run_2(box_10, LV_LABEL_LONG_MODE_WRAP);
        alarm_ui_run_29(lv_label_53, COLOR_TEXT_2, &subject_sheet_note);

        lv_obj_t * poff_hour = alarm_ui_run_0(poff_card, lv_pct(100), 96);
        lv_obj_set_flag(poff_hour, LV_OBJ_FLAG_CLICKABLE, true);
        lv_obj_set_style_bg_color(poff_hour, COLOR_SURFACE, 0);
        alarm_ui_run_9(poff_hour, COLOR_RAISED, LV_STATE_PRESSED, (255 * 100 / 100), RADIUS_LG);
        alarm_ui_run_40(poff_hour, COLOR_LINE);
        alarm_ui_run_26(poff_hour, SPACE_XL, SPACE_XL);
        alarm_ui_run_1(poff_hour, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(poff_hour);
        alarm_ui_run_48(poff_hour, SPACE_XL, "poff_hour");
        icon_chip_create(poff_hour, "\xEE\x82\x87", 64, icons_28, COLOR_BLUE);

        lv_obj_t * box_11 = alarm_ui_run_8(poff_hour);
        alarm_ui_run_1(box_11, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box_11, 4, 0);
        lv_obj_t * lv_label_54 = alarm_ui_run_2(box_11, LV_LABEL_LONG_MODE_WRAP);
        alarm_ui_run_73(lv_label_54, alarm_ui_tr("An hour later"));

        lv_obj_t * lv_label_55 = alarm_ui_run_2(box_11, LV_LABEL_LONG_MODE_WRAP);
        alarm_ui_run_29(lv_label_55, COLOR_TEXT_2, &subject_poff_hour);

        lv_obj_t * lv_label_56 = alarm_ui_run_23(poff_hour, "\xEE\x81\xAF");
        alarm_ui_run_4(lv_label_56, icons_28, COLOR_TEXT_3);

        lv_obj_t * poff_night = alarm_ui_run_0(poff_card, lv_pct(100), 96);
        lv_obj_set_flag(poff_night, LV_OBJ_FLAG_CLICKABLE, true);
        lv_obj_set_style_bg_color(poff_night, COLOR_SURFACE, 0);
        alarm_ui_run_9(poff_night, COLOR_RAISED, LV_STATE_PRESSED, (255 * 100 / 100), RADIUS_LG);
        alarm_ui_run_40(poff_night, COLOR_LINE);
        alarm_ui_run_26(poff_night, SPACE_XL, SPACE_XL);
        alarm_ui_run_1(poff_night, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(poff_night);
        alarm_ui_run_48(poff_night, SPACE_XL, "poff_night");
        icon_chip_create(poff_night, "\xEE\x8C\x84", 64, icons_28, COLOR_BLUE);

        lv_obj_t * box_12 = alarm_ui_run_8(poff_night);
        alarm_ui_run_1(box_12, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box_12, 4, 0);
        lv_obj_t * lv_label_57 = alarm_ui_run_2(box_12, LV_LABEL_LONG_MODE_WRAP);
        alarm_ui_run_12(lv_label_57, font_title, &subject_poff_night_title);

        lv_obj_t * lv_label_58 = alarm_ui_run_2(box_12, LV_LABEL_LONG_MODE_WRAP);
        alarm_ui_run_29(lv_label_58, COLOR_TEXT_2, &subject_poff_night);

        lv_obj_t * lv_label_59 = alarm_ui_run_23(poff_night, "\xEE\x81\xAF");
        alarm_ui_run_4(lv_label_59, icons_28, COLOR_TEXT_3);

        lv_obj_t * box_13 = alarm_ui_run_0(poff_card, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(box_13, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_main_place(box_13, LV_FLEX_ALIGN_END, 0);
        lv_obj_t * poff_cancel = alarm_ui_run_22(box_13, LV_SIZE_CONTENT, LV_OBJ_FLAG_CLICKABLE);
        alarm_ui_run_54(poff_cancel, &style_button, 0, &style_button_pressed, LV_STATE_PRESSED);
        alarm_ui_run_10(poff_cancel, ui_on_sheet_cancel, "poff_cancel");
        alarm_ui_run_62(poff_cancel, &subject_sheet_cancel);

        lv_obj_t * form_sheet = form_sheet_create(lv_obj_0);
        alarm_ui_run_43(form_sheet, &subject_sheet, 10, "form_sheet");

        lv_obj_t * modal = modal_pad_create(lv_obj_0);
        alarm_ui_run_42(modal, &subject_modal_pad, 0, "modal");

        lv_obj_t * toast = alarm_ui_run_0(lv_obj_0, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(toast, 52, 0);
        alarm_ui_run_56(toast, LV_ALIGN_BOTTOM_MID, TOAST_X, TOAST_Y);
        alarm_ui_run_9(toast, COLOR_PRESSED, 0, (255 * 100 / 100), RADIUS_FULL);
        alarm_ui_run_40(toast, COLOR_TOAST_LINE);
        alarm_ui_run_26(toast, SPACE_XL, SPACE_XL);
        alarm_ui_run_70(toast, 12, 12);
        alarm_ui_run_1(toast, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(toast);
        alarm_ui_run_42(toast, &subject_toast_visible, 0, "toast");
        lv_obj_t * toast_text = alarm_ui_run_55(toast, 800);
        lv_obj_set_style_text_align(toast_text, LV_TEXT_ALIGN_CENTER, 0);
        alarm_ui_run_12(toast_text, font_label, &subject_toast);

        the_root = lv_obj_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
