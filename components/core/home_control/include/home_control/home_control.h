/*
 * home_control.h - the Devices registry (docs/features/24): merges the device sources, applies
 * the security classes (HCTL-005: unlock, opening and any cover position short of fully closed
 * ask for a user code, judged by effect - the platform checks it with the engine before calling
 * hc_act), and tracks one pending action per device with a bounded lifetime and its result
 * (HCTL-006, HCTL-012): the platform calls hc_tick so a lost reply or a dropped connection
 * fails it instead of leaving the device busy forever.
 *
 * Portable C11, caller-owned state, no heap. The platform wraps it with its lock
 * (device_service on the panel, the fake world in the PC preview).
 */
#ifndef HOME_CONTROL_H
#define HOME_CONTROL_H

#include "home_control/device_source.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Home Assistant, Zigbee, Z-Wave. hc_refresh fills HC_MAX_DEVICES in registration order: the
 * platform registers Z-Wave (dsrc_zw, at most DSRC_ZW_MAX devices) BEFORE Home Assistant, so a
 * long Home Assistant list can't crowd the panel's own outputs out. */
#define HC_MAX_SOURCES 3
#define HC_MAX_DEVICES 64
#define HC_ERROR_LEN   24
/* HCTL-012: how long hc_act's caller has to hear back before hc_tick fails it as "timeout". */
#define HC_ACT_TIMEOUT_MS UINT64_C(30000)

typedef struct {
    dev_info_t info;
    uint32_t pending;             /* request id of the action under way, 0 = none */
    uint32_t pending_gen;         /* its source's connection generation when sent (HCTL-012) */
    uint64_t pending_deadline_ms; /* hc_tick fails it past this time, 0 = not started yet */
    bool failed;                  /* the last action failed (cleared by the next act) */
    /* dev_action_t of the action under way, or of the last one when it failed - whoever asked
     * for it (the screen, the console, an automation), so the screen can say "Didn't unlock";
     * ACT_NONE when neither. Fills the padding: the row doesn't grow. */
    uint8_t action;
    /* HCTL-012: whether it happened isn't known - the registry's own failure of an action in
     * flight ("timeout": no reply by the deadline; "offline": its connection dropped with it in
     * flight), or a source's answer that it sent the action on and couldn't confirm it (Home
     * Assistant's "timeout": its service call didn't finish in time; the sink's unknown). false
     * for a source's refusal made before anything was sent (Home Assistant's "not_allowed",
     * "unavailable", "failed"; Z-Wave's "offline", "busy", "send_failed"...): those say it
     * wasn't done. Set by where the failure comes from, never by the registry reading its word;
     * read it with hc_outcome_unknown. Fills the padding: the row doesn't grow. */
    bool unsure;
    /* why: "not_allowed", "unavailable", "failed" (the source or HA said so), "timeout" (no
     * reply by the deadline, or HA's service call didn't finish in time), "offline" (its
     * connection dropped while it was in flight, or the source said its device can't be
     * reached) */
    char error[HC_ERROR_LEN];
    /* What it showed when its action failed (BL-175): a refresh that reports something else
     * - its state word, level, setpoint or a scene's run time - clears the failure. */
    uint32_t failed_seen;
} hc_device_t;

typedef struct {
    void *ctx;
    /* The device list or a state changed: the UI re-reads. Also when a send the source refused
     * at once (hc_act's HC_ERR_OFFLINE) cleared the failure its row showed. With result, it tells
     * of every change of a row but one: hc_act's HC_OK (the row shows it pending), which the
     * caller knows of. */
    void (*changed)(void *ctx);
    /* An action finished (ok, or why not); the device row already carries it. request_id is
     * the one hc_act_auto returned (HCTL-012): every action that was sent hears back exactly
     * once - its answer, "timeout", or "offline" (its connection dropped, or a refresh dropped
     * its device). ref may name a device that is no longer listed. */
    void (*result)(void *ctx, uint32_t request_id, const char *ref, bool ok, const char *error);
} hc_port_t;

