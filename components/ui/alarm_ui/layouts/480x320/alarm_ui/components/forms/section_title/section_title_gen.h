/**
 * @file section_title_gen.h
 */

#ifndef SECTION_TITLE_H
#define SECTION_TITLE_H

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

lv_obj_t * section_title_create(lv_obj_t * parent, const char * text);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*SECTION_TITLE_H*/
