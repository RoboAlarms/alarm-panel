/**
 * @file chip_gen.h
 */

#ifndef CHIP_H
#define CHIP_H

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

lv_obj_t * chip_create(lv_obj_t * parent, const char * text, lv_subject_t * subject);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*CHIP_H*/
