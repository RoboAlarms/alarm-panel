/**
 * @file pad_key_gen.h
 */

#ifndef PAD_KEY_H
#define PAD_KEY_H

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

lv_obj_t * pad_key_create(lv_obj_t * parent, const char * text, const lv_font_t * font);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*PAD_KEY_H*/
