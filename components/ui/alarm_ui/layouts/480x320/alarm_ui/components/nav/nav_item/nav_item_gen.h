/**
 * @file nav_item_gen.h
 */

#ifndef NAV_ITEM_H
#define NAV_ITEM_H

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

lv_obj_t * nav_item_create(lv_obj_t * parent, const char * glyph, const char * text);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*NAV_ITEM_H*/
