/**
 * @file alarm_ui_runs_gen.c
 * @brief Runs of statements shared by the generated screens and components
 */

#include "alarm_ui_gen.h"

lv_obj_t * alarm_ui_run_0(lv_obj_t * parent, int32_t a0, int32_t a1)
{
    lv_obj_t * o = box_create(parent);
    lv_obj_set_width(o, a0);
    lv_obj_set_height(o, a1);
    return o;
}

void alarm_ui_run_1(lv_obj_t * o, int32_t a0)
{
    lv_obj_set_style_layout(o, LV_LAYOUT_FLEX, 0);
    lv_obj_set_style_flex_flow(o, a0, 0);
}

lv_obj_t * alarm_ui_run_2(lv_obj_t * parent, int32_t a0)
{
    lv_obj_t * o = lv_label_create(parent);
    lv_obj_set_width(o, lv_pct(100));
    lv_label_set_long_mode(o, a0);
    return o;
}

void alarm_ui_run_3(lv_obj_t * o)
{
    lv_obj_set_style_flex_cross_place(o, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_flex_track_place(o, LV_FLEX_ALIGN_CENTER, 0);
}

void alarm_ui_run_4(lv_obj_t * o, const lv_font_t * a0, lv_color_t a1)
{
    lv_obj_set_style_text_font(o, a0, 0);
    lv_obj_set_style_text_color(o, a1, 0);
}

lv_obj_t * alarm_ui_run_5(lv_obj_t * parent, lv_style_t * a0, int32_t a1)
{
    lv_obj_t * o = box_create(parent);
    lv_obj_set_width(o, LV_SIZE_CONTENT);
    lv_obj_set_flag(o, LV_OBJ_FLAG_CLICKABLE, true);
    lv_obj_set_ext_click_area(o, 6);
    lv_obj_add_style(o, &style_button, 0);
    lv_obj_add_style(o, a0, a1);
    return o;
}

void alarm_ui_run_6(lv_style_t * o, lv_color_t a0, int32_t a1)
{
    lv_style_set_bg_color(o, a0);
    lv_style_set_bg_opa(o, a1);
}

void alarm_ui_run_7(lv_style_t * o, int32_t a0, int32_t a1, int32_t a2, int32_t a3, int32_t a4)
{
    lv_style_set_pad_left(o, a0);
    lv_style_set_pad_right(o, a1);
    lv_style_set_pad_top(o, a2);
    lv_style_set_pad_bottom(o, a3);
    lv_style_set_layout(o, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_style_set_flex_cross_place(o, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_track_place(o, LV_FLEX_ALIGN_CENTER);
    lv_style_set_pad_column(o, a4);
}

lv_obj_t * alarm_ui_run_8(lv_obj_t * parent)
{
    lv_obj_t * o = box_create(parent);
    lv_obj_set_flex_grow(o, 1);
    lv_obj_set_height(o, LV_SIZE_CONTENT);
    return o;
}

void alarm_ui_run_9(lv_obj_t * o, lv_color_t a0, int32_t a1, int32_t a2, int32_t a3)
{
    lv_obj_set_style_bg_color(o, a0, a1);
    lv_obj_set_style_bg_opa(o, a2, 0);
    lv_obj_set_style_radius(o, a3, 0);
}

void alarm_ui_run_10(lv_obj_t * o, lv_event_cb_t a0, const char * a1)
{
    lv_obj_add_event_cb(o, a0, LV_EVENT_CLICKED, NULL);
    lv_obj_set_name(o, a1);
}

lv_obj_t * alarm_ui_run_11(lv_obj_t * parent, const char * a0)
{
    lv_obj_t * o = lv_label_create(parent);
    lv_obj_set_align(o, LV_ALIGN_CENTER);
    lv_label_set_text(o, a0);
    return o;
}

void alarm_ui_run_12(lv_obj_t * o, const lv_font_t * a0, lv_subject_t * a1)
{
    lv_obj_set_style_text_font(o, a0, 0);
    lv_label_bind_text(o, a1, NULL);
}

void alarm_ui_run_13(lv_obj_t * o)
{
    lv_obj_set_flag(o, LV_OBJ_FLAG_SCROLLABLE, true);
    lv_obj_set_flag(o, LV_OBJ_FLAG_CLICKABLE, true);
    lv_obj_set_flag(o, LV_OBJ_FLAG_USER_1, true);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
}

lv_obj_t * alarm_ui_run_14(lv_obj_t * parent, const char * a0)
{
    lv_obj_t * o = lv_label_create(parent);
    lv_label_set_text(o, a0);
    return o;
}

void alarm_ui_run_15(lv_style_t * o, int32_t a0, int32_t a1)
{
    lv_style_set_pad_left(o, a0);
    lv_style_set_pad_right(o, a1);
    lv_style_set_layout(o, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_style_set_flex_cross_place(o, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_track_place(o, LV_FLEX_ALIGN_CENTER);
}

void alarm_ui_run_16(lv_obj_t * o, lv_style_t * a0, int32_t a1, lv_style_t * a2, int32_t a3)
{
    lv_obj_add_style(o, a0, a1);
    lv_obj_add_style(o, a2, a3);
}

void alarm_ui_run_17(lv_style_t * o, int32_t a0)
{
    lv_style_set_layout(o, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(o, a0);
    lv_style_set_flex_main_place(o, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_cross_place(o, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_track_place(o, LV_FLEX_ALIGN_CENTER);
}

void alarm_ui_run_18(lv_obj_t * o)
{
    lv_obj_set_style_border_side(o, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, COLOR_LINE, 0);
    lv_obj_set_style_pad_top(o, SPACE_MD, 0);
    lv_obj_set_style_pad_left(o, SPACE_XS, 0);
    lv_obj_set_style_pad_right(o, SPACE_XS, 0);
}

void alarm_ui_run_19(lv_style_t * o, int32_t a0, int32_t a1, int32_t a2)
{
    lv_style_set_width(o, a0);
    lv_style_set_height(o, a1);
    lv_style_set_radius(o, a2);
}

lv_obj_t * alarm_ui_run_20(lv_obj_t * parent)
{
    lv_obj_t * o = box_create(parent);
    lv_obj_set_flag(o, LV_OBJ_FLAG_CLICKABLE, true);
    return o;
}

void alarm_ui_run_21(lv_obj_t * o, int32_t a0, lv_color_t a1, int32_t a2)
{
    lv_obj_set_style_radius(o, a0, 0);
    lv_obj_set_style_bg_color(o, a1, 0);
    lv_obj_set_style_bg_opa(o, a2, 0);
}

lv_obj_t * alarm_ui_run_22(lv_obj_t * parent, int32_t a0, int32_t a1)
{
    lv_obj_t * o = box_create(parent);
    lv_obj_set_width(o, a0);
    lv_obj_set_flag(o, a1, true);
    return o;
}

lv_obj_t * alarm_ui_run_23(lv_obj_t * parent, const char * a0)
{
    lv_obj_t * o = lv_label_create(parent);
    lv_label_set_text(o, a0);
    return o;
}

void alarm_ui_run_24(lv_style_t * o, int32_t a0)
{
    lv_style_set_radius(o, a0);
    lv_style_set_border_width(o, 1);
    lv_style_set_border_color(o, COLOR_LINE);
}

lv_obj_t * alarm_ui_run_25(lv_obj_t * parent, lv_style_t * a0)
{
    lv_obj_t * o = box_create(parent);
    lv_obj_add_style(o, a0, 0);
    return o;
}

void alarm_ui_run_26(lv_obj_t * o, int32_t a0, int32_t a1)
{
    lv_obj_set_style_pad_left(o, a0, 0);
    lv_obj_set_style_pad_right(o, a1, 0);
}

lv_obj_t * alarm_ui_run_27(lv_obj_t * parent, int32_t a0)
{
    lv_obj_t * o = box_create(parent);
    lv_obj_add_style(o, &style_step, 0);
    lv_obj_add_style(o, &style_step_on, LV_STATE_CHECKED);
    lv_obj_bind_state_if_ge(o, &subject_wiz_step, LV_STATE_CHECKED, a0);
    return o;
}

lv_obj_t * alarm_ui_run_28(lv_obj_t * parent, const char * a0, lv_subject_t * a1, int32_t a2, lv_subject_t * a3, int32_t a4)
{
    lv_obj_t * o = segment_create(parent, a0);
    lv_obj_bind_style(o, &style_segment_active, 0, a1, a2);
    lv_obj_add_subject_set_int_event(o, a3, LV_EVENT_CLICKED, a4);
    return o;
}

void alarm_ui_run_29(lv_obj_t * o, lv_color_t a0, lv_subject_t * a1)
{
    lv_obj_set_style_text_color(o, a0, 0);
    lv_label_bind_text(o, a1, NULL);
}

lv_obj_t * alarm_ui_run_30(lv_obj_t * parent)
{
    lv_obj_t * o = box_create(parent);
    lv_obj_set_width(o, lv_pct(100));
    lv_obj_set_flex_grow(o, 1);
    return o;
}

void alarm_ui_run_31(lv_obj_t * o, int32_t a0, int32_t a1)
{
    lv_obj_set_width(o, a0);
    lv_obj_set_height(o, a1);
}

void alarm_ui_run_32(lv_obj_t * o, lv_style_t * a0)
{
    lv_obj_set_flag(o, LV_OBJ_FLAG_CLICKABLE, true);
    lv_obj_add_style(o, a0, 0);
}

lv_obj_t * alarm_ui_run_33(lv_obj_t * parent)
{
    lv_obj_t * o = lv_label_create(parent);
    lv_obj_set_align(o, LV_ALIGN_CENTER);
    return o;
}

void alarm_ui_run_34(lv_obj_t * o)
{
    lv_obj_set_align(o, LV_ALIGN_CENTER);
    lv_obj_add_style(o, &style_sheet_card, 0);
}

void alarm_ui_run_35(lv_obj_t * o, int32_t a0)
{
    lv_label_set_long_mode(o, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_height(o, a0, 0);
}

lv_obj_t * alarm_ui_run_36(lv_obj_t * parent, int32_t a0, const char * a1)
{
    lv_obj_t * o = lv_roller_create(parent);
    lv_obj_set_width(o, a0);
    lv_obj_add_style(o, &style_roller, 0);
    lv_obj_add_style(o, &style_roller_selected, LV_PART_SELECTED);
    lv_obj_set_name(o, a1);
    return o;
}

void alarm_ui_run_37(lv_obj_t * o, const lv_font_t * a0, const char * a1)
{
    lv_obj_set_style_text_font(o, a0, 0);
    lv_obj_set_name(o, a1);
}

void alarm_ui_run_38(lv_style_t * o)
{
    lv_style_set_width(o, lv_pct(100));
    lv_style_set_height(o, LV_SIZE_CONTENT);
}

void alarm_ui_run_39(lv_obj_t * o, int32_t a0, lv_subject_t * a1, int32_t a2)
{
    lv_obj_set_style_pad_column(o, a0, 0);
    lv_obj_bind_flag_if_not_eq(o, a1, LV_OBJ_FLAG_HIDDEN, a2);
}

void alarm_ui_run_40(lv_obj_t * o, lv_color_t a0)
{
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, a0, 0);
}

lv_obj_t * alarm_ui_run_41(lv_obj_t * parent, const char * a0, const char * a1, int32_t a2)
{
    lv_obj_t * o = settings_item_create(parent, a0, a1);
    lv_obj_bind_style(o, &style_nav_active, 0, &subject_settings_section, a2);
    return o;
}

void alarm_ui_run_42(lv_obj_t * o, lv_subject_t * a0, int32_t a1, const char * a2)
{
    lv_obj_bind_flag_if_eq(o, a0, LV_OBJ_FLAG_HIDDEN, a1);
    lv_obj_set_name(o, a2);
}

void alarm_ui_run_43(lv_obj_t * o, lv_subject_t * a0, int32_t a1, const char * a2)
{
    lv_obj_bind_flag_if_not_eq(o, a0, LV_OBJ_FLAG_HIDDEN, a1);
    lv_obj_set_name(o, a2);
}

lv_obj_t * alarm_ui_run_44(lv_obj_t * parent, int32_t a0)
{
    lv_obj_t * o = lv_label_create(parent);
    lv_obj_set_flex_grow(o, 1);
    lv_label_set_long_mode(o, a0);
    return o;
}

void alarm_ui_run_45(lv_style_t * o, const lv_font_t * a0, lv_color_t a1)
{
    lv_style_set_text_font(o, a0);
    lv_style_set_text_color(o, a1);
}

lv_obj_t * alarm_ui_run_46(lv_obj_t * parent, const char * a0, const char * a1, const char * a2, lv_color_t a3)
{
    lv_obj_t * o = arm_tile_create(parent, a0, a1, a2, a3);
    lv_obj_set_width(o, lv_pct(100));
    lv_obj_set_flex_grow(o, 1);
    lv_obj_add_style(o, &style_disabled, LV_STATE_DISABLED);
    lv_obj_bind_state_if_ge(o, &subject_link_home, LV_STATE_DISABLED, 1);
    return o;
}

void alarm_ui_run_47(lv_obj_t * o, const char * a0)
{
    lv_obj_set_style_text_color(o, COLOR_TEXT_2, 0);
    lv_obj_set_name(o, a0);
}

void alarm_ui_run_48(lv_obj_t * o, int32_t a0, const char * a1)
{
    lv_obj_set_style_pad_column(o, a0, 0);
    lv_obj_set_name(o, a1);
}

void alarm_ui_run_49(lv_obj_t * o, int32_t a0, const char * a1)
{
    lv_obj_set_style_pad_row(o, a0, 0);
    lv_obj_set_name(o, a1);
}

void alarm_ui_run_50(lv_obj_t * o)
{
    lv_spinner_set_anim_duration(o, 1000);
    lv_spinner_set_arc_sweep(o, 240);
    lv_obj_set_style_arc_width(o, 4, 0);
    lv_obj_set_style_arc_color(o, COLOR_LINE, 0);
    lv_obj_set_style_arc_width(o, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(o, COLOR_BLUE, LV_PART_INDICATOR);
    lv_obj_bind_flag_if_eq(o, &subject_busy, LV_OBJ_FLAG_HIDDEN, 0);
}

void alarm_ui_run_51(lv_obj_t * o, int32_t a0, const char * a1)
{
    lv_obj_add_subject_set_int_event(o, &subject_view, LV_EVENT_CLICKED, a0);
    lv_obj_set_name(o, a1);
}

void alarm_ui_run_52(lv_obj_t * o, lv_style_t * a0, int32_t a1, lv_style_t * a2, lv_subject_t * a3, int32_t a4)
{
    lv_obj_add_style(o, a0, a1);
    lv_obj_bind_style(o, a2, 0, a3, a4);
}

void alarm_ui_run_53(lv_style_t * o, int32_t a0, int32_t a1, int32_t a2, int32_t a3)
{
    lv_style_set_pad_left(o, a0);
    lv_style_set_pad_right(o, a1);
    lv_style_set_pad_top(o, a2);
    lv_style_set_pad_bottom(o, a3);
    lv_style_set_layout(o, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
}

void alarm_ui_run_54(lv_obj_t * o, lv_style_t * a0, int32_t a1, lv_style_t * a2, int32_t a3)
{
    lv_obj_add_style(o, a0, a1);
    lv_obj_add_style(o, a2, a3);
}

void alarm_ui_run_55(lv_obj_t * o, int32_t a0, int32_t a1, int32_t a2)
{
    lv_obj_set_align(o, a0);
    lv_obj_set_x(o, a1);
    lv_obj_set_y(o, a2);
}

void alarm_ui_run_56(lv_obj_t * o, int32_t a0)
{
    lv_obj_set_style_flex_cross_place(o, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_row(o, a0, 0);
}

void alarm_ui_run_57(lv_obj_t * o, int32_t a0, const char * a1)
{
    lv_obj_set_style_text_font(o, font_label, 0);
    lv_obj_set_style_text_align(o, a0, 0);
    lv_obj_set_name(o, a1);
}

void alarm_ui_run_58(lv_obj_t * o)
{
    lv_obj_set_style_flex_main_place(o, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_row(o, 2, 0);
}

void alarm_ui_run_59(lv_obj_t * o, int32_t a0, int32_t a1, int32_t a2)
{
    lv_obj_set_flag(o, a0, a1);
    lv_obj_set_ext_click_area(o, a2);
}

void alarm_ui_run_60(lv_obj_t * o)
{
    lv_obj_set_style_text_line_space(o, -2, 0);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(o, font_caption, 0);
}

lv_obj_t * alarm_ui_run_61(lv_obj_t * parent, lv_subject_t * a0)
{
    lv_obj_t * o = lv_label_create(parent);
    lv_label_bind_text(o, a0, NULL);
    return o;
}

void alarm_ui_run_62(lv_style_t * o, int32_t a0, int32_t a1)
{
    lv_style_set_radius(o, a0);
    lv_style_set_bg_opa(o, a1);
}

void alarm_ui_run_63(lv_style_t * o, int32_t a0, int32_t a1)
{
    lv_style_set_height(o, a0);
    lv_style_set_radius(o, a1);
}

void alarm_ui_run_64(lv_style_t * o, int32_t a0, lv_color_t a1)
{
    lv_style_set_border_width(o, a0);
    lv_style_set_border_color(o, a1);
}

void alarm_ui_run_65(lv_obj_t * o, int32_t a0, int32_t a1)
{
    lv_obj_set_style_flex_cross_place(o, a0, 0);
    lv_obj_set_style_pad_column(o, a1, 0);
}

lv_obj_t * alarm_ui_run_66(lv_obj_t * parent, int32_t a0)
{
    lv_obj_t * o = lv_label_create(parent);
    lv_label_set_long_mode(o, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_max_width(o, a0, 0);
    return o;
}

void alarm_ui_run_67(lv_obj_t * o, lv_style_t * a0, int32_t a1, const char * a2)
{
    lv_obj_add_style(o, a0, a1);
    lv_obj_set_name(o, a2);
}

lv_obj_t * alarm_ui_run_68(lv_obj_t * parent, const char * a0)
{
    lv_obj_t * o = time_rollers_create(parent, ROLLER_W, ROLLER_AMPM_W);
    lv_obj_set_name(o, a0);
    return o;
}

void alarm_ui_run_69(lv_obj_t * o, lv_color_t a0, int32_t a1, int32_t a2, int32_t a3, int32_t a4)
{
    lv_obj_set_style_bg_grad_color(o, a0, 0);
    lv_obj_set_style_bg_grad_dir(o, a1, 0);
    lv_obj_set_style_bg_main_opa(o, a2, 0);
    lv_obj_set_style_bg_grad_opa(o, a3, 0);
    lv_obj_set_style_bg_main_stop(o, 0, 0);
    lv_obj_set_style_bg_grad_stop(o, a4, 0);
}

void alarm_ui_run_70(lv_obj_t * o, int32_t a0, int32_t a1)
{
    lv_obj_set_style_pad_top(o, a0, 0);
    lv_obj_set_style_pad_bottom(o, a1, 0);
}

lv_obj_t * alarm_ui_run_71(lv_obj_t * parent, const char * a0)
{
    lv_obj_t * o = day_toggle_create(parent);
    lv_obj_set_name(o, a0);
    return o;
}

void alarm_ui_run_72(lv_obj_t * o, int32_t a0, lv_subject_t * a1, int32_t a2)
{
    lv_obj_set_style_pad_row(o, a0, 0);
    lv_obj_bind_flag_if_not_eq(o, a1, LV_OBJ_FLAG_HIDDEN, a2);
}

void alarm_ui_run_73(lv_obj_t * o, const char * a0)
{
    lv_label_set_text(o, a0);
    lv_obj_set_style_text_font(o, font_title, 0);
}

void alarm_ui_run_74(lv_obj_t * o, int32_t a0)
{
    lv_obj_set_style_text_font(o, font_title, 0);
    lv_obj_bind_flag_if_not_eq(o, &subject_zone_filter, LV_OBJ_FLAG_HIDDEN, a0);
}
