/**
 * @file icon_chip_gen.h
 */

#ifndef ICON_CHIP_H
#define ICON_CHIP_H

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

lv_obj_t * icon_chip_create(lv_obj_t * parent, const char * glyph, int32_t size, const lv_font_t * font, lv_color_t color);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*ICON_CHIP_H*/
