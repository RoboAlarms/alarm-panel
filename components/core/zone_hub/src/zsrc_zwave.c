/*
 * zsrc_zwave.c - Z-Wave devices as zones (see zsrc_zwave.h; docs/features/36 zones.md). Portable:
 * components/core/zwave_zones feeds it on the panel and in the PC simulator, the host tests feed
 * it directly.
 */
#include <stdio.h>
#include <string.h>

#include "zone_hub/zsrc_zwave.h"

/* ---- small helpers ------------------------------------------------------------------------- */

static uint64_t add_ms(uint64_t t, uint64_t ms)
{
    return (t > UINT64_MAX - ms) ? UINT64_MAX : t + ms;
}

static uint32_t bit(unsigned n)
{
    return n < 32u ? 1u << n : 0u;
}

/* ---- addresses (zones.md §1) ----------------------------------------------------------------- */

zsrc_address_t zsrc_zwave_address(uint32_t home_id, uint16_t generation, uint16_t node,
                                  uint8_t endpoint, uint8_t slot)
{
    const uint64_t id = (uint64_t)home_id << 32 | (uint64_t)generation << 16 | (uint64_t)node;
    return zsrc_address(ZSRC_ZWAVE, (uint8_t)((endpoint & 0x07u) << 5 | (slot & 0x1Fu)), id);
}

bool zsrc_zw_address_parse(const zsrc_address_t *a, uint32_t *home_id, uint16_t *generation,
                           uint16_t *node, uint8_t *endpoint, uint8_t *slot)
{
    if (a == NULL || a->kind != ZSRC_ZWAVE || a->ref[0] != '\0' || (a->input & 0x1Fu) == 0u ||
        (a->id & 0xFFFFu) == 0u || ((a->id >> 16) & 0xFFFFu) == 0u) {
        return false;
    }
    *home_id = (uint32_t)(a->id >> 32);
    *generation = (uint16_t)(a->id >> 16);
    *node = (uint16_t)a->id;
    *endpoint = (uint8_t)(a->input >> 5);
    *slot = (uint8_t)(a->input & 0x1Fu);
    return true;
}

/* ---- the states a device holds, and which input each feeds ----------------------------------- */

/* The states of one endpoint (bits of vars / vknown): a notification's state variables
 * (command-classes.md §4.5), Binary Sensor types, Alarm Sensor types, Basic. */
enum {
    V_SMOKE = 0, V_CO, V_OVERHEAT, V_UNDERHEAT, V_WATER, V_DOOR, V_INTRUSION, V_MOTION, V_MAINS,
    V_FREEZE, V_GAS_COMB, V_GAS_TOXIC,
    V_BIN_GENERAL, V_BIN_SMOKE, V_BIN_CO, V_BIN_HEAT, V_BIN_WATER, V_BIN_FREEZE, V_BIN_AUX,
    V_BIN_DOOR, V_BIN_TILT, V_BIN_MOTION, V_BIN_GLASS,
    V_ALARM_SMOKE, V_ALARM_CO, V_ALARM_HEAT, V_ALARM_WATER, V_ALARM_GENERAL,
    V_BASIC,
    V_COUNT
};

/* zones.md §2's "Reported by" column, read the other way round. */
static const uint8_t V_SLOT[V_COUNT] = {
    ZSRC_ZW_SLOT_SMOKE, ZSRC_ZW_SLOT_CO, ZSRC_ZW_SLOT_HEAT, ZSRC_ZW_SLOT_FREEZE,
    ZSRC_ZW_SLOT_WATER, ZSRC_ZW_SLOT_CONTACT, ZSRC_ZW_SLOT_INTRUSION, ZSRC_ZW_SLOT_MOTION,
    ZSRC_ZW_SLOT_MAINS, ZSRC_ZW_SLOT_FREEZE, ZSRC_ZW_SLOT_GAS, ZSRC_ZW_SLOT_GAS,
    ZSRC_ZW_SLOT_GENERAL, ZSRC_ZW_SLOT_SMOKE, ZSRC_ZW_SLOT_CO, ZSRC_ZW_SLOT_HEAT,
    ZSRC_ZW_SLOT_WATER, ZSRC_ZW_SLOT_FREEZE, ZSRC_ZW_SLOT_GENERAL, ZSRC_ZW_SLOT_CONTACT,
    ZSRC_ZW_SLOT_TILT, ZSRC_ZW_SLOT_MOTION, ZSRC_ZW_SLOT_GLASS_BREAK,
    ZSRC_ZW_SLOT_SMOKE, ZSRC_ZW_SLOT_CO, ZSRC_ZW_SLOT_HEAT, ZSRC_ZW_SLOT_WATER,
    ZSRC_ZW_SLOT_GENERAL,
    ZSRC_ZW_SLOT_GENERAL,
};

/* Binary Sensor types (command-classes.md §4.3) that are inputs; 08 is the device's tamper. */
static const struct {
    uint8_t type, var;
} BINARY[] = {
    {0x01u, V_BIN_GENERAL}, {0x02u, V_BIN_SMOKE}, {0x03u, V_BIN_CO},     {0x05u, V_BIN_HEAT},
    {0x06u, V_BIN_WATER},   {0x07u, V_BIN_FREEZE}, {0x09u, V_BIN_AUX},   {0x0Au, V_BIN_DOOR},
    {0x0Bu, V_BIN_TILT},    {0x0Cu, V_BIN_MOTION}, {0x0Du, V_BIN_GLASS},
};
#define BINARY_TAMPER 0x08u
#define BINARY_GENERAL 0x01u

/* Alarm Sensor types (§4.22) zones.md §2 names: 00 general purpose, 01 smoke, 02 CO, 04 heat,
 * 05 water (ZW-043). */
static const struct {
    uint8_t type, var;
} ALARM_SENSOR[] = {{0x00u, V_ALARM_GENERAL}, {0x01u, V_ALARM_SMOKE}, {0x02u, V_ALARM_CO},
                    {0x04u, V_ALARM_HEAT},    {0x05u, V_ALARM_WATER}};

/* Why a device is tampered, low on battery or in trouble (bits of tamper_src etc.). */
#define T_COVER_HS  0x01u /* Home Security 03 */
#define T_COVER_SYS 0x02u /* System 06 */
#define T_BINARY    0x04u /* Binary Sensor type 08 */
#define T_MOVED     0x08u /* Home Security 09 on a device with a cover: ZSRC_ZW_MOVED_S */
#define COND_BINARY 0x80000000u /* cond[e]: Binary Sensor type 08 at the endpoint (ZW21) */
#define B_REPORT    0x01u /* Battery Report */
#define B_PM_BATT   0x02u /* Power Management 0A/0B: replace battery soon/now */
#define B_PM_CHARGE 0x04u /* 0E/0F: charge battery soon/now */
#define B_PM_BACKUP 0x08u /* 10: backup battery low */
/* Trouble: one bit per state variable (Z-Wave JS Notifications.ts), so the idle of one variable
 * never ends another's (ZW-013). Runtime only (trouble_src): nothing stores these bits. */
#define R_SMOKE         0x0001u /* Smoke 04/05: replacement required, end of life */
#define R_CO            0x0002u /* CO 04/05: replacement required, end of life */
#define R_HEAT          0x0004u /* Heat 08: replacement required, end of life */
#define R_GAS           0x0008u /* Gas 06: replacement required */
#define R_SYS_HW        0x0010u /* System 01/03: hardware failure */
#define R_SYS_SW        0x0020u /* System 02/04: software failure */
#define R_SMOKE_INSPECT 0x0040u /* Smoke 07: periodic inspection due */
#define R_SMOKE_DUST    0x0080u /* Smoke 08: dust in the device */
#define R_CO_INSPECT    0x0100u /* CO 07: periodic inspection due */
#define R_CO_TEST       0x0200u /* CO 03 with parameter 02: test failed */
#define R_HEAT_DUST     0x0400u /* Heat 0A: dust in the device */
#define R_HEAT_INSPECT  0x0800u /* Heat 0B: periodic inspection due */
#define CO_TEST_OK      0x01u   /* CO 03's parameter (command-classes.md §4.5) */
#define CO_TEST_FAILED  0x02u

/* What a notification event does (zones.md §3). */
enum {
    A_STATE = 1, /* its variable takes this state: arg V_* active */
    A_CLEAR,     /* its variable takes its inactive state (door closed, mains back): arg V_* */
    A_PULSE,     /* a momentary input: arg the slot */
    A_TAMPER,    /* arg T_*; its idle restores */
    A_TROUBLE,   /* arg R_*; its idle restores */
    A_BATTERY,   /* arg B_*; its idle restores */
    A_MOVED,     /* product moved: tamper on a device with a cover, else the impact input */
    A_TEST,      /* a test of the detector: a check-in, "tested" on its page (open question 3);
                  * arg R_*: the trouble a failed test (parameter 02) raises, a passed one ends */
    A_CHECKIN,   /* a heartbeat */
    A_NOTHING    /* understood, and nothing for a zone (alarm silenced, an invalid code) */
};

#define F_NO_IDLE 0x01u /* the variable has no idle state (door, mains, CO test) */

typedef struct {
    uint8_t type, event, action;
    uint16_t arg;  /* per action: a V_*, a slot, T_*, R_* or B_* */
    uint8_t var;   /* the state variable within the type (idle ends it); 0: an event */
    uint8_t flags; /* F_* */
} nrow_t;

