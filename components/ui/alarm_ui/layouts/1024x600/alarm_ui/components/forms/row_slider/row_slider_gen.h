/**
 * @file row_slider_gen.h
 */

#ifndef ROW_SLIDER_H
#define ROW_SLIDER_H

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

lv_obj_t * row_slider_create(lv_obj_t * parent, const char * glyph, const char * title, const char * value);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*ROW_SLIDER_H*/
