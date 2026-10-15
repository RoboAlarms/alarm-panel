/*
 * zsrc_zigbee.c - Zigbee sensors as zones (see zsrc_zigbee.h). Portable: components/core/zigbee_host
 * feeds it on the panel, the host tests and the PC preview feed it directly.
 */
#include <stdio.h>
#include <string.h>

#include "zone_hub/zsrc_zigbee.h"

/* ---- small helpers ------------------------------------------------------------------------- */

static uint64_t add_ms(uint64_t t, uint64_t ms)
{
    return (t > UINT64_MAX - ms) ? UINT64_MAX : t + ms;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%.31s", src != NULL ? src : "");
}

const char *zsrc_zb_ieee_str(uint64_t ieee, char *buf, size_t len)
{
    snprintf(buf, len, "0x%08lX%08lX", (unsigned long)(ieee >> 32),
             (unsigned long)(ieee & 0xFFFFFFFFu));
    return buf;
}

zsrc_address_t zsrc_zb_address(uint64_t ieee, uint8_t endpoint)
{
    return zsrc_address(ZSRC_ZIGBEE, endpoint, ieee);
}

alarm_device_class_t zsrc_zb_class_of_zone_type(uint16_t zone_type)
{
    switch (zone_type) {
    case ZSRC_ZB_ZONE_TYPE_MOTION: return ALARM_DEVCLASS_MOTION;
    case ZSRC_ZB_ZONE_TYPE_CONTACT: return ALARM_DEVCLASS_DOOR;
    case ZSRC_ZB_ZONE_TYPE_HANDLE: return ALARM_DEVCLASS_DOOR;
    case ZSRC_ZB_ZONE_TYPE_FIRE: return ALARM_DEVCLASS_SMOKE;
    case ZSRC_ZB_ZONE_TYPE_WATER: return ALARM_DEVCLASS_WATER;
    case ZSRC_ZB_ZONE_TYPE_CO: return ALARM_DEVCLASS_CO;
    case ZSRC_ZB_ZONE_TYPE_PERSONAL: return ALARM_DEVCLASS_PANIC_BUTTON;
    case ZSRC_ZB_ZONE_TYPE_VIBRATION: return ALARM_DEVCLASS_VIBRATION;
    case ZSRC_ZB_ZONE_TYPE_REMOTE: return ALARM_DEVCLASS_KEYFOB;
    case ZSRC_ZB_ZONE_TYPE_KEYFOB: return ALARM_DEVCLASS_KEYFOB;
    case ZSRC_ZB_ZONE_TYPE_GLASS_BREAK: return ALARM_DEVCLASS_GLASS_BREAK;
    default: return ALARM_DEVCLASS_GENERIC; /* CIE, keypads, sirens: not a sensor we can use */
    }
}

alarm_device_class_t zsrc_zb_class_of_clusters(uint16_t clusters)
{
    if ((clusters & ZSRC_ZB_CL_OCCUPANCY) != 0u) {
        return ALARM_DEVCLASS_MOTION;
    }
    if ((clusters & ZSRC_ZB_CL_ON_OFF) != 0u) {
        return ALARM_DEVCLASS_DOOR; /* a contact that reports as a switch (ZB-004) */
    }
    if ((clusters & ZSRC_ZB_CL_TEMP) != 0u) {
        return ALARM_DEVCLASS_TEMPERATURE;
    }
    return ALARM_DEVCLASS_GENERIC;
}

/* Clusters we can actually turn into zone events. */
static bool usable_clusters(uint16_t clusters)
{
    return (clusters & (uint16_t)(ZSRC_ZB_CL_IAS_ZONE | ZSRC_ZB_CL_ON_OFF | ZSRC_ZB_CL_OCCUPANCY |
                                  ZSRC_ZB_CL_TEMP)) != 0u;
}

uint16_t zsrc_zb_cluster_bit(uint16_t cluster)
{
    switch (cluster) {
    case ZSRC_ZB_ZCL_IAS_ZONE: return ZSRC_ZB_CL_IAS_ZONE;
    case ZSRC_ZB_ZCL_ON_OFF: return ZSRC_ZB_CL_ON_OFF;
    case ZSRC_ZB_ZCL_OCCUPANCY: return ZSRC_ZB_CL_OCCUPANCY;
    case ZSRC_ZB_ZCL_TEMP_MEAS: return ZSRC_ZB_CL_TEMP;
    case ZSRC_ZB_ZCL_POWER_CFG: return ZSRC_ZB_CL_POWER_CFG;
    case ZSRC_ZB_ZCL_POLL_CTRL: return ZSRC_ZB_CL_POLL_CTRL;
    case ZSRC_ZB_ZCL_TUYA: return ZSRC_ZB_CL_TUYA;
    case ZSRC_ZB_ZCL_IAS_WD: return ZSRC_ZB_CL_IAS_WD;
    case ZSRC_ZB_ZCL_IAS_ACE: return ZSRC_ZB_CL_IAS_ACE;
    case ZSRC_ZB_ZCL_IDENTIFY: return ZSRC_ZB_CL_IDENTIFY;
    case ZSRC_ZB_ZCL_BASIC: return ZSRC_ZB_CL_BASIC;
    case ZSRC_ZB_ZCL_OTA: return ZSRC_ZB_CL_OTA;
    case ZSRC_ZB_ZCL_DIAGNOSTICS: return ZSRC_ZB_CL_DIAGNOSTICS;
    default: return 0u;
    }
}

/* A device that listens all the time (a router, or an end device with its receiver on) can be
 * spoken to at once; a sleepy one only in its wake windows (ZB-003, ZB-005). */
static bool listening(uint8_t logical_type, uint8_t mac_caps)
{
    return logical_type == ZSRC_ZB_LOGICAL_ROUTER || logical_type == ZSRC_ZB_LOGICAL_COORDINATOR ||
           (mac_caps & ZSRC_ZB_MAC_RX_ON_IDLE) != 0u;
}

/* Basic's power source says mains (or DC) and no backup battery. */
static bool mains_only(uint8_t power_source)
{
    const unsigned ps = (unsigned)power_source & 0x7Fu;
    const bool backup = ((unsigned)power_source & 0x80u) != 0u;
    return !backup && (ps == 1u || ps == 2u || ps == 4u || ps == 5u || ps == 6u);
}

/* ---- devices in their setup, devices forgotten ---------------------------------------------- */

static bool in_setup(const zsrc_zb_t *z, uint64_t ieee)
{
    for (size_t i = 0; i < ZSRC_ZB_SETUP_MAX; i++) {
        if (z->setup[i] == ieee && ieee != 0u) {
            return true;
        }
    }
    return false;
}

static void setup_mark(zsrc_zb_t *z, uint64_t ieee, bool on)
{
    for (size_t i = 0; i < ZSRC_ZB_SETUP_MAX; i++) {
        if (z->setup[i] == ieee) {
            if (!on) {
                z->setup[i] = 0u;
            }
            return;
        }
    }
    if (!on) {
        return;
    }
    for (size_t i = 0; i < ZSRC_ZB_SETUP_MAX; i++) {
        if (z->setup[i] == 0u) {
            z->setup[i] = ieee;
            return;
        }
    }
    /* Full: the source enrolls this one itself, which only repeats a CIE write. */
}

static bool is_forgotten(const zsrc_zb_t *z, uint64_t ieee)
{
    for (size_t i = 0; i < ZSRC_ZB_FORGOTTEN_MAX; i++) {
        if (z->forgotten[i] == ieee && ieee != 0u) {
            return true;
        }
    }
    return false;
}

static void forgotten_mark(zsrc_zb_t *z, uint64_t ieee, bool on)
{
    for (size_t i = 0; i < ZSRC_ZB_FORGOTTEN_MAX; i++) {
        if (z->forgotten[i] == ieee) {
            if (!on) {
                z->forgotten[i] = 0u;
            }
            return;
        }
    }
    if (!on) {
        return;
    }
    for (size_t i = 0; i < ZSRC_ZB_FORGOTTEN_MAX; i++) {
        if (z->forgotten[i] == 0u) {
            z->forgotten[i] = ieee;
            return;
        }
    }
    /* Full: the oldest goes (it keeps the normal grace), the newest is remembered. */
    memmove(&z->forgotten[0], &z->forgotten[1],
            (ZSRC_ZB_FORGOTTEN_MAX - 1u) * sizeof(z->forgotten[0]));
    z->forgotten[ZSRC_ZB_FORGOTTEN_MAX - 1u] = ieee;
}

/* ---- the device table ---------------------------------------------------------------------- */

static zsrc_zb_device_t *find_dev(zsrc_zb_t *z, uint64_t ieee, uint8_t endpoint)
{
    for (size_t i = 0; i < ZSRC_ZB_DEV_MAX; i++) {
        if (z->dev[i].used && z->dev[i].ieee == ieee && z->dev[i].endpoint == endpoint) {
            return &z->dev[i];
        }
    }
    return NULL;
}

const zsrc_zb_device_t *zsrc_zb_device(const zsrc_zb_t *z, uint64_t ieee, uint8_t endpoint)
{
    return find_dev((zsrc_zb_t *)z, ieee, endpoint);
}

size_t zsrc_zb_devices(const zsrc_zb_t *z, const zsrc_zb_device_t **out, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < ZSRC_ZB_DEV_MAX && n < max; i++) {
        if (z->dev[i].used) {
            out[n++] = &z->dev[i];
        }
    }
    return n;
}

bool zsrc_zb_jammed(const zsrc_zb_t *z)
{
    return z->jammed;
}

static zsrc_zb_watch_t *find_watch(zsrc_zb_t *z, uint64_t ieee, uint8_t endpoint)
{
    for (size_t i = 0; i < ZSRC_ZB_WATCH_MAX; i++) {
        if (z->watch[i].used && z->watch[i].ieee == ieee && z->watch[i].endpoint == endpoint) {
            return &z->watch[i];
        }
    }
    return NULL;
}