/* command-classes.md §4.5's events the panel understands. */
static const nrow_t NROWS[] = {
    {0x01u, 0x01u, A_STATE, V_SMOKE, 1u, 0u},
    {0x01u, 0x02u, A_STATE, V_SMOKE, 1u, 0u},
    {0x01u, 0x03u, A_TEST, 0u, 2u, 0u},
    {0x01u, 0x06u, A_NOTHING, 0u, 2u, 0u},
    {0x01u, 0x04u, A_TROUBLE, R_SMOKE, 3u, 0u}, /* maintenance: replacement, end of life */
    {0x01u, 0x05u, A_TROUBLE, R_SMOKE, 3u, 0u},
    {0x01u, 0x07u, A_TROUBLE, R_SMOKE_INSPECT, 4u, 0u}, /* periodic inspection due */
    {0x01u, 0x08u, A_TROUBLE, R_SMOKE_DUST, 5u, 0u},    /* dust in the device */
    {0x02u, 0x01u, A_STATE, V_CO, 1u, 0u},
    {0x02u, 0x02u, A_STATE, V_CO, 1u, 0u},
    {0x02u, 0x03u, A_TEST, R_CO_TEST, 2u, F_NO_IDLE}, /* parameter 01 OK, 02 failed */
    {0x02u, 0x04u, A_TROUBLE, R_CO, 3u, 0u}, /* maintenance: replacement, end of life */
    {0x02u, 0x05u, A_TROUBLE, R_CO, 3u, 0u},
    {0x02u, 0x07u, A_TROUBLE, R_CO_INSPECT, 5u, 0u}, /* periodic inspection due */
    {0x02u, 0x06u, A_NOTHING, 0u, 4u, 0u},
    {0x04u, 0x01u, A_STATE, V_OVERHEAT, 1u, 0u},
    {0x04u, 0x02u, A_STATE, V_OVERHEAT, 1u, 0u},
    {0x04u, 0x03u, A_PULSE, ZSRC_ZW_SLOT_HEAT, 0u, 0u}, /* rapid temperature rise: an event */
    {0x04u, 0x04u, A_PULSE, ZSRC_ZW_SLOT_HEAT, 0u, 0u},
    {0x04u, 0x05u, A_STATE, V_UNDERHEAT, 1u, 0u},
    {0x04u, 0x06u, A_STATE, V_UNDERHEAT, 1u, 0u},
    {0x04u, 0x07u, A_TEST, 0u, 2u, 0u},
    {0x04u, 0x08u, A_TROUBLE, R_HEAT, 3u, 0u}, /* maintenance: replacement, end of life */
    {0x04u, 0x0Au, A_TROUBLE, R_HEAT_DUST, 4u, 0u},    /* dust in the device */
    {0x04u, 0x0Bu, A_TROUBLE, R_HEAT_INSPECT, 5u, 0u}, /* periodic inspection due */
    {0x04u, 0x09u, A_NOTHING, 0u, 2u, 0u},
    {0x05u, 0x01u, A_STATE, V_WATER, 1u, 0u},
    {0x05u, 0x02u, A_STATE, V_WATER, 1u, 0u},
    {0x06u, 0x16u, A_STATE, V_DOOR, 1u, F_NO_IDLE},
    {0x06u, 0x17u, A_CLEAR, V_DOOR, 1u, F_NO_IDLE},
    {0x07u, 0x01u, A_STATE, V_INTRUSION, 1u, 0u},
    {0x07u, 0x02u, A_STATE, V_INTRUSION, 1u, 0u},
    {0x07u, 0x03u, A_TAMPER, T_COVER_HS, 2u, 0u},
    {0x07u, 0x04u, A_NOTHING, 0u, 0u, 0u},
    {0x07u, 0x05u, A_PULSE, ZSRC_ZW_SLOT_GLASS_BREAK, 0u, 0u},
    {0x07u, 0x06u, A_PULSE, ZSRC_ZW_SLOT_GLASS_BREAK, 0u, 0u},
    {0x07u, 0x07u, A_STATE, V_MOTION, 3u, 0u},
    {0x07u, 0x08u, A_STATE, V_MOTION, 3u, 0u},
    {0x07u, 0x09u, A_MOVED, 0u, 0u, 0u},
    {0x07u, 0x0Au, A_PULSE, ZSRC_ZW_SLOT_IMPACT, 0u, 0u},
    {0x08u, 0x02u, A_STATE, V_MAINS, 1u, F_NO_IDLE},
    {0x08u, 0x03u, A_CLEAR, V_MAINS, 1u, F_NO_IDLE},
    {0x08u, 0x0Au, A_BATTERY, B_PM_BATT, 2u, 0u},
    {0x08u, 0x0Bu, A_BATTERY, B_PM_BATT, 2u, 0u},
    {0x08u, 0x0Eu, A_BATTERY, B_PM_CHARGE, 3u, 0u},
    {0x08u, 0x0Fu, A_BATTERY, B_PM_CHARGE, 3u, 0u},
    {0x08u, 0x10u, A_BATTERY, B_PM_BACKUP, 4u, 0u},
    {0x09u, 0x01u, A_TROUBLE, R_SYS_HW, 1u, 0u}, /* hardware failure: 01, 03 (with a code) */
    {0x09u, 0x03u, A_TROUBLE, R_SYS_HW, 1u, 0u},
    {0x09u, 0x02u, A_TROUBLE, R_SYS_SW, 2u, 0u}, /* software failure: 02, 04 (with a code) */
    {0x09u, 0x04u, A_TROUBLE, R_SYS_SW, 2u, 0u},
    {0x09u, 0x05u, A_CHECKIN, 0u, 0u, 0u},
    {0x09u, 0x06u, A_TAMPER, T_COVER_SYS, 3u, 0u},
    {0x0Au, 0x01u, A_PULSE, ZSRC_ZW_SLOT_EMERGENCY, 0u, 0u},
    {0x0Au, 0x02u, A_PULSE, ZSRC_ZW_SLOT_EMERGENCY, 0u, 0u},
    {0x0Au, 0x03u, A_PULSE, ZSRC_ZW_SLOT_EMERGENCY, 0u, 0u},
    {0x0Au, 0x04u, A_PULSE, ZSRC_ZW_SLOT_EMERGENCY, 0u, 0u},
    {0x10u, 0x03u, A_STATE, V_FREEZE, 1u, 0u},
    {0x12u, 0x01u, A_STATE, V_GAS_COMB, 1u, 0u},
    {0x12u, 0x02u, A_STATE, V_GAS_COMB, 1u, 0u},
    {0x12u, 0x03u, A_STATE, V_GAS_TOXIC, 2u, 0u},
    {0x12u, 0x04u, A_STATE, V_GAS_TOXIC, 2u, 0u},
    {0x12u, 0x05u, A_TEST, 0u, 3u, 0u},
    {0x12u, 0x06u, A_TROUBLE, R_GAS, 4u, 0u},
};
#define NROW_COUNT (sizeof(NROWS) / sizeof(NROWS[0]))
_Static_assert(NROW_COUNT <= ZSRC_ZW_NROW_WORDS * 32u,
               "zsrc_zw_device_t's v1_events keeps one bit per NROWS row: raise ZSRC_ZW_NROW_WORDS");

static const nrow_t *nrow(uint8_t type, uint8_t event)
{
    for (size_t i = 0; i < NROW_COUNT; i++) {
        if (NROWS[i].type == type && NROWS[i].event == event) {
            return &NROWS[i];
        }
    }
    return NULL;
}

/* ---- which event a variable holds (ZW21, ZW22) ------------------------------------------------
 * A notification variable holds one event at a time, per endpoint (Z-Wave JS's value per
 * endpoint and variable): an idle naming another event than the one it holds is stale and dropped.
 * A state variable's event is its V_* bit in vars[] plus, for the second event of a pair (Smoke
 * 02 after 01), its bit in alt[]; a tamper, trouble or battery variable's is its row's bit in
 * cond[], so each endpoint owns its conditions and the device's are their sum (sum_conditions). */

/* A condition's event: tamper, trouble, battery, and CO's failed test. */
static bool cond_row(const nrow_t *row)
{
    return row->action == A_TAMPER || row->action == A_TROUBLE || row->action == A_BATTERY ||
           (row->action == A_TEST && row->arg != 0u);
}

/* Its bit in cond[]: the condition rows counted in NROWS's order; bit 31 is COND_BINARY, so the
 * table keeps at most 31 condition rows (it has 23). */
static uint32_t cond_bit(const nrow_t *row)
{
    unsigned k = 0u;
    for (const nrow_t *p = NROWS; p < row; p++) {
        k += cond_row(p) ? 1u : 0u;
    }
    return k < 31u ? bit(k) : 0u;
}

/* Whether a state event is the second of its pair: an earlier row gives the same state. */
static bool second(const nrow_t *row)
{
    for (const nrow_t *p = NROWS; p < row; p++) {
        if (p->type == row->type && p->action == row->action && p->arg == row->arg) {
            return true;
        }
    }
    return false;
}

_Static_assert(V_GAS_TOXIC < 16 && V_DOOR < 16 && V_MAINS < 16,
               "zsrc_zw_device_t's alt keeps one bit per notification state variable");

/* The event a variable holds at an endpoint now; NULL when none (idle, or never reported). */
static const nrow_t *held(const zsrc_zw_device_t *d, uint8_t ep, uint8_t type, uint8_t var)
{
    for (size_t r = 0; var != 0u && r < NROW_COUNT; r++) {
        const nrow_t *row = &NROWS[r];
        if (row->type != type || row->var != var) {
            continue;
        }
        if (row->action == A_STATE) {
            const bool alt = ((unsigned)d->alt[ep] & bit(row->arg)) != 0u;
            if ((d->vars[ep] & bit(row->arg)) != 0u && alt == second(row)) {
                return row;
            }
        } else if (cond_row(row) && (d->cond[ep] & cond_bit(row)) != 0u) {
            return row;
        }
    }
    return NULL;
}

/* A condition variable takes this event at the endpoint (its other events end). */
static void set_cond(zsrc_zw_device_t *d, uint8_t ep, const nrow_t *row)
{
    for (size_t r = 0; r < NROW_COUNT; r++) {
        const nrow_t *o = &NROWS[r];
        if (o->type == row->type && o->var == row->var && cond_row(o)) {
            d->cond[ep] &= ~cond_bit(o);
        }
    }
    d->cond[ep] |= cond_bit(row);
}

/* The device's tamper, battery and trouble: every endpoint's, and its own (moved, the Battery
 * Report). */
static void sum_conditions(zsrc_zw_device_t *d)
{
    unsigned tamper = (unsigned)d->tamper_src & T_MOVED;
    unsigned low = (unsigned)d->lowbat_src & B_REPORT;
    unsigned trouble = 0u;
    uint32_t any = 0u;
    for (size_t e = 0; e < ZSRC_ZW_EP_MAX; e++) {
        any |= d->cond[e];
    }
    if ((any & COND_BINARY) != 0u) {
        tamper |= T_BINARY;
    }
    unsigned k = 0u;
    for (size_t r = 0; r < NROW_COUNT; r++) {
        const nrow_t *row = &NROWS[r];
        if (!cond_row(row)) {
            continue;
        }
        if ((any & bit(k)) != 0u && k < 31u) {
            switch (row->action) {
            case A_TAMPER: tamper |= row->arg; break;
            case A_BATTERY: low |= row->arg; break;
            default: trouble |= row->arg; break; /* A_TROUBLE, A_TEST */
            }
        }
        k++;
    }
    d->tamper_src = (uint8_t)tamper;
    d->lowbat_src = (uint8_t)low;
    d->trouble_src = (uint16_t)trouble;
}

/* Momentary inputs (zones.md §3): "on", then "off" after the binding's hold. */
static int momentary(uint8_t slot)
{
    switch (slot) {
    case ZSRC_ZW_SLOT_GLASS_BREAK: return 0;
    case ZSRC_ZW_SLOT_IMPACT: return 1;
    case ZSRC_ZW_SLOT_EMERGENCY: return 2;
    default: return -1;
    }
}

/* Where a slot's last pulse is kept (pulse_at): the momentary inputs, and heat, whose rapid
 * temperature rise (Heat 03/04) is an event held like them on an active/idle input (ZW-013). */
static int pulse_index(uint8_t slot)
{
    return slot == ZSRC_ZW_SLOT_HEAT ? 3 : momentary(slot);
}

static bool life_safety(uint8_t slot)
{
    return slot == ZSRC_ZW_SLOT_SMOKE || slot == ZSRC_ZW_SLOT_CO || slot == ZSRC_ZW_SLOT_HEAT ||
           slot == ZSRC_ZW_SLOT_GAS;
}

/* The slot a state or a pulse feeds on this device: the quirk may move one (zones.md §8). */
static uint8_t route(const zsrc_zw_device_t *d, uint8_t slot)
{
    const zsrc_zw_quirk_t *q = d->quirk;
    return q != NULL && q->remap_from != 0u && slot == q->remap_from ? q->remap_to : slot;
}

static uint8_t var_slot(const zsrc_zw_device_t *d, unsigned v)
{
    const zsrc_zw_quirk_t *q = d->quirk;
    if (v == V_BASIC && q != NULL && q->basic_slot != 0u) {
        return q->basic_slot;
    }
    return route(d, V_SLOT[v]);
}

/* Slots with an active (known) state at an endpoint. */
static uint32_t slots_of(const zsrc_zw_device_t *d, uint32_t vars)
{
    uint32_t s = 0u;
    for (unsigned v = 0; v < V_COUNT; v++) {
        if ((vars & bit(v)) != 0u) {
            s |= bit(var_slot(d, v));
        }
    }
    return s;
}

/* ---- the tables --------------------------------------------------------------------------- */

static zsrc_zw_device_t *find_dev(zsrc_zw_t *z, uint16_t node)
{
    for (size_t i = 0; node != 0u && i < ZSRC_ZW_DEV_MAX; i++) {
        if (z->dev[i].used && z->dev[i].node == node) {
            return &z->dev[i];
        }
    }
    return NULL;
}

