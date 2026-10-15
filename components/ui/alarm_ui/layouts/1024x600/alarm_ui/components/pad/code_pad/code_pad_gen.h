/**
 * @file code_pad_gen.h
 */

#ifndef CODE_PAD_H
#define CODE_PAD_H

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

lv_obj_t * code_pad_create(lv_obj_t * parent, int32_t top_grow, int32_t words_max);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*CODE_PAD_H*/
