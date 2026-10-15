/*
 * host_zigbee_demo.c - the interim Zigbee backend of the PC preview and simulator (see the
 * header). Every name, model and address here is fictional demo data (AGENTS.md "Conventions").
 */
#include "host_zigbee_demo.h"

#include <stdio.h>
#include <string.h>

#include "zone_hub/zsrc_zigbee.h"

#define DEMO_DEVS     16u
#define LISTEN_MS     180000u /* the join window: 3 minutes (ZB-002) */
#define STARTING_MS   800u    /* the radio told to listen */
#define AUTO_JOIN_MS  5000u   /* the demo's door sensor joins this far into the window */
#define STEP_MS       2000u   /* each setup step of a joining device */
#define OP_MS         1000u   /* a ping, a blink, a forget... */
#define RETRY_MS      2000u   /* a paused radio started again */
#define REMOVE_WAIT_MS 120000u /* a sleeping device asked to leave: the wait for it to wake */
#define WAKE_MS       8000u   /* unscripted, it wakes this far into the wait and answers */
#define IEEE_BASE     0x02ABCD0000000000ull /* fictional addresses */
#define SIREN_TEST_MS 2000u   /* "Test it": ZBW_TEST_S of its sound */
#define SIRENS_MAX    4u      /* zbz_wd.h ZBW_SIRENS_MAX */
#define KEYPAD_FIRST  1u      /* the device numbers a Zigbee keypad may have (alarm-devices.md §4) */
#define KEYPAD_LAST   8u
#define PANIC_KNOWN   0x0Fu   /* 1 << alarm_panic_t of the four kinds */

static const uint8_t CHANNELS[UI_ZB_CHANNELS] = {11u, 15u, 20u, 25u};

typedef struct {
    bool used;
    ui_zb_device_page_t pg;
} demo_dev_t;

static struct {
    bool (*allowed)(void);
    uint64_t now;
    bool scripted;             /* host_zigbee_demo_state() was called: nothing joins by itself */
    ui_zb_status_t st;
    demo_dev_t dev[DEMO_DEVS];
    uint16_t next_id;
    ui_zb_progress_t prog;
    uint64_t listen_end;       /* LISTENING: when the window closes */
    uint64_t starting_end;     /* STARTING: when it listens */
    uint64_t join_at;          /* the automatic join; 0 none */
    uint64_t step_at;          /* the next setup step of the joined devices */
    uint64_t op_end;           /* another operation: when it ends */
    uint64_t retry_end;        /* RETRY: when the radio works again; 0 none */
    uint64_t restart_end;      /* RESTART: when the radio works again; 0 none */
    uint64_t answer_at;        /* REMOVE of a sleeping device: when it wakes and answers; 0 never */
    uint16_t setup_id;         /* SETUP: the device set up again */
    uint8_t channel_to;        /* CHANNEL: where the network moves */
    uint64_t code_ieee;        /* ADD_CODE: the address the radio listens for */
    size_t ncodes;
    ui_zb_code_t codes[UI_ZB_CODES_MAX];
    host_zb_hooks_t hooks;
    bool code_once_set;        /* the next reset's code check answers code_once */
    host_zb_code_t code_once;
    bool siren_silent;         /* HOST_ZB_SIREN_SILENT: the next siren test gets no answer */
    uint8_t slots_taken;       /* keypad numbers 1-8 held by panel-link keypads (bit 0 = 1) */
    bool slots_race;           /* HOST_ZB_KEYPAD_RACE: the next keypad request finds 1-8 taken */
    uint8_t fob_held_by;       /* HOST_ZB_FOB_TAKEN: a user with a key fob not in this list; 0 none */
    ui_zb_req_result_t refuse_next; /* the next role request's answer; UI_ZB_REQ_OK none */
} D;

/* ---- the model ------------------------------------------------------------------------------- */

static demo_dev_t *find(uint16_t id)
{
    for (size_t i = 0; i < DEMO_DEVS; i++) {
        if (D.dev[i].used && D.dev[i].pg.dev.id == id) {
            return &D.dev[i];
        }
    }
    return NULL;
}

static uint16_t count_devices(uint16_t *not_heard)
{
    uint16_t n = 0;
    uint16_t silent = 0;
    for (size_t i = 0; i < DEMO_DEVS; i++) {
        if (D.dev[i].used) {
            n++;
            silent = (uint16_t)(silent + (D.dev[i].pg.dev.unreachable ? 1u : 0u));
        }
    }
    if (not_heard != NULL) {
        *not_heard = silent;
    }
    return n;
}

/* ---- install codes (page 10), checked as zbz_codes checks them -------------------------------- */

static uint32_t now_s(void)
{
    return D.hooks.now_s != NULL ? D.hooks.now_s() : 0u;
}

/* The hex digits of a code as typed (spaces, dashes and colons skipped); -1 for anything else. */
static int code_bytes(const char *text, uint8_t *out, size_t cap)
{
    int n = 0;
    for (const char *c = text; *c != '\0'; c++) {
        if (*c == ' ' || *c == '-' || *c == ':') {
            continue;
        }
        int v = -1;
        if (*c >= '0' && *c <= '9') {
            v = *c - '0';
        } else if (*c >= 'a' && *c <= 'f') {
            v = *c - 'a' + 10;
        } else if (*c >= 'A' && *c <= 'F') {
            v = *c - 'A' + 10;
        }
        if (v < 0) {
            return -1;
        }
        if ((size_t)n / 2u < cap) {
            out[n / 2] = n % 2 == 0 ? (uint8_t)((unsigned)v << 4) : (uint8_t)(out[n / 2] | (uint8_t)v);
        }
        n++;
    }
    return n;
}

/* CRC-16/X.25 over the code's key, as zbz_code_crc(). */
static uint16_t code_crc(const uint8_t *b, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = (uint16_t)(crc ^ b[i]);
        for (int k = 0; k < 8; k++) {
            crc = (crc & 1u) != 0u ? (uint16_t)((crc >> 1) ^ 0x8408u) : (uint16_t)(crc >> 1);
        }
    }
    return (uint16_t)~crc;
}

/* ZB-015: BAD_ADDRESS, BAD_CODE (its length, not hex) and CHECKSUM (a typo), each its own. */
static ui_zb_req_result_t code_check(const ui_zb_request_t *req)
{
    if (req->ieee == 0u || req->ieee == UINT64_MAX) {
        return UI_ZB_REQ_BAD_ADDRESS;
    }
    uint8_t b[18];
    const int n = code_bytes(req->code, b, sizeof(b));
    if (n != 16 && n != 20 && n != 28 && n != 36) {
        return UI_ZB_REQ_BAD_CODE;
    }
    const size_t len = (size_t)n / 2u;
    const uint16_t want = (uint16_t)((unsigned)b[len - 2u] | (unsigned)b[len - 1u] << 8);
    return code_crc(b, len - 2u) == want ? UI_ZB_REQ_OK : UI_ZB_REQ_CHECKSUM;
}

static ui_zb_req_result_t code_keep(uint64_t ieee)
{
    for (size_t i = 0; i < D.ncodes; i++) {
        if (D.codes[i].ieee == ieee) {
            D.codes[i].added_at = now_s(); /* given again: the new code replaces it */
            return UI_ZB_REQ_OK;
        }
    }
    if (D.ncodes >= UI_ZB_CODES_MAX) {
        return UI_ZB_REQ_FULL;
    }
    D.codes[D.ncodes].ieee = ieee;
    D.codes[D.ncodes].added_at = now_s();
    D.ncodes++;
    return UI_ZB_REQ_OK;
}

