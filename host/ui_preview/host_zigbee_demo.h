/*
 * host_zigbee_demo.h - an interim Zigbee backend for the PC preview and simulator: ui_backend_t's
 * zb_* calls (ui_backend.h) over a small demo model, so the touchscreen's Zigbee pages
 * (ui_zigbee.c, BL-144, docs/features/20 devices.md §9) can be drawn and walked before the real
 * zigbee_zones panel runs on the PC.
 *
 * The model: the drawing's fictional devices (screens/zigbee.js), the radio working on channel 15,
 * two install codes waiting, and an add that listens for 3 minutes - a door sensor joins 5 s in
 * and is set up step by step (2 s each) until it is Ready. Ping, Blink, Set it up again, Rename,
 * Forget, Remove, Replace, Try again (a paused radio) and Dismiss answer like the real backend,
 * each operation ending about a second later; so do the install codes (each checked as the real
 * backend checks it: its length, its CRC, the address), the channel move and noise check, the
 * radio's restart and the network's reset (the installer code checked by the engine, through
 * host_panel.c's hook). Removing a sleeping device waits up to 2 minutes for it to wake. The
 * siren, keypad and key fob have the settings the proposal draws (pages 14-16), and SIREN,
 * SIREN_TEST, KEYPAD and FOB are answered as zbz_panel answers them (alarm-devices.md): a
 * keypad's number given by the panel (FULL when none of 1-8 is free), one fob for each user
 * (IN_USE), "Test it" an operation of 2 s whose end is the page's last_test.
 *
 * Interim: a later unit replaces these insides with zbz_panel and a fake radio; the calls below
 * are what host_panel.c and the preview's walk rely on, and they stay.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui_backend/ui_backend.h"

/* What host_zigbee_demo_state() sets up at once, so the preview's walk can show every page. */
typedef enum {
    HOST_ZB_WORKING = 0,   /* the radio works on channel 15, the eight demo devices (the start) */
    HOST_ZB_EMPTY,         /* ... with no devices */
    HOST_ZB_STARTING,      /* the radio starts */
    HOST_ZB_NOT_CONNECTED, /* nothing in the wireless socket */
    HOST_ZB_NOT_ANSWERING, /* it answered once and went silent */
    HOST_ZB_PAUSED,        /* the restart guard paused it after two restarts */
    HOST_ZB_SOCKET_ZWAVE,  /* the socket holds the Z-Wave radio */
    HOST_ZB_INTERFERENCE,  /* working, and its channel is drowned out */
    /* an add (UI_ZB_REQ_ADD) that runs: what happens next */
    HOST_ZB_JOIN_TWO,      /* a door sensor joined and is Ready, a motion sensor is being read */
    HOST_ZB_DOZE,          /* ... and the motion sensor fell asleep before it finished */
    HOST_ZB_WAKE,          /* ... it woke and finished: Ready, in no zone */
    HOST_ZB_JOIN_KNOWN,    /* a device already in the network joined again */
    HOST_ZB_JOIN_PLUG,     /* a smart plug joined: nothing the panel uses as a zone */
    HOST_ZB_JOIN_FAILS,    /* one began to join and didn't finish */
    HOST_ZB_WINDOW_ENDS,   /* the 3 minutes ran out now */
    /* a remove (UI_ZB_REQ_REMOVE) of a sleeping device that waits for it to wake */
    HOST_ZB_REMOVE_ANSWERS,  /* it woke and left the network */
    HOST_ZB_REMOVE_NO_ANSWER, /* the wait ran out: it didn't answer */
    HOST_ZB_JOIN_ONE,      /* an add that runs: one door sensor joined and is Ready */
    /* the tier B roles (BL-147, BL-148) */
    HOST_ZB_SIREN_SILENT,  /* the siren's next "Test it" gets no answer */
    HOST_ZB_KEYPAD_NO_SLOT, /* panel-link keypads hold the numbers 1-8 and the Zigbee keypad has
                             * none: its page says no keypad number is free (FULL) */
    HOST_ZB_KEYPAD_RACE,   /* the Zigbee keypad has no number yet, one is free when its page is
                            * read, and panel-link keypads take the last ones just before its next
                            * request: that request is refused FULL */
    HOST_ZB_FOB_TAKEN,     /* the installer (user 1) has another key fob already: a key fob given
                            * to the installer is refused IN_USE */
    HOST_ZB_ROLE_INVALID,  /* the next siren, keypad or key fob request is refused INVALID (a
                            * setting this panel can't use) */
    HOST_ZB_SIREN_ARMED,   /* a partition is armed: the next siren test is refused NOT_ALLOWED */
} host_zb_demo_t;

/* Which of the model's devices host_zigbee_demo_publish() gives the zone source. */
typedef enum {
    HOST_ZB_PUBLISH_ZONES = 0, /* the sensors of zones 1 and 5 (the proposal's Front Door and Hallway
                                * Motion), each zone taking its own as its sensor */
    HOST_ZB_PUBLISH_NEWEST,    /* the newest ready sensor (one just added from the sensor picker) */
} host_zb_publish_t;

/* What the reset's installer code check said (UI_ZB_REQ_RESET, ZB-017). */
typedef enum {
    HOST_ZB_CODE_OK = 0, /* the installer's code */
    HOST_ZB_CODE_WRONG,  /* any other code, counted toward the keypad's lockout */
    HOST_ZB_CODE_LOCKED, /* too many wrong codes */
} host_zb_code_t;

/* What the demo needs from the pretend panel around it (host_panel.c); any may be NULL. */
typedef struct {
    host_zb_code_t (*installer_code)(const char *code); /* the engine's check */
    uint32_t (*now_s)(void);                            /* Unix seconds; 0 the clock isn't set */
    /* the zone source sees a device's input (zsrc_zigbee, as the radio would describe it) ... */
    void (*join)(uint64_t ieee, uint8_t endpoint, uint8_t device_class, const char *manufacturer,
                 const char *model, bool router);
    /* ... and a zone takes it as its sensor */
    void (*bind)(uint16_t zone, zsrc_address_t addr, uint8_t device_class);
} host_zb_hooks_t;

/* Starts the model (HOST_ZB_WORKING); `allowed` says whether a change may be made now (installer
 * mode with every partition disarmed), as the real backend checks it itself. */
void host_zigbee_demo_init(bool (*allowed)(void));
/* Simulated time passes (the add's countdown, joins, operations ending). */
void host_zigbee_demo_advance(uint32_t ms);
/* Fills out's zb_* entries. */
void host_zigbee_demo_backend(ui_backend_t *out);
/* Sets up a state at once (above). Once called, the add no longer runs by itself: the caller
 * scripts what joins. */
void host_zigbee_demo_state(host_zb_demo_t what);
/* The pretend panel's hooks (copied). */
void host_zigbee_demo_hooks(const host_zb_hooks_t *hooks);
/* The next reset's installer code check answers `answer` instead of asking hooks.installer_code
 * (once): the walk reaches the keypad's lockout without locking the pretend panel's keypad. */
void host_zigbee_demo_code_once(host_zb_code_t answer);
/* Some of the model's ready sensors, given to the zone source (hooks.join) as the real panel's zone
 * source knows them, so the zone page and the sensor picker show them (interim: the real backend
 * has one world). A device given already is left alone. */
void host_zigbee_demo_publish(host_zb_publish_t which);