/* The quirks table has the last word on what a device is and how patient we are with it. */
static void apply_quirks(zsrc_zb_device_t *d)
{
    const zsrc_zb_quirk_t *q = zsrc_zb_quirk_for(d->manufacturer, d->model);
    d->quirks = q != NULL ? q->quirks : 0u;
    d->note = q != NULL ? q->note : NULL;
    alarm_device_class_t cls = ALARM_DEVCLASS_GENERIC;
    if (q != NULL && q->device_class != ALARM_DEVCLASS_GENERIC) {
        cls = (alarm_device_class_t)q->device_class;
    }
    if (cls == ALARM_DEVCLASS_GENERIC) {
        cls = zsrc_zb_class_of_zone_type(d->zone_type);
    }
    if (cls == ALARM_DEVCLASS_GENERIC) {
        cls = zsrc_zb_class_of_clusters(d->clusters);
    }
    d->device_class = (uint8_t)cls;
    d->supervision_s = (q != NULL) ? q->supervision_s : 0u;
}

uint32_t zsrc_zb_supervision_s(const zsrc_zb_t *z, const zsrc_zb_device_t *d)
{
    if (d->supervision_s != 0u) {
        return d->supervision_s; /* the quirks table knows this model better */
    }
    if (d->router) {
        return z->router_supervision_s != 0u ? z->router_supervision_s
                                             : ZSRC_ZB_ROUTER_SUPERVISION_S;
    }
    uint32_t s = z->supervision_s != 0u ? z->supervision_s : ZSRC_ZB_SUPERVISION_S;
    if (d->check_in_s != 0u) {
        /* ZB-005: two missed check-ins and some slack, so a device that checks in less often
         * than the default window allows is not called unreachable between two of them. */
        uint64_t c = 2u * (uint64_t)d->check_in_s + ZSRC_ZB_CHECKIN_SLACK_S;
        if (c > ZSRC_ZB_SUPERVISION_MAX_S) {
            c = ZSRC_ZB_SUPERVISION_MAX_S;
        }
        if (c > s) {
            s = (uint32_t)c;
        }
    }
    return s;
}

static uint64_t supervision_ms(const zsrc_zb_t *z, const zsrc_zb_device_t *d)
{
    return (uint64_t)zsrc_zb_supervision_s(z, d) * 1000u;
}

/* ---- what we tell the hub ------------------------------------------------------------------ */

static void emit_event(zsrc_zb_t *z, const zsrc_zb_device_t *d, alarm_zone_event_t e, uint64_t now)
{
    if (!z->told_up || z->sink.event == NULL) {
        return;
    }
    const zsrc_address_t a = zsrc_zb_address(d->ieee, d->endpoint);
    z->sink.event(z->sink.ctx, &a, e, now);
}

static void emit_reachable(zsrc_zb_t *z, const zsrc_zb_device_t *d, bool ok, uint64_t now)
{
    if (!z->told_up || z->sink.reachable == NULL) {
        return;
    }
    const zsrc_address_t a = zsrc_zb_address(d->ieee, d->endpoint);
    z->sink.reachable(z->sink.ctx, &a, ok, now);
}

static void emit_input(zsrc_zb_t *z, const zsrc_zb_device_t *d, const char *state, int32_t value,
                       uint64_t now)
{
    if (!z->told_up || z->sink.input == NULL) {
        return;
    }
    const zsrc_address_t a = zsrc_zb_address(d->ieee, d->endpoint);
    z->sink.input(z->sink.ctx, &a, state, value, now);
}

/* The hub only hears about the link once; jamming counts as the link being down, because a
 * drowned channel is exactly "no sensor can be heard" (ZB-006, ZSRC-015). */
static void update_link(zsrc_zb_t *z, uint64_t now)
{
    const bool up = z->link_up && !z->jammed;
    if (up == z->told_up) {
        return;
    }
    z->told_up = up;
    if (up) {
        z->link_at = now;
        /* Recovery from interference needs fresh reports too, just like a reset. */
        for (size_t i = 0; i < ZSRC_ZB_DEV_MAX; i++) {
            z->dev[i].heard_at = 0u;
            z->dev[i].unreachable = false;
            z->dev[i].status_known = false;
            z->dev[i].on_known = false;
            z->dev[i].has_value = false;
            z->dev[i].checkin_at = 0u;
        }
    }
    if (!up && z->jammed && z->sink.link_now != NULL) {
        /* ZB-006: the channel has been drowned for half a minute already; the hour of grace
         * meant for a passing outage doesn't apply (decided 2026-09-28, BL-104). */
        z->sink.link_now(z->sink.ctx, ZSRC_ZIGBEE, now);
    } else if (z->sink.link != NULL) {
        z->sink.link(z->sink.ctx, ZSRC_ZIGBEE, up, now);
    }
}

/* ---- IAS Zone ------------------------------------------------------------------------------- */

/* Either alarm bit means faulted unless the device is known to use only one of them: on a panel
 * a missed break-in is worse than a fault the owner can see and query (ZB-003, ZB-007). */
static uint16_t alarm_mask(const zsrc_zb_device_t *d)
{
    if ((d->quirks & ZSRC_ZB_QUIRK_ALARM2) != 0u) {
        return ZSRC_ZB_IAS_ALARM2;
    }
    return (uint16_t)(ZSRC_ZB_IAS_ALARM1 | ZSRC_ZB_IAS_ALARM2);
}

static bool watch_invert(zsrc_zb_t *z, const zsrc_zb_device_t *d)
{
    const zsrc_zb_watch_t *w = find_watch(z, d->ieee, d->endpoint);
    const bool binding = w != NULL && w->invert;
    const bool quirk = (d->quirks & ZSRC_ZB_QUIRK_INVERT) != 0u;
    return binding != quirk;
}

/* Which of an endpoint's clusters feeds the zone bound to it (ZB-004). The mapping says whether
 * the zone looks at an alarm or at a measurement: a water sensor that also reports a temperature
 * can feed a freeze zone, and then its own water alarm is not that zone's business. For an alarm,
 * IAS Zone wins where the endpoint has it - On/Off and Occupancy are for contacts and motion that
 * don't use IAS - so a switch or occupancy report never overrides an IAS alarm (ZB32). Reports,
 * the replay to a new binding and the poll after a reconnect all ask this, so they agree (ZB31). */
typedef enum {
    ZB_INPUT_NONE,   /* nothing on this endpoint can feed the zone */
    ZB_INPUT_IAS,    /* IAS Zone Status's alarm bits */
    ZB_INPUT_SWITCH, /* On/Off or Occupancy (the device has no IAS Zone) */
    ZB_INPUT_TEMP,   /* Temperature Measurement */
} zb_input_t;

static zb_input_t watched_input(const zsrc_zb_watch_t *w, const zsrc_zb_device_t *d)
{
    const uint8_t mode = w != NULL ? w->mode : (uint8_t)ZSRC_MAP_BINARY;
    if (mode == ZSRC_MAP_ABOVE || mode == ZSRC_MAP_BELOW) {
        return (d->clusters & ZSRC_ZB_CL_TEMP) != 0u ? ZB_INPUT_TEMP : ZB_INPUT_NONE;
    }
    if (mode != ZSRC_MAP_BINARY && mode != ZSRC_MAP_STATES) {
        return ZB_INPUT_NONE;
    }
    if ((d->clusters & ZSRC_ZB_CL_IAS_ZONE) != 0u) {
        return ZB_INPUT_IAS;
    }
    if ((d->clusters & (uint16_t)(ZSRC_ZB_CL_OCCUPANCY | ZSRC_ZB_CL_ON_OFF)) != 0u) {
        return ZB_INPUT_SWITCH;
    }
    return ZB_INPUT_NONE;
}

static bool watch_wants_the_alarm(zsrc_zb_t *z, const zsrc_zb_device_t *d)
{
    return watched_input(find_watch(z, d->ieee, d->endpoint), d) == ZB_INPUT_IAS;
}

/* Is the device's zone faulted right now, as the binding wants it read? */
static bool status_faulted(zsrc_zb_t *z, const zsrc_zb_device_t *d)
{
    const bool raw = (d->status & alarm_mask(d)) != 0u;
    return raw != watch_invert(z, d);
}

/* Low battery is the union of the IAS battery bits and the reported percentage, so the two can
 * never contradict each other on the zone page. */
static bool battery_is_low(const zsrc_zb_device_t *d)
{
    if (d->status_known &&
        (d->status & (uint16_t)(ZSRC_ZB_IAS_BATTERY | ZSRC_ZB_IAS_BATT_DEFECT)) != 0u) {
        return true;
    }
    return d->battery_pct != ZSRC_ZB_BATTERY_UNKNOWN && d->battery_pct <= ZSRC_ZB_BATTERY_LOW_PCT;
}

static void update_battery(zsrc_zb_t *z, zsrc_zb_device_t *d, bool first, uint64_t now)
{
    const bool low = battery_is_low(d);
    if (low == d->battery_low && !first) {
        return;
    }
    d->battery_low = low;
    if (low) {
        emit_event(z, d, ALARM_ZEV_LOW_BATTERY, now);
    } else if (!first) {
        emit_event(z, d, ALARM_ZEV_LOW_BATTERY_RESTORE, now);
    }
}

/* ZB-022: the running walk test covers every zone this endpoint feeds (zsrc_zb_walk_test_input). */
static bool in_walk_scope(zsrc_zb_t *z, const zsrc_zb_device_t *d)
{
    const zsrc_zb_watch_t *w = find_watch(z, d->ieee, d->endpoint);
    return z->walk_test && w != NULL && w->walk;
}

/* ZB-022: the source has asked this device into test mode for the walk test that is running and
 * its zones are in that test's scope, so a trip with its TEST bit is a test. Anything else - an
 * input the walk test does not cover (another partition's, a fire or CO zone's), the walk test
 * over while the device has not yet heard its Normal Operation request, or a device that set the
 * bit by itself - is not: its trips are faults, and the engine decides, so a fire, CO or panic
 * sensor left in test mode still sounds its alarm (fail-safe). */
static bool trips_are_tests(zsrc_zb_t *z, const zsrc_zb_device_t *d)
{
    return (d->status & ZSRC_ZB_IAS_TEST) != 0u &&
           (d->test == ZSRC_ZB_TEST_ASKED_ON || d->test == ZSRC_ZB_TEST_ON) &&
           in_walk_scope(z, d);
}

