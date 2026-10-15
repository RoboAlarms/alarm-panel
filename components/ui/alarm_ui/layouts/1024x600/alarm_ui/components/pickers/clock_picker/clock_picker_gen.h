/**
 * @file clock_picker_gen.h
 */

#ifndef CLOCK_PICKER_H
#define CLOCK_PICKER_H

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

lv_obj_t * clock_picker_create(lv_obj_t * parent, int32_t cal_w, int32_t day_h, int32_t roll_w, int32_t ampm_w, int32_t gap, int32_t roll_y);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*CLOCK_PICKER_H*/