const zsrc_zw_device_t *zsrc_zw_find(const zsrc_zw_t *z, uint16_t node)
{
    for (size_t i = 0; node != 0u && i < ZSRC_ZW_DEV_MAX; i++) {
        if (z->dev[i].used && z->dev[i].node == node) {
            return &z->dev[i];
        }
    }
    return NULL;
}

static bool offers(const zsrc_zw_device_t *d, uint8_t endpoint, uint8_t slot)
{
    return endpoint < ZSRC_ZW_EP_MAX && (d->offered[endpoint] & bit(slot)) != 0u;
}

/* The watch names this device (whatever became of it). */
static bool watch_of(const zsrc_zw_t *z, const zsrc_zw_watch_t *w, const zsrc_zw_device_t *d)
{
    return w->used && d->used && w->home_id == z->home_id && w->node == d->node &&
           w->generation == d->generation;
}

/* ... and the device can feed it: it is in the network and offers that input. */
static bool live(const zsrc_zw_t *z, const zsrc_zw_watch_t *w, const zsrc_zw_device_t *d)
{
    return watch_of(z, w, d) && !d->gone && offers(d, w->endpoint, w->slot);
}

static zsrc_address_t watch_addr(const zsrc_zw_watch_t *w)
{
    return zsrc_zwave_address(w->home_id, w->generation, w->node, w->endpoint, w->slot);
}

/* Endpoints with a live watch (bit e). */
static uint8_t watched_endpoints(const zsrc_zw_t *z, const zsrc_zw_device_t *d)
{
    uint8_t eps = 0u;
    for (size_t i = 0; i < ZSRC_ZW_WATCH_MAX; i++) {
        if (live(z, &z->watch[i], d)) {
            eps = (uint8_t)((unsigned)eps | bit((unsigned)z->watch[i].endpoint));
        }
    }
    return eps;
}

/* ---- what the hub hears ---------------------------------------------------------------------- */

static void out_input(zsrc_zw_t *z, const zsrc_zw_watch_t *w, const char *state, int32_t value,
                      uint64_t now)
{
    if (z->link_up && z->sink.input != NULL) {
        const zsrc_address_t a = watch_addr(w);
        z->emitted++;
        z->sink.input(z->sink.ctx, &a, state, value, now);
    }
}

static void out_event(zsrc_zw_t *z, const zsrc_zw_watch_t *w, alarm_zone_event_t ev, uint64_t now)
{
    if (z->link_up && z->sink.event != NULL) {
        const zsrc_address_t a = watch_addr(w);
        z->emitted++;
        z->sink.event(z->sink.ctx, &a, ev, now);
    }
}

/* Not counted in `emitted`: a frame that only ends a device's unreachability changed nothing of its
 * inputs, and is their check-in (zones.md §3). */
static void out_reachable(zsrc_zw_t *z, const zsrc_zw_watch_t *w, bool ok, uint64_t now)
{
    if (z->link_up && z->sink.reachable != NULL) {
        const zsrc_address_t a = watch_addr(w);
        z->sink.reachable(z->sink.ctx, &a, ok, now);
    }
}

static bool state_known(const zsrc_zw_device_t *d, uint8_t ep, uint8_t slot)
{
    return (slots_of(d, d->vknown[ep]) & bit(slot)) != 0u;
}

static bool state_active(const zsrc_zw_device_t *d, uint8_t ep, uint8_t slot)
{
    return (slots_of(d, d->vars[ep] & d->vknown[ep]) & bit(slot)) != 0u;
}

/* One input's state, as the source knows it now; `ended`: its pulse just ended, so it is told
 * even when no state of it was ever reported (heat's rapid rise on a device that never sent
 * overheat: "off" again, ZW-013). */
static void tell_state_ex(zsrc_zw_t *z, const zsrc_zw_device_t *d, const zsrc_zw_watch_t *w,
                          uint64_t now, bool ended)
{
    const uint8_t ep = w->endpoint;
    const uint8_t slot = w->slot;
    if (slot == ZSRC_ZW_SLOT_TEMPERATURE) {
        if (((unsigned)d->temp_known & bit((unsigned)ep)) != 0u) {
            out_input(z, w, NULL, d->temp_x100[ep], now);
        }
        return;
    }
    const bool pulse = pulse_index(slot) >= 0 && w->pulse_until != 0u;
    const bool known = state_known(d, ep, slot) || pulse || ended ||
                       (momentary(slot) >= 0 && d->heard_at != 0u);
    if (!known) {
        return;
    }
    const bool on = pulse || state_active(d, ep, slot);
    const bool contact = slot == ZSRC_ZW_SLOT_CONTACT;
    out_input(z, w, on ? (contact ? "open" : "on") : (contact ? "closed" : "off"), on ? 100 : 0,
              now);
}

static void tell_state(zsrc_zw_t *z, const zsrc_zw_device_t *d, const zsrc_zw_watch_t *w,
                       uint64_t now)
{
    tell_state_ex(z, d, w, now, false);
}

static void tell_slot(zsrc_zw_t *z, zsrc_zw_device_t *d, uint8_t ep, uint8_t slot, uint64_t now)
{
    for (size_t i = 0; i < ZSRC_ZW_WATCH_MAX; i++) {
        const zsrc_zw_watch_t *w = &z->watch[i];
        if (live(z, w, d) && w->endpoint == ep && w->slot == slot) {
            tell_state(z, d, w, now);
        }
    }
}

/* A device-level event for its inputs: every one, or only the life-safety ones (trouble). */
static void tell_device(zsrc_zw_t *z, zsrc_zw_device_t *d, alarm_zone_event_t ev, bool safety_only,
                        uint64_t now)
{
    for (size_t i = 0; i < ZSRC_ZW_WATCH_MAX; i++) {
        const zsrc_zw_watch_t *w = &z->watch[i];
        if (live(z, w, d) && (!safety_only || life_safety(w->slot))) {
            out_event(z, w, ev, now);
        }
    }
}

/* Tamper, low battery and trouble, told on change (zones.md §3). The told_* flags are what the hub
 * heard: while the link is down nothing is told, and a condition that changed meanwhile (the 30 s
 * of "moved") is told when the device next speaks - the hub keeps conditions across a break. */
static void sync_conditions(zsrc_zw_t *z, zsrc_zw_device_t *d, uint64_t now)
{
    sum_conditions(d);
    if (!z->link_up) {
        return;
    }
    const bool tamper = d->tamper_src != 0u;
    if (tamper != d->told_tamper) {
        d->told_tamper = tamper;
        tell_device(z, d, tamper ? ALARM_ZEV_TAMPER : ALARM_ZEV_TAMPER_RESTORE, false, now);
    }
    const bool low = d->lowbat_src != 0u;
    if (low != d->told_lowbat) {
        d->told_lowbat = low;
        tell_device(z, d, low ? ALARM_ZEV_LOW_BATTERY : ALARM_ZEV_LOW_BATTERY_RESTORE, false, now);
    }
    const bool trouble = d->trouble_src != 0u;
    if (trouble != d->told_trouble) {
        d->told_trouble = trouble;
        tell_device(z, d, trouble ? ALARM_ZEV_TROUBLE : ALARM_ZEV_TROUBLE_RESTORE, true, now);
    }
}

/* A check-in for every input of the device, at most once a minute. A momentary input's says what
 * it is (idle, unless a hold runs): a hold whose end was not heard - it ended while the link was
 * down - ends here. So does heat's rapid-rise hold that a break cut (pulse_cut): its input is
 * told again, "off" unless an overheat state was reported since (ZW-013, ZSRC-015). */
static void checkin(zsrc_zw_t *z, zsrc_zw_device_t *d, uint64_t now)
{
    if (d->checkin_at != 0u && now >= d->checkin_at &&
        now - d->checkin_at < ZSRC_ZW_CHECKIN_EVERY_MS) {
        return;
    }
    d->checkin_at = now;
    for (size_t i = 0; i < ZSRC_ZW_WATCH_MAX; i++) {
        zsrc_zw_watch_t *w = &z->watch[i];
        if (!live(z, w, d)) {
            continue;
        }
        const bool cut = w->pulse_cut;
        w->pulse_cut = false;
        if (momentary(w->slot) >= 0) {
            tell_state(z, d, w, now);
        } else if (cut) {
            tell_state_ex(z, d, w, now, true);
        } else {
            out_event(z, w, ALARM_ZEV_CHECKIN, now);
        }
    }
}

/* ---- what a device offers (zones.md §2) ------------------------------------------------------ */

static bool has_event(uint32_t events, uint8_t event)
{
    return events == 0u || (events & bit(event)) != 0u; /* 0: the device could not say (v2) */
}

/* The events of the panel's table its Supported Reports name one by one (an "all" or unknown
 * list names none): V1 Alarm Type and Level that name one of them are read as it (ZW-043). */
static void listed_events(uint32_t out[ZSRC_ZW_NROW_WORDS], const zsrc_zw_endpoint_desc_t *e)
{
    memset(out, 0, ZSRC_ZW_NROW_WORDS * sizeof(out[0]));
    for (size_t t = 0; e != NULL && t < e->notif_count && t < ZSRC_ZW_TYPES_MAX; t++) {
        for (size_t r = 0; r < NROW_COUNT; r++) {
            if (NROWS[r].type == e->notif[t].type &&
                (e->notif[t].events & bit((unsigned)NROWS[r].event)) != 0u) {
                out[r / 32u] |= bit((unsigned)(r % 32u));
            }
        }
    }
}

static bool moved_is_impact(const zsrc_zw_device_t *d)
{
    return !d->has_cover ||
           (d->quirk != NULL && (d->quirk->flags & ZSRC_ZW_QUIRK_MOVED_IS_IMPACT) != 0u);
}

/* The input a notification event feeds, 0 when none (a tamper, a trouble, a test). */
static uint32_t row_slots(const zsrc_zw_device_t *d, const nrow_t *row)
{
    if (row->action == A_STATE || row->action == A_CLEAR) {
        return bit(V_SLOT[row->arg]);
    }
    if (row->action == A_PULSE) {
        return bit(row->arg);
    }
    if (row->action == A_MOVED && moved_is_impact(d)) {
        return bit(ZSRC_ZW_SLOT_IMPACT);
    }
    return 0u;
}

/* The inputs its quirk's alarm map feeds (ZW-043): offered at the device itself, whatever its
 * interview found - a Notification v1 device lists no types. */
static uint32_t alarm_map_slots(const zsrc_zw_device_t *d)
{
    uint32_t s = 0u;
    const zsrc_zw_quirk_t *q = d->quirk;
    for (size_t i = 0; q != NULL && i < ZSRC_ZW_ALARM_MAP_MAX; i++) {
        const zsrc_zw_alarm_map_t *m = &q->alarm_map[i];
        if (m->alarm_type == 0u) {
            break;
        }
        const nrow_t *row = m->event != 0u ? nrow(m->type, m->event) : NULL;
        if (row != NULL) {
            s |= row_slots(d, row);
        }
    }
    return s;
}

/* The inputs of one endpoint's notification types, Binary Sensor and Alarm Sensor types. */
static uint32_t reported_slots(const zsrc_zw_device_t *d, const zsrc_zw_endpoint_desc_t *e)
{
    uint32_t s = 0u;
    for (size_t t = 0; t < e->notif_count && t < ZSRC_ZW_TYPES_MAX; t++) {
        for (size_t r = 0; r < NROW_COUNT; r++) {
            const nrow_t *row = &NROWS[r];
            if (row->type == e->notif[t].type && has_event(e->notif[t].events, row->event)) {
                s |= row_slots(d, row);
            }
        }
    }
    for (size_t i = 0; i < sizeof(BINARY) / sizeof(BINARY[0]); i++) {
        if ((e->binary_types & bit(BINARY[i].type)) != 0u) {
            s |= bit(V_SLOT[BINARY[i].var]);
        }
    }
    if (e->binary && e->binary_types == 0u) {
        s |= bit(ZSRC_ZW_SLOT_GENERAL); /* v1: a report without a type */
    }
    for (size_t i = 0; i < sizeof(ALARM_SENSOR) / sizeof(ALARM_SENSOR[0]); i++) {
        if (((unsigned)e->alarm_types & bit((unsigned)ALARM_SENSOR[i].type)) != 0u) {
            s |= bit(V_SLOT[ALARM_SENSOR[i].var]);
        }
    }
    return s;
}