/* Turn a status word into zone events. On the first report the zone is told where it stands
 * (open or closed) plus whatever else is wrong; after that only the bits that changed. `replay`:
 * the status is what the device said earlier, told again to a new binding.
 *
 * ZB-022: while its trips are tests (trips_are_tests), a fault is reported as ALARM_ZEV_TEST,
 * never as a fault (the engine marks the zone in the walk test); a restore stays a restore. A test
 * is never replayed. Once its trips are no longer tests, a zone that was told a test is told where
 * it stands, so a sensor left open after its test is not missed. */
static void report_status(zsrc_zb_t *z, zsrc_zb_device_t *d, uint16_t old, bool first,
                          bool replay, uint64_t now)
{
    const uint16_t changed = first ? 0xFFFFu : (uint16_t)(old ^ d->status);

    if (watch_wants_the_alarm(z, d)) {
        const bool faulted = status_faulted(z, d);
        const bool alarm_changed = (changed & alarm_mask(d)) != 0u;
        if (trips_are_tests(z, d)) {
            if (alarm_changed) {
                if (!faulted) {
                    emit_event(z, d, ALARM_ZEV_RESTORE, now);
                    d->test_masked = false;
                } else {
                    if (!replay) {
                        emit_event(z, d, ALARM_ZEV_TEST, now);
                    }
                    d->test_masked = true; /* the zone does not know it is open */
                }
            }
        } else if (d->test_masked || first || alarm_changed) {
            d->test_masked = false;
            emit_event(z, d, faulted ? ALARM_ZEV_FAULT : ALARM_ZEV_RESTORE, now);
        }
    }
    if ((d->quirks & ZSRC_ZB_QUIRK_NO_TAMPER) == 0u) {
        const bool tamper = (d->status & ZSRC_ZB_IAS_TAMPER) != 0u;
        if ((changed & ZSRC_ZB_IAS_TAMPER) != 0u &&
            (tamper || !first || (old & ZSRC_ZB_IAS_TAMPER) != 0u)) {
            emit_event(z, d, tamper ? ALARM_ZEV_TAMPER : ALARM_ZEV_TAMPER_RESTORE, now);
        }
    }
    const bool trouble = (d->status & ZSRC_ZB_IAS_TROUBLE) != 0u;
    if ((changed & ZSRC_ZB_IAS_TROUBLE) != 0u &&
        (trouble || !first || (old & ZSRC_ZB_IAS_TROUBLE) != 0u)) {
        emit_event(z, d, trouble ? ALARM_ZEV_TROUBLE : ALARM_ZEV_TROUBLE_RESTORE, now);
    }
    update_battery(z, d, false, now);
}

/* ---- enrollment (ZB-003) ---------------------------------------------------------------------- */

/* One attempt at whatever step this device owes us. Called when it is awake (a frame arrived),
 * and on a timer for mains-powered routers, never faster than ZSRC_ZB_ENROLL_GAP_MS. */
static void try_enroll(zsrc_zb_t *z, zsrc_zb_device_t *d, uint64_t now)
{
    if (d->enroll != ZSRC_ZB_ENROLL_CIE && d->enroll != ZSRC_ZB_ENROLL_RESPOND) {
        return;
    }
    if (in_setup(z, d->ieee)) {
        return; /* zbz_panel's setup does the CIE write and the enrollment in order (§5) */
    }
    if (now < d->next_try) {
        return;
    }
    d->next_try = add_ms(now, ZSRC_ZB_ENROLL_GAP_MS);
    if (d->tries < 0xFFu) {
        d->tries++;
    }
    bool ok = false;
    if (d->enroll == ZSRC_ZB_ENROLL_CIE) {
        ok = z->radio.ias_cie_write != NULL &&
             z->radio.ias_cie_write(z->radio.ctx, d->ieee, d->endpoint);
        /* Success only means the write went out: the device still has to ask to enroll, or
         * (ZSRC_ZB_QUIRK_NO_ENROLL) simply start reporting. Either way we stay here until it
         * does, or until the tries run out. */
    } else {
        ok = z->radio.ias_enroll_response != NULL &&
             z->radio.ias_enroll_response(z->radio.ctx, d->ieee, d->endpoint, d->zone_id);
        if (ok) {
            d->enroll = ZSRC_ZB_ENROLL_DONE;
            return;
        }
    }
    if (!ok && d->tries >= ZSRC_ZB_ENROLL_TRIES) {
        /* Out of patience. Plenty of devices report their status perfectly well without ever
         * completing enrollment, so this is not the end of the zone - only of the handshake. */
        d->enroll = ZSRC_ZB_ENROLL_FAILED;
    } else if (ok && d->tries >= ZSRC_ZB_ENROLL_TRIES) {
        d->enroll = ZSRC_ZB_ENROLL_FAILED;
    }
}

/* The IAS zone id the device is enrolled with: one live endpoint per table slot, so freed ids
 * are reused without ever colliding. */
static void ensure_zone_id(zsrc_zb_t *z, zsrc_zb_device_t *d)
{
    if (d->zone_id == 0u) {
        d->zone_id = (uint8_t)(d - z->dev + 1);
    }
}

static void start_enrollment(zsrc_zb_t *z, zsrc_zb_device_t *d, uint64_t now)
{
    if ((d->clusters & ZSRC_ZB_CL_IAS_ZONE) == 0u) {
        d->enroll = ZSRC_ZB_ENROLL_NONE;
        return;
    }
    if (d->enroll != ZSRC_ZB_ENROLL_NONE) {
        return;
    }
    if (in_setup(z, d->ieee)) {
        ensure_zone_id(z, d); /* the setup's Enroll Response carries it */
        return;
    }
    d->enroll = ZSRC_ZB_ENROLL_CIE;
    d->tries = 0u;
    d->next_try = 0u;
    try_enroll(z, d, now);
}

/* ---- life cycle ------------------------------------------------------------------------------- */

void zsrc_zb_init(zsrc_zb_t *z, const zsrc_zb_radio_t *radio, const zsrc_sink_t *sink)
{
    memset(z, 0, sizeof(*z));
    if (radio != NULL) {
        z->radio = *radio;
    }
    if (sink != NULL) {
        z->sink = *sink;
    }
    for (size_t i = 0; i < ZSRC_ZB_DEV_MAX; i++) {
        z->dev[i].battery_pct = ZSRC_ZB_BATTERY_UNKNOWN;
    }
}

/* ZB31 (ZB-005, ZSRC-015): after a reconnect, ask a listening device for the input its zone
 * watches (watched_input), so the answer settles the zone at once. An endpoint with IAS Zone is
 * asked for its status as well: its tamper, trouble and battery bits feed the zone whatever the
 * mapping, and none of them is known again until the device says so. An endpoint with nothing
 * the zone can use is still asked for something, as proof that it is there. */
static void poll_after_reconnect(zsrc_zb_t *z, const zsrc_zb_watch_t *w, const zsrc_zb_device_t *d)
{
    if (z->radio.read_attribute == NULL) {
        return;
    }
    uint16_t cluster = 0u;
    switch (watched_input(w, d)) {
    case ZB_INPUT_TEMP:
        cluster = ZSRC_ZB_ZCL_TEMP_MEAS;
        break;
    case ZB_INPUT_SWITCH:
        cluster = (d->clusters & ZSRC_ZB_CL_OCCUPANCY) != 0u ? ZSRC_ZB_ZCL_OCCUPANCY
                                                             : ZSRC_ZB_ZCL_ON_OFF;
        break;
    case ZB_INPUT_IAS:
    case ZB_INPUT_NONE:
    default:
        break;
    }
    if (cluster != 0u) {
        (void)z->radio.read_attribute(z->radio.ctx, d->ieee, d->endpoint, cluster, 0x0000u);
    }
    if ((d->clusters & ZSRC_ZB_CL_IAS_ZONE) != 0u) {
        (void)z->radio.read_attribute(z->radio.ctx, d->ieee, d->endpoint, ZSRC_ZB_ZCL_IAS_ZONE,
                                      0x0002u);
    } else if (cluster == 0u) {
        if ((d->clusters & ZSRC_ZB_CL_OCCUPANCY) != 0u) {
            cluster = ZSRC_ZB_ZCL_OCCUPANCY;
        } else if ((d->clusters & ZSRC_ZB_CL_ON_OFF) != 0u) {
            cluster = ZSRC_ZB_ZCL_ON_OFF;
        } else if ((d->clusters & ZSRC_ZB_CL_TEMP) != 0u) {
            cluster = ZSRC_ZB_ZCL_TEMP_MEAS;
        }
        if (cluster != 0u) {
            (void)z->radio.read_attribute(z->radio.ctx, d->ieee, d->endpoint, cluster, 0x0000u);
        }
    }
}

void zsrc_zb_link(zsrc_zb_t *z, bool up, uint64_t now)
{
    if (z->link_up == up) {
        return;
    }
    z->link_up = up;
    z->energy_high = false;
    z->jammed = false;
    z->next_scan = add_ms(now, ZSRC_ZB_SCAN_MS);
    if (up) {
        z->link_at = now;
        /* ZSRC-015: nothing a device said before the break counts. Every one of them starts
         * its supervision window again from here and has to speak for itself. */
        for (size_t i = 0; i < ZSRC_ZB_DEV_MAX; i++) {
            z->dev[i].heard_at = 0u;
            z->dev[i].unreachable = false;
            z->dev[i].status_known = false;
            z->dev[i].on_known = false;
            z->dev[i].has_value = false;
            z->dev[i].burst_n = 0u; /* ZB-028: a burst doesn't outlive the break */
            z->dev[i].reread = ZSRC_ZB_REREAD_NONE;
        }
    } else {
        if (z->learning && z->radio.permit_join != NULL) {
            (void)z->radio.permit_join(z->radio.ctx, 0u);
        }
        z->learning = false; /* the join window cannot be open with no radio behind it */
        z->learn_until = 0u;
    }
    update_link(z, now);
    if (up) {
        /* A mains-powered router can answer at once, so ask rather than wait out its window. */
        for (size_t i = 0; i < ZSRC_ZB_DEV_MAX; i++) {
            zsrc_zb_device_t *d = &z->dev[i];
            const zsrc_zb_watch_t *w = d->used ? find_watch(z, d->ieee, d->endpoint) : NULL;
            if (w == NULL || !d->router) {
                continue;
            }
            poll_after_reconnect(z, w, d);
        }
    }
}

