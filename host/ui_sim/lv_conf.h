/*
 * lv_conf.h - LVGL configuration of the interactive simulator. The same settings as the
 * scripted preview (host/ui_preview/lv_conf.h), plus a real window.
 *
 * The Windows backend runs the window on its own thread and takes lv_lock() around what it
 * does there, so LV_USE_OS has to be a real one - with LV_OS_NONE those locks do nothing and
 * the window thread would race the simulator's own lv_timer_handler().
 */
#ifndef UI_SIM_LV_CONF_H
#define UI_SIM_LV_CONF_H

#include "../ui_preview/lv_conf.h"

#ifdef _WIN32
#undef LV_USE_OS
#define LV_USE_OS     LV_OS_WINDOWS
#define LV_USE_WINDOWS 1
#endif

#define LV_USE_CANVAS 1

#endif /* UI_SIM_LV_CONF_H */
