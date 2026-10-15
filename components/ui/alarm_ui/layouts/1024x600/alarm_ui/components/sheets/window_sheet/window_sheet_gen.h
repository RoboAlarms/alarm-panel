/**
 * @file window_sheet_gen.h
 */

#ifndef WINDOW_SHEET_H
#define WINDOW_SHEET_H

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

lv_obj_t * window_sheet_create(lv_obj_t * parent);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*WINDOW_SHEET_H*/