bool zsrc_zb_device_seen(zsrc_zb_t *z, const zsrc_zb_desc_t *d0, uint64_t now)
{
    if (!z->link_up || d0 == NULL || d0->ieee == 0u || d0->endpoint == 0u ||
        d0->endpoint > 240u) {
        return false;
    }
    zsrc_zb_device_t *d = find_dev(z, d0->ieee, d0->endpoint);
    if (d == NULL) {
        for (size_t i = 0; i < ZSRC_ZB_DEV_MAX; i++) {
            if (!z->dev[i].used) {
                d = &z->dev[i];
                break;
            }
        }
        if (d == NULL) {
            return false; /* ZSRC_ZB_DEV_MAX paired devices already */
        }
        memset(d, 0, sizeof(*d));
        d->used = true;
        d->ieee = d0->ieee;
        d->endpoint = d0->endpoint;
        d->battery_pct = ZSRC_ZB_BATTERY_UNKNOWN;
        d->new_join = z->learning; /* ZB-002: the picker offers this one first */
        d->join = d0->join;
    } else if (d0->join != (uint8_t)ZSRC_ZB_JOIN_UNKNOWN) {
        d->join = d0->join;
    }
    forgotten_mark(z, d0->ieee, false); /* described again: it joined again (ZB-013) */
    d->nwk = d0->nwk;
    d->clusters = d0->clusters;
    d->router = d0->router;
    if (d0->zone_type != ZSRC_ZB_ZONE_TYPE_NONE) {
        d->zone_type = d0->zone_type;
    } else if (d->zone_type == 0u && (d0->clusters & ZSRC_ZB_CL_IAS_ZONE) == 0u) {
        d->zone_type = ZSRC_ZB_ZONE_TYPE_NONE;
    }
    copy_str(d->manufacturer, sizeof(d->manufacturer), d0->manufacturer);
    copy_str(d->model, sizeof(d->model), d0->model);
    apply_quirks(d);
    d->heard_at = now; /* joining, or answering discovery, is the device speaking */
    if (d->unreachable) {
        d->unreachable = false;
        emit_reachable(z, d, true, now);
    }
    if ((d->quirks & ZSRC_ZB_QUIRK_UNSUPPORTED) == 0u) {
        start_enrollment(z, d, now);
    }
    return true;
}

void zsrc_zb_device_gone(zsrc_zb_t *z, uint64_t ieee, uint64_t now)
{
    for (size_t i = 0; i < ZSRC_ZB_DEV_MAX; i++) {
        zsrc_zb_device_t *d = &z->dev[i];
        if (!d->used || d->ieee != ieee) {
            continue;
        }
        if (!d->unreachable && find_watch(z, d->ieee, d->endpoint) != NULL) {
            emit_reachable(z, d, false, now); /* its zone runs the grace into CHECK */
        }
        memset(d, 0, sizeof(*d));
        d->battery_pct = ZSRC_ZB_BATTERY_UNKNOWN;
    }
}

/* ---- walk-test mode (ZB-022) ----------------------------------------------------------------- */

/* A bound, enrolled IAS sensor that we can speak to, whose zones the walk test covers: the walk
 * test asks it into test mode. */
static bool test_wanted(zsrc_zb_t *z, const zsrc_zb_device_t *d)
{
    return d->used && (d->clusters & ZSRC_ZB_CL_IAS_ZONE) != 0u &&
           d->enroll == ZSRC_ZB_ENROLL_DONE && (d->quirks & ZSRC_ZB_QUIRK_UNSUPPORTED) == 0u &&
           in_walk_scope(z, d);
}

static uint32_t new_tag(zsrc_zb_t *z, const zsrc_zb_device_t *d)
{
    z->tag_seq++;
    if (z->tag_seq == 0u) {
        z->tag_seq = 1u;
    }
    return ZSRC_ZB_TAG_SOURCE | ((uint32_t)(d - z->dev) << 16) | (uint32_t)z->tag_seq;
}

/* One step of a device's test mode, at a moment it can be spoken to: a frame from it (sleepy or
 * not) or the tick (a listening device). Asks no faster than ZSRC_ZB_ENROLL_GAP_MS. */
static void test_step(zsrc_zb_t *z, zsrc_zb_device_t *d, uint64_t now)
{
    const bool wanted = test_wanted(z, d);
    if (d->test == ZSRC_ZB_TEST_NONE && wanted) {
        d->test = ZSRC_ZB_TEST_OWED_ON; /* enrolled or bound since the walk test began */
    }
    if (!wanted && (d->test == ZSRC_ZB_TEST_OWED_ON || d->test == ZSRC_ZB_TEST_ASKED_ON ||
                    d->test == ZSRC_ZB_TEST_ON)) {
        /* Out of the walk test's scope since (unbound): back to normal operation. */
        d->test = d->test_mode ? ZSRC_ZB_TEST_OWED_OFF : ZSRC_ZB_TEST_NONE;
    }
    const bool gap = d->test_at == 0u || now < d->test_at ||
                     now - d->test_at >= ZSRC_ZB_ENROLL_GAP_MS;
    const bool refresh = z->walk_test &&
                         (d->test == ZSRC_ZB_TEST_ASKED_ON || d->test == ZSRC_ZB_TEST_ON) &&
                         now >= d->test_at && now - d->test_at >= ZSRC_ZB_TEST_REFRESH_MS;
    if ((d->test == ZSRC_ZB_TEST_OWED_ON && gap) || refresh) {
        if (z->radio.ias_test_mode == NULL) {
            d->test = ZSRC_ZB_TEST_REFUSED; /* this radio can't: the sensor's faults stay faults */
            return;
        }
        const uint32_t tag = new_tag(z, d);
        d->test_at = now;
        if (z->radio.ias_test_mode(z->radio.ctx, d->ieee, d->endpoint,
                                   (uint8_t)ZSRC_ZB_TEST_MODE_LEN, tag)) {
            d->test_tag = tag;
            d->test_mode = true;
            if (d->test == ZSRC_ZB_TEST_OWED_ON) {
                d->test = ZSRC_ZB_TEST_ASKED_ON;
            }
        }
        return;
    }
    if (d->test == ZSRC_ZB_TEST_OWED_OFF && gap) {
        if (z->radio.ias_test_mode == NULL) {
            d->test = ZSRC_ZB_TEST_NONE;
            d->test_mode = false;
            return;
        }
        const uint32_t tag = new_tag(z, d);
        d->test_at = now;
        if (z->radio.ias_test_mode(z->radio.ctx, d->ieee, d->endpoint, 0u, tag)) {
            d->test_tag = tag;
            d->test = ZSRC_ZB_TEST_ASKED_OFF;
            d->test_mode = false;
        }
    }
}

/* One device after its walk-test scope changed (the walk test started or ended, or its input
 * went in or out of the scope): asked in, asked back, or let be. A zone told a test while its
 * sensor is still open is told the fault at once once its trips are no longer tests (ZB-022). */
static void test_scope_changed(zsrc_zb_t *z, zsrc_zb_device_t *d, uint64_t now)
{
    if (test_wanted(z, d)) {
        if (d->test != ZSRC_ZB_TEST_ASKED_ON && d->test != ZSRC_ZB_TEST_ON) {
            d->test = ZSRC_ZB_TEST_OWED_ON;
            d->test_at = 0u; /* a new walk test asks at once */
        }
    } else if (d->test_mode) {
        d->test = ZSRC_ZB_TEST_OWED_OFF; /* asked in, whatever it answered: ask it back */
        d->test_at = 0u;
    } else if (d->test != ZSRC_ZB_TEST_REFUSED || !z->walk_test) {
        d->test = ZSRC_ZB_TEST_NONE;
    }
    if (d->test_masked && d->status_known && z->told_up && !trips_are_tests(z, d)) {
        report_status(z, d, d->status, false, false, now); /* the fault it still is */
    }
    /* A listening device hears it now; a sleepy one at its next frame. */
    if (z->told_up && d->router) {
        test_step(z, d, now);
    }
}

void zsrc_zb_walk_test(zsrc_zb_t *z, bool on, uint64_t now)
{
    z->walk_test = on;
    if (!on) {
        for (size_t i = 0; i < ZSRC_ZB_WATCH_MAX; i++) {
            z->watch[i].walk = false; /* the next walk test names its own scope */
        }
    }
    for (size_t i = 0; i < ZSRC_ZB_DEV_MAX; i++) {
        zsrc_zb_device_t *d = &z->dev[i];
        if (d->used) {
            test_scope_changed(z, d, now);
        }
    }
}

void zsrc_zb_walk_test_input(zsrc_zb_t *z, const zsrc_address_t *a, bool on, uint64_t now)
{
    if (z == NULL || a == NULL || a->kind != ZSRC_ZIGBEE) {
        return;
    }
    zsrc_zb_watch_t *w = find_watch(z, a->id, a->input);
    if (w == NULL || w->walk == on) {
        return;
    }
    w->walk = on;
    zsrc_zb_device_t *d = find_dev(z, a->id, a->input);
    if (d != NULL) {
        test_scope_changed(z, d, now);
    }
}

