/**
 * @file arm_tile_gen.h
 */

#ifndef ARM_TILE_H
#define ARM_TILE_H

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

lv_obj_t * arm_tile_create(lv_obj_t * parent, const char * glyph, const char * title, const char * subtitle, lv_color_t color);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*ARM_TILE_H*/