static bool code_forget(uint64_t ieee)
{
    for (size_t i = 0; i < D.ncodes; i++) {
        if (D.codes[i].ieee == ieee) {
            memmove(&D.codes[i], &D.codes[i + 1u], (D.ncodes - i - 1u) * sizeof(D.codes[0]));
            D.ncodes--;
            return true;
        }
    }
    return false;
}

/* The proposal's two codes waiting, added this morning (fictional addresses). */
static void seed_codes(void)
{
    const uint32_t t = now_s();
    D.ncodes = 2;
    D.codes[0].ieee = IEEE_BASE | 0x0101u;
    D.codes[0].added_at = t != 0u ? t - 5u * 60u : 0u;
    D.codes[1].ieee = IEEE_BASE | 0x0102u;
    D.codes[1].added_at = t != 0u ? t - 5u * 86400u : 0u;
}

/* The latest noise check (page 8): channel 11 busy with Wi-Fi, 15 some noise, 20 the quietest. */
static void seed_survey(void)
{
    static const int8_t DBM[UI_ZB_CHANNELS] = {-70, -82, -95, -93};
    D.st.survey_known = 0x0Fu;
    memcpy(D.st.survey_dbm, DBM, sizeof(DBM));
    D.st.quietest = 20;
}

typedef struct {
    const char *name, *model, *manufacturer, *model_id, *sw;
    uint8_t kind, cls, power, battery, signal, join;
    bool router, low, unreachable;
    uint32_t heard_s;
    uint16_t zone, zone2;            /* its inputs' zones (zone2: a second input) */
    const char *words, *words2, *state;
} seed_t;

static demo_dev_t *add_dev(const seed_t *s)
{
    for (size_t i = 0; i < DEMO_DEVS; i++) {
        if (D.dev[i].used) {
            continue;
        }
        demo_dev_t *d = &D.dev[i];
        memset(d, 0, sizeof(*d));
        d->used = true;
        ui_zb_device_page_t *pg = &d->pg;
        ui_zb_device_t *v = &pg->dev;
        v->id = ++D.next_id;
        snprintf(v->name, sizeof(v->name), "%s", s->name != NULL ? s->name : "");
        snprintf(v->model, sizeof(v->model), "%s", s->model);
        v->state = UI_ZB_DEV_READY;
        v->kind = s->kind;
        v->power = s->power;
        v->router = s->router;
        v->battery_pct = s->battery;
        v->battery_low = s->low;
        v->heard_s = s->heard_s;
        v->signal = s->signal;
        v->unreachable = s->unreachable;
        v->device_class = s->cls;
        v->join = s->join;
        v->setup_done = 5;
        v->setup_total = 5;
        pg->ieee = IEEE_BASE | v->id;
        pg->nwk = (uint16_t)(0x1A00u + v->id);
        snprintf(pg->manufacturer, sizeof(pg->manufacturer), "%s", s->manufacturer);
        snprintf(pg->model_id, sizeof(pg->model_id), "%s", s->model_id);
        snprintf(pg->sw_build, sizeof(pg->sw_build), "%s", s->sw != NULL ? s->sw : "");
        pg->sleepy = s->power == UI_ZB_POWER_BATTERY;
        pg->can_identify = true;
        if (s->kind == UI_ZB_KIND_SIREN) { /* page 14: every alarm, its strobe, high, after 1 h */
            pg->siren.fit = UI_ZB_FIT_AVAILABLE;
            pg->siren.partitions = 0x01u;
            pg->siren.burglary = true;
            pg->siren.fire = true;
            pg->siren.strobe = true;
            pg->siren.volume = 2u;
            pg->siren.live = true;
            pg->siren.supervision_min = 60u;
        } else if (s->kind == UI_ZB_KIND_KEYPAD) { /* page 15: Home, its state, every button */
            pg->keypad.fit = UI_ZB_FIT_AVAILABLE;
            pg->keypad.partition = 1u;
            pg->keypad.panic_mask = (uint8_t)((1u << ALARM_PANIC_POLICE) |
                                              (1u << ALARM_PANIC_FIRE) | (1u << ALARM_PANIC_MEDICAL));
            pg->keypad.show_state = true;
            pg->keypad.device = KEYPAD_FIRST;
            pg->keypad.live = true;
        } else if (s->kind == UI_ZB_KIND_KEYFOB) { /* page 16: Alex's (user 3), arms Home only */
            pg->fob.fit = UI_ZB_FIT_AVAILABLE;
            pg->fob.user = 3u;
            pg->fob.partition = 1u;
            pg->fob.live = true;
        }
        if (s->kind == UI_ZB_KIND_SENSOR) {
            const uint16_t zones[2] = {s->zone, s->zone2};
            const char *words[2] = {s->words, s->words2};
            for (size_t k = 0; k < 2u; k++) {
                if (words[k] == NULL) {
                    continue;
                }
                ui_zb_input_t *in = &pg->inputs[pg->input_count++];
                in->endpoint = (uint8_t)(k + 1u);
                in->addr = zsrc_zb_address(pg->ieee, in->endpoint);
                in->device_class = k == 0u ? s->cls : (uint8_t)ALARM_DEVCLASS_CO;
                snprintf(in->words, sizeof(in->words), "%s", words[k]);
                snprintf(in->state, sizeof(in->state), "%s", s->state != NULL ? s->state : "");
                in->zone = zones[k];
                if (zones[k] != 0u && v->zone_count < UI_ZB_ZONES_MAX) {
                    v->zones[v->zone_count++] = zones[k];
                }
            }
        }
        return d;
    }
    return NULL;
}

