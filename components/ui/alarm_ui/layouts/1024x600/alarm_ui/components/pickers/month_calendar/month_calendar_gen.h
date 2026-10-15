/**
 * @file month_calendar_gen.h
 */

#ifndef MONTH_CALENDAR_H
#define MONTH_CALENDAR_H

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

lv_obj_t * month_calendar_create(lv_obj_t * parent, int32_t cal_w, int32_t day_h);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*MONTH_CALENDAR_H*/