/* Basic feeds an input only when nothing better does (zones.md §5), or the quirk says which. */
static bool basic_used(const zsrc_zw_device_t *d)
{
    if (d->quirk != NULL && d->quirk->basic_slot != 0u) {
        return true;
    }
    for (size_t e = 0; e < ZSRC_ZW_EP_MAX; e++) {
        if (d->base[e] != 0u) {
            return false;
        }
    }
    return true;
}

static uint32_t remapped(const zsrc_zw_device_t *d, uint32_t slots)
{
    const zsrc_zw_quirk_t *q = d->quirk;
    if (q != NULL && q->remap_from != 0u && (slots & bit(q->remap_from)) != 0u) {
        slots = (slots & ~bit(q->remap_from)) | bit(q->remap_to);
    }
    return slots;
}

static void compute_offers(zsrc_zw_device_t *d)
{
    const zsrc_zw_quirk_t *q = d->quirk;
    const bool basic = basic_used(d);
    d->ready = false;
    for (size_t e = 0; e < ZSRC_ZW_EP_MAX; e++) {
        uint32_t s = 0u;
        if (d->inputs[e] == ZSRC_ZW_INPUTS_OFFERED) {
            s = d->base[e];
            if (((unsigned)d->basic_eps & bit((unsigned)e)) != 0u && basic) {
                s |= bit(var_slot(d, V_BASIC));
            }
            if (((unsigned)d->air_temp_eps & bit((unsigned)e)) != 0u) {
                s |= bit(ZSRC_ZW_SLOT_TEMPERATURE);
            }
            s = remapped(d, s);
            if (q != NULL) {
                s = (s | q->offer[e]) & ~q->hide[e];
            }
            s &= ~bit(0u); /* slot 0 is nothing */
        }
        if (e == 0u && q != NULL && q->panic_scenes != 0u) {
            /* a button its quirk names: the device itself, whatever else it reports */
            s = (s | bit(ZSRC_ZW_SLOT_EMERGENCY)) & ~q->hide[0];
        }
        d->offered[e] = s;
        d->ready = d->ready || s != 0u;
    }
}

/* ---- life cycle ----------------------------------------------------------------------------- */

void zsrc_zw_init(zsrc_zw_t *z, const zsrc_sink_t *sink)
{
    memset(z, 0, sizeof(*z));
    if (sink != NULL) {
        z->sink = *sink;
    }
}

void zsrc_zw_set_quirks(zsrc_zw_t *z, const zsrc_zw_quirk_t *rows, size_t count)
{
    z->quirks = rows;
    z->quirk_count = rows != NULL ? count : 0u;
}

const zsrc_zw_quirk_t *zsrc_zw_quirk_lookup(const zsrc_zw_t *z, uint16_t manufacturer,
                                            uint16_t product_type, uint16_t product_id)
{
    if (z != NULL && z->quirks != NULL) {
        for (size_t i = 0; i < z->quirk_count; i++) {
            if (zsrc_zw_quirk_matches(&z->quirks[i], manufacturer, product_type, product_id)) {
                return &z->quirks[i];
            }
        }
    }
    return zsrc_zw_quirk_for(manufacturer, product_type, product_id);
}

void zsrc_zw_network(zsrc_zw_t *z, uint32_t home_id)
{
    if (home_id == z->home_id) {
        return;
    }
    z->home_id = home_id;
    for (size_t i = 0; i < ZSRC_ZW_DEV_MAX; i++) {
        memset(&z->dev[i], 0, sizeof(z->dev[i])); /* another network: none of them is in it */
    }
}

/* Whether a notification variable holds a condition (a tamper, a trouble or a battery
 * condition) rather than a state: the hub keeps conditions across a break, so the quirk's timer
 * that ends one must outlive the break too (zones.md §8). */
static bool var_is_condition(uint8_t type, uint8_t var)
{
    for (size_t r = 0; var != 0u && r < NROW_COUNT; r++) {
        const nrow_t *row = &NROWS[r];
        if (row->type == type && row->var == var &&
            (row->action == A_TAMPER || row->action == A_TROUBLE || row->action == A_BATTERY)) {
            return true;
        }
    }
    return false;
}

/* ZSRC-015: nothing a device said before the break counts. Its conditions stay (the hub keeps
 * them), and so do the quirk's timers that end them; a forgotten state's timer goes with it. */
static void forget_state(zsrc_zw_device_t *d)
{
    memset(d->vars, 0, sizeof(d->vars));
    memset(d->vknown, 0, sizeof(d->vknown));
    memset(d->alt, 0, sizeof(d->alt)); /* cond stays: conditions outlive the break */
    d->temp_known = 0u;
    memset(d->pulse_at, 0, sizeof(d->pulse_at));
    memset(d->motion_until, 0, sizeof(d->motion_until));
    for (size_t k = 0; k < ZSRC_ZW_IDLE_RESET_MAX; k++) {
        if (!var_is_condition(d->idle_reset[k].type, d->idle_reset[k].var)) {
            d->idle_reset[k].at = 0u;
        }
    }
    d->scene_seq_known = false;
    d->heard_at = 0u;
    d->checkin_at = 0u;
    d->unreachable = false;
}

/* The hub hears the link down while jammed at once: the jam detector already waited its 30 s
 * (network.md §9), so the link's grace for a passing outage doesn't apply. */
static void tell_jammed(zsrc_zw_t *z, uint64_t now)
{
    if (z->sink.link_now != NULL) {
        z->sink.link_now(z->sink.ctx, ZSRC_ZWAVE, now);
    } else if (z->sink.link != NULL) {
        z->sink.link(z->sink.ctx, ZSRC_ZWAVE, false, now);
    }
}

/* A sleeper that never checks in can't be asked and isn't supervised (zones.md §6): the source
 * speaks for it, as for a one-way 433 MHz sensor, so the link's trouble can clear. */
static void vouch_for_sleepers(zsrc_zw_t *z, uint64_t now)
{
    for (size_t i = 0; i < ZSRC_ZW_DEV_MAX; i++) {
        zsrc_zw_device_t *d = &z->dev[i];
        if (d->used && !d->gone && !d->listening && d->check_in_s == 0u) {
            for (size_t k = 0; k < ZSRC_ZW_WATCH_MAX; k++) {
                if (live(z, &z->watch[k], d)) {
                    out_reachable(z, &z->watch[k], true, now);
                }
            }
        }
    }
}

void zsrc_zw_link(zsrc_zw_t *z, bool up, uint64_t now)
{
    z->now = now;
    if (up == z->link_up) {
        return;
    }
    z->link_up = up;
    if (!up) {
        for (size_t i = 0; i < ZSRC_ZW_WATCH_MAX; i++) {
            zsrc_zw_watch_t *w = &z->watch[i];
            if (w->pulse_until != 0u) {
                w->pulse_cut = true; /* its end may fall in the break: told at a check-in */
            }
        }
    }
    if (up) {
        for (size_t i = 0; i < ZSRC_ZW_DEV_MAX; i++) {
            zsrc_zw_device_t *d = &z->dev[i];
            if (d->used) {
                forget_state(d);
                d->refresh = watched_endpoints(z, d); /* asked afresh (zones.md §3) */
            }
        }
        for (size_t i = 0; i < ZSRC_ZW_WATCH_MAX; i++) {
            z->watch[i].pulse_until = 0u;
        }
    }
    if (up && z->jammed) {
        tell_jammed(z, now); /* up, but the air is still jammed: the hub keeps it down */
    } else if (z->sink.link != NULL) {
        z->sink.link(z->sink.ctx, ZSRC_ZWAVE, up, now);
    }
    if (!up) {
        return;
    }
    vouch_for_sleepers(z, now);
}

void zsrc_zw_jammed(zsrc_zw_t *z, bool jammed, uint64_t now)
{
    z->now = now;
    if (jammed == z->jammed) {
        return;
    }
    z->jammed = jammed;
    if (!z->link_up) {
        return; /* the hub already hears the link down; zsrc_zw_link tells it the rest */
    }
    if (jammed) {
        tell_jammed(z, now);
        return;
    }
    /* The air is clear. What the devices said during the jam stands (it was heard and told);
     * frames may have been lost all the same, so every watched input is asked afresh. The hub
     * now waits for each sensor to speak again (ZSRC-015): a device's next frame is passed on as
     * its check-in, however recent the last one was. */
    for (size_t i = 0; i < ZSRC_ZW_DEV_MAX; i++) {
        zsrc_zw_device_t *d = &z->dev[i];
        if (d->used && !d->gone) {
            d->refresh = (uint8_t)((unsigned)d->refresh | (unsigned)watched_endpoints(z, d));
            d->checkin_at = 0u;
        }
    }
    if (z->sink.link != NULL) {
        z->sink.link(z->sink.ctx, ZSRC_ZWAVE, true, now);
    }
    vouch_for_sleepers(z, now);
}

/* Its zones lose it: CHECK after the grace, at once for one that left (grace_of). */
static void lose(zsrc_zw_t *z, zsrc_zw_device_t *d, uint64_t now)
{
    for (size_t i = 0; i < ZSRC_ZW_WATCH_MAX; i++) {
        const zsrc_zw_watch_t *w = &z->watch[i];
        if (watch_of(z, w, d) && offers(d, w->endpoint, w->slot)) {
            out_reachable(z, w, false, now);
        }
    }
}

static zsrc_zw_device_t *new_dev(zsrc_zw_t *z)
{
    zsrc_zw_device_t *gone = NULL;
    for (size_t i = 0; i < ZSRC_ZW_DEV_MAX; i++) {
        if (!z->dev[i].used) {
            return &z->dev[i];
        }
        if (z->dev[i].gone && gone == NULL) {
            gone = &z->dev[i]; /* a device that left: only remembered to say so */
        }
    }
    return gone;
}

