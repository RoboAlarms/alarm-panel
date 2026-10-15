/**
 * @file badge_gen.h
 */

#ifndef BADGE_H
#define BADGE_H

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

lv_obj_t * badge_create(lv_obj_t * parent, lv_subject_t * subject, lv_color_t color);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*BADGE_H*/