/* The end of a request the source made itself. */
static void source_done(zsrc_zb_t *z, uint32_t tag, uint8_t result)
{
    const size_t i = (size_t)((tag >> 16) & 0x7FFFu);
    if (i >= ZSRC_ZB_DEV_MAX || !z->dev[i].used || z->dev[i].test_tag != tag) {
        return; /* a slot freed or asked again since: stale */
    }
    zsrc_zb_device_t *d = &z->dev[i];
    const bool ok = result == (uint8_t)ZSRC_ZB_DONE_OK;
    const bool no = result == (uint8_t)ZSRC_ZB_DONE_UNSUPPORTED ||
                    result == (uint8_t)ZSRC_ZB_DONE_REFUSED;
    if (d->test == ZSRC_ZB_TEST_ASKED_ON) {
        if (ok) {
            d->test = ZSRC_ZB_TEST_ON;
        } else if (no) {
            d->test = ZSRC_ZB_TEST_REFUSED; /* no test mode: its trips stay faults */
            d->test_mode = false;
        } else {
            d->test = ZSRC_ZB_TEST_OWED_ON; /* asleep or out of reach: its next frame */
        }
    } else if (d->test == ZSRC_ZB_TEST_ASKED_OFF) {
        if (ok || no) {
            d->test = ZSRC_ZB_TEST_NONE;
        } else {
            d->test = ZSRC_ZB_TEST_OWED_OFF; /* it may still be in test mode: ask again */
            d->test_mode = true;
        }
    }
}

/* ZB-028 (B020): a status from an IAS device. One while the source waits for its re-read is the
 * answer (or newer still): the re-read is done, and the change it brings starts no burst. A change
 * of its alarm bits within ZSRC_ZB_BURST_MS of the last one makes a burst, which owes a re-read
 * once the device has been quiet for ZSRC_ZB_REREAD_MS (every further change puts it off). */
static void note_burst(zsrc_zb_t *z, zsrc_zb_device_t *d, uint16_t old, bool first, uint64_t now)
{
    if (d->reread == ZSRC_ZB_REREAD_ASKED || d->reread == ZSRC_ZB_REREAD_NEXT_FRAME) {
        d->reread = ZSRC_ZB_REREAD_NONE;
        d->burst_n = 0u;
        d->burst_at = 0u;
        return;
    }
    if (first || ((uint16_t)(old ^ d->status) & alarm_mask(d)) == 0u ||
        find_watch(z, d->ieee, d->endpoint) == NULL || z->radio.read_attribute == NULL) {
        return;
    }
    if (d->burst_at != 0u && now >= d->burst_at && now - d->burst_at <= ZSRC_ZB_BURST_MS) {
        if (d->burst_n < UINT8_MAX) {
            d->burst_n++;
        }
    } else {
        d->burst_n = 1u;
    }
    d->burst_at = now;
    if (d->burst_n >= 2u) {
        d->reread = ZSRC_ZB_REREAD_DUE;
        d->reread_at = add_ms(now, ZSRC_ZB_REREAD_MS);
        d->reread_tries = 0u;
    }
}

/* ZB-028: the re-read, at a moment the device can be asked: the tick (`frame` false) once the
 * burst has settled - a sleepy device's parent holds the read for its next poll, which often
 * follows its last report - or a frame from it (`frame` true) after a read went unanswered. */
static void reread_step(zsrc_zb_t *z, zsrc_zb_device_t *d, bool frame, uint64_t now)
{
    switch (d->reread) {
    case ZSRC_ZB_REREAD_DUE:
        if (now < d->reread_at) {
            return;
        }
        break;
    case ZSRC_ZB_REREAD_NEXT_FRAME:
        if (!frame && !d->router) {
            return;
        }
        break;
    case ZSRC_ZB_REREAD_ASKED:
        if (now >= d->reread_at && now - d->reread_at >= ZSRC_ZB_REREAD_WAIT_MS) {
            d->reread = d->reread_tries < ZSRC_ZB_REREAD_TRIES ? ZSRC_ZB_REREAD_NEXT_FRAME
                                                                : ZSRC_ZB_REREAD_NONE;
        }
        return;
    default:
        return;
    }
    d->reread_tries++;
    d->reread_at = now;
    if (z->radio.read_attribute != NULL &&
        z->radio.read_attribute(z->radio.ctx, d->ieee, d->endpoint, ZSRC_ZB_ZCL_IAS_ZONE, 0x0002u)) {
        d->reread = ZSRC_ZB_REREAD_ASKED;
    } else {
        d->reread = d->reread_tries < ZSRC_ZB_REREAD_TRIES ? ZSRC_ZB_REREAD_NEXT_FRAME
                                                            : ZSRC_ZB_REREAD_NONE;
    }
}

/* Every frame is proof the device is alive, and the one moment a sleepy device can be spoken
 * to (ZB-003, ZB-005). */
static void heard(zsrc_zb_t *z, zsrc_zb_device_t *d, uint8_t lqi, uint64_t now)
{
    d->heard_at = now;
    if (lqi != 0u) {
        d->lqi = lqi;
    }
    const bool watched = find_watch(z, d->ieee, d->endpoint) != NULL;
    if (d->unreachable) {
        d->unreachable = false;
        if (watched) {
            emit_reachable(z, d, true, now);
        }
    } else if (watched) {
        emit_reachable(z, d, true, now); /* keeps the hub's grace timer from ever starting */
        if (d->checkin_at == 0u || now - d->checkin_at >= ZSRC_ZB_CHECKIN_EVERY_MS) {
            d->checkin_at = now;
            emit_event(z, d, ALARM_ZEV_CHECKIN, now); /* keeps the engine's supervision alive */
        }
    }
    try_enroll(z, d, now);
    test_step(z, d, now);
    reread_step(z, d, true, now);
}

void zsrc_zb_frame_seen(zsrc_zb_t *z, uint64_t ieee, uint8_t lqi, uint64_t now)
{
    if (!z->link_up) {
        return;
    }
    for (size_t i = 0; i < ZSRC_ZB_DEV_MAX; i++) {
        if (z->dev[i].used && z->dev[i].ieee == ieee) {
            heard(z, &z->dev[i], lqi, now);
        }
    }
}

/* A device that talks to us before discovery has finished still has to be heard. */
static zsrc_zb_device_t *ensure_dev(zsrc_zb_t *z, uint64_t ieee, uint8_t endpoint,
                                    uint16_t clusters, uint64_t now)
{
    if (!z->link_up || ieee == 0u || endpoint == 0u || endpoint > 240u) {
        return NULL;
    }
    zsrc_zb_device_t *d = find_dev(z, ieee, endpoint);
    if (d != NULL) {
        return d;
    }
    if (is_forgotten(z, ieee)) {
        return NULL; /* ZB-013: forgotten, and its key may outlive that; a new join brings it back */
    }
    zsrc_zb_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.ieee = ieee;
    desc.endpoint = endpoint;
    desc.clusters = clusters;
    desc.zone_type = ZSRC_ZB_ZONE_TYPE_NONE;
    if (!zsrc_zb_device_seen(z, &desc, now)) {
        return NULL;
    }
    return find_dev(z, ieee, endpoint);
}

void zsrc_zb_ias_enroll_request(zsrc_zb_t *z, uint64_t ieee, uint8_t endpoint, uint16_t zone_type,
                                uint64_t now)
{
    zsrc_zb_device_t *d = ensure_dev(z, ieee, endpoint, ZSRC_ZB_CL_IAS_ZONE, now);
    if (d == NULL) {
        return;
    }
    d->clusters |= ZSRC_ZB_CL_IAS_ZONE;
    if (zone_type != ZSRC_ZB_ZONE_TYPE_NONE) {
        d->zone_type = zone_type;
        apply_quirks(d);
    }
    ensure_zone_id(z, d);
    d->enroll = ZSRC_ZB_ENROLL_RESPOND;
    d->tries = 0u;
    d->next_try = 0u;
    heard(z, d, 0u, now); /* it just spoke: this is the wake window the response has to catch */
}

void zsrc_zb_ias_status(zsrc_zb_t *z, uint64_t ieee, uint8_t endpoint, uint16_t status,
                        uint64_t now)
{
    zsrc_zb_device_t *d = ensure_dev(z, ieee, endpoint, ZSRC_ZB_CL_IAS_ZONE, now);
    if (d == NULL) {
        return;
    }
    d->clusters |= ZSRC_ZB_CL_IAS_ZONE;
    /* A device that reports its status is working, whatever became of the handshake: plenty
     * never ask to enroll at all (ZB-007). */
    if (d->enroll == ZSRC_ZB_ENROLL_CIE || d->enroll == ZSRC_ZB_ENROLL_RESPOND ||
        d->enroll == ZSRC_ZB_ENROLL_FAILED) {
        d->enroll = ZSRC_ZB_ENROLL_DONE;
    }
    const uint16_t old = d->status;
    const bool first = !d->status_known;
    d->status = status;
    d->status_known = true;
    note_burst(z, d, old, first, now); /* before heard(): this status may be the re-read's answer */
    heard(z, d, 0u, now);
    if (find_watch(z, ieee, endpoint) != NULL) {
        report_status(z, d, old, first, false, now);
    }
}

void zsrc_zb_attr_bool(zsrc_zb_t *z, uint64_t ieee, uint8_t endpoint, uint16_t cluster, bool on,
                       uint64_t now)
{
    if (cluster != ZSRC_ZB_ZCL_OCCUPANCY && cluster != ZSRC_ZB_ZCL_ON_OFF) {
        return;
    }
    const uint16_t bit =
        cluster == ZSRC_ZB_ZCL_OCCUPANCY ? ZSRC_ZB_CL_OCCUPANCY : ZSRC_ZB_CL_ON_OFF;
    zsrc_zb_device_t *d = ensure_dev(z, ieee, endpoint, bit, now);
    if (d == NULL) {
        return;
    }
    d->clusters |= bit;
    d->on = on;
    d->on_known = true;
    heard(z, d, 0u, now);
    const zsrc_zb_watch_t *w = find_watch(z, ieee, endpoint);
    if (w == NULL || watched_input(w, d) != ZB_INPUT_SWITCH) {
        return; /* the zone wants this device's IAS alarm or its measured value (ZB32) */
    }
    /* "on"/"off" rather than "occupied"/"clear": the hub's binary mapping knows these words,
     * and a state list in the binding can still say exactly one of them (ZSRC-006). */
    emit_input(z, d, on ? "on" : "off", on ? 1 : 0, now);
}