bool zsrc_zw_device(zsrc_zw_t *z, const zsrc_zw_desc_t *s, uint64_t now)
{
    if (s == NULL || s->node == 0u || s->generation == 0u) {
        return false;
    }
    z->now = now;
    zsrc_zw_device_t *d = find_dev(z, s->node);
    if (d != NULL && d->generation != s->generation) {
        if (!d->gone) {
            lose(z, d, now); /* another device got its node ID: this one is gone */
        }
        memset(d, 0, sizeof(*d));
        d = NULL;
    }
    const bool fresh = d == NULL;
    if (fresh) {
        d = new_dev(z);
        if (d == NULL) {
            return false;
        }
        memset(d, 0, sizeof(*d));
        d->used = true;
        d->node = s->node;
        d->generation = s->generation;
        d->battery_pct = ZSRC_ZW_BATTERY_UNKNOWN;
        d->new_join = z->learning && z->link_up;
    }
    const bool back = d->gone; /* replaced: a new device under its generation (network.md §10) */
    d->gone = false;
    d->has_identity = s->has_identity;
    d->manufacturer = s->manufacturer;
    d->product_type = s->product_type;
    d->product_id = s->product_id;
    d->security = s->security;
    d->listening = s->listening;
    d->check_in_s = s->check_in_s;
    d->quirk = s->has_identity
                   ? zsrc_zw_quirk_lookup(z, s->manufacturer, s->product_type, s->product_id)
                   : NULL;
    d->has_cover = false;
    for (size_t e = 0; e < ZSRC_ZW_EP_MAX; e++) {
        const zsrc_zw_endpoint_desc_t *ep = &s->ep[e];
        for (size_t t = 0; t < ep->notif_count && t < ZSRC_ZW_TYPES_MAX; t++) {
            const uint8_t type = ep->notif[t].type;
            const uint32_t ev = ep->notif[t].events;
            d->has_cover = d->has_cover || (type == 0x07u && (ev & bit(0x03u)) != 0u) ||
                           (type == 0x09u && (ev & bit(0x06u)) != 0u);
        }
    }
    d->basic_eps = 0u;
    d->air_temp_eps = 0u;
    for (size_t e = 0; e < ZSRC_ZW_EP_MAX; e++) {
        const zsrc_zw_endpoint_desc_t *ep = &s->ep[e];
        const bool here = e == 0u || e <= s->endpoint_count;
        d->inputs[e] = here ? ep->inputs : (uint8_t)ZSRC_ZW_INPUTS_NOT_READY;
        d->base[e] = here ? reported_slots(d, ep) : 0u;
        if (here && e == 0u) {
            d->base[0] |= alarm_map_slots(d);
        }
        listed_events(d->v1_events[e], here ? ep : NULL);
        d->binary_single[e] = 0u;
        for (size_t i = 0; here && i < sizeof(BINARY) / sizeof(BINARY[0]); i++) {
            if (ep->binary_types == bit(BINARY[i].type)) {
                d->binary_single[e] = BINARY[i].type;
            }
        }
        if (here && (ep->basic || ((unsigned)d->sends_basic_eps & bit((unsigned)e)) != 0u)) {
            d->basic_eps = (uint8_t)((unsigned)d->basic_eps | bit((unsigned)e));
        }
        if (here && ep->air_temperature) {
            d->air_temp_eps = (uint8_t)((unsigned)d->air_temp_eps | bit((unsigned)e));
        }
    }
    compute_offers(d);
    const uint8_t eps = watched_endpoints(z, d);
    if (z->link_up && eps != 0u) {
        d->refresh = (uint8_t)(d->refresh | eps); /* its watched inputs want their state */
        if (back) {
            for (size_t i = 0; i < ZSRC_ZW_WATCH_MAX; i++) {
                if (live(z, &z->watch[i], d)) {
                    out_reachable(z, &z->watch[i], true, now);
                }
            }
        }
    }
    return true;
}

void zsrc_zw_device_gone(zsrc_zw_t *z, uint16_t node, uint16_t generation, uint64_t now)
{
    z->now = now;
    zsrc_zw_device_t *d = find_dev(z, node);
    if (d == NULL || d->generation != generation || d->gone) {
        return;
    }
    lose(z, d, now);
    d->gone = true;
    d->refresh = 0u;
}

bool zsrc_zw_check_in(zsrc_zw_t *z, uint16_t node, uint16_t generation, uint32_t check_in_s)
{
    zsrc_zw_device_t *d = find_dev(z, node);
    if (d == NULL || d->generation != generation || d->gone || d->listening) {
        return false;
    }
    const bool stopped = check_in_s == 0u && d->check_in_s != 0u;
    d->check_in_s = check_in_s; /* grace_of() and caps_of() read it at the hub's next look */
    if (stopped && !z->jammed) {
        /* ZW-015: no longer supervised - the source speaks for it, as for any sleeper that never
         * checks in (vouch_for_sleepers), so a CHECK its silence raised can clear */
        for (size_t k = 0; k < ZSRC_ZW_WATCH_MAX; k++) {
            if (live(z, &z->watch[k], d)) {
                out_reachable(z, &z->watch[k], true, z->now);
            }
        }
    }
    return true;
}

void zsrc_zw_reachable(zsrc_zw_t *z, uint16_t node, bool reachable, uint64_t now)
{
    z->now = now;
    zsrc_zw_device_t *d = find_dev(z, node);
    if (d == NULL || d->gone) {
        return;
    }
    d->unreachable = !reachable;
    for (size_t i = 0; i < ZSRC_ZW_WATCH_MAX; i++) {
        if (live(z, &z->watch[i], d)) {
            out_reachable(z, &z->watch[i], reachable, now);
        }
    }
}

bool zsrc_zw_take_refresh(zsrc_zw_t *z, uint16_t *node, uint8_t *endpoints)
{
    for (size_t i = 0; z->link_up && i < ZSRC_ZW_DEV_MAX; i++) {
        zsrc_zw_device_t *d = &z->dev[i];
        if (d->used && !d->gone && d->refresh != 0u) {
            *node = d->node;
            *endpoints = d->refresh;
            d->refresh = 0u;
            return true;
        }
    }
    return false;
}

/* ---- reports (zones.md §3) ------------------------------------------------------------------- */

/* One report's effect on an endpoint, told once it is all applied. */
typedef struct {
    uint8_t ep;
    uint32_t touched; /* slots whose state was reported */
} change_t;

/* A variable takes a state, the event `on` (NULL: its inactive state): the other states of its
 * variable end (command-classes.md §4.5). */
static void set_var(zsrc_zw_device_t *d, change_t *c, uint8_t type, uint8_t var, const nrow_t *on)
{
    for (size_t r = 0; r < NROW_COUNT; r++) {
        const nrow_t *row = &NROWS[r];
        if (row->type == type && row->var == var &&
            (row->action == A_STATE || row->action == A_CLEAR)) {
            d->vars[c->ep] &= ~bit(row->arg);
            d->vknown[c->ep] |= bit(row->arg);
            d->alt[c->ep] = (uint16_t)((unsigned)d->alt[c->ep] & ~bit(row->arg));
            c->touched |= bit(var_slot(d, row->arg));
        }
    }
    if (on != NULL && on->action == A_STATE) {
        d->vars[c->ep] |= bit(on->arg);
        if (second(on)) {
            d->alt[c->ep] = (uint16_t)((unsigned)d->alt[c->ep] | bit(on->arg));
        }
    }
}

/* The variable ends (idle), whatever event it holds: its state, tamper, trouble or battery
 * condition at the endpoint. */
static void idle_var(zsrc_zw_device_t *d, change_t *c, uint8_t type, uint8_t var)
{
    for (size_t i = 0; var != 0u && i < ZSRC_ZW_IDLE_RESET_MAX; i++) {
        if (d->idle_reset[i].ep == c->ep && d->idle_reset[i].type == type &&
            d->idle_reset[i].var == var) {
            d->idle_reset[i].at = 0u; /* ended: the quirk's timer has nothing left to end */
        }
    }
    for (size_t r = 0; r < NROW_COUNT; r++) {
        const nrow_t *row = &NROWS[r];
        if (row->type != type || row->var != var || var == 0u || (row->flags & F_NO_IDLE) != 0u) {
            continue;
        }
        switch (row->action) {
        case A_STATE:
        case A_CLEAR:
            set_var(d, c, type, var, NULL);
            break;
        default:
            if (cond_row(row)) {
                d->cond[c->ep] &= ~cond_bit(row); /* this endpoint's only (ZW21) */
            }
            break;
        }
    }
}

/* ---- the quirk's idle_after_s (zones.md §8; Z-Wave JS's forceNotificationIdleReset) ---------- */

/* Whether the quirk idles this event's variable for the device: a variable that has an idle state
 * (not a door's open/closed, not mains: F_NO_IDLE) and holds something - a state, a tamper, a
 * trouble or a battery condition. Motion keeps its own timer (motion_until), which Binary Sensor
 * motion re-arms too. */
static bool idle_resets(const zsrc_zw_device_t *d, const nrow_t *row)
{
    const zsrc_zw_quirk_t *q = d->quirk;
    if (q == NULL || q->idle_after_s == 0u || row->var == 0u || (row->flags & F_NO_IDLE) != 0u) {
        return false;
    }
    switch (row->action) {
    case A_STATE: return row->arg != V_MOTION;
    case A_TAMPER:
    case A_TROUBLE:
    case A_BATTERY: return true;
    default: return false;
    }
}

/* The quirk's timer ends a variable as the device's own idle would, and says so. */
static void idle_reset_end(zsrc_zw_t *z, zsrc_zw_device_t *d, size_t i, uint64_t now)
{
    change_t c = {d->idle_reset[i].ep, 0u};
    const uint8_t type = d->idle_reset[i].type;
    const uint8_t var = d->idle_reset[i].var;
    d->idle_reset[i].at = 0u;
    idle_var(d, &c, type, var);
    for (uint8_t s = 1u; s < 32u; s++) {
        if ((c.touched & bit(s)) != 0u) {
            tell_slot(z, d, c.ep, s, now);
        }
    }
    sync_conditions(z, d, now);
}

/* A variable was set: its own timer starts again from now. With every entry taken (more than
 * ZSRC_ZW_IDLE_RESET_MAX variables set on one device), the one set longest ago ends now: a
 * variable the device never ends must not stay latched. */
static void idle_reset_arm(zsrc_zw_t *z, zsrc_zw_device_t *d, uint8_t ep, uint8_t type,
                           uint8_t var, uint64_t now)
{
    const uint64_t at = add_ms(now, (uint64_t)d->quirk->idle_after_s * 1000u);
    size_t free_i = ZSRC_ZW_IDLE_RESET_MAX;
    size_t soonest = 0u;
    for (size_t i = 0; i < ZSRC_ZW_IDLE_RESET_MAX; i++) {
        if (d->idle_reset[i].at == 0u) {
            if (free_i == ZSRC_ZW_IDLE_RESET_MAX) {
                free_i = i;
            }
            continue;
        }
        if (d->idle_reset[i].ep == ep && d->idle_reset[i].type == type &&
            d->idle_reset[i].var == var) {
            d->idle_reset[i].at = at;
            return;
        }
        if (d->idle_reset[i].at < d->idle_reset[soonest].at || d->idle_reset[soonest].at == 0u) {
            soonest = i;
        }
    }
    if (free_i == ZSRC_ZW_IDLE_RESET_MAX) {
        idle_reset_end(z, d, soonest, now);
        free_i = soonest;
    }
    d->idle_reset[free_i].at = at;
    d->idle_reset[free_i].ep = ep;
    d->idle_reset[free_i].type = type;
    d->idle_reset[free_i].var = var;
}

/* A momentary input: "on" for each of its watches' holds (zones.md §3). */
static void pulse(zsrc_zw_t *z, zsrc_zw_device_t *d, uint8_t ep, uint8_t slot, uint64_t now)
{
    slot = route(d, slot);
    const int k = pulse_index(slot);
    if (k >= 0) {
        d->pulse_at[ep][k] = now;
    }
    if (z->learning && offers(d, ep, slot)) {
        d->changed[ep] |= bit(slot);
    }
    for (size_t i = 0; i < ZSRC_ZW_WATCH_MAX; i++) {
        zsrc_zw_watch_t *w = &z->watch[i];
        if (live(z, w, d) && w->endpoint == ep && w->slot == slot) {
            const uint32_t hold = w->hold_s != 0u ? (uint32_t)w->hold_s : ZSRC_ZW_HOLD_S;
            w->pulse_until = add_ms(now, (uint64_t)hold * 1000u);
            w->pulse_cut = false;
            tell_state(z, d, w, now);
        }
    }
}

/* ZW-043: a report that names its event only by its V1 Alarm Type and Level - a v1 alarm, or a
 * later one whose notification type is 00 - is read as the notification event they stand for: one
 * its Supported Reports list under the type the alarm type names, with the level as the event
 * (Z-Wave JS GH#1034), else the first row of its quirk's alarm map that matches (Z-Wave JS's
 * compat.alarmMapping). A report that names a notification type is read as that, whatever its
 * V1 bytes, and a longer one with status FE (nothing pending) is never read. False: nothing the
 * panel can read (dropped). */
