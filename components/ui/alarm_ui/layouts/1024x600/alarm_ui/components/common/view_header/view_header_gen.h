/**
 * @file view_header_gen.h
 */

#ifndef VIEW_HEADER_H
#define VIEW_HEADER_H

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

lv_obj_t * view_header_create(lv_obj_t * parent, const char * title, lv_subject_t * note);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*VIEW_HEADER_H*/
