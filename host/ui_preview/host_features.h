/*
 * host_features.h - the board's optional parts, as the PC builds see them (docs/features/33).
 *
 * cmake/alarm_board.cmake passes each option of the board's manifest as CONFIG_<option>=0|1,
 * as sdkconfig.h gives them to its firmware. The simulated panel (host_panel.c) offers only what
 * that firmware's backend would (components/services/ui_port_esp, and the Panel Manager's
 * additions), and the scripted walk (main.c) visits only the pages the board has. A build
 * without a board profile (the minimal UI smoke test) gets everything, as before profiles.
 */
#pragma once

#ifndef CONFIG_ALARM_FEATURE_MQTT
#define CONFIG_ALARM_FEATURE_MQTT 1
#endif
#ifndef CONFIG_ALARM_FEATURE_NOTIFY
#define CONFIG_ALARM_FEATURE_NOTIFY 1
#endif
#ifndef CONFIG_ALARM_FEATURE_HA
#define CONFIG_ALARM_FEATURE_HA 1
#endif
#ifndef CONFIG_ALARM_FEATURE_STORAGE
#define CONFIG_ALARM_FEATURE_STORAGE 1
#endif
#ifndef CONFIG_ALARM_FEATURE_PHOTO_SAVER
#define CONFIG_ALARM_FEATURE_PHOTO_SAVER 1
#endif
#ifndef CONFIG_ALARM_PANEL_WEB
#define CONFIG_ALARM_PANEL_WEB 1
#endif
#ifndef CONFIG_ALARM_HOME_CONTROL
#define CONFIG_ALARM_HOME_CONTROL 1
#endif
#ifndef CONFIG_ALARM_FEATURE_AUTOMATIONS
#define CONFIG_ALARM_FEATURE_AUTOMATIONS 1
#endif
/* The voice (docs/features/08 voice-first-build.md): until the boards' manifests carry it, on. */
#ifndef CONFIG_ALARM_FEATURE_VOICE
#define CONFIG_ALARM_FEATURE_VOICE 1
#endif
#ifndef CONFIG_ALARM_DEV_REMOTE
#define CONFIG_ALARM_DEV_REMOTE 1
#endif
