/**
 * @file screen_main_gen.h
 */

#ifndef SCREEN_MAIN_H
#define SCREEN_MAIN_H

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

lv_obj_t * screen_main_create(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*SCREEN_MAIN_H*/