/* The drawing's eight devices (docs/features/07-UI-LVGL-Pro/mockup/screens/zigbee.js, page 2). */
static void seed_devices(void)
{
    static const seed_t SEEDS[] = {
        {.name = NULL, .model = "Aqara water sensor", .manufacturer = "Aqara", .model_id = "SJCGQ11LM",
         .sw = "2.0.3", .kind = UI_ZB_KIND_SENSOR, .cls = ALARM_DEVCLASS_WATER,
         .power = UI_ZB_POWER_BATTERY, .battery = 64, .signal = UI_ZB_SIGNAL_WEAK,
         .join = UI_ZB_JOIN_STANDARD_KEY, .unreachable = true, .heard_s = 9000, .zone = 8,
         .words = "water", .state = "Dry"},
        {.name = "Alex's key fob", .model = "Aqara key fob", .manufacturer = "Aqara",
         .model_id = "WXKG11LM", .sw = "1.0.7", .kind = UI_ZB_KIND_KEYFOB, .cls = ALARM_DEVCLASS_KEYFOB,
         .power = UI_ZB_POWER_BATTERY, .battery = 97, .signal = UI_ZB_SIGNAL_GOOD,
         .join = UI_ZB_JOIN_STANDARD_KEY, .heard_s = 3600},
        {.name = "Bedroom Window", .model = "Aqara door sensor", .manufacturer = "Aqara",
         .model_id = "MCCGQ11LM", .sw = "3.0.1", .kind = UI_ZB_KIND_SENSOR, .cls = ALARM_DEVCLASS_WINDOW,
         .power = UI_ZB_POWER_BATTERY, .battery = 12, .low = true, .signal = UI_ZB_SIGNAL_GOOD,
         .join = UI_ZB_JOIN_STANDARD_KEY, .heard_s = 600, .zone = 7, .words = "window contact",
         .state = "Closed"},
        {.name = "Entry Keypad", .model = "Develco keypad", .manufacturer = "Develco",
         .model_id = "KEPZB-110", .sw = "4.0.2", .kind = UI_ZB_KIND_KEYPAD, .cls = ALARM_DEVCLASS_GENERIC,
         .power = UI_ZB_POWER_BATTERY, .battery = 81, .signal = UI_ZB_SIGNAL_GOOD,
         .join = UI_ZB_JOIN_INSTALL_CODE, .heard_s = 300},
        {.name = NULL, .model = "Aqara door sensor", .manufacturer = "Aqara", .model_id = "MCCGQ11LM",
         .sw = "3.0.1", .kind = UI_ZB_KIND_SENSOR, .cls = ALARM_DEVCLASS_DOOR,
         .power = UI_ZB_POWER_BATTERY, .battery = 92, .signal = UI_ZB_SIGNAL_GOOD,
         .join = UI_ZB_JOIN_INSTALL_CODE, .heard_s = 120, .zone = 1, .words = "door contact",
         .state = "Closed"},
        {.name = "Hall Siren", .model = "Heiman siren", .manufacturer = "Heiman", .model_id = "HS2WD-E",
         .sw = "1.1.0", .kind = UI_ZB_KIND_SIREN, .cls = ALARM_DEVCLASS_GENERIC,
         .power = UI_ZB_POWER_MAINS, .battery = UI_ZB_BATTERY_UNKNOWN, .router = true,
         .signal = UI_ZB_SIGNAL_GOOD, .join = UI_ZB_JOIN_STANDARD_KEY, .heard_s = 30},
        {.name = NULL, .model = "Aqara motion sensor", .manufacturer = "Aqara", .model_id = "RTCGQ11LM",
         .sw = "1.2.4", .kind = UI_ZB_KIND_SENSOR, .cls = ALARM_DEVCLASS_MOTION,
         .power = UI_ZB_POWER_BATTERY, .battery = 75, .signal = UI_ZB_SIGNAL_FAIR,
         .join = UI_ZB_JOIN_STANDARD_KEY, .heard_s = 240, .zone = 5, .words = "motion",
         .state = "Still"},
        {.name = NULL, .model = "Heiman smoke and CO alarm", .manufacturer = "Heiman",
         .model_id = "HS1CA-E", .sw = "2.1.0", .kind = UI_ZB_KIND_SENSOR, .cls = ALARM_DEVCLASS_SMOKE,
         .power = UI_ZB_POWER_BATTERY, .battery = 100, .signal = UI_ZB_SIGNAL_GOOD,
         .join = UI_ZB_JOIN_STANDARD_KEY, .heard_s = 900, .zone = 6, .zone2 = 9,
         .words = "smoke", .words2 = "CO", .state = "Normal"},
    };
    for (size_t i = 0; i < sizeof(SEEDS) / sizeof(SEEDS[0]); i++) {
        (void)add_dev(&SEEDS[i]);
    }
}

/* A device that joins during an add: a door sensor (automatic, and JOIN_TWO), a motion sensor
 * (JOIN_TWO), a smart plug (JOIN_PLUG). */
static demo_dev_t *joining_dev(uint8_t which)
{
    static const seed_t JOINERS[] = {
        {.model = "Aqara door sensor", .manufacturer = "Aqara", .model_id = "MCCGQ11LM", .sw = "3.0.1",
         .kind = UI_ZB_KIND_SENSOR, .cls = ALARM_DEVCLASS_DOOR, .power = UI_ZB_POWER_BATTERY,
         .battery = 100, .signal = UI_ZB_SIGNAL_GOOD, .join = UI_ZB_JOIN_STANDARD_KEY,
         .words = "door contact", .state = "Closed"},
        {.model = "Aqara motion sensor", .manufacturer = "Aqara", .model_id = "RTCGQ11LM", .sw = "1.2.4",
         .kind = UI_ZB_KIND_SENSOR, .cls = ALARM_DEVCLASS_MOTION, .power = UI_ZB_POWER_BATTERY,
         .battery = 100, .signal = UI_ZB_SIGNAL_FAIR, .join = UI_ZB_JOIN_STANDARD_KEY,
         .words = "motion", .state = "Still"},
        {.model = "Sonoff smart plug", .manufacturer = "Sonoff", .model_id = "S31ZB", .sw = "2.0.0",
         .kind = UI_ZB_KIND_OTHER, .cls = ALARM_DEVCLASS_GENERIC, .power = UI_ZB_POWER_MAINS,
         .battery = UI_ZB_BATTERY_UNKNOWN, .router = true, .signal = UI_ZB_SIGNAL_GOOD,
         .join = UI_ZB_JOIN_STANDARD_KEY},
    };
    demo_dev_t *d = add_dev(&JOINERS[which < 3u ? which : 0u]);
    if (d != NULL) {
        d->pg.dev.state = which == 2u ? UI_ZB_DEV_NOT_USABLE : UI_ZB_DEV_JOINING;
        d->pg.dev.setup_done = 0;
        d->pg.dev.heard_s = 0;
    }
    return d;
}

static ui_zb_joined_t *join_row(const demo_dev_t *d, uint8_t step)
{
    if (D.prog.joined_count >= UI_ZB_JOINED_MAX || d == NULL) {
        return NULL;
    }
    ui_zb_joined_t *j = &D.prog.joined[D.prog.joined_count++];
    memset(j, 0, sizeof(*j));
    j->id = d->pg.dev.id;
    j->step = step;
    snprintf(j->model, sizeof(j->model), "%s", d->pg.dev.model);
    return j;
}

/* A joined row's device follows the row's step. */
static void sync_dev(const ui_zb_joined_t *j)
{
    demo_dev_t *d = find(j->id);
    if (d == NULL || d->pg.dev.state == UI_ZB_DEV_NOT_USABLE) {
        return;
    }
    ui_zb_device_t *v = &d->pg.dev;
    switch (j->step) {
    case UI_ZB_STEP_JOINING: v->state = UI_ZB_DEV_JOINING; v->setup_done = 1; break;
    case UI_ZB_STEP_READING: v->state = UI_ZB_DEV_SETTING_UP; v->setup_done = 2; break;
    case UI_ZB_STEP_SETTING_UP: v->state = UI_ZB_DEV_SETTING_UP; v->setup_done = 3; break;
    case UI_ZB_STEP_DONE:
        if (j->result == UI_ZB_RES_ADDED) {
            v->state = UI_ZB_DEV_READY;
            v->setup_done = v->setup_total;
        }
        break;
    default: break;
    }
    if (j->asleep && j->step != UI_ZB_STEP_DONE) {
        v->state = UI_ZB_DEV_WAKE_NEEDED;
    }
}

static bool adding(void)
{
    return D.prog.op == UI_ZB_OP_ADD || D.prog.op == UI_ZB_OP_ADD_CODE ||
           D.prog.op == UI_ZB_OP_REPLACE;
}

static void end_op(uint8_t result)
{
    D.prog.step = UI_ZB_STEP_DONE;
    D.prog.result = result;
    D.prog.seconds_left = 0;
    D.join_at = 0;
    D.listen_end = 0;
}

static void remove_dev(uint16_t id)
{
    demo_dev_t *d = find(id);
    if (d != NULL) {
        D.prog.zone_count = d->pg.dev.zone_count;
        memcpy(D.prog.zones, d->pg.dev.zones, sizeof(D.prog.zones));
        snprintf(D.prog.model, sizeof(D.prog.model), "%s", d->pg.dev.model);
        d->used = false;
    }
}