static bool v1_alarm(const zsrc_zw_device_t *d, uint8_t ep, const zsrc_zw_report_t *r,
                     zsrc_zw_report_t *out)
{
    const uint8_t at = r->notif_v1_type;
    const uint8_t al = r->notif_v1_level;
    if ((!r->notif_v1 && r->notif_type != 0u) || at == 0u) {
        return false; /* its own notification; alarm type 0 is never read */
    }
    if (!r->notif_v1 && r->notif_status == 0xFEu) {
        return false; /* "no pending notification": never an event, never an idle */
    }
    bool found = false;
    uint8_t type = 0u;
    uint8_t event = 0u;
    const nrow_t *row = al != 0u ? nrow(at, al) : NULL;
    if (row != NULL) {
        const size_t k = (size_t)(row - NROWS);
        found = (d->v1_events[ep][k / 32u] & bit((unsigned)(k % 32u))) != 0u;
        type = at;
        event = al;
    }
    const zsrc_zw_quirk_t *q = d->quirk;
    for (size_t i = 0; !found && q != NULL && i < ZSRC_ZW_ALARM_MAP_MAX; i++) {
        const zsrc_zw_alarm_map_t *m = &q->alarm_map[i];
        if (m->alarm_type == 0u) {
            break;
        }
        if (m->alarm_type == at &&
            (m->alarm_level == ZSRC_ZW_ALARM_LEVEL_ANY || m->alarm_level == al)) {
            found = true;
            type = m->type;
            event = m->event;
        }
    }
    if (!found) {
        return false;
    }
    *out = *r;
    out->notif_v1 = false;
    out->notif_status = 0xFFu;
    out->notif_type = type;
    out->notif_event = event;
    out->notif_param_len = 0u; /* an idle then ends every state of the type */
    memset(out->notif_params, 0, sizeof(out->notif_params));
    return true;
}

static void notification(zsrc_zw_t *z, zsrc_zw_device_t *d, change_t *c,
                         const zsrc_zw_report_t *r, uint64_t now)
{
    zsrc_zw_report_t mapped;
    if (v1_alarm(d, c->ep, r, &mapped)) {
        r = &mapped; /* on through the notification path: idle clears, a test is a test */
    }
    if (r->notif_v1 || r->notif_status != 0xFFu) {
        return; /* a v1 alarm nothing reads (ZW-043); reports off; nothing pending */
    }
    const uint8_t type = r->notif_type;
    if (r->notif_event == 0x00u) {
        if (r->notif_param_len >= 1u) {
            const nrow_t *row = nrow(type, r->notif_params[0]);
            const nrow_t *holds = row != NULL ? held(d, c->ep, type, row->var) : NULL;
            if (row != NULL && (holds == NULL || holds == row)) {
                idle_var(d, c, type, row->var); /* the variable of that event ends */
            } /* else it holds another event since: a stale idle, dropped (ZW22, Z-Wave JS).
               * Holding nothing, it is applied: our own exception - Z-Wave JS ignores it, but
               * its value database keeps the event across a link break where forget_state()
               * wipes ours, so a sleeping device's real idle after one would otherwise keep its
               * zone faulted until its next wake-up. */
        } else {
            for (size_t i = 0; i < NROW_COUNT; i++) { /* every state of the type ends */
                if (NROWS[i].type == type) {
                    idle_var(d, c, type, NROWS[i].var);
                }
            }
        }
        return;
    }
    const nrow_t *row = nrow(type, r->notif_event);
    if (row == NULL) {
        return;
    }
    switch (row->action) {
    case A_STATE: set_var(d, c, type, row->var, row); break;
    case A_CLEAR: set_var(d, c, type, row->var, NULL); break;
    case A_PULSE: pulse(z, d, c->ep, (uint8_t)row->arg, now); break;
    case A_TAMPER:
    case A_TROUBLE:
    case A_BATTERY: set_cond(d, c->ep, row); break;
    case A_MOVED:
        if (moved_is_impact(d)) {
            pulse(z, d, c->ep, ZSRC_ZW_SLOT_IMPACT, now);
        } else {
            d->tamper_src = (uint8_t)(d->tamper_src | T_MOVED); /* restored after 30 s */
            d->moved_until = add_ms(now, (uint64_t)ZSRC_ZW_MOVED_S * 1000u);
        }
        break;
    case A_TEST:
        /* CO 03's parameter (Z-Wave JS "Test status"): 02 a failed test is TROUBLE (no
         * "tested"), 01 a passed one ends it, and so does a CO 03 without a parameter (a test);
         * any other value is neither, only a check-in (ZW-013). */
        if (row->arg != 0u && r->notif_param_len >= 1u && r->notif_params[0] == CO_TEST_FAILED) {
            set_cond(d, c->ep, row);
        } else if (row->arg == 0u || r->notif_param_len == 0u ||
                   r->notif_params[0] == CO_TEST_OK) {
            if (row->arg != 0u) {
                d->cond[c->ep] &= ~cond_bit(row);
            }
            d->tested_at = now;
        }
        checkin(z, d, now);
        break;
    case A_CHECKIN: checkin(z, d, now); break;
    default: break;
    }
    if (idle_resets(d, row)) {
        idle_reset_arm(z, d, c->ep, type, row->var, now); /* zones.md §8 idle_after_s */
    }
}

static void binary(zsrc_zw_device_t *d, change_t *c, const zsrc_zw_report_t *r)
{
    uint8_t type = r->binary_has_type ? r->binary_type : d->binary_single[c->ep];
    if (type == 0xFFu) {
        /* "Any": the endpoint's one supported type; several (or none of ours) is ambiguous -
         * dropped, counted (zones.md §2, a quirk can name the type). */
        type = d->binary_single[c->ep];
        if (type == 0u) {
            d->binary_any_ambiguous++;
            return;
        }
    }
    if (type == 0u) {
        type = BINARY_GENERAL; /* a v1 report of a device with no single type */
    }
    if (type == BINARY_TAMPER) {
        /* the endpoint's own (ZW21): another endpoint's restore leaves it */
        d->cond[c->ep] = r->binary_on ? (d->cond[c->ep] | COND_BINARY)
                                      : (d->cond[c->ep] & ~COND_BINARY);
        return;
    }
    for (size_t i = 0; i < sizeof(BINARY) / sizeof(BINARY[0]); i++) {
        if (BINARY[i].type == type) {
            const unsigned v = BINARY[i].var;
            d->vars[c->ep] = r->binary_on ? (d->vars[c->ep] | bit(v)) : (d->vars[c->ep] & ~bit(v));
            d->vknown[c->ep] |= bit(v);
            c->touched |= bit(var_slot(d, v));
        }
    }
}

/* zones.md §3: FF or below 20 % or v2 "replace" or "disconnected" is low; 30 % and up without
 * them is restored. */
static void battery(zsrc_zw_device_t *d, const zsrc_zw_report_t *r)
{
    const uint8_t level = r->battery_level;
    if (level > 100u && level != 0xFFu) {
        return; /* invalid: dropped (command-classes.md §4.6) */
    }
    d->battery_pct = (uint8_t)(level == 0xFFu ? 0u : (unsigned)level);
    const bool replace = r->battery_replace != 0u || r->battery_disconnected;
    if (level == 0xFFu || level < ZSRC_ZW_BATTERY_LOW_PCT || replace) {
        d->lowbat_src = (uint8_t)(d->lowbat_src | B_REPORT);
    } else if (level >= ZSRC_ZW_BATTERY_OK_PCT) {
        d->lowbat_src = (uint8_t)((unsigned)d->lowbat_src & ~B_REPORT);
    }
}

static void multilevel(zsrc_zw_device_t *d, change_t *c, const zsrc_zw_report_t *r)
{
    if (r->ml_type != 0x01u || r->ml_scale > 1u) {
        return; /* only air temperature feeds a zone; the rest is for the device page */
    }
    int64_t v = r->ml_value_x100;
    if (r->ml_scale == 1u) {
        v = (v - 3200) * 5 / 9; /* Fahrenheit */
    }
    d->temp_x100[c->ep] = (int32_t)v;
    d->temp_known = (uint8_t)((unsigned)d->temp_known | bit((unsigned)c->ep));
    c->touched |= bit(ZSRC_ZW_SLOT_TEMPERATURE);
}

static void alarm_sensor(zsrc_zw_device_t *d, change_t *c, const zsrc_zw_report_t *r)
{
    for (size_t i = 0; i < sizeof(ALARM_SENSOR) / sizeof(ALARM_SENSOR[0]); i++) {
        if (ALARM_SENSOR[i].type == r->alarm_type) {
            const unsigned v = ALARM_SENSOR[i].var;
            d->vars[c->ep] = r->alarm_state != 0u ? (d->vars[c->ep] | bit(v))
                                                  : (d->vars[c->ep] & ~bit(v));
            d->vknown[c->ep] |= bit(v);
            c->touched |= bit(var_slot(d, v));
        }
    }
}

/* zones.md §5: Basic only when nothing better; one that sends it offers the general input. */
static void basic(zsrc_zw_device_t *d, change_t *c, const zsrc_zw_report_t *r)
{
    if (!basic_used(d)) {
        return; /* it reports its inputs otherwise: one event is not counted twice */
    }
    if (((unsigned)d->basic_eps & bit((unsigned)c->ep)) == 0u) {
        d->sends_basic_eps = (uint8_t)((unsigned)d->sends_basic_eps | bit((unsigned)c->ep));
        d->basic_eps = (uint8_t)((unsigned)d->basic_eps | bit((unsigned)c->ep));
        compute_offers(d);
    }
    d->vars[c->ep] = r->basic_value != 0u ? (d->vars[c->ep] | bit(V_BASIC))
                                          : (d->vars[c->ep] & ~bit(V_BASIC));
    d->vknown[c->ep] |= bit(V_BASIC);
    c->touched |= bit(var_slot(d, V_BASIC));
}

/* command-classes.md §4.21: a scene its quirk names is its emergency button (zones.md §4). A
 * repeated sequence number is the same press sent again: it counts once. */
static void central_scene(zsrc_zw_t *z, zsrc_zw_device_t *d, const zsrc_zw_report_t *r,
                          uint64_t now)
{
    if (d->scene_seq_known && r->scene_seq == d->scene_seq) {
        return;
    }
    d->scene_seq_known = true;
    d->scene_seq = r->scene_seq;
    const zsrc_zw_quirk_t *q = d->quirk;
    const unsigned attr = (unsigned)r->scene_attr & 0x07u;
    if (q == NULL || r->scene_number == 0u || r->scene_number > 31u || attr == 1u || attr > 6u ||
        (q->panic_scenes & bit((unsigned)r->scene_number)) == 0u) {
        return;
    }
    pulse(z, d, 0u, ZSRC_ZW_SLOT_EMERGENCY, now);
}

