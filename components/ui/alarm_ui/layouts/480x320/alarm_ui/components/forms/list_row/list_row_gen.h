/**
 * @file list_row_gen.h
 */

#ifndef LIST_ROW_H
#define LIST_ROW_H

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

lv_obj_t * list_row_create(lv_obj_t * parent, const char * glyph, lv_color_t color, const char * title, const char * detail, const char * value, const char * trail);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LIST_ROW_H*/