typedef struct {
    hc_port_t port;
    dsrc_sink_t sink; /* what hc_sink() hands out; its ctx is this hc_t */
    struct {
        const device_source_ops_t *ops;
        void *ctx;
        uint32_t generation; /* bumped when this source's connection drops (HCTL-012) */
        bool was_online;     /* the online() hc_refresh last saw, to catch the drop */
    } sources[HC_MAX_SOURCES];
    size_t n_sources;
    hc_device_t devices[HC_MAX_DEVICES];
    size_t n_devices;
    uint32_t next_request;
    /* hc_refresh's staging area (~15 KB): in here, not on the caller's stack - an hc_t is
     * meant to live in PSRAM on the panel (device_service allocates it there). */
    dev_info_t scratch[HC_MAX_DEVICES];
} hc_t;

void hc_init(hc_t *hc, const hc_port_t *port);
bool hc_register(hc_t *hc, const device_source_ops_t *ops, void *ctx);

/* The dsrc_sink_t a source reports through (register it with the source; ctx = the hc_t). */
const dsrc_sink_t *hc_sink(hc_t *hc);

/* Re-read every source's list (after sink.changed; the platform calls it on its lock). A
 * source whose connection just dropped fails whatever was pending on it as an unknown outcome
 * rather than carrying it forward forever, and a device that left the list fails its pending
 * action the same way: both through port.result as "offline", with the request id (HCTL-012). */
void hc_refresh(hc_t *hc);

/* HCTL-012: call this periodically (e.g. once a second) on the same lock as hc_act/hc_refresh
 * so a command whose reply never arrives fails as "timeout" instead of leaving its device busy
 * forever. now is uint64_t ms from any steady clock; hc keeps no clock of its own. */
void hc_tick(hc_t *hc, uint64_t now);

/* The devices, areas first favorites later (v1: source order). Returns how many. */
size_t hc_list(const hc_t *hc, const hc_device_t **out, size_t max);
const hc_device_t *hc_get(const hc_t *hc, const char *ref);

/* HCTL-012 (BL-171): the device's last action failed with an unknown outcome - it may have
 * happened: no answer by the deadline, or its connection or its row dropped with it in flight
 * (hc_tick's and hc_refresh's own failures), or its source said it sent it on and couldn't
 * confirm it (Home Assistant's "timeout"). False when nothing failed, and for a source's refusal
 * made before anything was sent: a Z-Wave "offline" or "send_failed", Home Assistant's
 * "not_allowed" or "unavailable". The screen says "Couldn't confirm" for true, "didn't" for
 * false. NULL: false. */
bool hc_outcome_unknown(const hc_device_t *d);

/* HCTL-005: this action on this kind, going to this value, asks for a valid user code first -
 * judged by effect: a cover's ACT_SET_LEVEL needs one unless value is 0 (fully closed), the
 * same as ACT_CLOSE; value is ignored for every other pair. The platform checks the code with
 * the engine and only then calls hc_act. */
bool hc_needs_code(dev_kind_t kind, dev_action_t action, int32_t value);

/* The action makes sense for the kind at all (the UI only offers these). */
bool hc_action_valid(dev_kind_t kind, dev_action_t action);

/* An ACT_SET_MODE's mode word, the one hc_act takes for a thermostat (the screen's mode keys, a
 * panel link keypad's DEV_ACT, docs/features/11 LINK-048): DEV_MODE_OFF "off", DEV_MODE_HEAT
 * "heat", DEV_MODE_COOL "cool", DEV_MODE_AUTO "auto" (dsrc_ha sends "heat_cool" where the
 * thermostat has only that). Anything else - 0, DEV_MODE_HEAT_COOL, two bits, an unknown bit -
 * is NULL: the caller refuses the action. */
const char *hc_mode_word(uint8_t bit);

typedef enum {
    HC_OK = 0,
    HC_ERR_UNKNOWN,  /* no such device */
    HC_ERR_OFFLINE,  /* its source can't reach it (HCTL-006) */
    HC_ERR_BUSY,     /* an action is already under way on it */
    HC_ERR_ARG,      /* the action doesn't fit the device */
    HC_ERR_NOT_ALLOWED, /* hc_act_auto: outside what an automation may do (HCTL-011, AUT-002) */
} hc_result_t;          /* append only: values may be logged */