void zsrc_zw_report(zsrc_zw_t *z, uint16_t node, const zsrc_zw_report_t *r, uint64_t now)
{
    z->now = now;
    zsrc_zw_device_t *d = find_dev(z, node);
    if (!z->link_up || d == NULL || d->gone || r == NULL) {
        return;
    }
    const uint32_t before = z->emitted;
    d->heard_at = now;
    if (d->unreachable) {
        d->unreachable = false; /* zw_health says so too; the hub hears it at once */
        for (size_t i = 0; i < ZSRC_ZW_WATCH_MAX; i++) {
            if (live(z, &z->watch[i], d)) {
                out_reachable(z, &z->watch[i], true, now);
            }
        }
    }
    change_t c = {r->endpoint, 0u};
    const bool ep_ok = r->endpoint < ZSRC_ZW_EP_MAX;
    const uint32_t was = ep_ok ? slots_of(d, d->vars[c.ep] & d->vknown[c.ep]) : 0u;
    switch (ep_ok ? r->kind : (uint8_t)ZSRC_ZW_RPT_FRAME) {
    case ZSRC_ZW_RPT_NOTIFICATION: notification(z, d, &c, r, now); break;
    case ZSRC_ZW_RPT_BINARY: binary(d, &c, r); break;
    case ZSRC_ZW_RPT_BATTERY: battery(d, r); break;
    case ZSRC_ZW_RPT_MULTILEVEL: multilevel(d, &c, r); break;
    case ZSRC_ZW_RPT_ALARM_SENSOR: alarm_sensor(d, &c, r); break;
    case ZSRC_ZW_RPT_BASIC: basic(d, &c, r); break;
    case ZSRC_ZW_RPT_WAKE_UP: checkin(z, d, now); break;
    case ZSRC_ZW_RPT_CENTRAL_SCENE: central_scene(z, d, r, now); break;
    default: break;
    }
    if (ep_ok) {
        const uint32_t is = slots_of(d, d->vars[c.ep] & d->vknown[c.ep]);
        if (z->learning) {
            d->changed[c.ep] |= (was ^ is) & d->offered[c.ep];
        }
        const zsrc_zw_quirk_t *q = d->quirk;
        const uint32_t motion = bit(V_MOTION) | bit(V_BIN_MOTION);
        if (q != NULL && q->idle_after_s != 0u && (d->vars[c.ep] & motion) != 0u &&
            (c.touched & bit(route(d, ZSRC_ZW_SLOT_MOTION))) != 0u) {
            /* motion it never ends by itself (zones.md §8 idle_after_s) */
            d->motion_until[c.ep] = add_ms(now, (uint64_t)q->idle_after_s * 1000u);
        }
        for (uint8_t s = 1u; s < 32u; s++) {
            if ((c.touched & bit(s)) != 0u) {
                tell_slot(z, d, c.ep, s, now);
            }
        }
    }
    sync_conditions(z, d, now);
    if (z->emitted == before) {
        checkin(z, d, now); /* a frame that changed nothing: it is alive */
    }
}

/* ---- the source interface -------------------------------------------------------------------- */

static bool zw_usable(void *ctx)
{
    const zsrc_zw_t *z = ctx;
    return z->link_up; /* ZSRC-005 */
}

/* The device an address names, when it is in the network. */
static zsrc_zw_device_t *addressed(zsrc_zw_t *z, const zsrc_address_t *a, uint8_t *ep,
                                   uint8_t *slot)
{
    uint32_t home = 0u;
    uint16_t gen = 0u;
    uint16_t node = 0u;
    if (!zsrc_zw_address_parse(a, &home, &gen, &node, ep, slot) || home != z->home_id) {
        return NULL;
    }
    zsrc_zw_device_t *d = find_dev(z, node);
    return d != NULL && !d->gone && d->generation == gen ? d : NULL;
}

static bool zw_valid(void *ctx, const zsrc_address_t *a)
{
    uint8_t ep = 0u;
    uint8_t slot = 0u;
    const zsrc_zw_device_t *d = addressed(ctx, a, &ep, &slot);
    return d != NULL && offers(d, ep, slot);
}

static bool zw_watch(void *ctx, const zsrc_address_t *a, const zsrc_mapping_t *m, uint64_t now)
{
    zsrc_zw_t *z = ctx;
    z->now = now;
    uint32_t home = 0u;
    uint16_t gen = 0u;
    uint16_t node = 0u;
    uint8_t ep = 0u;
    uint8_t slot = 0u;
    if (!zsrc_zw_address_parse(a, &home, &gen, &node, &ep, &slot)) {
        return false;
    }
    zsrc_zw_watch_t *w = NULL;
    zsrc_zw_watch_t *free_w = NULL;
    for (size_t i = 0; i < ZSRC_ZW_WATCH_MAX && w == NULL; i++) {
        zsrc_zw_watch_t *x = &z->watch[i];
        if (x->used && x->home_id == home && x->generation == gen && x->node == node &&
            x->endpoint == ep && x->slot == slot) {
            w = x;
        } else if (!x->used && free_w == NULL) {
            free_w = x;
        }
    }
    if (w == NULL) {
        if (free_w == NULL) {
            return false; /* ZSRC_ZW_WATCH_MAX inputs already */
        }
        w = free_w;
        memset(w, 0, sizeof(*w));
        w->used = true;
        w->home_id = home;
        w->generation = gen;
        w->node = node;
        w->endpoint = ep;
        w->slot = slot;
    }
    w->hold_s = 0u;
    if (m != NULL) {
        w->hold_s = m->hold_s;
    }
    /* A device the source doesn't know (yet, or any more) is still watched: its zone shows
     * CHECK after the grace until it reports or is rebound (ZSRC-010). */
    zsrc_zw_device_t *d = find_dev(z, node);
    if (d == NULL || !live(z, w, d) || !z->link_up) {
        return true;
    }
    d->checkin_at = 0u; /* its next frame reaches the new input */
    tell_state(z, d, w, now);
    if (d->told_tamper) {
        out_event(z, w, ALARM_ZEV_TAMPER, now);
    }
    if (d->told_lowbat) {
        out_event(z, w, ALARM_ZEV_LOW_BATTERY, now);
    }
    if (d->told_trouble && life_safety(slot)) {
        out_event(z, w, ALARM_ZEV_TROUBLE, now);
    }
    /* zones.md §3 "Fresh state": whenever a device is watched, it is asked */
    d->refresh = (uint8_t)((unsigned)d->refresh | bit((unsigned)ep));
    return true;
}

static void zw_unwatch(void *ctx, const zsrc_address_t *a)
{
    zsrc_zw_t *z = ctx;
    for (size_t i = 0; i < ZSRC_ZW_WATCH_MAX; i++) {
        zsrc_zw_watch_t *w = &z->watch[i];
        if (w->used) {
            const zsrc_address_t b = watch_addr(w);
            if (zsrc_address_equal(&b, a)) {
                memset(w, 0, sizeof(*w));
            }
        }
    }
}

static bool zw_learn(void *ctx, bool on, uint64_t now)
{
    zsrc_zw_t *z = ctx;
    z->now = now;
    if (on && !z->link_up) {
        return false;
    }
    z->learning = on;
    if (on) {
        for (size_t i = 0; i < ZSRC_ZW_DEV_MAX; i++) { /* the picker's session starts */
            z->dev[i].new_join = false;
            memset(z->dev[i].changed, 0, sizeof(z->dev[i].changed));
        }
    }
    return true;
}

static size_t zw_candidates(void *ctx, const char *query, uint32_t filter, zsrc_candidate_t *out,
                            size_t max)
{
    (void)query;
    (void)filter;
    zsrc_zw_t *z = ctx;
    size_t n = 0u;
    /* The device added from the picker first, then the inputs that changed while it was open,
     * then the rest (zones.md §2, screens.md §5). */
    for (int pass = 0; pass < 3 && n < max; pass++) {
        for (size_t i = 0; i < ZSRC_ZW_DEV_MAX && n < max; i++) {
            const zsrc_zw_device_t *d = &z->dev[i];
            if (!d->used || d->gone || !d->ready) {
                continue;
            }
            for (uint8_t ep = 0u; ep < ZSRC_ZW_EP_MAX && n < max; ep++) {
                for (uint8_t slot = 1u; slot < 32u && n < max; slot++) {
                    if (!offers(d, ep, slot)) {
                        continue;
                    }
                    const bool moved = (d->changed[ep] & bit(slot)) != 0u;
                    const int want = d->new_join ? 0 : moved ? 1 : 2;
                    if (want != pass) {
                        continue;
                    }
                    zsrc_candidate_t *c = &out[n++];
                    memset(c, 0, sizeof(*c));
                    c->addr = zsrc_zwave_address(z->home_id, d->generation, d->node, ep, slot);
                    zsrc_zw_device_name(d, c->name, sizeof(c->name));
                    zsrc_zw_input_words(d, ep, slot, c->detail, sizeof(c->detail));
                    zsrc_zw_state_words(d, ep, slot, z->now, c->state, sizeof(c->state));
                    c->device_class = (uint8_t)zsrc_zw_slot_class(d, slot);
                    c->changed = moved || d->new_join;
                    c->best = d->new_join;
                }
            }
        }
    }
    return n;
}

static void zw_describe(void *ctx, const zsrc_address_t *a, char *out, size_t len)
{
    zsrc_zw_t *z = ctx;
    uint32_t home = 0u;
    uint16_t gen = 0u;
    uint16_t node = 0u;
    uint8_t ep = 0u;
    uint8_t slot = 0u;
    if (!zsrc_zw_address_parse(a, &home, &gen, &node, &ep, &slot)) {
        snprintf(out, len, "Z-Wave (not an input)");
        return;
    }
    const zsrc_zw_device_t *d = addressed(z, a, &ep, &slot);
    char input[40];
    if (d == NULL) {
        zsrc_zw_device_t none;
        memset(&none, 0, sizeof(none));
        none.node = node;
        zsrc_zw_input_words(&none, ep, slot, input, sizeof(input));
        const zsrc_zw_device_t *old = find_dev(z, node);
        const bool gone = old != NULL && old->generation == gen && old->gone;
        snprintf(out, len, "Z-Wave device %u · %s (%s)", (unsigned)node, input,
                 home != z->home_id ? "another network" : gone ? "removed" : "not in the network");
        return;
    }
    char name[40];
    zsrc_zw_device_name(d, name, sizeof(name));
    zsrc_zw_input_words(d, ep, slot, input, sizeof(input));
    snprintf(out, len, "%s · %s", name, input);
}

/* zones.md §6: S2 + supervised = authenticated and life-safety; S0 = authenticated; none =
 * neither; a sleeper that never checks in is not supervised. */
static uint32_t zw_caps_of(void *ctx, const zsrc_address_t *a)
{
    const uint32_t base = ZSRC_CAP_LEARN | ZSRC_CAP_SUPERVISED | ZSRC_CAP_TAMPER |
                          ZSRC_CAP_BATTERY | ZSRC_CAP_OFFLINE_OK;
    uint8_t ep = 0u;
    uint8_t slot = 0u;
    const zsrc_zw_device_t *d = addressed(ctx, a, &ep, &slot);
    if (d == NULL) {
        return base;
    }
    const bool supervised = d->listening || d->check_in_s != 0u;
    uint32_t caps = supervised ? base : (base & ~ZSRC_CAP_SUPERVISED);
    const bool s2 = d->security == ZSRC_ZW_SEC_S2_UNAUTH || d->security == ZSRC_ZW_SEC_S2_AUTH ||
                    d->security == ZSRC_ZW_SEC_S2_ACCESS;
    if (s2 || d->security == ZSRC_ZW_SEC_S0) {
        caps |= ZSRC_CAP_AUTH;
    }
    if (s2 && supervised) {
        caps |= ZSRC_CAP_LIFE_SAFETY;
    }
    return caps;
}

/* network.md §8: the binding's default grace, per device. */
static uint32_t zw_grace_of(void *ctx, const zsrc_address_t *a)
{
    zsrc_zw_t *z = ctx;
    uint8_t ep = 0u;
    uint8_t slot = 0u;
    const zsrc_zw_device_t *d = addressed(z, a, &ep, &slot);
    if (d == NULL) {
        /* With the network up and its devices told, an address none of them has is a device
         * that left, or of another network: CHECK at once (zones.md §1). */
        return z->link_up ? ZSRC_ZW_GONE_GRACE_S : 0u;
    }
    if (d->listening) {
        return ZSRC_ZW_LISTEN_GRACE_S;
    }
    if (d->check_in_s == 0u) {
        return ZSRC_GRACE_NEVER; /* never checks in: not supervised */
    }
    const uint64_t g = (uint64_t)d->check_in_s + ZSRC_ZW_SLEEP_GRACE_S;
    return g < ZSRC_GRACE_NEVER ? (uint32_t)g : ZSRC_GRACE_NEVER - 1u;
}

