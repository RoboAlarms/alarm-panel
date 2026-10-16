/*
 * host_link_demo.h - a demo panel-link backend for the PC preview and simulator: ui_backend_t's
 * link_* calls (ui_backend.h, docs/features/11, BL-048) over a small model, so the touchscreen's
 * link pages (ui_link.c, the setup's role and pairing steps in ui_wizard.c) can be drawn and walked
 * before the real plink_service runs on the PC.
 *
 * The model: Harbor House (192.0.2.42) is the alarm panel; a keypad pairs as number 2 (number 1 is
 * the installer's table's). Off (the start), link_info answers false, as a firmware that hasn't
 * said: every page is as it is without the link, so the walk before the link's segment keeps its
 * pictures. The pairing answers like the protocol's (LINK-016): opening it gives a 2-minute window
 * counted down once a second, a keypad's look counts up, and the walk scripts the other side
 * (a keypad asks, the alarm panel is found, either side answers) with the calls below; the code is
 * 588460 on both screens. KEYPAD_SET and KEYPAD_REMOVE change the model's keypads, KP_DONE makes
 * this panel a keypad (connecting).
 *
 * Interim: plink_service and the PC virtual master and keypad (host/link_sim) replace it; the
 * calls below are what host_panel.c and the preview's walk rely on.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui_backend/ui_backend.h"

/* What host_link_demo_state() sets up at once. */
typedef enum {
    HOST_LINK_OFF = 0,    /* link_info answers false: nothing of the link shows (the start) */
    HOST_LINK_FRESH,      /* a panel on its first start: the alarm panel's role, no keypad */
    HOST_LINK_MASTER,     /* the alarm panel, no keypad on Wi-Fi yet */
    HOST_LINK_MASTER_TWO, /* ... with Hall keypad (2, connected) and Bedroom keypad (3, not
                           * responding for 35 minutes, its trouble raised) */
    HOST_LINK_KEYPAD_CONNECTING, /* keypad 2 of Harbor House, from its start until it answers */
    HOST_LINK_KEYPAD_ONLINE,     /* ... connected */
    HOST_LINK_KEYPAD_LOST,       /* ... no system communication for 6 minutes */
    HOST_LINK_KEYPAD_REMOVED,    /* ... Harbor House removed it */
} host_link_demo_t;

/* Starts the model (off). allowed: may the alarm panel change its keypads now (installer mode with
 * every partition disarmed), as the real service checks it; epoch_ms: the pretend panel's clock. */
void host_link_demo_init(bool (*allowed)(void), int64_t (*epoch_ms)(void));
/* Simulated time passes: the pairing window counts down, a keypad's looking counts up. */
void host_link_demo_advance(uint32_t ms);
/* Fills out's link_* entries (always: off, they answer as a firmware that hasn't said). */
void host_link_demo_backend(ui_backend_t *out);
void host_link_demo_state(host_link_demo_t what);

/* The other side of a pairing, scripted by the walk. On the alarm panel: */
void host_link_demo_keypad_asks(void);     /* a keypad asked: the code to compare */
void host_link_demo_keypad_confirms(void); /* ... and tapped Yes, pair */
/* On a keypad: */
void host_link_demo_master_found(void);  /* Harbor House found, open for pairing: the code */
void host_link_demo_master_allows(void); /* the installer tapped Yes, allow there */
/* Either side: the pairing ends with this failure (UI_LINK_FAIL_*). */
void host_link_demo_pair_fail(uint8_t fail);
/* The installer's table and Zigbee keypads hold these numbers (bit n - 1): with 1-8 all held,
 * opening pairing answers FULL (LINK-001). */
void host_link_demo_numbers_held(uint16_t mask);
/* A keypad: a panic still being sent (LINK-023), 10 minutes without the alarm panel (LINK-025). */
void host_link_demo_panic_pending(bool on);
void host_link_demo_maintenance(bool on);
/* A keypad: the alarm panel moved it to partition home (any number, to try the range check), a new
 * home_gen. */
void host_link_demo_home(uint8_t home);
/* The last request the UI made, kind 0 before any (the walk's checks). */
void host_link_demo_last_request(ui_link_req_t *out);
/* How many keypads the alarm panel has, and one of them by number (false: none). */
size_t host_link_demo_keypads(void);
bool host_link_demo_keypad(uint8_t device, ui_link_keypad_t *out);