/* Start an action; the outcome arrives through port.result (and the device row). A source that
 * refuses the send at once: HC_ERR_OFFLINE, nothing pending and nothing failed on the row - a
 * failure it showed before is cleared, and port.changed says so. */
hc_result_t hc_act(hc_t *hc, const char *ref, dev_action_t action, int32_t value,
                   const char *mode);

/* AUT-002 / AUT-015 (docs/features/41 D23-D25, ADR-050): the less secure pairs an automation
 * may send only while the installer option "Allow less secure automations" is on - every pair
 * hc_needs_code names (unlock; a cover or valve opened; a cover set short of closed) and a
 * cover's ACT_STOP (it can leave a door part-open). */
bool hc_auto_less_secure(dev_kind_t kind, dev_action_t action, int32_t value);

/* HCTL-011 / AUT-002: an automation may send this action to a device of this kind (docs/features/41
 * DESIGN section 3, ADR-050). True only when hc_action_valid, a mode word comes only with
 * ACT_SET_MODE, and the pair is in the table: light on (value -1 only: 0 is brightness 0), off
 * and toggle; fan on (-1) and off; switch on, off and toggle; lock lock and unlock; cover close,
 * open and stop; valve close and open; climate set-mode with mode exactly "off"; scene run. Never
 * a script, set-level, set-temp or another mode. A less secure pair (hc_auto_less_secure) only
 * when less_secure (the installer option). An automation asks for no HCTL-005 code: it acts as
 * the system master or installer who saved it. Asking a person to confirm a scene, a switch's
 * turn-on or toggle, or a less secure action after a door trigger is the automation layer's job. */
bool hc_auto_allowed(dev_kind_t kind, dev_action_t action, int32_t value, const char *mode,
                     bool less_secure);

/* AUT-002 / AUT-015 (docs/features/41 DESIGN section 3, layer 1): one row an automation's action
 * picker offers - what the touchscreen and the Panel Manager show, and what the save stores. */
typedef struct {
    uint8_t action;          /* dev_action_t */
    int32_t value;           /* -1: none (a light's or a fan's Turn on sends no level) */
    char mode[DEV_MODE_LEN]; /* "off" for a thermostat's SET_MODE; "" otherwise */
} hc_auto_offer_t;
#define HC_AUTO_OFFER_MAX 4

/* The rows the pickers show for a device of this kind from this source (dsrc_kind_t), in the
 * order on, off, toggle, lock, unlock, close, open, stop, set-mode "off", run; returns how many
 * (0..HC_AUTO_OFFER_MAX). Each row passes hc_auto_allowed(kind, action, value, mode,
 * less_secure) - so with the option off no Unlock, Open or Stop row exists (AUT-015) - and is
 * one its source can carry out (dsrc_ha_action_word for Home Assistant, dsrc_zw_level_for for
 * the panel's Z-Wave outputs: no toggle on Z-Wave, no stop for its garage door), so a picked
 * action never ends "unreachable" for being one the source refuses. A source this firmware
 * can't act on (Zigbee, none) gets no rows; a script never does. */
size_t hc_auto_offer(dev_kind_t kind, uint8_t source, bool less_secure,
                     hc_auto_offer_t out[HC_AUTO_OFFER_MAX]);

/* hc_act for an automation: the same checks in the same order, but the action is judged
 * against the device's live kind with hc_auto_allowed(..., less_secure) (HC_ERR_NOT_ALLOWED,
 * nothing sent). *request_id (may be NULL) gets the id port.result will carry, 0 when nothing
 * was sent. */
hc_result_t hc_act_auto(hc_t *hc, const char *ref, dev_action_t action, int32_t value,
                        const char *mode, bool less_secure, uint32_t *request_id);

#ifdef __cplusplus
}
#endif

#endif /* HOME_CONTROL_H */