void zsrc_zb_attr_value(zsrc_zb_t *z, uint64_t ieee, uint8_t endpoint, uint16_t cluster,
                        int32_t value_x100, uint64_t now)
{
    if (cluster != ZSRC_ZB_ZCL_TEMP_MEAS || value_x100 == -32768 ||
        value_x100 < -27315 || value_x100 > 32767) {
        return; /* ZCL invalid/unknown or outside Temperature Measurement's range. */
    }
    const uint16_t bit = cluster == ZSRC_ZB_ZCL_TEMP_MEAS ? ZSRC_ZB_CL_TEMP : 0u;
    zsrc_zb_device_t *d = ensure_dev(z, ieee, endpoint, bit, now);
    if (d == NULL) {
        return;
    }
    d->clusters |= bit;
    d->value_x100 = value_x100;
    d->has_value = true;
    heard(z, d, 0u, now);
    const zsrc_zb_watch_t *w = find_watch(z, ieee, endpoint);
    if (w == NULL || watched_input(w, d) != ZB_INPUT_TEMP) {
        return; /* the zone wants this device's alarm, not its thermometer */
    }
    emit_input(z, d, NULL, value_x100, now);
}

void zsrc_zb_battery(zsrc_zb_t *z, uint64_t ieee, uint8_t percent, uint64_t now)
{
    if (!z->link_up) {
        return;
    }
    for (size_t i = 0; i < ZSRC_ZB_DEV_MAX; i++) {
        zsrc_zb_device_t *d = &z->dev[i];
        if (!d->used || d->ieee != ieee) {
            continue;
        }
        d->battery_pct = percent > 100u && percent != ZSRC_ZB_BATTERY_UNKNOWN ? 100u : percent;
        heard(z, d, 0u, now);
        if (find_watch(z, d->ieee, d->endpoint) != NULL) {
            update_battery(z, d, false, now);
        }
    }
}

void zsrc_zb_energy(zsrc_zb_t *z, int8_t dbm, uint64_t now)
{
    if (!z->link_up) {
        return;
    }
    z->energy_dbm = dbm;
    if (dbm >= ZSRC_ZB_JAM_DBM) {
        if (!z->energy_high || now < z->energy_at ||
            now - z->energy_at > 2u * ZSRC_ZB_SCAN_MS) {
            z->jam_since = now;
        } else if (!z->jammed && now >= z->jam_since &&
                   now - z->jam_since >= ZSRC_ZB_JAM_HOLD_MS) {
            z->jammed = true; /* ZB-006: held high for half a minute is not a passing microwave */
        }
        z->energy_high = true;
    } else {
        z->energy_high = false;
        z->jam_since = 0u;
        z->jammed = false;
    }
    z->energy_at = now;
    update_link(z, now);
}

/* ---- the hand-offs from zigbee_zones (BL-143; devices.md §2, §5, §6) --------------------------- */

/* What a device said before it went away, or before it was reset and joined again, is not what
 * it says now: nothing of it may replay to a zone (ZSRC-015, ZB-013). */
static void forget_live(zsrc_zb_device_t *d)
{
    d->status = 0u;
    d->status_known = false;
    d->on = false;
    d->on_known = false;
    d->has_value = false;
    d->value_x100 = 0;
    d->battery_pct = ZSRC_ZB_BATTERY_UNKNOWN;
    d->checkin_at = 0u;
    d->test = ZSRC_ZB_TEST_NONE;
    d->test_mode = false;
    d->test_at = 0u;
    d->test_masked = false;
}

void zsrc_zb_device_forget(zsrc_zb_t *z, uint64_t ieee, uint64_t now)
{
    if (ieee == 0u) {
        return;
    }
    for (size_t i = 0; i < ZSRC_ZB_DEV_MAX; i++) {
        zsrc_zb_device_t *d = &z->dev[i];
        if (!d->used || d->ieee != ieee) {
            continue;
        }
        if (!d->unreachable && find_watch(z, d->ieee, d->endpoint) != NULL) {
            emit_reachable(z, d, false, now); /* grace_of: CHECK at once */
        }
        memset(d, 0, sizeof(*d));
        d->battery_pct = ZSRC_ZB_BATTERY_UNKNOWN;
    }
    setup_mark(z, ieee, false);
    forgotten_mark(z, ieee, true);
}

void zsrc_zb_device_setup(zsrc_zb_t *z, uint64_t ieee, bool running, uint64_t now)
{
    if (ieee == 0u) {
        return;
    }
    setup_mark(z, ieee, running);
    for (size_t i = 0; i < ZSRC_ZB_DEV_MAX; i++) {
        zsrc_zb_device_t *d = &z->dev[i];
        if (!d->used || d->ieee != ieee || (d->clusters & ZSRC_ZB_CL_IAS_ZONE) == 0u) {
            continue;
        }
        ensure_zone_id(z, d);
        if (running || !z->link_up || (d->quirks & ZSRC_ZB_QUIRK_UNSUPPORTED) != 0u) {
            continue;
        }
        /* Over, and the setup didn't enroll it: the source's own patient enrollment, as before
         * BL-143 (a setup that STOPPED must not leave an IAS sensor half enrolled). */
        if (d->enroll == ZSRC_ZB_ENROLL_NONE) {
            start_enrollment(z, d, now);
        } else {
            try_enroll(z, d, now);
        }
    }
}

void zsrc_zb_ias_enrolled(zsrc_zb_t *z, uint64_t ieee, uint8_t endpoint, uint64_t now)
{
    (void)now;
    zsrc_zb_device_t *d = find_dev(z, ieee, endpoint);
    if (d == NULL) {
        return;
    }
    d->clusters |= ZSRC_ZB_CL_IAS_ZONE;
    ensure_zone_id(z, d);
    d->enroll = ZSRC_ZB_ENROLL_DONE;
    d->tries = 0u;
}

void zsrc_zb_check_in_interval(zsrc_zb_t *z, uint64_t ieee, uint32_t seconds, uint64_t now)
{
    for (size_t i = 0; i < ZSRC_ZB_DEV_MAX; i++) {
        zsrc_zb_device_t *d = &z->dev[i];
        if (d->used && d->ieee == ieee) {
            d->check_in_s = seconds;
            if (z->link_up) {
                heard(z, d, 0u, now); /* the answer to a read is the device speaking */
            }
        }
    }
}

/* The first used endpoint slot of device `ieee` at or after `from`, NULL past the last. */
static zsrc_zb_device_t *next_slot(zsrc_zb_t *z, uint64_t ieee, zsrc_zb_device_t *from)
{
    for (zsrc_zb_device_t *d = from; d < z->dev + ZSRC_ZB_DEV_MAX; d++) {
        if (d->used && d->ieee == ieee) {
            return d;
        }
    }
    return NULL;
}

/* Each endpoint slot of a device: one loop, so a `break` in the body leaves it. */
#define FOR_DEVICE(z_, ieee_, d_)                                                                  \
    for (zsrc_zb_device_t *d_ = next_slot((z_), (ieee_), (z_)->dev); d_ != NULL;                  \
         d_ = next_slot((z_), (ieee_), d_ + 1))

static void take_node(zsrc_zb_device_t *d, uint8_t logical_type, uint8_t mac_caps)
{
    d->node_known = true;
    d->logical_type = logical_type;
    d->mac_caps = mac_caps;
    d->router = listening(logical_type, mac_caps);
}

