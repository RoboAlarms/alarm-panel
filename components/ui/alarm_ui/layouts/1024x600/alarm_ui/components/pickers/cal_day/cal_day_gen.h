/**
 * @file cal_day_gen.h
 */

#ifndef CAL_DAY_H
#define CAL_DAY_H

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

lv_obj_t * cal_day_create(lv_obj_t * parent);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*CAL_DAY_H*/
