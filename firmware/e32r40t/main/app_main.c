/*
 * app_main.c - LCDWiki 4.0" ESP32-32E display (E32R40T) alarm panel.
 *
 * The start-up every board shares is components/app/panel_app (docs/features/33 step 7). This board
 * adds its console commands for bringing the panel up: `lcd` (colour order, inversion,
 * orientation, live) and `touch` (the calibration and raw readings).
 */
#include "board.h"
#include "panel_app.h"
#include "panel_ota.h"

void app_main(void)
{
    static const panel_app_config_t cfg = {
        /* NULL model strings: the integrations name the board (board_name()). */
        /* The touch UI isn't proven on this board yet (docs/features/19 Phase 3), so a new firmware
         * on probation has to show the engine, the event log and the zone sources working
         * (R18), not the screen. */
        .health_mask = PANEL_OTA_OK_ENGINE | PANEL_OTA_OK_LOG | PANEL_OTA_OK_SENSORS,
        .console = board_e32r40t_console,
    };
    panel_app_run(&cfg);
}