bool zsrc_zb_on_report(zsrc_zb_t *z, const zsrc_zb_report_t *r, uint64_t now)
{
    if (r == NULL) {
        return false;
    }
    switch (r->kind) {
    case ZSRC_ZB_REP_DONE:
        if ((r->tag & ZSRC_ZB_TAG_SOURCE) != 0u) {
            source_done(z, r->tag, r->u.done.result);
            return true;
        }
        return false; /* zbz_panel's request: its answer, if any, came as its own report */
    case ZSRC_ZB_REP_NETWORK:
        z->channel = r->u.network.channel;
        z->panel_ieee = r->u.network.panel_ieee;
        return false;
    case ZSRC_ZB_REP_JOIN_REFUSED:
    case ZSRC_ZB_REP_SURVEY:
    case ZSRC_ZB_REP_NONE:
        return false; /* not a device of ours: zbz_panel's */
    default:
        break;
    }
    if (!z->link_up || r->ieee == 0u) {
        return false; /* like every inbound call: nothing counts while the radio is down */
    }
    const uint64_t ieee = r->ieee;
    switch (r->kind) {
    case ZSRC_ZB_REP_JOINED:
        FOR_DEVICE(z, ieee, d) {
            d->join = r->u.joined.join;
            if (r->nwk != 0u) {
                d->nwk = r->nwk;
            }
            if (!r->u.joined.rejoin) {
                /* A fresh join of an address we know: it was reset, so it forgot its CIE address
                 * and the panel forgets what it said (ZB-012: its setup runs again). */
                forget_live(d);
                d->enroll = ZSRC_ZB_ENROLL_NONE;
                d->tries = 0u;
                d->next_try = 0u;
                d->new_join = z->learning;
                heard(z, d, 0u, now);
                if ((d->quirks & ZSRC_ZB_QUIRK_UNSUPPORTED) == 0u) {
                    start_enrollment(z, d, now);
                }
            } else {
                heard(z, d, 0u, now);
            }
        }
        break;
    case ZSRC_ZB_REP_ANNOUNCE:
        FOR_DEVICE(z, ieee, d) {
            if (r->nwk != 0u) {
                d->nwk = r->nwk; /* a rejoin keeps everything else */
            }
            const uint8_t caps = r->u.announce.mac_caps;
            take_node(d, d->node_known ? d->logical_type
                                       : ((caps & ZSRC_ZB_MAC_FFD) != 0u ? ZSRC_ZB_LOGICAL_ROUTER
                                                                         : ZSRC_ZB_LOGICAL_END_DEVICE),
                      caps);
            heard(z, d, 0u, now);
        }
        break;
    case ZSRC_ZB_REP_LEFT:
        if (!r->u.left.rejoin || r->u.left.asked) {
            zsrc_zb_device_gone(z, ieee, now); /* its zones run the grace into CHECK */
            break;
        }
        /* It will be back: its zones lose it until it speaks again, and what it said before
         * doesn't replay (ZSRC-015). */
        FOR_DEVICE(z, ieee, d) {
            forget_live(d);
            d->heard_at = 0u;
            if (!d->unreachable) {
                d->unreachable = true;
                if (find_watch(z, d->ieee, d->endpoint) != NULL) {
                    emit_reachable(z, d, false, now);
                }
            }
        }
        break;
    case ZSRC_ZB_REP_NODE_DESC:
        FOR_DEVICE(z, ieee, d) {
            take_node(d, r->u.node.logical_type, r->u.node.mac_caps);
            heard(z, d, 0u, now);
        }
        break;
    case ZSRC_ZB_REP_POWER_DESC:
        FOR_DEVICE(z, ieee, d) {
            if (!d->node_known) {
                d->router = r->u.power.current_mode == 0u; /* receiver on when idle */
            }
            if (d->power_source == 0u) {
                const unsigned cur = r->u.power.current_source;
                d->power_source = (cur & 0x01u) != 0u ? 1u : (cur & 0x06u) != 0u ? 3u : 0u;
            }
            heard(z, d, 0u, now);
        }
        break;
    case ZSRC_ZB_REP_SIMPLE_DESC: {
        const uint8_t ep = r->u.simple.endpoint != 0u ? r->u.simple.endpoint : r->endpoint;
        zsrc_zb_device_t *d = find_dev(z, ieee, ep);
        if (d == NULL) {
            /* Only endpoints zbz_panel described become slots: a vendor profile's or Green
             * Power's endpoint is not a sensor (testing.md §2). */
            FOR_DEVICE(z, ieee, other) {
                heard(z, other, 0u, now);
            }
            break;
        }
        uint16_t bits = 0u;
        const uint8_t n = r->u.simple.in_count < ZSRC_ZB_EP_CLUSTERS_MAX
                              ? r->u.simple.in_count
                              : (uint8_t)ZSRC_ZB_EP_CLUSTERS_MAX;
        for (uint8_t k = 0; k < n; k++) {
            bits = (uint16_t)(bits | zsrc_zb_cluster_bit(r->u.simple.in[k]));
        }
        const bool had_ias = (d->clusters & ZSRC_ZB_CL_IAS_ZONE) != 0u;
        d->clusters = (uint16_t)(d->clusters | bits); /* a cluster it reported on stays */
        apply_quirks(d);
        heard(z, d, 0u, now);
        if (!had_ias && (d->clusters & ZSRC_ZB_CL_IAS_ZONE) != 0u &&
            (d->quirks & ZSRC_ZB_QUIRK_UNSUPPORTED) == 0u) {
            start_enrollment(z, d, now);
        }
        break;
    }
    case ZSRC_ZB_REP_BASIC:
        FOR_DEVICE(z, ieee, d) {
            const zsrc_zb_basic_t *b = &r->u.basic;
            if ((b->have & ZSRC_ZB_BASIC_MANUFACTURER) != 0u) {
                copy_str(d->manufacturer, sizeof(d->manufacturer), b->manufacturer);
            }
            if ((b->have & ZSRC_ZB_BASIC_MODEL) != 0u) {
                copy_str(d->model, sizeof(d->model), b->model);
            }
            if ((b->have & ZSRC_ZB_BASIC_POWER) != 0u) {
                d->power_source = b->power_source;
            }
            apply_quirks(d); /* the quirks table answers to manufacturer and model */
            heard(z, d, 0u, now);
        }
        break;
    case ZSRC_ZB_REP_LINK:
        FOR_DEVICE(z, ieee, d) {
            if (r->u.link.rssi != 0) {
                d->rssi = r->u.link.rssi;
            }
            heard(z, d, r->u.link.lqi, now); /* lqi 0: not measured, the last one stays */
        }
        break;
    case ZSRC_ZB_REP_ACTIVE_EP:
    case ZSRC_ZB_REP_CHECK_IN:
    case ZSRC_ZB_REP_ZCL:
        /* It answered, checked in (a sleepy device listens for a moment: what is owed to it goes
         * now) or sent a command: alive and awake. */
        FOR_DEVICE(z, ieee, d) {
            heard(z, d, 0u, now);
        }
        break;
    default:
        break;
    }
    return false;
}

/* ---- the source interface --------------------------------------------------------------------- */

static bool zb_usable(void *ctx)
{
    const zsrc_zb_t *z = ctx;
    return z->link_up && !z->jammed; /* ZSRC-005 */
}

static bool zb_valid(void *ctx, const zsrc_address_t *a)
{
    zsrc_zb_t *z = ctx;
    if (a->kind != ZSRC_ZIGBEE || a->id == 0u || a->input == 0u || a->ref[0] != '\0') {
        return false;
    }
    const zsrc_zb_device_t *d = find_dev(z, a->id, a->input);
    /* A zone can only be pointed at a device that is paired and that we can actually read
     * (ZB-007: a Tuya radio on its own manufacturer cluster is listed, so the installer can see
     * that it joined, but binding it would make a zone that never faults). */
    return d != NULL && (d->quirks & ZSRC_ZB_QUIRK_UNSUPPORTED) == 0u && usable_clusters(d->clusters);
}

static bool zb_watch(void *ctx, const zsrc_address_t *a, const zsrc_mapping_t *m, uint64_t now)
{
    zsrc_zb_t *z = ctx;
    if (a->kind != ZSRC_ZIGBEE || a->id == 0u || a->input == 0u) {
        return false;
    }
    zsrc_zb_watch_t *w = find_watch(z, a->id, a->input);
    if (w == NULL) {
        for (size_t i = 0; i < ZSRC_ZB_WATCH_MAX; i++) {
            if (!z->watch[i].used) {
                w = &z->watch[i];
                break;
            }
        }
        if (w == NULL) {
            return false; /* ZSRC_ZB_WATCH_MAX zones on this source already */
        }
        memset(w, 0, sizeof(*w));
        w->used = true;
        w->ieee = a->id;
        w->endpoint = a->input;
    }
    w->invert = m != NULL && m->invert;
    w->mode = m != NULL ? m->mode : (uint8_t)ZSRC_MAP_BINARY;

    /* Say at once what we know, so a new binding or a changed mapping settles the zone without
     * waiting for the sensor to move - but only while the radio is up: what a device said
     * before the link broke is old news and must not count as it answering (ZSRC-015). */
    zsrc_zb_device_t *d = find_dev(z, a->id, a->input);
    if (d != NULL && z->told_up && d->heard_at != 0u && !d->unreachable &&
        now >= d->heard_at && now - d->heard_at < supervision_ms(z, d)) {
        /* The input the zone watches, as its reports would say it (ZB32); a known IAS status
         * is told whatever the mapping, for its tamper, trouble and battery bits. */
        const zb_input_t input = watched_input(w, d);
        if (d->status_known) {
            report_status(z, d, d->status, true, true, now);
        }
        if (d->on_known && input == ZB_INPUT_SWITCH) {
            emit_input(z, d, d->on ? "on" : "off", d->on ? 1 : 0, now);
        }
        if (d->has_value && input == ZB_INPUT_TEMP) {
            emit_input(z, d, NULL, d->value_x100, now);
        }
        update_battery(z, d, true, now);
    }
    return true;
}

static void zb_unwatch(void *ctx, const zsrc_address_t *a)
{
    zsrc_zb_t *z = ctx;
    zsrc_zb_watch_t *w = find_watch(z, a->id, a->input);
    if (w != NULL) {
        memset(w, 0, sizeof(*w));
    }
}

static bool zb_learn(void *ctx, bool on, uint64_t now)
{
    zsrc_zb_t *z = ctx;
    if (on) {
        if (!z->told_up || z->radio.permit_join == NULL) {
            return false;
        }
        if (!z->radio.permit_join(z->radio.ctx, (uint16_t)ZSRC_ZB_LEARN_S)) {
            return false;
        }
        z->learning = true;
        z->learn_until = add_ms(now, (uint64_t)ZSRC_ZB_LEARN_S * 1000u);
        for (size_t i = 0; i < ZSRC_ZB_DEV_MAX; i++) {
            z->dev[i].new_join = false;
        }
        return true;
    }
    z->learning = false;
    z->learn_until = 0u;
    if (z->radio.permit_join != NULL) {
        (void)z->radio.permit_join(z->radio.ctx, 0u); /* ZB-002: the window closes behind us */
    }
    return true;
}

/* The device's state in its own words, for the picker and the zone page: the alarm bits as the
 * model means them (the INVERT quirk turns them round, as status_faulted does), not as a zone's
 * own invert choice reads them. */
static void state_words(const zsrc_zb_device_t *d, char *out, size_t len)
{
    out[0] = '\0';
    if (d->device_class == ALARM_DEVCLASS_TEMPERATURE && d->has_value) {
        const int32_t v = d->value_x100;
        const long whole = (long)(v / 100);
        const long frac = (long)(v % 100 < 0 ? -(v % 100) : v % 100);
        snprintf(out, len, "%s%ld.%02ld", (v < 0 && whole == 0) ? "-" : "", whole, frac);
        return;
    }
    bool active;
    if (d->status_known) {
        active = ((d->status & alarm_mask(d)) != 0u) != ((d->quirks & ZSRC_ZB_QUIRK_INVERT) != 0u);
    } else if (d->on_known) {
        active = d->on;
    } else {
        return; /* nothing heard yet */
    }
    const char *on_word = "open";
    const char *off_word = "closed";
    switch (d->device_class) {
    case ALARM_DEVCLASS_MOTION:
        on_word = "motion";
        off_word = "clear";
        break;
    case ALARM_DEVCLASS_WATER:
        on_word = "wet";
        off_word = "dry";
        break;
    case ALARM_DEVCLASS_SMOKE:
    case ALARM_DEVCLASS_HEAT:
    case ALARM_DEVCLASS_CO:
    case ALARM_DEVCLASS_GAS:
    case ALARM_DEVCLASS_GLASS_BREAK:
    case ALARM_DEVCLASS_VIBRATION:
        on_word = "alarm";
        off_word = "clear";
        break;
    case ALARM_DEVCLASS_PANIC_BUTTON:
    case ALARM_DEVCLASS_KEYFOB:
        on_word = "pressed";
        off_word = "idle";
        break;
    default:
        break;
    }
    snprintf(out, len, "%s", active ? on_word : off_word);
}

