/**
 * @file panic_button_gen.h
 */

#ifndef PANIC_BUTTON_H
#define PANIC_BUTTON_H

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

lv_obj_t * panic_button_create(lv_obj_t * parent, const char * glyph, const char * text, lv_color_t color);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*PANIC_BUTTON_H*/
