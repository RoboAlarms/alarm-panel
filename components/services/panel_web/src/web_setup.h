#pragma once
/* web_setup.h - setting up a panel without a screen from a phone, and getting back into one
 * (docs/features/31 headless-setup.md, approved 2026-10-18; docs/features/42). The adapter of
 * web_setup_flow.c: its stored record, the setup label's secrets, the setup Wi-Fi, the accounts
 * and the recovery key, the network trial. panel_web.c owns HTTP; everything here runs on the
 * HTTPS worker except web_setup_poll() (the maintenance task) and the BOOT button's timer, and a
 * lock keeps them apart.
 *
 * Only a headless master (no touch UI, the link's master or no link) ever opens the setup Wi-Fi
 * or the setup listener: a panel with a screen keeps the Panel Manager off until someone turns
 * it on, as before. */
#include "alarm/alarm_types.h"
#include "cJSON.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* Called once by panel_web_start (it writes flash): loads the record and the label, makes them
 * on first start, and opens the setup Wi-Fi while setup isn't finished. fingerprint: the HTTPS
 * certificate's SHA-256 in hex (the label shows it). enable_web: turns the ordinary Panel
 * Manager on without restarting the server (once the accounts exist). */
esp_err_t web_setup_init(bool headless_master, const char *fingerprint,
                         esp_err_t (*enable_web)(void));

/* The HTTPS server must run for setup or recovery (even with the Panel Manager switched off). */
bool web_setup_listener(void);
/* Nobody has claimed the panel yet: only GET/POST /api/v1/setup answer. */
bool web_setup_unclaimed(void);

/* GET /api/v1/setup: what the setup pages need, nothing secret. via_ap: the request came over
 * the setup Wi-Fi; user: the signed-in person (0: nobody). */
cJSON *web_setup_status(bool via_ap, uint8_t user);

/* POST /api/v1/setup {"op": ...}. user/authority: the signed-in person (0: nobody); locked: a
 * partition is armed or an alarm sounds. Answers the reply, or NULL with *status ("409
 * Conflict") and *why (an error word of the Panel Manager's) set. */
cJSON *web_setup_action(const cJSON *o, uint8_t user, alarm_authority_t authority, bool via_ap,
                        bool locked, const char **status, const char **why);

/* Once a second (the maintenance task): claims and windows run out, a network trial that wasn't
 * confirmed in time goes back, the setup Wi-Fi follows. */
void web_setup_poll(void);

/* The board's BOOT button (true while held), sampled every 50 ms from here on: held five seconds
 * and let go, it opens recovery for five minutes (the setup Wi-Fi and listener). */
void web_setup_button(bool (*down)(void));

/* The setup Wi-Fi's name server (web_setup_dns.c): every name answered with the panel while setup
 * or recovery is open. Started by web_setup_init on a board without a screen. */
void web_setup_dns_start(void);

/* Setup over USB (web_setup_usb.c): `provision accounts|network|finish|cancel|status ...`, one
 * "PROVISION ..." line answered; the console's `web provision` in development builds, the USB
 * port's own lines in a release build (web_setup_usb_start, started by web_setup_init). */
int web_setup_provision(int argc, char **argv);
void web_setup_usb_start(void);

/* For the service console: the setup label (setup Wi-Fi, its password, the setup code, the
 * certificate's fingerprint) and the stage. */
void web_setup_print_label(void);
