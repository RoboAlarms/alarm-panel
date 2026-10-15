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

        alarm_ui_run_49(lv_obj_0, &style_screen, 0, &style_screen_alarm, &subject_alarm_active, 1);
        lv_obj_t * rail = alarm_ui_run_0(lv_obj_0, RAIL_W, lv_pct(100));
        alarm_ui_run_36(rail, COLOR_RAIL, (255 * 100 / 100));
        lv_obj_set_style_border_side(rail, LV_BORDER_SIDE_RIGHT, 0);
        alarm_ui_run_39(rail, COLOR_LINE);
        alarm_ui_run_1(rail, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_3(rail);
        lv_obj_set_style_pad_top(rail, SPACE_SM, 0);
        lv_obj_set_style_pad_bottom(rail, SPACE_SM, 0);
        lv_obj_set_style_pad_row(rail, 3, 0);
        lv_obj_set_name(rail, "rail");
        lv_obj_t * rail_logo = alarm_ui_run_0(rail, 44, 44);
        lv_obj_set_name(rail_logo, "rail_logo");
        lv_obj_t * rail_brand = lv_image_create(rail_logo);
        lv_obj_set_align(rail_brand, LV_ALIGN_CENTER);
        alarm_ui_run_22(rail_brand, 44, 44);
        lv_obj_set_style_image_recolor_opa(rail_brand, (255 * 100 / 100), 0);
        lv_obj_bind_style_prop(rail_brand, LV_STYLE_IMAGE_RECOLOR, 0, &subject_tone_color);
        lv_obj_set_name(rail_brand, "rail_brand");

        lv_obj_t * nav_home = nav_item_create(rail, "\xEE\x85\x98", alarm_ui_tr("Home"));
        alarm_ui_run_26(nav_home, 0, 0, "nav_home");

        lv_obj_t * nav_zones = nav_item_create(rail, "\xEE\x83\xBF", alarm_ui_tr("Zones"));
        alarm_ui_run_26(nav_zones, 1, 1, "nav_zones");
        badge_create(nav_zones, &subject_open_count, COLOR_AMBER);

        lv_obj_t * nav_activity = nav_item_create(rail, "\xEE\x87\xB5", alarm_ui_tr("Activity"));
        alarm_ui_run_26(nav_activity, 2, 2, "nav_activity");

        lv_obj_t * nav_troubles = nav_item_create(rail, "\xEE\x86\x93", alarm_ui_tr("Troubles"));
        lv_obj_bind_style(nav_troubles, &style_nav_active, 0, &subject_view, 3);
        lv_obj_add_subject_set_int_event(nav_troubles, &subject_view, LV_EVENT_CLICKED, 3);
        badge_create(nav_troubles, &subject_trouble_count, COLOR_AMBER);

        alarm_ui_run_33(rail, 8);

        lv_obj_t * nav_panic = nav_item_create(rail, "\xEE\x8B\xAF", alarm_ui_tr("Panic"));
        lv_obj_set_style_text_color(nav_panic, COLOR_RED, 0);
        alarm_ui_run_26(nav_panic, 4, 4, "nav_panic");

        lv_obj_t * bar = box_create(lv_obj_0);
        lv_obj_set_x(bar, RAIL_W);
        alarm_ui_run_22(bar, CONTENT_W, BAR_H);
        alarm_ui_run_30(bar, SPACE_LG, SPACE_LG);
        alarm_ui_run_1(bar, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(bar);
        lv_obj_set_style_pad_column(bar, 6, 0);
        lv_obj_set_name(bar, "bar");
        lv_obj_t * bar_left = alarm_ui_run_9(bar, lv_pct(100));
        alarm_ui_run_1(bar_left, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(bar_left);
        lv_obj_t * bar_quiet = alarm_ui_run_9(bar_left, lv_pct(100));
        alarm_ui_run_1(bar_quiet, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(bar_quiet);
        lv_obj_bind_flag_if_eq(bar_quiet, &subject_panel, LV_OBJ_FLAG_HIDDEN, 2);
        lv_obj_t * bar_date = alarm_ui_run_2(bar_quiet, LV_LABEL_LONG_MODE_DOTS);
        alarm_ui_run_4(bar_date, font_caption, COLOR_TEXT_2);
        lv_label_bind_text(bar_date, &subject_date, NULL);
        lv_obj_bind_flag_if_not_eq(bar_date, &subject_bar_banner, LV_OBJ_FLAG_HIDDEN, 0);

        lv_obj_t * program_banner = alarm_ui_run_0(bar_quiet, LV_SIZE_CONTENT, 30);
        alarm_ui_run_20(program_banner, LV_OBJ_FLAG_CLICKABLE, 4);
        alarm_ui_run_10(program_banner, COLOR_BLUE, (255 * 16 / 100), RADIUS_FULL);
        alarm_ui_run_30(program_banner, SPACE_LG, SPACE_LG);
        alarm_ui_run_1(program_banner, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(program_banner);
        alarm_ui_run_44(program_banner, SPACE_SM, &subject_bar_banner, 2);
        alarm_ui_run_13(program_banner, ui_on_program_exit, "program_banner");
        lv_obj_t * lv_label_0 = alarm_ui_run_47(program_banner, "\xEE\x86\xB1");
        alarm_ui_run_4(lv_label_0, icons_20, COLOR_BLUE);

        lv_obj_t * lv_label_1 = alarm_ui_run_31(program_banner, alarm_ui_tr("Leave installer"));
        alarm_ui_run_4(lv_label_1, font_caption, COLOR_BLUE);

        lv_obj_t * codes_banner = alarm_ui_run_0(bar_quiet, LV_SIZE_CONTENT, 30);
        alarm_ui_run_20(codes_banner, LV_OBJ_FLAG_CLICKABLE, 4);
        alarm_ui_run_10(codes_banner, COLOR_AMBER, (255 * 16 / 100), RADIUS_FULL);
        alarm_ui_run_30(codes_banner, SPACE_LG, SPACE_LG);
        alarm_ui_run_1(codes_banner, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(codes_banner);
        alarm_ui_run_44(codes_banner, SPACE_SM, &subject_bar_banner, 1);
        alarm_ui_run_13(codes_banner, ui_on_change_code, "codes_banner");
        lv_obj_t * lv_label_2 = alarm_ui_run_47(codes_banner, "\xEE\x92\xA3");
        alarm_ui_run_4(lv_label_2, icons_20, COLOR_AMBER);

        lv_obj_t * lv_label_3 = alarm_ui_run_31(codes_banner, alarm_ui_tr("Change codes"));
        alarm_ui_run_4(lv_label_3, font_caption, COLOR_AMBER);

        lv_obj_t * walk_banner = alarm_ui_run_0(bar_quiet, LV_SIZE_CONTENT, 30);
        alarm_ui_run_20(walk_banner, LV_OBJ_FLAG_CLICKABLE, 4);
        alarm_ui_run_10(walk_banner, COLOR_BLUE, (255 * 16 / 100), RADIUS_FULL);
        alarm_ui_run_30(walk_banner, SPACE_LG, SPACE_LG);
        alarm_ui_run_1(walk_banner, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(walk_banner);
        alarm_ui_run_44(walk_banner, SPACE_SM, &subject_bar_banner, 4);
        lv_obj_add_event_cb(walk_banner, ui_on_end_walk_test, LV_EVENT_CLICKED, (void *)"banner");
        lv_obj_set_name(walk_banner, "walk_banner");
        lv_obj_t * lv_label_4 = alarm_ui_run_47(walk_banner, "\xEE\x8E\xB9");
        alarm_ui_run_4(lv_label_4, icons_20, COLOR_BLUE);

        lv_obj_t * lv_label_5 = alarm_ui_run_41(walk_banner, LV_LABEL_LONG_MODE_DOTS, 190);
        alarm_ui_run_4(lv_label_5, font_caption, COLOR_BLUE);
        lv_label_bind_text(lv_label_5, &subject_walk_banner, NULL);

        lv_obj_t * remote_banner = alarm_ui_run_0(bar_quiet, LV_SIZE_CONTENT, 30);
        alarm_ui_run_20(remote_banner, LV_OBJ_FLAG_CLICKABLE, 4);
        alarm_ui_run_10(remote_banner, COLOR_RED, (255 * 100 / 100), RADIUS_FULL);
        alarm_ui_run_30(remote_banner, SPACE_LG, SPACE_LG);
        alarm_ui_run_1(remote_banner, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(remote_banner);
        alarm_ui_run_44(remote_banner, SPACE_SM, &subject_bar_banner, 3);
        alarm_ui_run_13(remote_banner, ui_on_remote_banner, "remote_banner");
        lv_obj_t * lv_label_6 = alarm_ui_run_47(remote_banner, "\xEE\x83\xA8");
        alarm_ui_run_4(lv_label_6, icons_20, COLOR_ON_TONE);

        lv_obj_t * lv_label_7 = alarm_ui_run_41(remote_banner, LV_LABEL_LONG_MODE_DOTS, 190);
        alarm_ui_run_4(lv_label_7, font_caption, COLOR_ON_TONE);
        lv_label_bind_text(lv_label_7, &subject_remote_banner, NULL);

        lv_obj_t * bar_state = alarm_ui_run_9(bar_left, lv_pct(100));
        alarm_ui_run_1(bar_state, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(bar_state);
        alarm_ui_run_44(bar_state, 6, &subject_panel, 2);
        lv_obj_t * bar_state_icon = lv_label_create(bar_state);
        alarm_ui_run_8(bar_state_icon, icons_20, &subject_hero_icon);
        lv_obj_bind_style_prop(bar_state_icon, LV_STYLE_TEXT_COLOR, 0, &subject_tone_color);

        lv_obj_t * bar_state_text = lv_label_create(bar_state);
        lv_obj_set_flex_grow(bar_state_text, 1);
        lv_label_set_long_mode(bar_state_text, LV_LABEL_LONG_MODE_DOTS);
        alarm_ui_run_8(bar_state_text, font_label, &subject_headline);
        lv_obj_bind_style_prop(bar_state_text, LV_STYLE_TEXT_COLOR, 0, &subject_tone_color);

        lv_obj_t * bar_state_count = lv_label_create(bar_state);
        lv_obj_set_style_text_font(bar_state_count, font_label, 0);
        lv_label_bind_text(bar_state_count, &subject_countdown, "%d s");
        lv_obj_bind_style_prop(bar_state_count, LV_STYLE_TEXT_COLOR, 0, &subject_tone_color);
        lv_obj_bind_flag_if_eq(bar_state_count, &subject_countdown, LV_OBJ_FLAG_HIDDEN, 0);

        lv_obj_t * nav_chime = alarm_ui_run_0(bar, 36, 36);
        alarm_ui_run_20(nav_chime, LV_OBJ_FLAG_CLICKABLE, 6);
        lv_obj_set_style_radius(nav_chime, RADIUS_FULL, 0);
        alarm_ui_run_37(nav_chime, &style_text_2, &style_nav_item_pressed, LV_STATE_PRESSED);
        lv_obj_bind_style(nav_chime, &style_nav_on, 0, &subject_chime, 1);
        lv_obj_bind_style(nav_chime, &style_disabled, 0, &subject_locked, 1);
        lv_obj_add_event_cb(nav_chime, ui_on_chime, LV_EVENT_CLICKED, NULL);
        lv_obj_t * lv_label_8 = alarm_ui_run_15(nav_chime);
        alarm_ui_run_8(lv_label_8, icons_20, &subject_chime_icon);

        lv_obj_t * bar_trouble = alarm_ui_run_0(bar, LV_SIZE_CONTENT, 36);
        alarm_ui_run_20(bar_trouble, LV_OBJ_FLAG_CLICKABLE, 6);
        alarm_ui_run_1(bar_trouble, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(bar_trouble);
        lv_obj_set_style_pad_column(bar_trouble, 4, 0);
        lv_obj_set_style_text_color(bar_trouble, COLOR_AMBER, 0);
        lv_obj_bind_flag_if_eq(bar_trouble, &subject_trouble_count, LV_OBJ_FLAG_HIDDEN, 0);
        lv_obj_add_subject_set_int_event(bar_trouble, &subject_view, LV_EVENT_CLICKED, 3);
        lv_obj_set_name(bar_trouble, "bar_trouble");
        lv_obj_t * lv_label_9 = lv_label_create(bar_trouble);
        alarm_ui_run_18(lv_label_9, "\xEE\x86\x93");

        lv_obj_t * lv_label_10 = lv_label_create(bar_trouble);
        lv_obj_set_style_text_font(lv_label_10, font_label, 0);
        lv_label_bind_text(lv_label_10, &subject_trouble_count, "%d");

        lv_obj_t * bar_wifi = lv_label_create(bar);
        alarm_ui_run_8(bar_wifi, icons_20, &subject_wifi_icon);
        alarm_ui_run_49(bar_wifi, &style_text_2, 0, &style_text_warn, &subject_wifi_ok, 0);

        lv_obj_t * bar_battery = alarm_ui_run_0(bar, LV_SIZE_CONTENT, 24);
        alarm_ui_run_1(bar_battery, LV_FLEX_FLOW_ROW);
        alarm_ui_run_46(bar_battery, LV_FLEX_ALIGN_CENTER, 2);
        alarm_ui_run_49(bar_battery, &style_text_2, 0, &style_text_warn, &subject_battery_warn, 1);
        lv_obj_bind_style(bar_battery, &style_text_error, 0, &subject_battery_critical, 1);
        lv_obj_t * box_0 = alarm_ui_run_0(bar_battery, 22, 22);
        lv_obj_t * bar_battery_icon = alarm_ui_run_15(box_0);
        alarm_ui_run_8(bar_battery_icon, icons_20, &subject_battery_icon);
        lv_obj_set_name(bar_battery_icon, "bar_battery_icon");

        lv_obj_t * bar_battery_strike = box_create(box_0);
        lv_obj_set_x(bar_battery_strike, 2);
        lv_obj_set_y(bar_battery_strike, 3);
        alarm_ui_run_22(bar_battery_strike, 24, 2);
        alarm_ui_run_36(bar_battery_strike, COLOR_RED, 255);
        lv_obj_set_style_transform_rotation(bar_battery_strike, 450, 0);
        lv_obj_set_style_transform_pivot_x(bar_battery_strike, 0, 0);
        lv_obj_set_style_transform_pivot_y(bar_battery_strike, 0, 0);
        alarm_ui_run_29(bar_battery_strike, &subject_battery_critical, 0, "bar_battery_strike");

        lv_obj_t * bar_battery_text = lv_label_create(bar_battery);
        alarm_ui_run_8(bar_battery_text, font_caption, &subject_battery_text);
        lv_obj_set_name(bar_battery_text, "bar_battery_text");

        lv_obj_t * bar_clock = alarm_ui_run_0(bar, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        alarm_ui_run_1(bar_clock, LV_FLEX_FLOW_ROW);
        alarm_ui_run_46(bar_clock, LV_FLEX_ALIGN_END, 3);
        lv_obj_t * bar_time = lv_label_create(bar_clock);
        alarm_ui_run_8(bar_time, font_title, &subject_time);

        lv_obj_t * bar_ampm = lv_label_create(bar_clock);
        alarm_ui_run_4(bar_ampm, font_caption, COLOR_TEXT_2);
        lv_obj_set_style_pad_bottom(bar_ampm, 2, 0);
        lv_label_bind_text(bar_ampm, &subject_ampm, NULL);

        lv_obj_t * bar_settings = alarm_ui_run_0(bar, 36, 36);
        alarm_ui_run_20(bar_settings, LV_OBJ_FLAG_CLICKABLE, 6);
        lv_obj_set_style_radius(bar_settings, RADIUS_FULL, 0);
        lv_obj_bind_style(bar_settings, &style_nav_active, 0, &subject_view, 5);
        lv_obj_bind_style(bar_settings, &style_disabled, 0, &subject_locked, 1);
        alarm_ui_run_13(bar_settings, ui_on_settings, "bar_settings");
        lv_obj_t * lv_label_11 = alarm_ui_run_15(bar_settings);
        lv_label_set_text(lv_label_11, "\xEE\x85\x94");
        alarm_ui_run_4(lv_label_11, icons_20, COLOR_TEXT_2);

        lv_obj_t * content = box_create(lv_obj_0);
        lv_obj_set_x(content, RAIL_W);
        lv_obj_set_y(content, BAR_H);
        alarm_ui_run_22(content, CONTENT_W, CONTENT_H);
        alarm_ui_run_30(content, SPACE_MD, SPACE_MD);
        lv_obj_set_style_pad_bottom(content, SPACE_MD, 0);
        lv_obj_t * view_home = alarm_ui_run_0(content, lv_pct(100), lv_pct(100));
        alarm_ui_run_1(view_home, LV_FLEX_FLOW_ROW);
        alarm_ui_run_44(view_home, SPACE_MD, &subject_view, 0);
        lv_obj_t * hero = hero_create(view_home);
        alarm_ui_run_22(hero, PANE_W, lv_pct(100));
        lv_obj_bind_flag_if_eq(hero, &subject_panel, LV_OBJ_FLAG_HIDDEN, 2);

        lv_obj_t * pane = alarm_ui_run_9(view_home, lv_pct(100));
        lv_obj_bind_flag_if_eq(pane, &subject_panel, LV_OBJ_FLAG_HIDDEN, 2);
        lv_obj_t * panel_arm = alarm_ui_run_0(pane, lv_pct(100), lv_pct(100));
        alarm_ui_run_1(panel_arm, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_40(panel_arm, SPACE_MD, &subject_panel, 0);
        lv_obj_set_name(panel_arm, "panel_arm");
        lv_obj_t * arm_tile_0 = arm_tile_create(panel_arm, "\xEE\x84\x8E", alarm_ui_tr("Away"), alarm_ui_tr("Everyone is leaving"), COLOR_ROSE);
        lv_obj_set_width(arm_tile_0, lv_pct(100));
        lv_obj_set_flex_grow(arm_tile_0, 1);
        lv_obj_add_event_cb(arm_tile_0, ui_on_arm, LV_EVENT_CLICKED, (void *)"away");

        lv_obj_t * arm_tile_1 = arm_tile_create(panel_arm, "\xEE\x83\xB5", alarm_ui_tr("Stay"), alarm_ui_tr("Someone is home"), COLOR_BLUE);
        lv_obj_set_width(arm_tile_1, lv_pct(100));
        lv_obj_set_flex_grow(arm_tile_1, 1);
        lv_obj_add_event_cb(arm_tile_1, ui_on_arm, LV_EVENT_CLICKED, (void *)"stay");

        lv_obj_t * arm_tile_2 = arm_tile_create(panel_arm, "\xEE\x84\x9E", alarm_ui_tr("Night"), alarm_ui_tr("Going to bed"), COLOR_VIOLET);
        lv_obj_set_width(arm_tile_2, lv_pct(100));
        lv_obj_set_flex_grow(arm_tile_2, 1);
        lv_obj_add_event_cb(arm_tile_2, ui_on_arm, LV_EVENT_CLICKED, (void *)"night");

        lv_obj_t * panel_disarm = alarm_ui_run_0(pane, lv_pct(100), lv_pct(100));
        lv_obj_bind_flag_if_not_eq(panel_disarm, &subject_panel, LV_OBJ_FLAG_HIDDEN, 1);
        lv_obj_t * disarm_tile = action_tile_create(panel_disarm, "\xEE\x84\x8C", &subject_disarm_title, &subject_disarm_note, COLOR_GREEN);
        alarm_ui_run_22(disarm_tile, lv_pct(100), lv_pct(100));
        lv_obj_add_event_cb(disarm_tile, ui_on_disarm, LV_EVENT_CLICKED, NULL);

        lv_obj_t * panel_memory = alarm_ui_run_0(pane, lv_pct(100), lv_pct(100));
        lv_obj_bind_flag_if_not_eq(panel_memory, &subject_panel, LV_OBJ_FLAG_HIDDEN, 3);
        lv_obj_t * memory_tile = action_tile_create(panel_memory, "\xEE\x88\xA4", &subject_disarm_title, &subject_memory_note, COLOR_AMBER);
        alarm_ui_run_22(memory_tile, lv_pct(100), lv_pct(100));
        alarm_ui_run_13(memory_tile, ui_on_clear_memory, "memory_tile");

        lv_obj_t * panel_walk = alarm_ui_run_0(pane, lv_pct(100), lv_pct(100));
        alarm_ui_run_1(panel_walk, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_40(panel_walk, SPACE_MD, &subject_panel, 4);
        lv_obj_t * walk_from = alarm_ui_run_0(panel_walk, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_10(walk_from, COLOR_BLUE, (255 * 16 / 100), RADIUS_MD);
        lv_obj_set_style_pad_all(walk_from, SPACE_SM, 0);
        alarm_ui_run_1(walk_from, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(walk_from);
        lv_obj_set_style_pad_column(walk_from, SPACE_SM, 0);
        alarm_ui_run_29(walk_from, &subject_walk_from_on, 0, "walk_from");
        lv_obj_t * lv_label_12 = alarm_ui_run_47(walk_from, "\xEE\x83\xA8");
        alarm_ui_run_4(lv_label_12, icons_20, COLOR_BLUE);

        lv_obj_t * walk_from_text = lv_label_create(walk_from);
        lv_obj_set_flex_grow(walk_from_text, 1);
        lv_label_set_long_mode(walk_from_text, LV_LABEL_LONG_MODE_WRAP);
        alarm_ui_run_4(walk_from_text, font_caption, COLOR_TEXT);
        lv_label_bind_text(walk_from_text, &subject_walk_from, NULL);

        lv_obj_t * walk_card = alarm_ui_run_33(panel_walk, lv_pct(100));
        alarm_ui_run_10(walk_card, COLOR_SURFACE, (255 * 100 / 100), RADIUS_XL);
        alarm_ui_run_39(walk_card, COLOR_LINE);
        lv_obj_set_style_pad_all(walk_card, SPACE_MD, 0);
        alarm_ui_run_1(walk_card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_flex_main_place(walk_card, LV_FLEX_ALIGN_CENTER, 0);
        alarm_ui_run_3(walk_card);
        lv_obj_set_style_pad_row(walk_card, SPACE_SM, 0);
        lv_obj_t * walk_count = alarm_ui_run_2(walk_card, LV_LABEL_LONG_MODE_WRAP);
        alarm_ui_run_45(walk_count, font_title, LV_TEXT_ALIGN_CENTER);
        lv_label_bind_text(walk_count, &subject_walk_count, NULL);

        lv_obj_t * walk_left = alarm_ui_run_2(walk_card, LV_LABEL_LONG_MODE_WRAP);
        alarm_ui_run_45(walk_left, font_caption, LV_TEXT_ALIGN_CENTER);
        lv_label_bind_text(walk_left, &subject_walk_left, NULL);
        alarm_ui_run_49(walk_left, &style_text_2, 0, &style_text_warn, &subject_walk_warn, 1);

        lv_obj_t * walk_zones = row_button_create(panel_walk, "\xEE\x81\xAF", alarm_ui_tr("Show zones"), COLOR_BLUE);
        lv_obj_set_height(walk_zones, 40);
        lv_obj_add_event_cb(walk_zones, ui_on_end_walk_test, LV_EVENT_CLICKED, (void *)"zones");
        lv_obj_set_name(walk_zones, "walk_zones");

        lv_obj_t * walk_extend = row_button_create(panel_walk, "\xEE\x82\x87", alarm_ui_tr("Extend"), COLOR_BLUE);
        lv_obj_set_height(walk_extend, 40);
        lv_obj_add_event_cb(walk_extend, ui_on_end_walk_test, LV_EVENT_CLICKED, (void *)"extend");
        lv_obj_set_name(walk_extend, "walk_extend");

        lv_obj_t * walk_end = row_button_create(panel_walk, "\xEE\x81\xAC", alarm_ui_tr("End test"), COLOR_GREEN);
        lv_obj_set_height(walk_end, 40);
        lv_obj_add_event_cb(walk_end, ui_on_end_walk_test, LV_EVENT_CLICKED, (void *)"end");
        lv_obj_set_name(walk_end, "walk_end");

        lv_obj_t * pad = code_pad_create(view_home, 1, 112);
        alarm_ui_run_22(pad, lv_pct(100), lv_pct(100));
        lv_obj_bind_flag_if_not_eq(pad, &subject_panel, LV_OBJ_FLAG_HIDDEN, 2);
        lv_obj_set_name(pad, "pad");

        lv_obj_t * view_zones = alarm_ui_run_0(content, lv_pct(100), lv_pct(100));
        alarm_ui_run_1(view_zones, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_40(view_zones, SPACE_MD, &subject_view, 1);
        lv_obj_t * box_1 = alarm_ui_run_0(view_zones, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(box_1, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(box_1);
        lv_obj_set_style_pad_column(box_1, SPACE_MD, 0);
        lv_obj_t * view_header_0 = view_header_create(box_1, alarm_ui_tr("Zones"), &subject_zones_note);
        lv_obj_set_flex_grow(view_header_0, 1);

        lv_obj_t * zone_filters = alarm_ui_run_0(box_1, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        alarm_ui_run_10(zone_filters, COLOR_SURFACE, (255 * 100 / 100), RADIUS_LG);
        lv_obj_set_style_pad_all(zone_filters, 3, 0);
        alarm_ui_run_1(zone_filters, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(zone_filters, 2, 0);
        lv_obj_set_name(zone_filters, "zone_filters");
        alarm_ui_run_27(zone_filters, alarm_ui_tr("All"), 0, 0);

        alarm_ui_run_27(zone_filters, alarm_ui_tr("Open"), 1, 1);

        alarm_ui_run_27(zone_filters, alarm_ui_tr("Bypassed"), 2, 2);

        alarm_ui_run_27(zone_filters, alarm_ui_tr("Trouble"), 3, 3);

        lv_obj_t * zone_list = alarm_ui_run_7(view_zones);
        alarm_ui_run_1(zone_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(zone_list, SPACE_SM, 0);
        alarm_ui_run_29(zone_list, &subject_zones_empty, 1, "zone_list");

        lv_obj_t * zones_none = alarm_ui_run_33(view_zones, lv_pct(100));
        alarm_ui_run_1(zones_none, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_flex_main_place(zones_none, LV_FLEX_ALIGN_CENTER, 0);
        alarm_ui_run_3(zones_none);
        lv_obj_set_style_pad_row(zones_none, SPACE_SM, 0);
        alarm_ui_run_29(zones_none, &subject_zones_empty, 0, "zones_none");
        icon_chip_create(zones_none, "\xEE\x88\xA6", 72, icons_40, COLOR_GREEN);

        lv_obj_t * lv_label_13 = alarm_ui_run_31(zones_none, alarm_ui_tr("No open zones"));
        alarm_ui_run_50(lv_label_13, 1);

        lv_obj_t * lv_label_14 = alarm_ui_run_31(zones_none, alarm_ui_tr("No bypassed zones"));
        alarm_ui_run_50(lv_label_14, 2);

        lv_obj_t * lv_label_15 = alarm_ui_run_31(zones_none, alarm_ui_tr("No zone troubles"));
        alarm_ui_run_50(lv_label_15, 3);

        lv_obj_t * view_activity = alarm_ui_run_0(content, lv_pct(100), lv_pct(100));
        alarm_ui_run_1(view_activity, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_40(view_activity, SPACE_MD, &subject_view, 2);
        view_header_create(view_activity, alarm_ui_tr("Activity"), &subject_events_note);

        lv_obj_t * event_list = alarm_ui_run_7(view_activity);
        alarm_ui_run_1(event_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(event_list, SPACE_SM, 0);
        lv_obj_set_name(event_list, "event_list");

        lv_obj_t * view_troubles = alarm_ui_run_0(content, lv_pct(100), lv_pct(100));
        alarm_ui_run_1(view_troubles, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_40(view_troubles, SPACE_MD, &subject_view, 3);
        lv_obj_set_name(view_troubles, "view_troubles");
        lv_obj_t * box_2 = alarm_ui_run_0(view_troubles, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(box_2, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(box_2);
        lv_obj_set_style_pad_column(box_2, SPACE_MD, 0);
        lv_obj_t * view_header_2 = view_header_create(box_2, alarm_ui_tr("Troubles"), &subject_troubles_note);
        lv_obj_set_flex_grow(view_header_2, 1);

        lv_obj_t * ack_button = alarm_ui_run_0(box_2, LV_SIZE_CONTENT, 44);
        lv_obj_set_flag(ack_button, LV_OBJ_FLAG_CLICKABLE, true);
        alarm_ui_run_10(ack_button, COLOR_RAISED, (255 * 100 / 100), RADIUS_FULL);
        alarm_ui_run_30(ack_button, SPACE_XL, SPACE_XL);
        alarm_ui_run_1(ack_button, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(ack_button);
        alarm_ui_run_48(ack_button, SPACE_SM, &subject_troubles_empty, 1);
        lv_obj_bind_style(ack_button, &style_disabled, 0, &subject_locked, 1);
        lv_obj_add_event_cb(ack_button, ui_on_ack_troubles, LV_EVENT_CLICKED, NULL);
        lv_obj_t * lv_label_16 = lv_label_create(ack_button);
        alarm_ui_run_18(lv_label_16, "\xEE\x81\xAC");

        lv_obj_t * lv_label_17 = alarm_ui_run_31(ack_button, alarm_ui_tr("Acknowledge"));
        lv_obj_set_style_text_font(lv_label_17, font_label, 0);

        lv_obj_t * trouble_list = alarm_ui_run_7(view_troubles);
        alarm_ui_run_1(trouble_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(trouble_list, SPACE_SM, 0);
        lv_obj_set_name(trouble_list, "trouble_list");

        lv_obj_t * trouble_pager = alarm_ui_run_0(view_troubles, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(trouble_pager, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_main_place(trouble_pager, LV_FLEX_ALIGN_CENTER, 0);
        alarm_ui_run_3(trouble_pager);
        alarm_ui_run_48(trouble_pager, SPACE_LG, &subject_troubles_pager, 0);
        lv_obj_t * troubles_back = alarm_ui_run_0(trouble_pager, 56, 44);
        alarm_ui_run_20(troubles_back, LV_OBJ_FLAG_CLICKABLE, 4);
        alarm_ui_run_10(troubles_back, COLOR_RAISED, (255 * 100 / 100), RADIUS_FULL);
        lv_obj_add_event_cb(troubles_back, ui_on_troubles_page, LV_EVENT_CLICKED, (void *)"back");
        lv_obj_set_name(troubles_back, "troubles_back");
        lv_obj_t * lv_label_18 = alarm_ui_run_15(troubles_back);
        alarm_ui_run_18(lv_label_18, "\xEE\x81\xAE");

        lv_obj_t * troubles_page_label = lv_label_create(trouble_pager);
        alarm_ui_run_4(troubles_page_label, font_label, COLOR_TEXT_2);
        lv_label_bind_text(troubles_page_label, &subject_troubles_page, NULL);

        lv_obj_t * troubles_more = alarm_ui_run_0(trouble_pager, 56, 44);
        alarm_ui_run_20(troubles_more, LV_OBJ_FLAG_CLICKABLE, 4);
        alarm_ui_run_10(troubles_more, COLOR_RAISED, (255 * 100 / 100), RADIUS_FULL);
        lv_obj_add_event_cb(troubles_more, ui_on_troubles_page, LV_EVENT_CLICKED, (void *)"more");
        lv_obj_set_name(troubles_more, "troubles_more");
        lv_obj_t * lv_label_19 = alarm_ui_run_15(troubles_more);
        alarm_ui_run_18(lv_label_19, "\xEE\x81\xAF");

        lv_obj_t * troubles_none = alarm_ui_run_33(view_troubles, lv_pct(100));
        alarm_ui_run_1(troubles_none, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_flex_main_place(troubles_none, LV_FLEX_ALIGN_CENTER, 0);
        alarm_ui_run_3(troubles_none);
        lv_obj_set_style_pad_row(troubles_none, SPACE_SM, 0);
        lv_obj_bind_flag_if_eq(troubles_none, &subject_troubles_empty, LV_OBJ_FLAG_HIDDEN, 0);
        icon_chip_create(troubles_none, "\xEE\x88\xA6", 72, icons_40, COLOR_GREEN);

        lv_obj_t * lv_label_20 = alarm_ui_run_31(troubles_none, alarm_ui_tr("No troubles"));
        lv_obj_set_style_text_font(lv_label_20, font_title, 0);

        lv_obj_t * lv_label_21 = alarm_ui_run_31(troubles_none, alarm_ui_tr("Power, network and sensors are all fine"));
        alarm_ui_run_4(lv_label_21, font_caption, COLOR_TEXT_2);

        lv_obj_t * view_panic = alarm_ui_run_0(content, lv_pct(100), lv_pct(100));
        alarm_ui_run_1(view_panic, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_flex_main_place(view_panic, LV_FLEX_ALIGN_CENTER, 0);
        alarm_ui_run_3(view_panic);
        alarm_ui_run_40(view_panic, SPACE_LG, &subject_view, 4);
        lv_obj_t * box_3 = alarm_ui_run_0(view_panic, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(box_3, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_3(box_3);
        lv_obj_set_style_pad_row(box_3, 2, 0);
        lv_obj_t * lv_label_22 = alarm_ui_run_31(box_3, alarm_ui_tr("Emergency"));
        lv_obj_set_style_text_font(lv_label_22, font_headline, 0);

        lv_obj_t * lv_label_23 = alarm_ui_run_2(box_3, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(lv_label_23, alarm_ui_tr("Hold a button for 2 seconds to set off the alarm on this panel"));
        lv_obj_set_style_text_align(lv_label_23, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(lv_label_23, COLOR_TEXT_2, 0);

        lv_obj_t * panic_report = alarm_ui_run_2(box_3, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(panic_report, LV_TEXT_ALIGN_CENTER, 0);
        alarm_ui_run_4(panic_report, font_caption, COLOR_TEXT_3);
        lv_label_bind_text(panic_report, &subject_panic_report, NULL);

        lv_obj_t * box_4 = alarm_ui_run_0(view_panic, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        alarm_ui_run_1(box_4, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(box_4, 24, 0);
        lv_obj_t * panic_police = panic_button_create(box_4, "\xEE\x8B\xAF", alarm_ui_tr("Police"), COLOR_RED);
        lv_obj_add_event_cb(panic_police, ui_on_panic, LV_EVENT_ALL, (void *)"police");
        lv_obj_set_name(panic_police, "panic_police");

        lv_obj_t * panic_fire = panic_button_create(box_4, "\xEE\x83\x92", alarm_ui_tr("Fire"), COLOR_ORANGE);
        lv_obj_add_event_cb(panic_fire, ui_on_panic, LV_EVENT_ALL, (void *)"fire");

        lv_obj_t * panic_medical = panic_button_create(box_4, "\xEE\x8D\xAE", alarm_ui_tr("Medical"), COLOR_ROSE);
        lv_obj_add_event_cb(panic_medical, ui_on_panic, LV_EVENT_ALL, (void *)"medical");

        lv_obj_t * view_settings = alarm_ui_run_0(content, lv_pct(100), lv_pct(100));
        alarm_ui_run_1(view_settings, LV_FLEX_FLOW_COLUMN);
        alarm_ui_run_40(view_settings, 6, &subject_view, 5);
        lv_obj_t * settings_nav = alarm_ui_run_0(view_settings, lv_pct(100), 44);
        alarm_ui_run_1(settings_nav, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_main_place(settings_nav, LV_FLEX_ALIGN_SPACE_BETWEEN, 0);
        alarm_ui_run_3(settings_nav);
        lv_obj_bind_flag_if_eq(settings_nav, &subject_settings_pad, LV_OBJ_FLAG_HIDDEN, 1);
        lv_obj_t * set_display = alarm_ui_run_23(settings_nav, "\xEE\x85\xB8", alarm_ui_tr("Display and sound"), alarm_ui_tr("Brightness and volume"), COLOR_BLUE, 0);
        lv_obj_add_event_cb(set_display, ui_on_settings_section, LV_EVENT_CLICKED, (void *)"display");
        lv_obj_set_name(set_display, "set_display");

        lv_obj_t * set_users = alarm_ui_run_23(settings_nav, "\xEE\x86\xA4", alarm_ui_tr("Users"), alarm_ui_tr("Codes and who can do what"), COLOR_VIOLET, 1);
        lv_obj_add_event_cb(set_users, ui_on_settings_section, LV_EVENT_CLICKED, (void *)"users");
        lv_obj_set_name(set_users, "set_users");

        lv_obj_t * set_wifi = alarm_ui_run_23(settings_nav, "\xEE\x86\xAE", alarm_ui_tr("Wi-Fi"), alarm_ui_tr("Network and signal"), COLOR_BLUE, 2);
        lv_obj_add_event_cb(set_wifi, ui_on_settings_section, LV_EVENT_CLICKED, (void *)"wifi");
        lv_obj_set_name(set_wifi, "set_wifi");

        lv_obj_t * set_time = alarm_ui_run_23(settings_nav, "\xEE\x8C\x84", alarm_ui_tr("Date and time"), alarm_ui_tr("Time zone, 24-hour clock"), COLOR_GREEN, 3);
        lv_obj_add_event_cb(set_time, ui_on_settings_section, LV_EVENT_CLICKED, (void *)"time");
        lv_obj_set_name(set_time, "set_time");

        lv_obj_t * set_integrations = alarm_ui_run_23(settings_nav, "\xEE\x98\xBC", alarm_ui_tr("Integrations"), alarm_ui_tr("Smart home and notifications"), COLOR_VIOLET, 4);
        lv_obj_add_event_cb(set_integrations, ui_on_settings_section, LV_EVENT_CLICKED, (void *)"integrations");
        lv_obj_set_name(set_integrations, "set_integrations");

        lv_obj_t * set_log = alarm_ui_run_23(settings_nav, "\xEE\x91\x9F", alarm_ui_tr("Event log"), alarm_ui_tr("Everything the panel did"), COLOR_GREEN, 5);
        lv_obj_add_event_cb(set_log, ui_on_settings_section, LV_EVENT_CLICKED, (void *)"log");
        lv_obj_set_name(set_log, "set_log");

        lv_obj_t * set_walk = alarm_ui_run_23(settings_nav, "\xEE\x8E\xB9", alarm_ui_tr("Walk test"), alarm_ui_tr("Check that every sensor works"), COLOR_BLUE, 8);
        lv_obj_add_event_cb(set_walk, ui_on_settings_section, LV_EVENT_CLICKED, (void *)"walk");
        lv_obj_set_name(set_walk, "set_walk");

        lv_obj_t * set_installer = alarm_ui_run_23(settings_nav, "\xEE\x86\xB1", alarm_ui_tr("Installer"), alarm_ui_tr("Zones, delays, codes, reset"), COLOR_AMBER, 6);
        lv_obj_add_event_cb(set_installer, ui_on_settings_section, LV_EVENT_CLICKED, (void *)"installer");
        lv_obj_set_name(set_installer, "set_installer");

        lv_obj_t * set_about = alarm_ui_run_23(settings_nav, "\xEE\x83\xB9", alarm_ui_tr("About"), alarm_ui_tr("Storage and system"), COLOR_ROSE, 7);
        lv_obj_add_event_cb(set_about, ui_on_settings_section, LV_EVENT_CLICKED, (void *)"about");
        lv_obj_set_name(set_about, "set_about");

        lv_obj_t * settings_page = alarm_ui_run_33(view_settings, lv_pct(100));
        alarm_ui_run_1(settings_page, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(settings_page, 6, 0);
        lv_obj_set_name(settings_page, "settings_page");
        lv_obj_t * page_head = alarm_ui_run_0(settings_page, lv_pct(100), LV_SIZE_CONTENT);
        alarm_ui_run_1(page_head, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(page_head);
        alarm_ui_run_48(page_head, SPACE_MD, &subject_settings_pad, 1);
        lv_obj_t * page_back = alarm_ui_run_0(page_head, 40, 40);
        alarm_ui_run_20(page_back, LV_OBJ_FLAG_CLICKABLE, 6);
        lv_obj_set_style_radius(page_back, RADIUS_FULL, 0);
        alarm_ui_run_36(page_back, COLOR_RAISED, (255 * 100 / 100));
        lv_obj_bind_flag_if_eq(page_back, &subject_page_back, LV_OBJ_FLAG_HIDDEN, 0);
        alarm_ui_run_13(page_back, ui_on_page_back, "page_back");
        lv_obj_t * lv_label_24 = alarm_ui_run_15(page_back);
        alarm_ui_run_18(lv_label_24, "\xEE\x81\xAE");

        lv_obj_t * box_5 = alarm_ui_run_9(page_head, LV_SIZE_CONTENT);
        alarm_ui_run_1(box_5, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box_5, 0, 0);
        lv_obj_t * lv_label_25 = alarm_ui_run_2(box_5, LV_LABEL_LONG_MODE_DOTS);
        alarm_ui_run_8(lv_label_25, font_title, &subject_page_title);

        lv_obj_t * lv_label_26 = alarm_ui_run_2(box_5, LV_LABEL_LONG_MODE_WRAP);
        alarm_ui_run_4(lv_label_26, font_caption, COLOR_TEXT_2);
        lv_label_bind_text(lv_label_26, &subject_page_note, NULL);

        lv_obj_t * page_spinner = lv_spinner_create(page_head);
        alarm_ui_run_22(page_spinner, 28, 28);
        alarm_ui_run_35(page_spinner);

        lv_obj_t * page_body = alarm_ui_run_7(settings_page);
        alarm_ui_run_1(page_body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(page_body, SPACE_SM, 0);
        alarm_ui_run_29(page_body, &subject_settings_pad, 1, "page_body");

        lv_obj_t * settings_pad_area = alarm_ui_run_0(settings_page, lv_pct(100), lv_pct(100));
        lv_obj_bind_flag_if_eq(settings_pad_area, &subject_settings_pad, LV_OBJ_FLAG_HIDDEN, 0);
        lv_obj_t * settings_pad = code_pad_create(settings_pad_area, 1, 112);
        alarm_ui_run_22(settings_pad, lv_pct(100), lv_pct(100));
        lv_obj_set_name(settings_pad, "settings_pad");

        lv_obj_t * text_sheet = text_sheet_create(lv_obj_0);
        lv_obj_bind_flag_if_not_eq(text_sheet, &subject_sheet, LV_OBJ_FLAG_HIDDEN, 1);

        lv_obj_t * pick_sheet = pick_sheet_create(lv_obj_0);
        lv_obj_bind_flag_if_not_eq(pick_sheet, &subject_sheet, LV_OBJ_FLAG_HIDDEN, 2);

        lv_obj_t * confirm_dialog = confirm_dialog_create(lv_obj_0);
        lv_obj_bind_flag_if_not_eq(confirm_dialog, &subject_sheet, LV_OBJ_FLAG_HIDDEN, 3);

        lv_obj_t * modal = modal_pad_create(lv_obj_0);
        alarm_ui_run_29(modal, &subject_modal_pad, 0, "modal");

        lv_obj_t * toast = alarm_ui_run_0(lv_obj_0, LV_SIZE_CONTENT, 40);
        alarm_ui_run_43(toast, LV_ALIGN_BOTTOM_MID, TOAST_X, TOAST_Y);
        lv_obj_set_style_max_width(toast, 400, 0);
        alarm_ui_run_10(toast, COLOR_PRESSED, (255 * 100 / 100), RADIUS_FULL);
        alarm_ui_run_39(toast, lv_color_hex(0x3A4658));
        alarm_ui_run_30(toast, SPACE_XL, SPACE_XL);
        alarm_ui_run_1(toast, LV_FLEX_FLOW_ROW);
        alarm_ui_run_3(toast);
        alarm_ui_run_29(toast, &subject_toast_visible, 0, "toast");
        lv_obj_t * toast_text = lv_label_create(toast);
        lv_obj_set_height(toast_text, 20);
        lv_label_set_long_mode(toast_text, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_max_width(toast_text, 376, 0);
        alarm_ui_run_8(toast_text, font_label, &subject_toast);

        the_root = lv_obj_0;
    }
    #endif

    LV_TRACE_OBJ_CREATE("finished");

    return the_root;
}