/* The window ran out (or was stopped): what the add ends with. */
static void window_ends(uint8_t nothing)
{
    bool unfinished = false;
    for (size_t i = 0; i < D.prog.joined_count; i++) {
        const ui_zb_joined_t *j = &D.prog.joined[i];
        unfinished = unfinished || j->step != UI_ZB_STEP_DONE;
    }
    if (D.prog.op == UI_ZB_OP_REPLACE && D.prog.joined_count > 0u && !unfinished) {
        end_op(UI_ZB_RES_REPLACED);
    } else if (D.prog.joined_count == 0u) {
        end_op(nothing);
    } else if (unfinished) {
        /* as zbz_panel ends it (row_step, setup_result): the unfinished row is DONE with its
         * result, and no longer asleep - its setup stopped, only "Set it up again" goes on */
        for (size_t i = 0; i < D.prog.joined_count; i++) {
            ui_zb_joined_t *j = &D.prog.joined[i];
            if (j->step != UI_ZB_STEP_DONE) {
                j->step = UI_ZB_STEP_DONE;
                j->result = UI_ZB_RES_SETUP_NOT_FINISHED;
                j->asleep = false;
            }
        }
        end_op(UI_ZB_RES_SETUP_NOT_FINISHED);
    } else {
        end_op(UI_ZB_RES_ADDED);
    }
}

/* A replace: the new device takes the old one's zones, and the old one goes. */
static void replace_done(uint16_t new_id)
{
    demo_dev_t *old = find(D.prog.old_id);
    demo_dev_t *nu = find(new_id);
    if (old == NULL || nu == NULL) {
        end_op(UI_ZB_RES_FAILED);
        return;
    }
    nu->pg.dev.zone_count = old->pg.dev.zone_count;
    memcpy(nu->pg.dev.zones, old->pg.dev.zones, sizeof(nu->pg.dev.zones));
    snprintf(nu->pg.dev.name, sizeof(nu->pg.dev.name), "%s", old->pg.dev.name);
    for (size_t i = 0; i < nu->pg.input_count && i < old->pg.input_count; i++) {
        nu->pg.inputs[i].zone = old->pg.inputs[i].zone;
    }
    remove_dev(D.prog.old_id);
    D.prog.id = new_id;
    snprintf(D.prog.model, sizeof(D.prog.model), "%s", nu->pg.dev.model);
    end_op(UI_ZB_RES_REPLACED);
}

static void step_joined(void)
{
    for (size_t i = 0; i < D.prog.joined_count; i++) {
        ui_zb_joined_t *j = &D.prog.joined[i];
        if (j->step == UI_ZB_STEP_DONE || j->asleep) {
            continue;
        }
        j->step = j->step == UI_ZB_STEP_JOINING   ? UI_ZB_STEP_READING
                  : j->step == UI_ZB_STEP_READING ? UI_ZB_STEP_SETTING_UP
                                                  : UI_ZB_STEP_DONE;
        if (j->step == UI_ZB_STEP_DONE) {
            j->result = UI_ZB_RES_ADDED;
        }
        sync_dev(j);
        if (j->step == UI_ZB_STEP_DONE && D.prog.op == UI_ZB_OP_REPLACE) {
            replace_done(j->id);
            return;
        }
    }
}

/* One slice of time (at most 100 ms). */
static void tick(void)
{
    if (D.retry_end != 0u && D.now >= D.retry_end) {
        D.retry_end = 0;
        D.st.radio = UI_ZB_RADIO_WORKING;
        D.st.restarts = 0;
    }
    if (D.restart_end != 0u && D.now >= D.restart_end) {
        D.restart_end = 0;
        D.st.radio = UI_ZB_RADIO_WORKING; /* zones keep their state: nothing else changes */
    }
    if (D.prog.op == UI_ZB_OP_REMOVE && D.prog.step == UI_ZB_STEP_WORKING && D.op_end != 0u) {
        if (D.answer_at != 0u && D.now >= D.answer_at) {
            D.answer_at = 0;
            D.op_end = D.now; /* it woke and answered: removed below */
            D.prog.seconds_left = 0;
        } else if (D.now < D.op_end) {
            D.prog.seconds_left = (uint32_t)((D.op_end - D.now + 999u) / 1000u);
        }
    }
    if (adding() && D.prog.step == UI_ZB_STEP_STARTING && D.now >= D.starting_end) {
        D.prog.step = UI_ZB_STEP_LISTENING;
        D.listen_end = D.now + LISTEN_MS;
    }
    if (adding() && D.prog.step == UI_ZB_STEP_LISTENING) {
        if (D.join_at != 0u && D.now >= D.join_at) {
            D.join_at = 0;
            demo_dev_t *nd = joining_dev(0);
            if (nd != NULL && D.prog.op == UI_ZB_OP_ADD_CODE) {
                nd->pg.ieee = D.code_ieee; /* the device of the code, joining with its own key */
                nd->pg.dev.join = UI_ZB_JOIN_INSTALL_CODE;
                (void)code_forget(D.code_ieee); /* joined: it leaves Install codes waiting */
            }
            ui_zb_joined_t *j = join_row(nd, UI_ZB_STEP_JOINING);
            if (j != NULL) {
                sync_dev(j);
                D.step_at = D.now + STEP_MS;
            }
        }
        if (D.step_at != 0u && D.now >= D.step_at) {
            D.step_at = D.now + STEP_MS;
            step_joined();
        }
        if (D.prog.step == UI_ZB_STEP_LISTENING) {
            if (D.now >= D.listen_end) {
                window_ends(UI_ZB_RES_NOTHING_JOINED);
            } else {
                D.prog.seconds_left = (uint32_t)((D.listen_end - D.now + 999u) / 1000u);
            }
        }
    }
    if (D.op_end != 0u && D.now >= D.op_end) {
        D.op_end = 0;
        switch (D.prog.op) {
        case UI_ZB_OP_PING: {
            const demo_dev_t *d = find(D.prog.id);
            end_op(d != NULL && !d->pg.dev.unreachable ? UI_ZB_RES_ANSWERED : UI_ZB_RES_NO_ANSWER);
            break;
        }
        case UI_ZB_OP_IDENTIFY: end_op(UI_ZB_RES_DONE); break;
        case UI_ZB_OP_SIREN_TEST: {
            demo_dev_t *d = find(D.prog.id);
            const uint8_t res = D.siren_silent ? UI_ZB_RES_NO_ANSWER : UI_ZB_RES_DONE;
            D.siren_silent = false;
            if (d != NULL) {
                d->pg.siren.last_test = res; /* RAM only, by device (not stored) */
            }
            end_op(res);
            break;
        }
        case UI_ZB_OP_SETUP: {
            demo_dev_t *d = find(D.prog.id);
            if (d != NULL) {
                d->pg.dev.state = UI_ZB_DEV_READY;
                d->pg.dev.setup_done = d->pg.dev.setup_total;
            }
            end_op(UI_ZB_RES_DONE);
            break;
        }
        case UI_ZB_OP_FORGET: remove_dev(D.prog.id); end_op(UI_ZB_RES_FORGOTTEN); break;
        case UI_ZB_OP_REMOVE:
            if (D.prog.seconds_left != 0u) { /* the wait ran out before it woke */
                end_op(UI_ZB_RES_NO_ANSWER);
            } else {
                remove_dev(D.prog.id);
                end_op(UI_ZB_RES_REMOVED);
            }
            break;
        case UI_ZB_OP_CHANNEL:
            D.st.channel = D.channel_to;
            D.st.jammed = false;
            end_op(UI_ZB_RES_DONE);
            break;
        case UI_ZB_OP_SURVEY:
            seed_survey();
            D.st.noise_known = true;
            end_op(UI_ZB_RES_DONE);
            break;
        case UI_ZB_OP_RESET:
            for (size_t i = 0; i < DEMO_DEVS; i++) {
                D.dev[i].used = false;
            }
            D.ncodes = 0;
            D.st.pan_id = 0x2B71;                   /* a new network, with new keys */
            D.st.ext_pan_id = 0x02ABCD0000000A1Eull;
            end_op(UI_ZB_RES_DONE);
            break;
        default: end_op(UI_ZB_RES_DONE); break;
        }
    }
}

