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
 * Smart Home on a keypad (BL-216; docs/features/11 "Smart Home and Home Assistant on keypads",
 * proposal P1): host_link_demo_backend also wraps the backend's dev_list and dev_act, which are the
 * pretend panel's own devices (host_panel.c's house). Off, or as the alarm panel, they answer as
 * they are. As a keypad they answer as plink_backend.c's kp_dev_list and kp_dev_act do, over that
 * house as its alarm panel's: no device before the list is in (connecting, not paired - no Smart
 * Home on the rail); while the link is lost, or no device data came for 30 s (LINK-046), every
 * device offline since the last device data (one its alarm panel already said is offline keeps
 * its own time) and a tap answered at once with nothing sent; otherwise a tap answered at once
 * where the keypad can tell (unreachable, refused, a code needed) and else judged by the alarm
 * panel (its dev_act). With the alarm panel's answers lost, a tap shows its device pending and,
 * 3 s on, failed and not confirmed (the keypad's give-up, HCTL-012), until the device's next
 * record or the next tap on it. (On a panel a tap with a code holds the pad for those 3 s.)
 *
 * Keypad updates (docs/features/11 "Updates through the link", proposal P2): each keypad of the
 * alarm panel has a part in its updates (ui_link_keypad_t.fw_state, fw_percent and the version it
 * runs), set by host_link_demo_keypad_fw to any UI_LINK_FW_* state; a keypad follows its alarm
 * panel's updates once host_link_demo_fw_follows says so (its own run is host_panel_fw_preset's).
 *
 * Interim: plink_service and the PC virtual master and keypad (host/link_sim) replace it; the
 * calls below are what host_panel.c and the preview's walk rely on.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
void host_link_demo_partition_policy(bool allowed, bool never, uint16_t seconds);
/* A keypad's alarm panel's pages (BL-223): the groups it serves (UI_LINK_AREA_*; 0, an older
 * alarm panel, as every keypad state starts), whether this keypad holds installer mode, and
 * who does otherwise (UI_LINK_HOLDER_* or a keypad number). Each change moves remote_gen. */
void host_link_demo_remote(uint32_t areas, bool installer, uint8_t holder);

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
    HOST_LINK_MASTER_THREE, /* the alarm panel with Hall keypad (2), Bedroom keypad (3) and
                             * Kitchen (4), all connected, on 0.6.1 and up to date */
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
/* A keypad without the alarm panel keeping a fire or CO tone (decided 2026-10-15: until OFF here
 * or the link returns); the walk clears it after Silence, as the service does. */
void host_link_demo_local_alarm(bool on);
/* A keypad that hears its alarm panel but can't read its configuration: it needs an update. */
void host_link_demo_needs_update(bool on);
/* A keypad: the alarm panel moved it to partition home (any number, to try the range check), a new
 * home_gen. */
void host_link_demo_home(uint8_t home);
/* A keypad (BL-216): its alarm panel still answers but its device data stops (on), so 30 s on
 * every device reads offline (LINK-046); off, the data comes again. */
void host_link_demo_devices_stop(bool on);
/* A keypad: the alarm panel's answers to its device actions are lost (on). */
void host_link_demo_devices_unanswered(bool on);
/* The alarm panel: keypad `device`'s part in its updates (UI_LINK_FW_*), percent done while it
 * downloads or receives, and the version it runs (NULL: as it was). RESTARTING takes it off the
 * link (not connected, its trouble held); the next state brings it back connected. */
void host_link_demo_keypad_fw(uint8_t device, const char *fw, uint8_t state, uint8_t percent);
/* A keypad: its alarm panel sends it its updates (follows), the download coming from the alarm
 * panel over the link (from_master) or from the update server. */
void host_link_demo_fw_follows(bool follows, bool from_master);
/* The last request the UI made, kind 0 before any (the walk's checks). */
void host_link_demo_last_request(ui_link_req_t *out);
/* How many keypads the alarm panel has, and one of them by number (false: none). */
size_t host_link_demo_keypads(void);
bool host_link_demo_keypad(uint8_t device, ui_link_keypad_t *out);

/* Enrollment scenarios: requesting, joined, unavailable and radio link loss. */
void host_link_demo_wifi_setup(uint8_t state, bool online);
/* Stored enrollment appearing after the UI starts, as after interrupted setup. */
void host_link_demo_resume_pairing(bool published);
/* The pairing ended (removed at the alarm panel, its record erased) before the setup's Done. */
void host_link_demo_unpaired(void);
