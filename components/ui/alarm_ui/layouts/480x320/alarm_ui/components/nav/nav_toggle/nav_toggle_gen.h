/**
 * @file nav_toggle_gen.h
 */

#ifndef NAV_TOGGLE_H
#define NAV_TOGGLE_H

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

lv_obj_t * nav_toggle_create(lv_obj_t * parent, lv_subject_t * icon, const char * text);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*NAV_TOGGLE_H*/