/* ---- ui_backend_t's zb_* --------------------------------------------------------------------- */

static bool be_zb_status(void *ctx, ui_zb_status_t *out)
{
    (void)ctx;
    *out = D.st;
    out->devices = count_devices(&out->not_heard);
    out->codes_waiting = (uint16_t)D.ncodes;
    out->learn_s = adding() && D.prog.step == UI_ZB_STEP_LISTENING ? (uint16_t)D.prog.seconds_left : 0u;
    return true;
}

static size_t be_zb_devices(void *ctx, ui_zb_device_t *out, size_t max)
{
    (void)ctx;
    size_t n = 0;
    for (size_t i = 0; i < DEMO_DEVS && n < max; i++) {
        if (D.dev[i].used) {
            out[n++] = D.dev[i].pg.dev;
        }
    }
    return n;
}

static uint8_t free_keypad_number(void);

static bool be_zb_device(void *ctx, uint16_t id, ui_zb_device_page_t *out)
{
    (void)ctx;
    const demo_dev_t *d = find(id);
    if (d == NULL) {
        return false;
    }
    *out = d->pg;
    const bool ready = d->pg.dev.state == UI_ZB_DEV_READY;
    const uint8_t fit = (uint8_t)(ready ? UI_ZB_FIT_AVAILABLE : UI_ZB_FIT_NOT_READY);
    switch (d->pg.dev.kind) {
    case UI_ZB_KIND_SIREN: out->siren.fit = fit; break;
    case UI_ZB_KIND_KEYPAD:
        out->keypad.fit = fit;
        if (ready && out->keypad.device == 0u && free_keypad_number() == 0u) {
            out->keypad.fit = UI_ZB_FIT_NO_DEVICE_SLOT;
        }
        break;
    case UI_ZB_KIND_KEYFOB: out->fob.fit = fit; break;
    default: break;
    }
    return true;
}

static void be_zb_progress(void *ctx, ui_zb_progress_t *out)
{
    (void)ctx;
    *out = D.prog;
}

static size_t be_zb_codes(void *ctx, ui_zb_code_t *out, size_t max)
{
    (void)ctx;
    size_t n = 0;
    for (; n < D.ncodes && n < max; n++) {
        out[n] = D.codes[n];
    }
    return n;
}

static bool busy(void)
{
    return D.prog.op != UI_ZB_OP_NONE && D.prog.step != UI_ZB_STEP_DONE &&
           D.prog.step != UI_ZB_STEP_IDLE;
}

static ui_zb_req_result_t start_add(uint8_t op, uint16_t old_id)
{
    if (busy()) {
        return UI_ZB_REQ_BUSY;
    }
    if (op == UI_ZB_OP_REPLACE && find(old_id) == NULL) {
        return UI_ZB_REQ_NOT_FOUND;
    }
    memset(&D.prog, 0, sizeof(D.prog));
    D.prog.op = op;
    D.prog.old_id = old_id;
    D.prog.step = UI_ZB_STEP_STARTING;
    D.starting_end = D.now + STARTING_MS;
    D.join_at = D.scripted ? 0u : D.now + AUTO_JOIN_MS;
    D.step_at = 0;
    return UI_ZB_REQ_OK;
}

static ui_zb_req_result_t start_op(uint8_t op, uint16_t id)
{
    if (busy()) {
        return UI_ZB_REQ_BUSY;
    }
    const demo_dev_t *d = find(id);
    if (d == NULL) {
        return UI_ZB_REQ_NOT_FOUND;
    }
    memset(&D.prog, 0, sizeof(D.prog));
    D.prog.op = op;
    D.prog.id = id;
    D.prog.step = UI_ZB_STEP_WORKING;
    snprintf(D.prog.model, sizeof(D.prog.model), "%s", d->pg.dev.model);
    D.op_end = D.now + OP_MS;
    D.answer_at = 0;
    if (op == UI_ZB_OP_REMOVE && d->pg.sleepy) {
        D.op_end = D.now + REMOVE_WAIT_MS; /* it answers when it wakes */
        D.prog.seconds_left = REMOVE_WAIT_MS / 1000u;
        D.answer_at = D.scripted ? 0u : D.now + WAKE_MS;
    } else if (op == UI_ZB_OP_REMOVE) {
        D.answer_at = D.now + OP_MS; /* awake: it answers at once */
    }
    return UI_ZB_REQ_OK;
}

/* An operation of the network itself (no device). */
static ui_zb_req_result_t start_net_op(uint8_t op, uint32_t ms)
{
    if (busy()) {
        return UI_ZB_REQ_BUSY;
    }
    memset(&D.prog, 0, sizeof(D.prog));
    D.prog.op = op;
    D.prog.step = UI_ZB_STEP_WORKING;
    D.op_end = D.now + ms;
    return UI_ZB_REQ_OK;
}

/* ---- the tier B roles (BL-147, BL-148), answered as zbz_panel answers them ---------------- */

/* The lowest keypad number no other Zigbee keypad and no panel-link keypad holds; 0 none. */
static uint8_t free_keypad_number(void)
{
    for (uint8_t n = KEYPAD_FIRST; n <= KEYPAD_LAST; n++) {
        bool held = (D.slots_taken & (1u << (n - 1u))) != 0u;
        for (size_t i = 0; i < DEMO_DEVS && !held; i++) {
            held = D.dev[i].used && D.dev[i].pg.keypad.device == n;
        }
        if (!held) {
            return n;
        }
    }
    return 0u;
}

static ui_zb_req_result_t req_siren(demo_dev_t *d, const ui_zb_siren_t *in)
{
    ui_zb_siren_t *sn = &d->pg.siren;
    if (in->partitions == 0u) { /* no partition clears it (Stop if it sounds) */
        const uint8_t fit = sn->fit;
        memset(sn, 0, sizeof(*sn));
        sn->fit = fit;
        return UI_ZB_REQ_OK;
    }
    if ((in->partitions & (uint8_t)~0x0Fu) != 0u || in->volume > 3u ||
        (in->supervision_min != 0u && in->supervision_min < 60u)) {
        return UI_ZB_REQ_INVALID;
    }
    if (sn->partitions == 0u) {
        size_t sirens = 0;
        for (size_t i = 0; i < DEMO_DEVS; i++) {
            sirens += D.dev[i].used && D.dev[i].pg.siren.partitions != 0u ? 1u : 0u;
        }
        if (sirens >= SIRENS_MAX) {
            return UI_ZB_REQ_FULL;
        }
    }
    sn->partitions = in->partitions;
    sn->burglary = in->burglary;
    sn->fire = in->fire;
    sn->strobe = in->strobe;
    sn->squawk = in->squawk;
    sn->volume = in->volume;
    sn->supervision_min = in->supervision_min;
    sn->device = 0u; /* a siren takes no device number */
    sn->live = true;
    return UI_ZB_REQ_OK;
}