static void zw_tick(void *ctx, uint64_t now)
{
    zsrc_zw_t *z = ctx;
    z->now = now;
    for (size_t i = 0; i < ZSRC_ZW_WATCH_MAX; i++) {
        zsrc_zw_watch_t *w = &z->watch[i];
        if (w->used && w->pulse_until != 0u && now >= w->pulse_until) {
            w->pulse_until = 0u; /* the momentary input ends */
            zsrc_zw_device_t *d = find_dev(z, w->node);
            if (d != NULL && live(z, w, d)) {
                tell_state_ex(z, d, w, now, true);
            }
        }
    }
    for (size_t i = 0; i < ZSRC_ZW_DEV_MAX; i++) {
        zsrc_zw_device_t *d = &z->dev[i];
        if (d->used && d->moved_until != 0u && now >= d->moved_until) {
            d->moved_until = 0u;
            /* "moved" restores itself */
            d->tamper_src = (uint8_t)((unsigned)d->tamper_src & ~T_MOVED);
            sync_conditions(z, d, now);
        }
        for (uint8_t ep = 0u; d->used && ep < ZSRC_ZW_EP_MAX; ep++) {
            if (d->motion_until[ep] != 0u && now >= d->motion_until[ep]) {
                d->motion_until[ep] = 0u; /* the quirk's idle_after: motion that never ends */
                d->vars[ep] &= ~(bit(V_MOTION) | bit(V_BIN_MOTION));
                tell_slot(z, d, ep, route(d, ZSRC_ZW_SLOT_MOTION), now);
            }
        }
        for (size_t k = 0; d->used && k < ZSRC_ZW_IDLE_RESET_MAX; k++) {
            if (d->idle_reset[k].at != 0u && now >= d->idle_reset[k].at) {
                idle_reset_end(z, d, k, now); /* the quirk's idle_after: any other variable */
            }
        }
    }
}

static uint64_t zw_next_tick(void *ctx, uint64_t now)
{
    (void)now;
    const zsrc_zw_t *z = ctx;
    uint64_t next = UINT64_MAX;
    for (size_t i = 0; i < ZSRC_ZW_WATCH_MAX; i++) {
        const zsrc_zw_watch_t *w = &z->watch[i];
        if (w->used && w->pulse_until != 0u && w->pulse_until < next) {
            next = w->pulse_until;
        }
    }
    for (size_t i = 0; i < ZSRC_ZW_DEV_MAX; i++) {
        const zsrc_zw_device_t *d = &z->dev[i];
        if (d->used && d->moved_until != 0u && d->moved_until < next) {
            next = d->moved_until;
        }
        for (size_t ep = 0; d->used && ep < ZSRC_ZW_EP_MAX; ep++) {
            if (d->motion_until[ep] != 0u && d->motion_until[ep] < next) {
                next = d->motion_until[ep];
            }
        }
        for (size_t k = 0; d->used && k < ZSRC_ZW_IDLE_RESET_MAX; k++) {
            if (d->idle_reset[k].at != 0u && d->idle_reset[k].at < next) {
                next = d->idle_reset[k].at;
            }
        }
    }
    return next;
}

const zone_source_ops_t *zsrc_zw_ops(void)
{
    static const zone_source_ops_t ops = {
        .kind = ZSRC_ZWAVE,
        /* What every Z-Wave device can do (zones.md §6); encryption differs per device, so
         * caps_of() says what each one adds or lacks. */
        .caps = ZSRC_CAP_LEARN | ZSRC_CAP_SUPERVISED | ZSRC_CAP_TAMPER | ZSRC_CAP_BATTERY |
                ZSRC_CAP_OFFLINE_OK,
        .name = "Z-Wave sensor",
        .grace_s = (uint16_t)ZSRC_ZW_GRACE_S,
        .link_grace_s = (uint16_t)ZSRC_ZW_LINK_GRACE_S,
        .usable = zw_usable,
        .valid = zw_valid,
        .watch = zw_watch,
        .unwatch = zw_unwatch,
        .learn = zw_learn,
        .candidates = zw_candidates,
        .describe = zw_describe,
        .tick = zw_tick,
        .next_tick = zw_next_tick,
        .caps_of = zw_caps_of,
        .grace_of = zw_grace_of,
    };
    return &ops;
}

/* ---- words (zones.md §1-2) ------------------------------------------------------------------- */

void zsrc_zw_device_name(const zsrc_zw_device_t *d, char *out, size_t len)
{
    if (d->quirk != NULL && d->quirk->name != NULL) {
        snprintf(out, len, "%s", d->quirk->name);
    } else if (d->has_identity) {
        snprintf(out, len, "Z-Wave %04X:%04X:%04X", (unsigned)d->manufacturer,
                 (unsigned)d->product_type, (unsigned)d->product_id);
    } else {
        snprintf(out, len, "Z-Wave device %u", (unsigned)d->node);
    }
}

static const char *slot_words(const zsrc_zw_device_t *d, uint8_t slot)
{
    switch (slot) {
    case ZSRC_ZW_SLOT_CONTACT:
        return zsrc_zw_slot_class(d, slot) == ALARM_DEVCLASS_WINDOW ? "window contact"
                                                                   : "door contact";
    case ZSRC_ZW_SLOT_INTRUSION: return "intrusion";
    case ZSRC_ZW_SLOT_MOTION: return "motion";
    case ZSRC_ZW_SLOT_GLASS_BREAK: return "glass break";
    case ZSRC_ZW_SLOT_TILT: return "tilt";
    case ZSRC_ZW_SLOT_IMPACT: return "impact";
    case ZSRC_ZW_SLOT_WATER: return "water";
    case ZSRC_ZW_SLOT_SMOKE: return "smoke";
    case ZSRC_ZW_SLOT_CO: return "carbon monoxide";
    case ZSRC_ZW_SLOT_HEAT: return "heat";
    case ZSRC_ZW_SLOT_FREEZE: return "freeze";
    case ZSRC_ZW_SLOT_GAS: return "gas";
    case ZSRC_ZW_SLOT_EMERGENCY: return "emergency button";
    case ZSRC_ZW_SLOT_TEMPERATURE: return "temperature";
    case ZSRC_ZW_SLOT_MAINS: return "mains power";
    case ZSRC_ZW_SLOT_GENERAL: return "general input";
    default: return "input";
    }
}

void zsrc_zw_input_words(const zsrc_zw_device_t *d, uint8_t endpoint, uint8_t slot, char *out,
                         size_t len)
{
    if (endpoint != 0u) {
        snprintf(out, len, "input %u · %s", (unsigned)endpoint, slot_words(d, slot));
    } else {
        snprintf(out, len, "%s", slot_words(d, slot));
    }
}

void zsrc_zw_state_words(const zsrc_zw_device_t *d, uint8_t endpoint, uint8_t slot, uint64_t now,
                         char *out, size_t len)
{
    out[0] = '\0';
    if (endpoint >= ZSRC_ZW_EP_MAX) {
        return;
    }
    if (slot == ZSRC_ZW_SLOT_TEMPERATURE) {
        if (((unsigned)d->temp_known & bit((unsigned)endpoint)) != 0u) {
            const int32_t v = d->temp_x100[endpoint];
            const int32_t t = (v >= 0 ? v + 5 : v - 5) / 10; /* tenths, rounded */
            const int32_t whole = t / 10;
            const int32_t tenth = t % 10 < 0 ? -(t % 10) : t % 10;
            snprintf(out, len, "%s%ld.%ld °C", (t < 0 && whole == 0) ? "-" : "", (long)whole,
                     (long)tenth);
        }
        return;
    }
    const int k = pulse_index(slot);
    const uint64_t at = k >= 0 ? d->pulse_at[endpoint][k] : 0u;
    const bool pulsing = at != 0u && now >= at && now - at < (uint64_t)ZSRC_ZW_HOLD_S * 1000u;
    if (momentary(slot) >= 0) {
        if (at == 0u && d->heard_at == 0u && !state_known(d, endpoint, slot)) {
            return;
        }
    } else if (!pulsing && !state_known(d, endpoint, slot) &&
               (at == 0u || d->heard_at == 0u)) {
        return; /* heat: its rapid rise, else its state; after a rise its hub heard "off" */
    }
    const bool on = pulsing || state_active(d, endpoint, slot);
    static const char *const ON[ZSRC_ZW_SLOT_COUNT] = {
        "", "Open", "Intrusion", "Motion", "Glass break", "Open", "Impact", "Wet", "Smoke",
        "CO", "Heat", "Freezing", "Gas", "Pressed", "", "Mains lost", "On"};
    static const char *const OFF[ZSRC_ZW_SLOT_COUNT] = {
        "", "Closed", "Clear", "Clear", "Clear", "Closed", "Clear", "Dry", "Clear",
        "Clear", "Clear", "Clear", "Clear", "Idle", "", "Mains on", "Off"};
    if (slot < ZSRC_ZW_SLOT_COUNT) {
        snprintf(out, len, "%s", on ? ON[slot] : OFF[slot]);
    } else {
        snprintf(out, len, "%s", on ? "On" : "Off");
    }
}

alarm_device_class_t zsrc_zw_slot_class(const zsrc_zw_device_t *d, uint8_t slot)
{
    const zsrc_zw_quirk_t *q = d->quirk;
    const bool override = q != NULL && q->device_class != ALARM_DEVCLASS_GENERIC;
    switch (slot) {
    case ZSRC_ZW_SLOT_CONTACT:
        return override ? (alarm_device_class_t)q->device_class : ALARM_DEVCLASS_DOOR;
    case ZSRC_ZW_SLOT_INTRUSION:
        if (override) {
            return (alarm_device_class_t)q->device_class;
        }
        /* "from the device (contact or motion), else GENERIC" */
        for (size_t e = 0; e < ZSRC_ZW_EP_MAX; e++) {
            if ((d->offered[e] & bit(ZSRC_ZW_SLOT_CONTACT)) != 0u) {
                return ALARM_DEVCLASS_DOOR;
            }
            if ((d->offered[e] & bit(ZSRC_ZW_SLOT_MOTION)) != 0u) {
                return ALARM_DEVCLASS_MOTION;
            }
        }
        return ALARM_DEVCLASS_GENERIC;
    case ZSRC_ZW_SLOT_MOTION: return ALARM_DEVCLASS_MOTION;
    case ZSRC_ZW_SLOT_GLASS_BREAK: return ALARM_DEVCLASS_GLASS_BREAK;
    case ZSRC_ZW_SLOT_TILT: return ALARM_DEVCLASS_GARAGE_DOOR;
    case ZSRC_ZW_SLOT_IMPACT: return ALARM_DEVCLASS_VIBRATION;
    case ZSRC_ZW_SLOT_WATER: return ALARM_DEVCLASS_WATER;
    case ZSRC_ZW_SLOT_SMOKE: return ALARM_DEVCLASS_SMOKE;
    case ZSRC_ZW_SLOT_CO: return ALARM_DEVCLASS_CO;
    case ZSRC_ZW_SLOT_HEAT: return ALARM_DEVCLASS_HEAT;
    case ZSRC_ZW_SLOT_FREEZE:
    case ZSRC_ZW_SLOT_TEMPERATURE: return ALARM_DEVCLASS_TEMPERATURE;
    case ZSRC_ZW_SLOT_GAS: return ALARM_DEVCLASS_GAS;
    case ZSRC_ZW_SLOT_EMERGENCY: return ALARM_DEVCLASS_PANIC_BUTTON;
    case ZSRC_ZW_SLOT_GENERAL:
        return override ? (alarm_device_class_t)q->device_class : ALARM_DEVCLASS_GENERIC;
    default: return ALARM_DEVCLASS_GENERIC;
    }
}