static void device_name(const zsrc_zb_device_t *d, char *out, size_t len)
{
    char ieee[24];
    if (d->model[0] != '\0') {
        snprintf(out, len, "%s", d->model);
    } else if (d->manufacturer[0] != '\0') {
        snprintf(out, len, "%s", d->manufacturer);
    } else {
        snprintf(out, len, "%s", zsrc_zb_ieee_str(d->ieee, ieee, sizeof(ieee)));
    }
}

static size_t zb_candidates(void *ctx, const char *query, uint32_t filter, zsrc_candidate_t *out,
                            size_t max)
{
    (void)query;
    (void)filter;
    zsrc_zb_t *z = ctx;
    size_t n = 0;
    /* Devices that joined during this learn window first: that is the one being held against
     * the door frame (ZB-002). */
    for (int pass = 0; pass < 2 && n < max; pass++) {
        for (size_t i = 0; i < ZSRC_ZB_DEV_MAX && n < max; i++) {
            const zsrc_zb_device_t *d = &z->dev[i];
            if (!d->used || d->new_join != (pass == 0)) {
                continue;
            }
            if (!usable_clusters(d->clusters) && (d->quirks & ZSRC_ZB_QUIRK_UNSUPPORTED) == 0u) {
                continue; /* a plug or a bulb: nothing here a zone could read */
            }
            zsrc_candidate_t *c = &out[n++];
            memset(c, 0, sizeof(*c));
            c->addr = zsrc_zb_address(d->ieee, d->endpoint);
            device_name(d, c->name, sizeof(c->name));
            char ieee[24];
            if (d->note != NULL) {
                snprintf(c->detail, sizeof(c->detail), "%s", d->note);
            } else if (d->manufacturer[0] != '\0' && d->model[0] != '\0') {
                snprintf(c->detail, sizeof(c->detail), "%s", d->manufacturer);
            } else {
                snprintf(c->detail, sizeof(c->detail), "%s",
                         zsrc_zb_ieee_str(d->ieee, ieee, sizeof(ieee)));
            }
            state_words(d, c->state, sizeof(c->state));
            c->device_class = d->device_class;
            c->changed = d->new_join;
        }
    }
    return n;
}

static void zb_describe(void *ctx, const zsrc_address_t *a, char *out, size_t len)
{
    zsrc_zb_t *z = ctx;
    char ieee[24];
    const zsrc_zb_device_t *d = find_dev(z, a->id, a->input);
    if (d == NULL) {
        snprintf(out, len, "%s (not paired)", zsrc_zb_ieee_str(a->id, ieee, sizeof(ieee)));
        return;
    }
    char name[48];
    device_name(d, name, sizeof(name));
    if (d->endpoint > 1u) {
        snprintf(out, len, "%s, endpoint %u", name, (unsigned)d->endpoint);
    } else {
        snprintf(out, len, "%s", name);
    }
}

static void zb_tick(void *ctx, uint64_t now)
{
    zsrc_zb_t *z = ctx;

    if (z->learning && z->learn_until != 0u && now >= z->learn_until) {
        (void)zb_learn(z, false, now); /* ZB-002: the window shuts by itself */
    }
    if (z->link_up && now >= z->next_scan) {
        z->next_scan = add_ms(now, ZSRC_ZB_SCAN_MS);
        if (z->radio.energy_scan != NULL) {
            (void)z->radio.energy_scan(z->radio.ctx);
        }
    }
    if (!z->told_up) {
        return; /* with the link down the hub owns the zones; nothing here to time */
    }
    for (size_t i = 0; i < ZSRC_ZB_DEV_MAX; i++) {
        zsrc_zb_device_t *d = &z->dev[i];
        if (!d->used) {
            continue;
        }
        if (d->router) {
            try_enroll(z, d, now); /* it never sleeps, so it never needs a wake window */
            test_step(z, d, now);
        }
        reread_step(z, d, false, now);
        if (d->unreachable) {
            continue;
        }
        const uint64_t since = d->heard_at != 0u ? d->heard_at : z->link_at;
        if (now - since >= supervision_ms(z, d)) {
            d->unreachable = true; /* ZB-005: silent past its window; the hub runs the grace */
            if (find_watch(z, d->ieee, d->endpoint) != NULL) {
                emit_reachable(z, d, false, now);
            }
        }
    }
}

#define ZB_CAPS                                                                                    \
    (ZSRC_CAP_LEARN | ZSRC_CAP_SUPERVISED | ZSRC_CAP_AUTH | ZSRC_CAP_TAMPER | ZSRC_CAP_BATTERY |    \
     ZSRC_CAP_OFFLINE_OK | ZSRC_CAP_LIFE_SAFETY)

/* What one sensor can promise (zone_source.h caps_of). Every Zigbee device is supervised by its
 * own frames and speaks over the network key, whichever way it joined: an install code is shown
 * on the device's page, never required for fire, CO or panic zones (decided
 * 2026-10-02, ZB-021). What differs is what it can report: tamper only from an IAS status word
 * the quirks table doesn't call noise, a low battery not from a mains device without one. */
static uint32_t zb_caps_of(void *ctx, const zsrc_address_t *a)
{
    zsrc_zb_t *z = ctx;
    uint32_t caps = ZB_CAPS;
    if (a->kind != ZSRC_ZIGBEE) {
        return caps;
    }
    const zsrc_zb_device_t *d = find_dev(z, a->id, a->input);
    if (d == NULL) {
        return caps; /* not described yet (a restart): what the source promises */
    }
    if ((d->clusters & ZSRC_ZB_CL_IAS_ZONE) == 0u || (d->quirks & ZSRC_ZB_QUIRK_NO_TAMPER) != 0u) {
        caps &= ~(uint32_t)ZSRC_CAP_TAMPER;
    }
    if (mains_only(d->power_source) && (d->clusters & ZSRC_ZB_CL_POWER_CFG) == 0u) {
        caps &= ~(uint32_t)ZSRC_CAP_BATTERY;
    }
    return caps;
}

/* The hub's grace for one sensor (zone_source.h grace_of): the source's hour (ZSRC_ZB_GRACE_S,
 * devices.md §7) for every device it knows or may yet hear from; for one the installer forgot,
 * CHECK at once - it will not come back by itself (ZB-013). */
static uint32_t zb_grace_of(void *ctx, const zsrc_address_t *a)
{
    const zsrc_zb_t *z = ctx;
    if (a->kind == ZSRC_ZIGBEE && find_dev((zsrc_zb_t *)z, a->id, a->input) == NULL &&
        is_forgotten(z, a->id)) {
        return ZSRC_ZB_GONE_GRACE_S;
    }
    return 0u;
}

static uint64_t zb_next(void *ctx, uint64_t now)
{
    const zsrc_zb_t *z = ctx;
    uint64_t next = UINT64_MAX;
    if (z->learning && z->learn_until != 0u && z->learn_until < next) {
        next = z->learn_until;
    }
    if (z->link_up && z->next_scan < next) {
        next = z->next_scan;
    }
    if (!z->told_up) {
        return next;
    }
    for (size_t i = 0; i < ZSRC_ZB_DEV_MAX; i++) {
        const zsrc_zb_device_t *d = &z->dev[i];
        if (!d->used) {
            continue;
        }
        if (d->router &&
            (d->enroll == ZSRC_ZB_ENROLL_CIE || d->enroll == ZSRC_ZB_ENROLL_RESPOND)) {
            next = d->next_try < next ? d->next_try : next;
        }
        if (d->router) {
            uint64_t t = UINT64_MAX;
            if (d->test == ZSRC_ZB_TEST_OWED_ON || d->test == ZSRC_ZB_TEST_OWED_OFF) {
                t = d->test_at == 0u ? now : add_ms(d->test_at, ZSRC_ZB_ENROLL_GAP_MS);
            } else if (z->walk_test &&
                       (d->test == ZSRC_ZB_TEST_ASKED_ON || d->test == ZSRC_ZB_TEST_ON)) {
                t = add_ms(d->test_at, ZSRC_ZB_TEST_REFRESH_MS);
            } else if (z->walk_test && d->test == ZSRC_ZB_TEST_NONE && test_wanted((zsrc_zb_t *)z, d)) {
                t = now; /* bound or enrolled since the walk test began */
            }
            next = t < next ? t : next;
        }
        if (d->reread == ZSRC_ZB_REREAD_DUE) {
            next = d->reread_at < next ? d->reread_at : next;
        } else if (d->reread == ZSRC_ZB_REREAD_ASKED) {
            const uint64_t t = add_ms(d->reread_at, ZSRC_ZB_REREAD_WAIT_MS);
            next = t < next ? t : next;
        } else if (d->reread == ZSRC_ZB_REREAD_NEXT_FRAME && d->router) {
            next = now < next ? now : next;
        }
        if (d->unreachable) {
            continue;
        }
        const uint64_t since = d->heard_at != 0u ? d->heard_at : z->link_at;
        const uint64_t t = add_ms(since, supervision_ms(z, d));
        next = t < next ? t : next;
    }
    return next;
}

const zone_source_ops_t *zsrc_zb_ops(void)
{
    static const zone_source_ops_t ops = {
        .kind = ZSRC_ZIGBEE,
        /* The trust table of docs/features/22: paired in a learn window, supervised by its own
         * check-ins, authenticated by the network key, tamper and battery straight off the IAS
         * status word, and it keeps working with no Wi-Fi and no Home Assistant - which is why
         * this is the one radio source trusted for fire, CO and panic zones by default
         * (ZSRC-008). */
        .caps = ZB_CAPS,
        .name = "Zigbee sensor",
        .grace_s = (uint16_t)ZSRC_ZB_GRACE_S,
        .usable = zb_usable,
        .valid = zb_valid,
        .watch = zb_watch,
        .unwatch = zb_unwatch,
        .learn = zb_learn,
        .candidates = zb_candidates,
        .describe = zb_describe,
        .tick = zb_tick,
        .next_tick = zb_next,
        .caps_of = zb_caps_of,
        .grace_of = zb_grace_of,
    };
    return &ops;
}