static ui_zb_req_result_t req_keypad(demo_dev_t *d, const ui_zb_keypad_t *in)
{
    ui_zb_keypad_t *kp = &d->pg.keypad;
    if (in->partition == 0u) { /* no partition clears it: its number is free again */
        const uint8_t fit = kp->fit;
        memset(kp, 0, sizeof(*kp));
        kp->fit = fit;
        return UI_ZB_REQ_OK;
    }
    if (in->partition > ALARM_MAX_PARTITIONS || (in->panic_mask & (uint8_t)~PANIC_KNOWN) != 0u) {
        return UI_ZB_REQ_INVALID;
    }
    if (D.slots_race) { /* panel-link keypads took the last numbers since the page was read */
        D.slots_race = false;
        D.slots_taken = 0xFFu;
    }
    if (kp->device == 0u) { /* the panel gives the number, never the request */
        kp->device = free_keypad_number();
        if (kp->device == 0u) {
            return UI_ZB_REQ_FULL;
        }
    }
    kp->partition = in->partition;
    kp->panic_mask = in->panic_mask;
    kp->show_state = in->show_state;
    kp->live = true;
    return UI_ZB_REQ_OK;
}

static ui_zb_req_result_t req_fob(demo_dev_t *d, const ui_zb_fob_t *in)
{
    ui_zb_fob_t *fb = &d->pg.fob;
    if (in->user == 0u) { /* nobody's: it does nothing */
        const uint8_t fit = fb->fit;
        memset(fb, 0, sizeof(*fb));
        fb->fit = fit;
        return UI_ZB_REQ_OK;
    }
    if (in->user > ALARM_MAX_USERS || in->partition == 0u || in->partition > ALARM_MAX_PARTITIONS) {
        return UI_ZB_REQ_INVALID;
    }
    if (D.fob_held_by == in->user) {
        return UI_ZB_REQ_IN_USE;
    }
    for (size_t i = 0; i < DEMO_DEVS; i++) {
        if (D.dev[i].used && &D.dev[i] != d && D.dev[i].pg.fob.user == in->user) {
            return UI_ZB_REQ_IN_USE; /* one key fob for each user */
        }
    }
    fb->user = in->user;
    fb->partition = in->partition;
    fb->may_disarm = in->may_disarm;
    fb->panic = in->panic;
    fb->live = true;
    return UI_ZB_REQ_OK;
}

static ui_zb_req_result_t req_role(const ui_zb_request_t *req)
{
    demo_dev_t *d = find(req->id);
    if (d == NULL) {
        return UI_ZB_REQ_NOT_FOUND;
    }
    const uint8_t want = req->kind == UI_ZB_REQ_KEYPAD ? (uint8_t)UI_ZB_KIND_KEYPAD
                         : req->kind == UI_ZB_REQ_FOB  ? (uint8_t)UI_ZB_KIND_KEYFOB
                                                       : (uint8_t)UI_ZB_KIND_SIREN;
    if (d->pg.dev.kind != want || d->pg.dev.state != UI_ZB_DEV_READY) {
        return UI_ZB_REQ_INVALID; /* not that kind of device, or not set up yet */
    }
    if (D.refuse_next != UI_ZB_REQ_OK) { /* scripted: this request's answer */
        const ui_zb_req_result_t r = D.refuse_next;
        D.refuse_next = UI_ZB_REQ_OK;
        return r;
    }
    switch (req->kind) {
    case UI_ZB_REQ_SIREN: return req_siren(d, &req->siren);
    case UI_ZB_REQ_KEYPAD: return req_keypad(d, &req->keypad);
    case UI_ZB_REQ_FOB: return req_fob(d, &req->fob);
    default: break;
    }
    /* SIREN_TEST: an operation; its end is the page's last_test */
    if (busy()) {
        return UI_ZB_REQ_BUSY;
    }
    if (D.st.radio != UI_ZB_RADIO_WORKING) {
        return UI_ZB_REQ_NO_RADIO;
    }
    if (d->pg.siren.partitions == 0u) {
        return UI_ZB_REQ_INVALID; /* not one of the panel's sirens yet */
    }
    const ui_zb_req_result_t r = start_op(UI_ZB_OP_SIREN_TEST, req->id);
    if (r == UI_ZB_REQ_OK) {
        D.op_end = D.now + SIREN_TEST_MS;
    }
    return r;
}

