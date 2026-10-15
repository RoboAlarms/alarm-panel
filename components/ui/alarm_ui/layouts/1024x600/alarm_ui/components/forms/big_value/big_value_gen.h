/**
 * @file big_value_gen.h
 */

#ifndef BIG_VALUE_H
#define BIG_VALUE_H

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

lv_obj_t * big_value_create(lv_obj_t * parent, const char * label, const char * value);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*BIG_VALUE_H*/
