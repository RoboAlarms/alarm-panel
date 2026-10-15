/*
 * board.h - CrowPanel Advanced 10.1" (ESP32-P4, rev V1.2) board support.
 *
 * The board contract and the CrowPanel P4 family's extras come from the family library
 * (components/boards/crowpanel_p4_family, crowpanel_p4.h); this board adds its description
 * (board_info(), board_name()), its wiring (board_pins.h) and its names in MQTT and Home
 * Assistant. Bring-up order: see board_api.h and docs/features/06-CrowPanel-P4-Platform.
 */
#pragma once

#include "board_pins.h"
#include "crowpanel_p4.h"

#define BOARD_MQTT_MODEL "CrowPanel Advanced 10.1\" (ESP32-P4)"
#define BOARD_HA_MODEL   "CrowPanel Advanced 10.1"