static ui_zb_req_result_t be_zb_request(void *ctx, const ui_zb_request_t *req)
{
    (void)ctx;
    if (req->kind == UI_ZB_REQ_DISMISS) {
        if (D.prog.step == UI_ZB_STEP_DONE) {
            /* devices still setting up go on by themselves: here they finish at once */
            for (size_t i = 0; i < D.prog.joined_count; i++) {
                ui_zb_joined_t *j = &D.prog.joined[i];
                if (j->step != UI_ZB_STEP_DONE && !j->asleep) {
                    j->step = UI_ZB_STEP_DONE;
                    j->result = UI_ZB_RES_ADDED;
                    sync_dev(j);
                }
            }
            memset(&D.prog, 0, sizeof(D.prog));
        }
        return UI_ZB_REQ_OK;
    }
    if (req->kind == UI_ZB_REQ_RESET) {
        /* ZB-017: the installer code first, before anything else (it counts toward the lockout) */
        const host_zb_code_t c = D.code_once_set                ? D.code_once
                                 : D.hooks.installer_code != NULL ? D.hooks.installer_code(
                                                                        req->installer_code)
                                                                  : HOST_ZB_CODE_WRONG;
        D.code_once_set = false;
        if (c != HOST_ZB_CODE_OK) {
            return c == HOST_ZB_CODE_LOCKED ? UI_ZB_REQ_LOCKED_OUT : UI_ZB_REQ_WRONG_CODE;
        }
    }
    if (D.allowed == NULL || !D.allowed()) {
        return UI_ZB_REQ_NOT_ALLOWED;
    }
    if (req->kind == UI_ZB_REQ_CODE_FORGET) {
        return code_forget(req->ieee) ? UI_ZB_REQ_OK : UI_ZB_REQ_NOT_FOUND;
    }
    if (req->kind == UI_ZB_REQ_CODE_ADD) { /* kept until its device joins: no radio needed */
        const ui_zb_req_result_t r = code_check(req);
        return r != UI_ZB_REQ_OK ? r : code_keep(req->ieee);
    }
    if (req->kind == UI_ZB_REQ_RESTART) {
        if (busy()) {
            return UI_ZB_REQ_BUSY;
        }
        D.st.radio = UI_ZB_RADIO_STARTING;
        D.restart_end = D.now + RETRY_MS;
        return UI_ZB_REQ_OK;
    }
    if (req->kind == UI_ZB_REQ_RETRY) {
        if (D.st.radio != UI_ZB_RADIO_PAUSED) {
            return UI_ZB_REQ_INVALID;
        }
        D.st.radio = UI_ZB_RADIO_STARTING;
        D.retry_end = D.now + RETRY_MS;
        return UI_ZB_REQ_OK;
    }
    if (D.st.radio != UI_ZB_RADIO_WORKING) {
        return UI_ZB_REQ_NO_RADIO;
    }
    switch (req->kind) {
    case UI_ZB_REQ_ADD:
        if (count_devices(NULL) >= DEMO_DEVS - 1u) {
            return UI_ZB_REQ_FULL;
        }
        return start_add(UI_ZB_OP_ADD, 0u);
    case UI_ZB_REQ_ADD_CODE: {
        ui_zb_req_result_t r = code_check(req);
        if (r == UI_ZB_REQ_OK && busy()) {
            r = UI_ZB_REQ_BUSY;
        }
        if (r == UI_ZB_REQ_OK) {
            r = code_keep(req->ieee); /* the code is given first: it stays when nothing joins */
        }
        if (r == UI_ZB_REQ_OK) {
            r = start_add(UI_ZB_OP_ADD_CODE, 0u);
            D.code_ieee = req->ieee;
        }
        return r;
    }
    case UI_ZB_REQ_CHANNEL: {
        bool offered = false;
        for (size_t i = 0; i < UI_ZB_CHANNELS; i++) {
            offered = offered || CHANNELS[i] == req->channel;
        }
        if (!offered || req->channel == D.st.channel) {
            return UI_ZB_REQ_INVALID;
        }
        const ui_zb_req_result_t r = start_net_op(UI_ZB_OP_CHANNEL, 3u * OP_MS);
        D.channel_to = req->channel;
        return r;
    }
    case UI_ZB_REQ_SURVEY: return start_net_op(UI_ZB_OP_SURVEY, 2u * OP_MS);
    case UI_ZB_REQ_RESET: return start_net_op(UI_ZB_OP_RESET, 2u * OP_MS);
    case UI_ZB_REQ_REPLACE: return start_add(UI_ZB_OP_REPLACE, req->id);
    case UI_ZB_REQ_KEEP_LISTENING:
        if (!adding() || D.prog.step != UI_ZB_STEP_LISTENING) {
            return UI_ZB_REQ_INVALID;
        }
        D.listen_end = D.now + LISTEN_MS;
        D.prog.seconds_left = LISTEN_MS / 1000u;
        return UI_ZB_REQ_OK;
    case UI_ZB_REQ_STOP:
        if (D.prog.op == UI_ZB_OP_REMOVE && D.prog.step == UI_ZB_STEP_WORKING) {
            D.op_end = 0; /* stop waiting: it stays in the network */
            D.answer_at = 0;
            end_op(UI_ZB_RES_STOPPED);
            return UI_ZB_REQ_OK;
        }
        if (!adding() || D.prog.step == UI_ZB_STEP_DONE) {
            return UI_ZB_REQ_INVALID;
        }
        window_ends(UI_ZB_RES_STOPPED);
        return UI_ZB_REQ_OK;
    case UI_ZB_REQ_PING: return start_op(UI_ZB_OP_PING, req->id);
    case UI_ZB_REQ_IDENTIFY: return start_op(UI_ZB_OP_IDENTIFY, req->id);
    case UI_ZB_REQ_FORGET: return start_op(UI_ZB_OP_FORGET, req->id);
    case UI_ZB_REQ_REMOVE: return start_op(UI_ZB_OP_REMOVE, req->id);
    case UI_ZB_REQ_SETUP: {
        const ui_zb_req_result_t r = start_op(UI_ZB_OP_SETUP, req->id);
        demo_dev_t *d = find(req->id);
        if (r == UI_ZB_REQ_OK && d != NULL) {
            d->pg.dev.state = UI_ZB_DEV_SETTING_UP;
            d->pg.dev.setup_done = 1;
            D.op_end = D.now + 3u * OP_MS;
        }
        return r;
    }
    case UI_ZB_REQ_RENAME: {
        demo_dev_t *d = find(req->id);
        if (d == NULL) {
            return UI_ZB_REQ_NOT_FOUND;
        }
        snprintf(d->pg.dev.name, sizeof(d->pg.dev.name), "%s", req->name);
        return UI_ZB_REQ_OK;
    }
    case UI_ZB_REQ_SIREN:
    case UI_ZB_REQ_SIREN_TEST:
    case UI_ZB_REQ_KEYPAD:
    case UI_ZB_REQ_FOB: return req_role(req);
    default: return UI_ZB_REQ_UNSUPPORTED;
    }
}

/* ---- the API --------------------------------------------------------------------------------- */

static void reset_model(void)
{
    bool (*allowed)(void) = D.allowed;
    const uint64_t now = D.now;
    const bool scripted = D.scripted;
    const host_zb_hooks_t hooks = D.hooks;
    memset(&D, 0, sizeof(D));
    D.allowed = allowed;
    D.now = now;
    D.scripted = scripted;
    D.hooks = hooks;
    D.st.radio = UI_ZB_RADIO_WORKING;
    D.st.network_known = true;
    D.st.channel = 15;
    D.st.pan_id = 0x1A62;
    D.st.ext_pan_id = 0x02ABCD00000000FFull;
    D.st.panel_ieee = IEEE_BASE | 0x00FEu;
    snprintf(D.st.radio_version, sizeof(D.st.radio_version), "%s", "1.4.1");
    D.st.noise_known = true;
    D.st.noise_dbm = -92;
    D.st.restarts = 1;
    seed_survey();
    seed_codes();
    seed_devices();
}

void host_zigbee_demo_init(bool (*allowed)(void))
{
    memset(&D, 0, sizeof(D));
    D.allowed = allowed;
    reset_model();
}

void host_zigbee_demo_advance(uint32_t ms)
{
    const uint64_t end = D.now + ms;
    while (D.now < end) {
        D.now = end - D.now > 100u ? D.now + 100u : end;
        tick();
    }
}

void host_zigbee_demo_backend(ui_backend_t *out)
{
    out->zb_status = be_zb_status;
    out->zb_devices = be_zb_devices;
    out->zb_device = be_zb_device;
    out->zb_request = be_zb_request;
    out->zb_progress = be_zb_progress;
    out->zb_codes = be_zb_codes;
}

/* An add that runs, listening with `left_s` to go (made when none runs). */
static void ensure_listening(uint32_t left_s)
{
    if (!adding() || D.prog.step == UI_ZB_STEP_DONE) {
        memset(&D.prog, 0, sizeof(D.prog));
        D.prog.op = UI_ZB_OP_ADD;
    }
    D.prog.step = UI_ZB_STEP_LISTENING;
    D.listen_end = D.now + (uint64_t)left_s * 1000u;
    D.prog.seconds_left = left_s;
    D.join_at = 0;
    D.step_at = 0; /* scripted: the rows stay where they are put */
}

static demo_dev_t *joined_dev(size_t i)
{
    return i < D.prog.joined_count ? find(D.prog.joined[i].id) : NULL;
}

