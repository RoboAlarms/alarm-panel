/**
 * @file row_switch_icon_gen.h
 */

#ifndef ROW_SWITCH_ICON_H
#define ROW_SWITCH_ICON_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
    #include "lvgl.h"
    #include "lvgl_private.h"
#else
    #include "lvgl/lvgl.h"
    #include "lvgl/lvgl_private.h"
#endif

#if defined(LV_USE_XML) && LV_USE_XML
    #include "lv_xml/lv_xml.h"
#endif

lv_obj_t * row_switch_icon_create(lv_obj_t * parent, const char * glyph, lv_color_t color, const char * title, const char * detail);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*ROW_SWITCH_ICON_H*/