void host_zigbee_demo_state(host_zb_demo_t what)
{
    D.scripted = true;
    switch (what) {
    case HOST_ZB_WORKING:
        reset_model();
        break;
    case HOST_ZB_EMPTY:
        reset_model();
        for (size_t i = 0; i < DEMO_DEVS; i++) {
            D.dev[i].used = false;
        }
        D.ncodes = 0;
        break;
    case HOST_ZB_STARTING: D.st.radio = UI_ZB_RADIO_STARTING; D.st.jammed = false; break;
    case HOST_ZB_NOT_CONNECTED:
        D.st.radio = UI_ZB_RADIO_NOT_CONNECTED;
        D.st.jammed = false;
        D.st.network_known = false;
        break;
    case HOST_ZB_NOT_ANSWERING: D.st.radio = UI_ZB_RADIO_NOT_ANSWERING; D.st.jammed = false; break;
    case HOST_ZB_PAUSED:
        D.st.radio = UI_ZB_RADIO_PAUSED;
        D.st.paused_why = UI_ZB_PAUSE_NO_ANSWER;
        D.st.restarts = 2;
        D.st.jammed = false;
        break;
    case HOST_ZB_SOCKET_ZWAVE: D.st.radio = UI_ZB_RADIO_SOCKET_ZWAVE; D.st.jammed = false; break;
    case HOST_ZB_INTERFERENCE:
        D.st.radio = UI_ZB_RADIO_WORKING;
        D.st.network_known = true;
        D.st.jammed = true;
        break;
    case HOST_ZB_JOIN_TWO: {
        ensure_listening(118);
        if (D.prog.joined_count == 0u) {
            ui_zb_joined_t *a = join_row(joining_dev(0), UI_ZB_STEP_DONE);
            if (a != NULL) {
                a->result = UI_ZB_RES_ADDED;
                sync_dev(a);
            }
            ui_zb_joined_t *b = join_row(joining_dev(1), UI_ZB_STEP_READING);
            if (b != NULL) {
                sync_dev(b);
            }
        }
        break;
    }
    case HOST_ZB_DOZE: {
        ensure_listening(72);
        if (D.prog.joined_count >= 2u) {
            ui_zb_joined_t *b = &D.prog.joined[1];
            b->step = UI_ZB_STEP_SETTING_UP;
            b->asleep = true;
            sync_dev(b);
        }
        break;
    }
    case HOST_ZB_WAKE: {
        demo_dev_t *d = joined_dev(1);
        for (size_t i = 0; i < DEMO_DEVS && d == NULL; i++) { /* after a dismiss: the newest */
            if (D.dev[i].used && D.dev[i].pg.dev.state == UI_ZB_DEV_WAKE_NEEDED) {
                d = &D.dev[i];
            }
        }
        if (d != NULL) {
            d->pg.dev.state = UI_ZB_DEV_READY;
            d->pg.dev.setup_done = d->pg.dev.setup_total;
        }
        if (D.prog.joined_count >= 2u) {
            D.prog.joined[1].asleep = false;
            D.prog.joined[1].step = UI_ZB_STEP_DONE;
            D.prog.joined[1].result = UI_ZB_RES_ADDED;
        }
        break;
    }
    case HOST_ZB_JOIN_KNOWN: {
        ensure_listening(90);
        D.prog.joined_count = 0;
        const demo_dev_t *front = NULL;
        for (size_t i = 0; i < DEMO_DEVS && front == NULL; i++) {
            if (D.dev[i].used && D.dev[i].pg.dev.zone_count > 0u && D.dev[i].pg.dev.zones[0] == 1u) {
                front = &D.dev[i];
            }
        }
        ui_zb_joined_t *j = join_row(front, UI_ZB_STEP_DONE);
        if (j != NULL) {
            j->result = UI_ZB_RES_ALREADY_ADDED;
        }
        break;
    }
    case HOST_ZB_JOIN_PLUG: {
        ensure_listening(65);
        D.prog.joined_count = 0;
        ui_zb_joined_t *j = join_row(joining_dev(2), UI_ZB_STEP_DONE);
        if (j != NULL) {
            j->result = UI_ZB_RES_NOT_A_SENSOR;
        }
        break;
    }
    case HOST_ZB_JOIN_FAILS:
        ensure_listening(30);
        D.prog.joined_count = 0;
        end_op(UI_ZB_RES_JOIN_FAILED);
        break;
    case HOST_ZB_WINDOW_ENDS:
        if (adding() && D.prog.step != UI_ZB_STEP_DONE) {
            window_ends(UI_ZB_RES_NOTHING_JOINED);
        }
        break;
    case HOST_ZB_JOIN_ONE: {
        ensure_listening(150);
        if (D.prog.joined_count == 0u) {
            ui_zb_joined_t *a = join_row(joining_dev(0), UI_ZB_STEP_DONE);
            if (a != NULL) {
                a->result = UI_ZB_RES_ADDED;
                sync_dev(a);
            }
        }
        break;
    }
    case HOST_ZB_SIREN_SILENT: D.siren_silent = true; break;
    case HOST_ZB_KEYPAD_NO_SLOT:
        D.slots_taken = 0xFFu;
        for (size_t i = 0; i < DEMO_DEVS; i++) {
            if (D.dev[i].used && D.dev[i].pg.dev.kind == UI_ZB_KIND_KEYPAD) {
                const uint8_t fit = D.dev[i].pg.keypad.fit;
                memset(&D.dev[i].pg.keypad, 0, sizeof(D.dev[i].pg.keypad));
                D.dev[i].pg.keypad.fit = fit;
            }
        }
        break;
    case HOST_ZB_KEYPAD_RACE:
        D.slots_taken = 0x7Fu; /* 8 is free for now */
        D.slots_race = true;
        for (size_t i = 0; i < DEMO_DEVS; i++) {
            if (D.dev[i].used && D.dev[i].pg.dev.kind == UI_ZB_KIND_KEYPAD) {
                const uint8_t fit = D.dev[i].pg.keypad.fit;
                memset(&D.dev[i].pg.keypad, 0, sizeof(D.dev[i].pg.keypad));
                D.dev[i].pg.keypad.fit = fit;
            }
        }
        break;
    case HOST_ZB_FOB_TAKEN: D.fob_held_by = 2u; break; /* the master holds another fob, not Alex's */
    case HOST_ZB_ROLE_INVALID: D.refuse_next = UI_ZB_REQ_INVALID; break;
    case HOST_ZB_SIREN_ARMED: D.refuse_next = UI_ZB_REQ_NOT_ALLOWED; break;
    case HOST_ZB_REMOVE_ANSWERS:
    case HOST_ZB_REMOVE_NO_ANSWER:
        if (D.prog.op == UI_ZB_OP_REMOVE && D.prog.step == UI_ZB_STEP_WORKING) {
            D.op_end = 0;
            D.answer_at = 0;
            if (what == HOST_ZB_REMOVE_ANSWERS) {
                remove_dev(D.prog.id);
                end_op(UI_ZB_RES_REMOVED);
            } else {
                end_op(UI_ZB_RES_NO_ANSWER);
            }
        }
        break;
    default: break;
    }
}

void host_zigbee_demo_hooks(const host_zb_hooks_t *hooks)
{
    memset(&D.hooks, 0, sizeof(D.hooks));
    if (hooks != NULL) {
        D.hooks = *hooks;
    }
    seed_codes(); /* their times, now that the clock is known */
}

void host_zigbee_demo_code_once(host_zb_code_t answer)
{
    D.code_once = answer;
    D.code_once_set = true;
}

void host_zigbee_demo_publish(host_zb_publish_t which)
{
    const demo_dev_t *newest = NULL;
    for (size_t i = 0; i < DEMO_DEVS; i++) {
        const demo_dev_t *d = &D.dev[i];
        if (d->used && d->pg.dev.kind == UI_ZB_KIND_SENSOR && d->pg.dev.state == UI_ZB_DEV_READY &&
            (newest == NULL || d->pg.dev.id > newest->pg.dev.id)) {
            newest = d;
        }
    }
    for (size_t i = 0; i < DEMO_DEVS; i++) {
        const demo_dev_t *d = &D.dev[i];
        if (!d->used || d->pg.dev.kind != UI_ZB_KIND_SENSOR || d->pg.dev.state != UI_ZB_DEV_READY ||
            (which == HOST_ZB_PUBLISH_NEWEST && d != newest)) {
            continue;
        }
        for (size_t k = 0; k < d->pg.input_count; k++) {
            const ui_zb_input_t *in = &d->pg.inputs[k];
            const bool ours = in->zone == 1u || in->zone == 5u;
            if (which == HOST_ZB_PUBLISH_ZONES && !ours) {
                continue;
            }
            if (D.hooks.join != NULL) {
                D.hooks.join(d->pg.ieee, in->endpoint, in->device_class, d->pg.manufacturer,
                             d->pg.dev.model, d->pg.dev.router);
            }
            if (which == HOST_ZB_PUBLISH_ZONES && D.hooks.bind != NULL) {
                D.hooks.bind(in->zone, in->addr, in->device_class);
            }
        }
    }
}
