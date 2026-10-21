/*
 * ui_sources.c - zone sources in the zone editor (docs/features/22): "Where is the sensor?" when a
 * zone is added or its sensor changes, the live picker of a source's sensors (the wired
 * terminals, while the source learns), and the zone page's source rows: source, contact and
 * response time for wired contacts, the zone's state right now and what the source can promise,
 * in words (ZSRC-009). The platform is reached through the zone_* entries of ui_backend_t;
 * without them the zone editor keeps its old rows.
 *
 * Only this picker and a zone's Source row may name an integration (docs/features/07 UI-018).
 */
#include <stdio.h>
#include <string.h>

#include "i18n/i18n_date.h"
#include "ui_internal.h"
#include "zone_hub/zone_eol.h"
#include "zone_hub/zsrc_rf.h"
#include "zone_hub/zsrc_wired.h"
#if ALARM_UI_ZWAVE
#include "zone_hub/zsrc_zwave.h"
#endif

#define POLL_MS        1000u /* live words change at most once a second (UI-020) */
#define MAX_CANDIDATES 16u

static uint16_t s_zone; /* zone getting a sensor */
static bool s_new;      /* ... which is created once a sensor is picked */
static uint8_t s_role;  /* ... for this input: ZSRC_ROLE_MAIN, _TAMPER or _BATTERY */
static ui_zone_source_t s_srcs[ZONE_HUB_MAX_SOURCES];
static size_t s_nsrcs;
static zone_hub_info_t s_info; /* big: kept off the stack */

/* the picker page */
static uint8_t s_kind;
static char s_query[48]; /* the picker's search words (sources with ZSRC_CAP_SEARCH) */
static bool s_learning;
static bool s_best_heard; /* the RF source named the sensor under test: beeped once (RF-011) */
static lv_obj_t *s_list;
static size_t s_rows_mark;
static zsrc_candidate_t *s_cands; /* shown */
static zsrc_candidate_t *s_fresh; /* just read */
static size_t s_ncands;
static uint32_t s_polled;
/* ... a 433 MHz zone's tamper or low-battery code: its own sensor's codes, one each (RF-021) */
static bool s_codes;
static zone_hub_binding_t s_codes_of; /* the zone's binding when the page opened */

/* the zone page's live state */
static lv_obj_t *s_state_row;
static uint16_t s_state_zone;

bool ui_sources_available(void)
{
    return S.be.zone_sources != NULL && S.be.zone_bind != NULL && S.be.zone_info != NULL;
}

/* ---- words ------------------------------------------------------------------------------------ */

/* The kind of sensor as a choice's title or a page's note: the text comes back translated. */
static const char *kind_name(uint8_t kind)
{
    switch (kind) {
    case ZSRC_SIM: return TR("Simulator");
    case ZSRC_WIRED: return TR("Wired contact");
    case ZSRC_ZIGBEE: return TR("Zigbee sensor");
    case ZSRC_RF: return TR("433 MHz sensor");
    case ZSRC_HA: return TR("Home Assistant sensor");
    case ZSRC_KEYPAD: return TR("Keypad input");
#if ALARM_UI_ZWAVE
    case ZSRC_ZWAVE: return TR("Z-Wave sensor"); /* docs/features/36 screens.md §5 */
#endif
    default: return TR("Other sensor");
    }
}

/* The kind of sensor in a row's value, in a few words: translated too. The names of the
 * protocols and of the integration (Zigbee, 433 MHz, Z-Wave, Home Assistant) are names, not words. */
static const char *kind_short(uint8_t kind)
{
    switch (kind) {
    case ZSRC_SIM: return TR("Simulator");
    case ZSRC_WIRED: return TR("Wired");
    case ZSRC_ZIGBEE: return NOTR("Zigbee"); /* a protocol name */
    case ZSRC_RF: return NOTR("433 MHz");    /* a radio band */
    case ZSRC_HA: return NOTR("Home Assistant"); /* a product name */
    case ZSRC_KEYPAD: return TR("Keypad input");
#if ALARM_UI_ZWAVE
    case ZSRC_ZWAVE: return NOTR("Z-Wave"); /* a protocol name */
#endif
    default: return TR_C("kind of sensor", "Other");
    }
}

#if ALARM_UI_ZWAVE
/* What a Z-Wave input is, word for word as zsrc_zwave.c's slot_words(), inside a line and as a
 * title. */
static const struct {
    uint8_t slot;
    const char *inline_words;
    const char *title;
} ZW_INPUT_WORDS[] = {
    /* TR: what a Z-Wave sensor's input is, inside a line ("door contact · In use: Kitchen") and as
     * a title ("Door contact"). At most 30 bytes */
    {ZSRC_ZW_SLOT_CONTACT, TR_NOOP("door contact"), TR_NOOP("Door contact")},
    {ZSRC_ZW_SLOT_INTRUSION, TR_NOOP("intrusion"), TR_NOOP("Intrusion")},
    {ZSRC_ZW_SLOT_MOTION, TR_NOOP("motion"), TR_NOOP("Motion")},
    {ZSRC_ZW_SLOT_GLASS_BREAK, TR_NOOP("glass break"), TR_NOOP("Glass break")},
    {ZSRC_ZW_SLOT_TILT, TR_NOOP("tilt"), TR_NOOP("Tilt")},
    {ZSRC_ZW_SLOT_IMPACT, TR_NOOP("impact"), TR_NOOP("Impact")},
    {ZSRC_ZW_SLOT_WATER, TR_NOOP("water"), TR_NOOP("Water")},
    {ZSRC_ZW_SLOT_SMOKE, TR_NOOP("smoke"), TR_NOOP("Smoke")},
    {ZSRC_ZW_SLOT_CO, TR_NOOP("carbon monoxide"), TR_NOOP("Carbon monoxide")},
    {ZSRC_ZW_SLOT_HEAT, TR_NOOP("heat"), TR_NOOP("Heat")},
    {ZSRC_ZW_SLOT_FREEZE, TR_NOOP("freeze"), TR_NOOP("Freeze")},
    {ZSRC_ZW_SLOT_GAS, TR_NOOP("gas"), TR_NOOP("Gas")},
    {ZSRC_ZW_SLOT_EMERGENCY, TR_NOOP("emergency button"), TR_NOOP("Emergency button")},
    {ZSRC_ZW_SLOT_TEMPERATURE, TR_NOOP("temperature"), TR_NOOP("Temperature")},
    {ZSRC_ZW_SLOT_MAINS, TR_NOOP("mains power"), TR_NOOP("Mains power")},
    {ZSRC_ZW_SLOT_GENERAL, TR_NOOP("general input"), TR_NOOP("General input")},
};

static const char *zw_input_what(uint8_t slot, uint8_t device_class, bool title)
{
    if (slot == ZSRC_ZW_SLOT_CONTACT && device_class == ALARM_DEVCLASS_WINDOW) {
        /* TR: a Z-Wave window contact, inside a line and as a title. At most 30 bytes */
        return title ? TR("Window contact") : TR("window contact");
    }
    for (size_t i = 0; i < sizeof(ZW_INPUT_WORDS) / sizeof(ZW_INPUT_WORDS[0]); i++) {
        if (ZW_INPUT_WORDS[i].slot == slot) {
            return title ? TR(ZW_INPUT_WORDS[i].title) : TR(ZW_INPUT_WORDS[i].inline_words);
        }
    }
    /* TR: a Z-Wave input the panel can't name, inside a line and as a title. At most 30 bytes */
    return title ? TR("Input") : TR("input");
}

void ui_zw_input_words(uint8_t endpoint, uint8_t slot, uint8_t device_class, bool title, char *out,
                       size_t len)
{
    const char *what = zw_input_what(slot, device_class, title && endpoint == 0u);
    if (endpoint == 0u) {
        i18n_utf8_copy(out, len, what);
    } else if (title) {
        /* TR: one input of a Z-Wave device that has several, as a title; {0} its number, {1}
         * what it is ("water"). At most 40 bytes */
        I18N_FORMAT(out, len, TR("Input {0} · {1}"), (unsigned)endpoint, what);
    } else {
        /* TR: one input of a Z-Wave device that has several, inside a line; {0} its number, {1}
         * what it is ("water"). At most 40 bytes */
        I18N_FORMAT(out, len, TR("input {0} · {1}"), (unsigned)endpoint, what);
    }
}
#endif

/* "<kind> · <fact>", both already translated. */
static void kind_and(char *out, size_t len, uint8_t kind, const char *fact)
{
    snprintf(out, len, "%s · %s", kind_short(kind), fact);
}

static const char *kind_detail(uint8_t kind)
{
    switch (kind) {
    case ZSRC_SIM: return TR("No sensor: the service console opens and closes it, for testing");
    case ZSRC_WIRED: return TR("A contact wired to the panel's terminals");
    case ZSRC_ZIGBEE: return TR("Paired straight to the panel's own radio");
    case ZSRC_HA: return TR("Any sensor Home Assistant knows");
    case ZSRC_RF: return TR("Learned by opening and closing it");
    case ZSRC_KEYPAD: return TR("A contact wired to a keypad");
#if ALARM_UI_ZWAVE
    case ZSRC_ZWAVE: return TR("Paired to the panel's Z-Wave radio");
#endif
    default: return NULL;
    }
}

static const char *kind_glyph(uint8_t kind)
{
    switch (kind) {
    case ZSRC_SIM: return UI_ICON_WRENCH;
    case ZSRC_WIRED: return UI_ICON_PLUG;
    case ZSRC_HA: return UI_ICON_HOUSE_WIFI;
    case ZSRC_KEYPAD: return UI_ICON_APP_WINDOW;
    default: return UI_ICON_RADIO_TOWER;
    }
}

/* The picker's order for real sensors (screens.md §5: Z-Wave "after Home Assistant and before
 * Zigbee"), independent of whatever order the backend happens to register sources in. The
 * simulator is always last (ZSRC_CAP_TEST, ZSRC-005) and never reaches this table. */
static uint8_t kind_rank(uint8_t kind)
{
    switch (kind) {
    case ZSRC_WIRED: return 0;
    case ZSRC_HA: return 1;
#if ALARM_UI_ZWAVE
    case ZSRC_ZWAVE: return 2;
#endif
    case ZSRC_ZIGBEE: return 3;
    case ZSRC_RF: return 4;
    case ZSRC_KEYPAD: return 5;
    default: return 6;
    }
}

/* "Open" / "Closed" from a source's "open" / "closed"; other states as they are. */
/* The words sources report a state in (zsrc_zwave.c's zsrc_zw_state_words), so the screen can
 * say them in its language. */
static const char *const SENSOR_STATES[] = {
    /* TR: what a sensor reports right now, in the sensor picker's value column and after "Right
     * now:". At most 23 bytes */
    TR_NOOP_C("sensor state", "Open"), TR_NOOP_C("sensor state", "Closed"),
    TR_NOOP_C("sensor state", "Intrusion"), TR_NOOP_C("sensor state", "Motion"),
    TR_NOOP_C("sensor state", "Glass break"), TR_NOOP_C("sensor state", "Impact"),
    TR_NOOP_C("sensor state", "Wet"), TR_NOOP_C("sensor state", "Dry"),
    TR_NOOP_C("sensor state", "Smoke"), TR_NOOP_C("sensor state", "CO"),
    TR_NOOP_C("sensor state", "Heat"), TR_NOOP_C("sensor state", "Freezing"),
    TR_NOOP_C("sensor state", "Gas"), TR_NOOP_C("sensor state", "Pressed"),
    TR_NOOP_C("sensor state", "Idle"), TR_NOOP_C("sensor state", "Clear"),
    TR_NOOP_C("sensor state", "Mains lost"), TR_NOOP_C("sensor state", "Mains on"),
    TR_NOOP_C("sensor state", "On"), TR_NOOP_C("sensor state", "Off"),
};

const char *ui_sensor_state_words(const char *state)
{
    for (size_t i = 0; i < sizeof(SENSOR_STATES) / sizeof(SENSOR_STATES[0]); i++) {
        if (strcmp(state, SENSOR_STATES[i]) == 0) {
            return TR_C("sensor state", SENSOR_STATES[i]);
        }
    }
    return state;
}

static void state_words(const char *state, char *out, size_t len)
{
    if (strcmp(state, "open") == 0) {
        /* TR: the state of a sensor, in the value column of the sensor picker. At most 23 bytes */
        i18n_utf8_copy(out, len, TR_C("zone state", "Open"));
    } else if (strcmp(state, "closed") == 0) {
        /* TR: the state of a sensor, in the value column of the sensor picker. At most 23 bytes */
        i18n_utf8_copy(out, len, TR_C("zone state", "Closed"));
    } else {
        const char *words = ui_sensor_state_words(state);
        i18n_utf8_copy(out, len, words);
        /* A source's own word ("off", "clear") with a capital first, like every other value
         * (BL-131, rule 4): only the copy shown, and only an ASCII first letter. */
        if (words == state && out[0] >= 'a' && out[0] <= 'z') {
            out[0] = (char)(out[0] - 'a' + 'A');
        }
    }
}

/* A candidate's details in the screen's language: a Z-Wave input's words are made here from its
 * address (the source says them in English); other sources' details - places, names - are kept. */
static const char *candidate_detail(const zsrc_candidate_t *c, char *buf, size_t len)
{
#if ALARM_UI_ZWAVE
    uint32_t home;
    uint16_t generation, node;
    uint8_t endpoint, slot;
    if (c->addr.kind == ZSRC_ZWAVE &&
        zsrc_zw_address_parse(&c->addr, &home, &generation, &node, &endpoint, &slot)) {
        ui_zw_input_words(endpoint, slot, c->device_class, false, buf, len);
        return buf;
    }
#endif
#if ALARM_UI_ZIGBEE
    /* a Zigbee input says what it senses (BL-144 page 11: "Contact"); its title has the model */
    if (c->addr.kind == ZSRC_ZIGBEE && c->device_class != (uint8_t)ALARM_DEVCLASS_GENERIC) {
        i18n_utf8_copy(buf, len, ui_device_class_name((alarm_device_class_t)c->device_class));
        return buf;
    }
#endif
    (void)buf;
    (void)len;
    return c->detail;
}

static const char *bind_error(zone_hub_result_t r)
{
    switch (r) {
    case ZONE_HUB_ERR_IN_USE: return TR("That sensor already belongs to another zone");
    case ZONE_HUB_ERR_BUSY: return TR("Disarm first");
    case ZONE_HUB_ERR_FULL: return TR("No room for more sensors");
    case ZONE_HUB_ERR_REFUSED:
    case ZONE_HUB_ERR_NO_SOURCE: return TR("That sensor isn't available");
    default: return TR("Not saved");
    }
}

const char *ui_sources_short(uint16_t zone, char *buf, size_t len)
{
    if (!ui_sources_available()) {
        return NULL;
    }
    if (!S.be.zone_info(S.be.ctx, zone, &s_info)) {
        return TR("No sensor");
    }
    if (s_info.lost) {
        return TR("Sensor lost");
    }
    if (!s_info.source_present) {
        return TR("Sensor missing");
    }
    const uint8_t kind = s_info.binding.in[ZSRC_ROLE_MAIN].addr.kind;
    if (s_info.check) {
        /* TR: the installer's zone list, after the kind of sensor and " · ". At most 30 bytes */
        kind_and(buf, len, kind, TR("not reachable"));
    } else {
        i18n_utf8_copy(buf, len, kind_short(kind));
    }
    return buf;
}

/* ---- binding ---------------------------------------------------------------------------------- */

/* A candidate's device class, defaulting to door: the simulator and wired terminals always report
 * GENERIC (no way to guess a class from a raw input), which keeps today's behaviour for them. */
static alarm_device_class_t candidate_class(const zsrc_candidate_t *c)
{
    return c->device_class != ALARM_DEVCLASS_GENERIC ? (alarm_device_class_t)c->device_class
                                                     : ALARM_DEVCLASS_DOOR;
}

/* cls: the candidate's device class (ALARM_DEVCLASS_DOOR for the simulator and wired terminals,
 * which offer none) - a new zone's type and class come from it, not always Perimeter/door
 * (docs/features/22 "Defaults from the device class"); ignored unless is_new and the main role. */
static bool bind(uint16_t zone, bool is_new, const zsrc_address_t *a, alarm_device_class_t cls)
{
    zone_hub_binding_t b;
    memset(&b, 0, sizeof(b));
    if (s_role != ZSRC_ROLE_MAIN) { /* a tamper or low-battery input beside the main sensor */
        if (!S.be.zone_info(S.be.ctx, zone, &s_info) || s_info.lost) {
            ui_toast(TR("Pick the zone's own sensor first"));
            return false;
        }
        b = s_info.binding;
        memset(&b.in[s_role], 0, sizeof(b.in[s_role]));
        b.in[s_role].addr = *a;
        zsrc_mapping_defaults(&b.in[s_role].map, ALARM_DEVCLASS_GENERIC);
        if (s_role == ZSRC_ROLE_BATTERY && a->kind == ZSRC_RF) {
            b.in[s_role].map.hold_s = ZSRC_RF_BATTERY_HOLD_S; /* not over at the next closed code */
        }
        const zone_hub_result_t r = S.be.zone_bind(S.be.ctx, zone, &b);
        if (r != ZONE_HUB_OK) {
            ui_beep(UI_BEEP_ERROR);
            ui_toast(bind_error(r));
            return false;
        }
        return true;
    }
    b.in[ZSRC_ROLE_MAIN].addr = *a;
    const alarm_device_class_t map_cls =
        is_new ? cls : (alarm_device_class_t)S.edit->zones[zone - 1u].device_class;
    zsrc_mapping_defaults(&b.in[ZSRC_ROLE_MAIN].map, map_cls);
    if (!is_new && S.be.zone_info(S.be.ctx, zone, &s_info) && !s_info.lost) {
        if (s_info.binding.in[ZSRC_ROLE_MAIN].addr.kind == a->kind) {
            b.in[ZSRC_ROLE_MAIN].map = s_info.binding.in[ZSRC_ROLE_MAIN].map; /* keep its options */
        }
        b.in[ZSRC_ROLE_TAMPER] = s_info.binding.in[ZSRC_ROLE_TAMPER]; /* and its other inputs */
        b.in[ZSRC_ROLE_BATTERY] = s_info.binding.in[ZSRC_ROLE_BATTERY];
    }
    const zone_hub_result_t r = S.be.zone_bind(S.be.ctx, zone, &b);
    if (r != ZONE_HUB_OK) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(bind_error(r));
        return false;
    }
    if (is_new) {
        ui_installer_create_zone_for(zone, cls);
    }
    return true;
}

/* A new mapping for the zone's sensor (contact, response time). */
static void rebind_with(void (*change)(zsrc_mapping_t *m, int32_t v), int32_t v)
{
    zone_hub_binding_t b;
    if (!S.be.zone_info(S.be.ctx, s_zone, &s_info)) {
        return;
    }
    b = s_info.binding;
    change(&b.in[ZSRC_ROLE_MAIN].map, v);
    const zone_hub_result_t r = S.be.zone_bind(S.be.ctx, s_zone, &b);
    if (r != ZONE_HUB_OK) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(bind_error(r));
    }
    ui_page_rebuild();
}

/* ---- "Where is the sensor?" ------------------------------------------------------------------- */

static int32_t picker_arg(uint16_t zone, uint8_t kind, bool is_new)
{
    return (int32_t)((uint32_t)zone | ((uint32_t)kind << 16) | (is_new ? 1u << 24 : 0u) |
                     ((uint32_t)s_role << 25));
}

static void source_chosen(int32_t index)
{
    if (index < 0 || (size_t)index >= s_nsrcs) {
        return;
    }
    const uint8_t kind = s_srcs[index].kind;
    if (kind != ZSRC_SIM) {
        ui_page_open(PAGE_ZONE_SOURCE, picker_arg(s_zone, kind, s_new));
        return;
    }
    const zsrc_address_t a = zsrc_address(ZSRC_SIM, s_role, s_zone);
    const bool is_new = s_new;
    if (!bind(s_zone, is_new, &a, ALARM_DEVCLASS_DOOR)) { /* the simulator offers no class */
        return;
    }
    if (is_new) {
        ui_page_open(PAGE_ZONE, s_zone);
    } else {
        ui_page_rebuild();
    }
}

void ui_sources_pick(uint16_t zone, bool is_new)
{
    if (!ui_sources_available()) {
        return;
    }
    s_role = ZSRC_ROLE_MAIN;
    s_zone = zone;
    s_new = is_new;
    s_query[0] = '\0'; /* a fresh pick starts unfiltered */
    ui_zone_source_t all[ZONE_HUB_MAX_SOURCES];
    const size_t n = S.be.zone_sources(S.be.ctx, all, ZONE_HUB_MAX_SOURCES);
    /* Only what can be used now (ZSRC-005); real sensors first, the simulator last. */
    s_nsrcs = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < n; i++) {
            const bool test = (all[i].caps & ZSRC_CAP_TEST) != 0u;
            if (all[i].usable && test == (pass == 1)) {
                s_srcs[s_nsrcs++] = all[i];
            }
        }
    }
    /* screens.md §5: Z-Wave sits after Home Assistant and before Zigbee, whatever order the
     * backend happens to register sources in; the simulator (pass 1, above) stays last. */
    for (size_t i = 1; i < s_nsrcs && (s_srcs[i].caps & ZSRC_CAP_TEST) == 0u; i++) {
        const ui_zone_source_t v = s_srcs[i];
        size_t j = i;
        for (; j > 0u && kind_rank(s_srcs[j - 1u].kind) > kind_rank(v.kind); j--) {
            s_srcs[j] = s_srcs[j - 1u];
        }
        s_srcs[j] = v;
    }
    if (s_nsrcs == 0u) {
        ui_toast(TR("No kind of sensor can be added now"));
        return;
    }
    uint8_t current = ZSRC_NONE;
    if (!is_new && S.be.zone_info(S.be.ctx, zone, &s_info)) {
        current = s_info.binding.in[ZSRC_ROLE_MAIN].addr.kind;
    }
    static ui_choice_t choices[ZONE_HUB_MAX_SOURCES];
    int32_t selected = -1;
    for (size_t i = 0; i < s_nsrcs; i++) {
        const bool test = (s_srcs[i].caps & ZSRC_CAP_TEST) != 0u;
        choices[i].glyph = kind_glyph(s_srcs[i].kind);
        choices[i].tone = test ? ui_tone_gray() : ui_tone_blue();
        choices[i].title = kind_name(s_srcs[i].kind);
        choices[i].detail = kind_detail(s_srcs[i].kind);
        if (s_srcs[i].kind == current) {
            selected = (int32_t)i;
        }
    }
    ui_pick_sheet(TR("Where is the sensor?"),
                  is_new ? TR("How the new zone's sensor is connected")
                         : TR("How this zone's sensor is connected"),
                  choices, s_nsrcs, selected, source_chosen);
}

/* ---- the picker page: a source's sensors, live ----------------------------------------------- */

static void on_candidate(void *ctx)
{
    const size_t i = (size_t)(uintptr_t)ctx;
    if (i >= s_ncands) {
        return;
    }
    const zsrc_address_t a = s_cands[i].addr;
    const alarm_device_class_t cls = candidate_class(&s_cands[i]);
    const uint16_t zone = s_zone;
    const bool is_new = s_new;
    if (!bind(zone, is_new, &a, cls)) {
        return;
    }
    ui_page_back(); /* saves a new zone; learning stops with the page */
    if (is_new) {
        ui_page_open(PAGE_ZONE, zone);
    }
}

/* The catalog search of SEARCH sources (Home Assistant): filter by name, room or id. */
static void rebuild_soon(void *unused)
{
    (void)unused;
    ui_page_rebuild();
}

static void query_changed(const char *text)
{
    snprintf(s_query, sizeof(s_query), "%s", text);
    lv_async_call(rebuild_soon, NULL); /* the list and the search row both change */
}

static void edit_query(void *ctx)
{
    (void)ctx;
    ui_text_sheet_kind(TR("Find the sensor"), TR("Name, room or entity id. Empty shows everything"),
                       s_query, UI_TEXT_PLAIN, (int)sizeof(s_query) - 1, query_changed);
}

/* Whether this kind searches a catalog (ZSRC_CAP_SEARCH) rather than listening. */
static bool kind_searches(uint8_t kind)
{
    ui_zone_source_t all[ZONE_HUB_MAX_SOURCES];
    const size_t n = S.be.zone_sources != NULL
                         ? S.be.zone_sources(S.be.ctx, all, ZONE_HUB_MAX_SOURCES)
                         : 0u;
    for (size_t i = 0; i < n; i++) {
        if (all[i].kind == kind) {
            return (all[i].caps & ZSRC_CAP_SEARCH) != 0u;
        }
    }
    return false;
}

static bool read_candidates(zsrc_candidate_t *out, size_t *n)
{
    *n = S.be.zone_candidates != NULL
             ? S.be.zone_candidates(S.be.ctx, s_kind, s_query, out, MAX_CANDIDATES)
             : 0u;
    return *n <= MAX_CANDIDATES;
}

/* 433 MHz: what the panel has heard so far, in words, and the sensor to use once it is sure
 * (RF-011) - one tap instead of hunting through every code on the air. */
static void rf_status(void)
{
    size_t best = MAX_CANDIDATES;
    size_t heard = 0;
    size_t first = MAX_CANDIDATES;
    for (size_t i = 0; i < s_ncands; i++) {
        if (s_cands[i].best) {
            best = i;
        }
        if (s_cands[i].changed) {
            heard++;
            if (first == MAX_CANDIDATES) {
                first = i;
            }
        }
    }
    char text[128];
    if (best < s_ncands) {
        const zsrc_candidate_t *c = &s_cands[best];
        const bool mine = !s_new && c->zone == s_zone;
        if (c->zone != 0u && !mine) {
            char name[ALARM_NAME_LEN + 8];
            /* TR: the 433 MHz picker; {0} is the sensor's code name, {1} the zone that uses it */
            I18N_FORMAT(text, sizeof(text), TR("{0} answered, but it already feeds {1}."), c->name,
                        ui_zone_name(S.edit, c->zone, name, sizeof(name)));
            ui_paragraph(s_list, text, ui_tone_amber());
            return;
        }
        /* TR: the 433 MHz picker; {0} is the sensor's code name */
        I18N_FORMAT(text, sizeof(text), TR("Found it: {0} answered each time."), c->name);
        ui_paragraph(s_list, text, ui_tone_green());
        /* TR: the 433 MHz picker, a button; {0} is the sensor's code name */
        I18N_FORMAT(text, sizeof(text), TR("Use {0}"), c->name);
        ui_row_action(s_list, UI_ICON_CIRCLE_CHECK, text, ui_tone_green(), on_candidate,
                      (void *)(uintptr_t)best);
        if (!s_best_heard) {
            s_best_heard = true;
            ui_beep(UI_BEEP_TEST); /* heard across the room, as a panel beeps when it enrolls */
        }
        return;
    }
    if (heard == 0u) {
        i18n_utf8_copy(text, sizeof(text), TR("Listening: nothing heard yet."));
    } else if (heard == 1u) {
        /* TR: the 433 MHz picker; {0} is the sensor's code name */
        I18N_FORMAT(text, sizeof(text),
                    TR("Heard {0}. Close the door (or press the button again) to be sure."),
                    s_cands[first].name);
    } else {
        /* TR: the 433 MHz picker; {0} is how many sensors (two or more) */
        I18N_FORMAT(text, sizeof(text),
                    TRN("Heard {0} sensors. Open and close the door once more: the one that answers "
                        "is picked.",
                        "Heard {0} sensors. Open and close the door once more: the one that answers "
                        "is picked.",
                        (uint32_t)heard),
                    (unsigned)heard);
    }
    ui_paragraph(s_list, text, ui_tone_gray());
}

/* ---- a 433 MHz zone's tamper or low-battery code (RF-021) --------------------------------------- */

/* Which input of the zone a code is bound to, as a whole sentence. */
static const char *role_code_line(uint8_t role)
{
    return role == ZSRC_ROLE_TAMPER    ? TR("The zone's tamper code")
           : role == ZSRC_ROLE_BATTERY ? TR("The zone's low-battery code")
                                       : TR("The zone's own code");
}

/* The role of this zone a code is bound to, or ZSRC_ROLE_COUNT. */
static uint8_t code_role(const zsrc_address_t *a)
{
    for (uint8_t r = 0; r < ZSRC_ROLE_COUNT; r++) {
        if (s_codes_of.in[r].addr.kind != ZSRC_NONE && zsrc_address_equal(&s_codes_of.in[r].addr, a)) {
            return r;
        }
    }
    return ZSRC_ROLE_COUNT;
}

static void code_picked(const zsrc_address_t *a)
{
    /* always an existing zone's tamper or low-battery code (s_role != ZSRC_ROLE_MAIN): the class
     * is not used */
    if (bind(s_zone, false, a, ALARM_DEVCLASS_GENERIC)) {
        ui_page_back(); /* learning stops with the page */
    }
}

static uint8_t s_hand_codes[ZSRC_RF_CODES];

static void hand_code_chosen(int32_t index)
{
    uint8_t proto;
    uint32_t device;
    if (index < 0 || index >= (int32_t)ZSRC_RF_CODES ||
        !zsrc_rf_address_parts(&s_codes_of.in[ZSRC_ROLE_MAIN].addr, &proto, &device, NULL)) {
        return;
    }
    const zsrc_address_t a = zsrc_rf_address(proto, device, s_hand_codes[index]);
    code_picked(&a);
}

/* Every code the sensor's protocol has, less the ones this zone already uses for something
 * else: a low-battery code can't be made to happen on the spot, and a manual may name it. */
static void pick_code_by_hand(void *ctx)
{
    (void)ctx;
    uint8_t proto;
    uint32_t device;
    if (!zsrc_rf_address_parts(&s_codes_of.in[ZSRC_ROLE_MAIN].addr, &proto, &device, NULL)) {
        return;
    }
    static ui_choice_t choices[ZSRC_RF_CODES];
    static char titles[ZSRC_RF_CODES][24];
    size_t n = 0;
    int32_t selected = -1;
    for (uint8_t code = 0; code < zsrc_rf_codes(proto) && n < ZSRC_RF_CODES; code++) {
        const zsrc_address_t a = zsrc_rf_address(proto, device, code);
        const uint8_t role = code_role(&a);
        if (role != ZSRC_ROLE_COUNT && role != s_role) {
            continue; /* the zone's own code, or its other input's */
        }
        bool heard = false;
        bool closer = false;
        for (size_t i = 0; i < s_ncands; i++) {
            if (zsrc_address_equal(&s_cands[i].addr, &a)) {
                heard = true;
                closer = s_cands[i].unusable; /* its closed code (RF-021) */
            }
        }
        if (closer) {
            continue;
        }
        if (role == s_role) {
            selected = (int32_t)n;
        }
        char hex[8];
        snprintf(hex, sizeof(hex), "%X", (unsigned)code);
        /* TR: a code of a 433 MHz sensor in the by-hand list; {0} is a hexadecimal code.
         * At most 21 bytes */
        I18N_FORMAT(titles[n], sizeof(titles[n]), TR("Code {0}"), hex);
        choices[n].glyph = kind_glyph(ZSRC_RF);
        choices[n].tone = heard ? ui_tone_amber() : ui_tone_blue();
        choices[n].title = titles[n];
        choices[n].detail = heard ? TR("Sent while this page was open") : NULL;
        s_hand_codes[n] = code;
        n++;
    }
    ui_pick_sheet(s_role == ZSRC_ROLE_TAMPER ? TR("Tamper code") : TR("Low-battery code"),
                  TR("The code its manual or seller gives"), choices, n, selected, hand_code_chosen);
}

static void on_code(void *ctx)
{
    const size_t i = (size_t)(uintptr_t)ctx;
    if (i < s_ncands) {
        code_picked(&s_cands[i].addr);
    }
}

/* Whether this code can be the input being picked: not the zone's own code or its other
 * input's (bind() would refuse), not another zone's, not its closed code (the source knows). */
static bool code_pickable(const zsrc_candidate_t *c)
{
    const uint8_t role = code_role(&c->addr);
    const bool other = role != ZSRC_ROLE_COUNT && role != s_role;
    const bool taken = c->zone != 0u && c->zone != s_zone;
    return !other && !taken && !c->unusable;
}

static void fill_codes(void)
{
    const bool tamper = s_role == ZSRC_ROLE_TAMPER;
    size_t pickable = 0;
    for (size_t i = 0; i < s_ncands; i++) {
        pickable += code_pickable(&s_cands[i]) ? 1u : 0u;
    }
    if (s_ncands == 0u) {
        ui_paragraph(s_list, TR("Listening: nothing heard from this sensor yet."), ui_tone_gray());
    } else if (pickable == 0u) {
        /* A contact whose cover sends its open or closed code can't report a tamper apart from
         * the door (B012: one sends its closed code for both). */
        ui_paragraph(s_list,
                     tamper ? TR("Only its door codes so far. If taking the cover off sends one of "
                                 "them, this sensor can't report a tamper apart from the door: leave "
                                 "Tamper input as None.")
                            : TR("Only its door codes so far."),
                     ui_tone_amber());
    }
    for (size_t i = 0; i < s_ncands; i++) {
        const zsrc_candidate_t *c = &s_cands[i];
        const uint8_t role = code_role(&c->addr);
        const bool taken = c->zone != 0u && c->zone != s_zone;
        char detail[96];
        char name[ALARM_NAME_LEN + 8];
        if (taken) {
            char fact[ALARM_NAME_LEN + 40];
            /* TR: a 433 MHz code in the tamper or low-battery list, after its details and " · ";
             * {0} is the zone that uses the code. At most 40 bytes */
            I18N_FORMAT(fact, sizeof(fact), TR("In use: {0}"),
                        ui_zone_name(S.edit, c->zone, name, sizeof(name)));
            I18N_FORMAT(detail, sizeof(detail), "{0} · {1}", c->detail, fact);
        } else if (role != ZSRC_ROLE_COUNT) {
            I18N_FORMAT(detail, sizeof(detail), "{0} · {1}", c->detail, role_code_line(role));
        } else {
            i18n_utf8_copy(detail, sizeof(detail), c->detail);
        }
        const bool ok = code_pickable(c);
        ui_row_link(s_list, kind_glyph(ZSRC_RF), ok ? ui_tone_amber() : ui_tone_gray(), c->name,
                    detail, c->state, ok ? UI_ICON_CHEVRON_RIGHT : NULL, ok ? on_code : NULL,
                    (void *)(uintptr_t)i);
    }
    ui_row_link(s_list, UI_ICON_PENCIL, ui_tone_blue(), TR("Pick the code by hand"),
                tamper ? NULL : TR("When the sensor can't be made to send it now"), NULL,
                UI_ICON_CHEVRON_RIGHT, pick_code_by_hand, NULL);
}

static void fill(void)
{
    if (s_list == NULL) {
        return;
    }
    lv_obj_clean(s_list);
    ui_rows_release(s_rows_mark);
    if (s_codes) {
        fill_codes();
        return;
    }
    const bool rf = s_kind == ZSRC_RF;
    if (rf) {
        rf_status();
    }
    bool nearby = false;
    for (size_t i = 0; i < s_ncands; i++) {
        const zsrc_candidate_t *c = &s_cands[i];
        const bool mine = !s_new && c->zone == s_zone;
        const bool taken = c->zone != 0u && !mine;
        char what_buf[48];
        const char *what = candidate_detail(c, what_buf, sizeof(what_buf));
        if (rf && !c->changed && !nearby) {
            nearby = true; /* the source lists these last: on the air before learning began */
            ui_section(s_list, TR("Already transmitting before you started"));
        }
        char detail[96];
        char name[ALARM_NAME_LEN + 8];
        if (taken) {
            char fact[ALARM_NAME_LEN + 40];
            /* TR: a sensor in the picker, after its details and " · "; {0} is the zone that uses
             * it. At most 40 bytes */
            I18N_FORMAT(fact, sizeof(fact), TR("In use: {0}"),
                        ui_zone_name(S.edit, c->zone, name, sizeof(name)));
            I18N_FORMAT(detail, sizeof(detail), "{0}{1}{2}", what, what[0] ? " · " : "", fact);
        } else if (mine) {
            I18N_FORMAT(detail, sizeof(detail), "{0}{1}{2}", what, what[0] ? " · " : "",
                        TR("This zone's sensor"));
        } else if (c->best && !rf) { /* a device just added from here (screens.md §5) */
            I18N_FORMAT(detail, sizeof(detail), "{0}{1}{2}", what, what[0] ? " · " : "",
                        TR("Just added"));
        } else if (c->changed && s_kind == ZSRC_ZIGBEE) {
            /* a Zigbee source marks only a device that joined while it listened (ZB-002) */
            I18N_FORMAT(detail, sizeof(detail), "{0}{1}{2}", what, what[0] ? " · " : "",
                        TR("Just added"));
        } else if (c->changed && !rf) {
            I18N_FORMAT(detail, sizeof(detail), "{0}{1}{2}", what, what[0] ? " · " : "",
                        TR("Just opened or closed"));
        } else {
            i18n_utf8_copy(detail, sizeof(detail), what);
        }
        char value[24];
        state_words(c->state, value, sizeof(value));
        const lv_color_t tone =
            taken ? ui_tone_gray() : (c->changed || c->best) ? ui_tone_amber() : ui_tone_blue();
        ui_row_link(s_list, kind_glyph(s_kind), tone, c->name, detail, value,
                    taken ? NULL : UI_ICON_CHEVRON_RIGHT, taken ? NULL : on_candidate,
                    (void *)(uintptr_t)i);
    }
    if (s_ncands == 0u && !rf) {
        ui_paragraph(s_list, TR("Nothing to pick: this panel has none."), ui_tone_gray());
    }
}

#if ALARM_UI_ZWAVE
/* screens.md §5, "Pick the sensor": pair a new device, then come straight back here with its
 * inputs offered first (PAGE_ZWAVE_ADD's arg 1). */
static void add_new_zwave_device(void *ctx)
{
    (void)ctx;
    ui_page_open(PAGE_ZWAVE_ADD, 1);
}
#endif

void ui_sources_build_picker(lv_obj_t *body, int32_t arg, char *title, size_t tlen, char *note,
                             size_t nlen)
{
    s_zone = (uint16_t)((uint32_t)arg & 0xFFFFu);
    const uint8_t kind = (uint8_t)(((uint32_t)arg >> 16) & 0xFFu);
    if (s_learning && s_kind != kind) {
        ui_sources_stop(); /* another source was listening */
    }
    s_kind = kind;
    s_new = (((uint32_t)arg >> 24) & 1u) != 0u;
    s_role = (uint8_t)(((uint32_t)arg >> 25) & 3u);
    if (s_role >= ZSRC_ROLE_COUNT) {
        s_role = ZSRC_ROLE_MAIN;
    }
    const bool wired = s_kind == ZSRC_WIRED;
    const bool rf = s_kind == ZSRC_RF;
    /* A 433 MHz zone's tamper or low-battery input: a code of the zone's own sensor (RF-021).
     * The first code it sends may be the door's closing one, so the installer picks. */
    s_codes = false;
    if (rf) {
        s_query[0] = '\0';
        if (s_role != ZSRC_ROLE_MAIN && S.be.zone_info(S.be.ctx, s_zone, &s_info) && !s_info.lost) {
            s_codes_of = s_info.binding;
            zsrc_rf_codes_query(&s_codes_of.in[ZSRC_ROLE_MAIN].addr, s_query, sizeof(s_query));
            s_codes = s_query[0] != '\0';
        }
    }
    const bool tamper = s_role == ZSRC_ROLE_TAMPER;
    if (s_codes) {
        i18n_utf8_copy(title, tlen, tamper ? TR("Find the tamper code") : TR("Find the low-battery code"));
        i18n_utf8_copy(note, nlen, tamper ? TR("Open and close the door, then take the cover off")
                                          : TR("Pick the code it sends"));
    } else {
        i18n_utf8_copy(title, tlen,
                       wired ? TR("Pick the terminal") : rf ? TR("Find the sensor") : TR("Pick the sensor"));
        i18n_utf8_copy(note, nlen, wired ? TR("Open and close the sensor: its terminal shows it")
                                   : rf  ? TR("Open the door, then close it")
                                         : kind_name(s_kind));
    }
    if (s_cands == NULL || s_fresh == NULL) {
        lv_free(s_cands);
        lv_free(s_fresh);
        s_cands = lv_malloc(MAX_CANDIDATES * sizeof(*s_cands));
        s_fresh = lv_malloc(MAX_CANDIDATES * sizeof(*s_fresh));
        if (s_cands == NULL || s_fresh == NULL) {
            lv_free(s_cands);
            lv_free(s_fresh);
            s_cands = NULL;
            s_fresh = NULL;
            ui_toast(TR("Not enough memory to show the sensors"));
            return;
        }
    }
    if (!s_learning && S.be.zone_learn != NULL) {
        S.be.zone_learn(S.be.ctx, s_kind, true);
        s_learning = true;
        s_best_heard = false;
    }
    if (wired) {
        ui_paragraph(body,
                     TR("Wire the contact between a terminal and GND. A normally closed contact "
                        "reads Closed while the door is shut."),
                     ui_tone_gray());
    } else if (s_codes && tamper) {
        ui_paragraph(body,
                     TR("Open and close the door once, then take the sensor's cover off (or let go "
                        "of its tamper switch). Pick the code that appeared as the cover came off."),
                     ui_tone_gray());
    } else if (s_codes) {
        ui_paragraph(body,
                     TR("Open and close the door once. A sensor sends its low-battery code only when "
                        "its battery runs low: power it from a lower voltage to hear it, or pick the "
                        "code by hand."),
                     ui_tone_gray());
    } else if (rf) {
        ui_paragraph(body,
                     TR("Start with the door closed. A remote or a motion sensor: trigger it twice."),
                     ui_tone_gray());
    }
    if (kind_searches(s_kind)) {
        ui_row_link(body, UI_ICON_PENCIL, ui_tone_blue(), TR("Find the sensor"), NULL,
                    s_query[0] != '\0' ? s_query : TR("Everything"), UI_ICON_CHEVRON_RIGHT,
                    edit_query, NULL);
    }
#if ALARM_UI_ZWAVE
    if (s_kind == ZSRC_ZWAVE && !s_codes) {
        ui_row_link(body, UI_ICON_PLUS, ui_tone_blue(), TR("Add a new Z-Wave device"),
                    TR("Pair it now, then pick its input"), NULL, UI_ICON_CHEVRON_RIGHT,
                    add_new_zwave_device, NULL);
    }
#endif
#if ALARM_UI_ZIGBEE
    if (s_kind == ZSRC_ZIGBEE && !s_codes) {
        ui_zigbee_picker_row(body); /* BL-144 page 11: join a new one, then pick its input */
    }
#endif
    s_list = lv_obj_create(body);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, SPACE_SM, 0);
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    s_rows_mark = ui_rows_mark();
    read_candidates(s_cands, &s_ncands);
    s_polled = lv_tick_get();
    fill();
}

/* ---- the zone page ---------------------------------------------------------------------------- */

static void on_change_source(void *ctx)
{
    (void)ctx;
    ui_sources_pick(s_zone, false);
}

static void set_invert(zsrc_mapping_t *m, int32_t v)
{
    m->invert = v != 0;
}

/* The Contact choices: a plain contact either way, or an end-of-line loop (ZONE-005, BL-063) on a
 * terminal that measures. index = the choice; its wiring and sense: */
static const struct {
    uint8_t input;  /* ZEOL_IN_* */
    bool invert;
} CONTACTS[] = {
    {ZEOL_IN_NONE, false}, {ZEOL_IN_NONE, true}, {ZEOL_IN_NC, false}, {ZEOL_IN_NO, false},
    {ZEOL_IN_DEOL, false},
};

/* The zone's terminal can be an end-of-line loop (its candidate says so). */
static bool terminal_measures(void)
{
    if (S.be.zone_candidates == NULL) {
        return false;
    }
    static zsrc_candidate_t c[ZSRC_WIRED_MAX];
    const size_t n = S.be.zone_candidates(S.be.ctx, ZSRC_WIRED, "", c, ZSRC_WIRED_MAX);
    for (size_t i = 0; i < n; i++) {
        if (c[i].addr.id == s_info.binding.in[ZSRC_ROLE_MAIN].addr.id) {
            return c[i].eol;
        }
    }
    return false;
}

static int32_t contact_index(const zsrc_input_t *in)
{
    for (size_t i = 0; i < sizeof(CONTACTS) / sizeof(CONTACTS[0]); i++) {
        if (CONTACTS[i].input == in->addr.input &&
            (in->addr.input != ZEOL_IN_NONE || CONTACTS[i].invert == in->map.invert)) {
            return (int32_t)i;
        }
    }
    return 0;
}

static const char *contact_words(const zsrc_input_t *in)
{
    switch (contact_index(in)) {
    case 1: return TR("Normally open");
    /* TR: a wired zone's Contact: a normally closed contact with an end-of-line resistor. At
     * most 40 bytes */
    case 2: return TR("Closed, end-of-line resistor");
    /* TR: a wired zone's Contact: normally open devices across an end-of-line resistor. At
     * most 40 bytes */
    case 3: return TR("Open, end-of-line resistor");
    /* TR: a wired zone's Contact: two end-of-line resistors. At most 40 bytes */
    case 4: return TR("Double end-of-line");
    default: return TR("Normally closed");
    }
}

static void contact_chosen(int32_t index)
{
    if (index < 0 || (size_t)index >= sizeof(CONTACTS) / sizeof(CONTACTS[0]) ||
        !S.be.zone_info(S.be.ctx, s_zone, &s_info)) {
        return;
    }
    zone_hub_binding_t b = s_info.binding;
    b.in[ZSRC_ROLE_MAIN].addr.input = CONTACTS[index].input;
    b.in[ZSRC_ROLE_MAIN].map.invert = CONTACTS[index].invert; /* never on a loop: it reports states */
    const zone_hub_result_t r = S.be.zone_bind(S.be.ctx, s_zone, &b);
    if (r != ZONE_HUB_OK) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(bind_error(r));
    }
    ui_page_rebuild();
}

static void pick_contact(void *ctx)
{
    (void)ctx;
    static ui_choice_t choices[5];
    choices[0] = (ui_choice_t){NULL, {0}, TR("Normally closed"),
                               TR("Closed while the door is shut: a cut wire shows as open")};
    choices[1] = (ui_choice_t){NULL, {0}, TR("Normally open"),
                               TR("Closes when it trips: a cut wire goes unnoticed")};
    /* TR: a choice of a wired zone's Contact (ZONE-005) */
    choices[2] = (ui_choice_t){NULL, {0}, TR("Closed, end-of-line resistor"),
                               /* TR: under the choice; 2 kOhm and 4.7 kOhm are the resistors */
                               TR("Doors, windows, motion: 2 kOhm at the sensor, 4.7 kOhm pull-up. A short is a tamper")};
    /* TR: a choice of a wired zone's Contact (ZONE-005) */
    choices[3] = (ui_choice_t){NULL, {0}, TR("Open, end-of-line resistor"),
                               /* TR: under the choice */
                               TR("Smoke detectors, panic buttons: a cut wire is a trouble")};
    /* TR: a choice of a wired zone's Contact (ZONE-005) */
    choices[4] = (ui_choice_t){NULL, {0}, TR("Double end-of-line"),
                               /* TR: under the choice */
                               TR("Two 2 kOhm resistors: a cut or shorted wire is a tamper")};
    const size_t n = terminal_measures() ? 5u : 2u;
    ui_pick_sheet(TR("Contact"), TR("How the sensor's contact works"), choices, n,
                  contact_index(&s_info.binding.in[ZSRC_ROLE_MAIN]), contact_chosen);
}

static const uint16_t RESPONSE_MS[] = {10u, 350u, 700u, 1200u};

static void set_response(zsrc_mapping_t *m, int32_t v)
{
    m->response_ms = RESPONSE_MS[v];
}

static void response_chosen(int32_t index)
{
    if (index >= 0 && (size_t)index < sizeof(RESPONSE_MS) / sizeof(RESPONSE_MS[0])) {
        rebind_with(set_response, index);
    }
}

static uint16_t response_of(const zsrc_mapping_t *m)
{
    return m->response_ms != 0u ? m->response_ms : ZSRC_WIRED_RESPONSE_MS;
}

/* A response time in words: "350 ms", "1.2 s". Fits 16 bytes. */
static void response_words(uint16_t ms, char *out, size_t len)
{
    if (ms >= 1000u) {
        char num[16];
        i18n_number(num, sizeof(num), ui_region(), (int64_t)(ms / 100u), 1);
        /* TR: a response time of a wired contact in seconds, {0} a number with a decimal like 1.2.
         * At most 10 bytes */
        I18N_FORMAT(out, len, TR_C("response time seconds", "{0} s"), num);
    } else {
        /* TR: a response time of a wired contact in milliseconds. At most 10 bytes */
        I18N_FORMAT(out, len, TRN("{0} ms", "{0} ms", (uint32_t)ms), (unsigned)ms);
    }
}

static void pick_response(void *ctx)
{
    (void)ctx;
    static ui_choice_t choices[4];
    static char titles[4][16];
    for (size_t i = 0; i < 4u; i++) {
        response_words(RESPONSE_MS[i], titles[i], sizeof(titles[i]));
        choices[i].glyph = NULL;
        choices[i].tone = (lv_color_t){0};
        choices[i].title = titles[i];
    }
    choices[0].detail = TR("Fast sensors: glass break, shock");
    choices[1].detail = TR("Doors, windows and motion (usual)");
    choices[2].detail = TR("Contacts that bounce");
    choices[3].detail = TR("Slow or noisy wiring");
    int32_t selected = 1;
    const uint16_t ms = response_of(&s_info.binding.in[ZSRC_ROLE_MAIN].map);
    for (size_t i = 0; i < sizeof(RESPONSE_MS) / sizeof(RESPONSE_MS[0]); i++) {
        if (RESPONSE_MS[i] == ms) {
            selected = (int32_t)i;
        }
    }
    ui_pick_sheet(TR("Response time"), TR("How long a change must last before it counts"), choices, 4u,
                  selected, response_chosen);
}

/* How long a sensor that only says "open" keeps the zone open (RF-022): the steps the picker
 * offers; 0 is automatic (the source's own 30 s, ended by the door's next closed code). */
static const uint16_t HOLD_S[] = {0u, 30u, 60u, 300u, 3600u, 43200u};
#define HOLD_STEPS (sizeof(HOLD_S) / sizeof(HOLD_S[0]))

/* The sources that read hold_s: RF, and the momentary inputs of Z-Wave (glass break, impact,
 * emergency). */
static bool hold_applies(const zsrc_address_t *a)
{
    if (a->kind == ZSRC_RF) {
        return true;
    }
#if ALARM_UI_ZWAVE
    if (a->kind == ZSRC_ZWAVE) {
        uint32_t home;
        uint16_t gen, node;
        uint8_t ep, slot;
        if (zsrc_zw_address_parse(a, &home, &gen, &node, &ep, &slot)) {
            return slot == ZSRC_ZW_SLOT_GLASS_BREAK || slot == ZSRC_ZW_SLOT_IMPACT ||
                   slot == ZSRC_ZW_SLOT_EMERGENCY;
        }
    }
#endif
    return false;
}

static void hold_words(uint16_t s, char *out, size_t len)
{
    switch (s) {
    case 0u: i18n_utf8_copy(out, len, TR("Automatic")); break;
    case 30u: i18n_utf8_copy(out, len, TR("30 seconds")); break;
    case 60u: i18n_utf8_copy(out, len, TR("1 minute")); break;
    case 300u: i18n_utf8_copy(out, len, TR("5 minutes")); break;
    case 3600u: i18n_utf8_copy(out, len, TR("1 hour")); break;
    case 43200u: i18n_utf8_copy(out, len, TR("12 hours")); break;
    default:
        /* TR: a hold time set in the Panel Manager that the picker has no step for, {0} a number
         * of seconds. At most 16 bytes */
        I18N_FORMAT(out, len, TR_C("hold time seconds", "{0} s"), (unsigned)s);
        break;
    }
}

static void set_hold(zsrc_mapping_t *m, int32_t v)
{
    m->hold_s = HOLD_S[v];
}

static void hold_chosen(int32_t index)
{
    if (index >= 0 && (size_t)index < HOLD_STEPS) {
        rebind_with(set_hold, index);
    }
}

static void pick_hold(void *ctx)
{
    (void)ctx;
    static ui_choice_t choices[HOLD_STEPS];
    static char titles[HOLD_STEPS][16];
    int32_t selected = 0;
    for (size_t i = 0; i < HOLD_STEPS; i++) {
        hold_words(HOLD_S[i], titles[i], sizeof(titles[i]));
        choices[i].glyph = NULL;
        choices[i].tone = (lv_color_t){0};
        choices[i].title = titles[i];
        if (HOLD_S[i] == s_info.binding.in[ZSRC_ROLE_MAIN].map.hold_s) {
            selected = (int32_t)i;
        }
    }
    for (size_t i = 1; i + 1u < HOLD_STEPS; i++) {
        choices[i].detail = NULL;
    }
    /* TR: the hold picker's Automatic: a door contact closes the zone with its closed code; a
     * sensor that sends only one code (motion, glass break) closes it after 30 seconds */
    choices[0].detail = TR("Closes at the sensor's closed code, or after 30 seconds");
    choices[HOLD_STEPS - 1u].detail = TR("For a low-battery code");
    /* TR: the hold picker's note: a set time keeps the zone open that long even when the sensor
     * sends its closed code */
    ui_pick_sheet(TR("Hold"),
                  TR("How long the zone stays open after the sensor last spoke. A set time "
                     "ignores the sensor's closed code."),
                  choices, HOLD_STEPS, selected, hold_chosen);
}

/* ---- a zone's sensor settings (everything the Panel Manager's Sensor link page sets) --------- */

static void open_settings(void *ctx)
{
    (void)ctx;
    ui_page_open(PAGE_ZONE_SENSOR, s_zone);
}

/* TR(MODE_NAMES[i]) where shown */
static const char *const MODE_NAMES[ZSRC_MAP_COUNT] = {
    TR_NOOP("On or open"), TR_NOOP("Above a value"), TR_NOOP("Below a value"),
    TR_NOOP("In a named state")};
/* The same, in the zone page's Sensor settings row (TR(MODE_SUMMARY[i]) where shown) */
static const char *const MODE_SUMMARY[ZSRC_MAP_COUNT] = {
    TR_NOOP("Open when on"), TR_NOOP("Open above a value"), TR_NOOP("Open below a value"),
    TR_NOOP("Open in a named state")};

static void set_mode(zsrc_mapping_t *m, int32_t v)
{
    m->mode = (uint8_t)v;
}

static void mode_chosen(int32_t index)
{
    if (index >= 0 && index < ZSRC_MAP_COUNT) {
        rebind_with(set_mode, index);
    }
}

static void pick_mode(void *ctx)
{
    (void)ctx;
    static ui_choice_t choices[ZSRC_MAP_COUNT];
    for (size_t i = 0; i < ZSRC_MAP_COUNT; i++) {
        choices[i].glyph = NULL;
        choices[i].tone = (lv_color_t){0};
        choices[i].title = TR(MODE_NAMES[i]);
    }
    choices[0].detail = TR("On, open, true or 1: a door, a motion sensor");
    choices[1].detail = TR("A reading at or over the threshold: heat, humidity");
    choices[2].detail = TR("A reading at or under the threshold: freezing");
    choices[3].detail = TR("One of the states listed: open, unlocked");
    ui_pick_sheet(TR("Open when"), TR("When the zone counts its sensor as open"), choices, ZSRC_MAP_COUNT,
                  s_info.binding.in[ZSRC_ROLE_MAIN].map.mode, mode_chosen);
}

static void set_threshold(zsrc_mapping_t *m, int32_t v)
{
    m->threshold_x100 = v;
}

static void set_hysteresis(zsrc_mapping_t *m, int32_t v)
{
    m->hysteresis_x100 = v < 0 ? -v : v;
}

/* "5", "-3.5", "12.25" in hundredths; false when it isn't a number. */
static bool parse_x100(const char *text, int32_t *out)
{
    long whole = 0, frac = 0;
    int digits = 0;
    bool neg = false, any = false;
    const char *p = text;
    while (*p == ' ') {
        p++;
    }
    if (*p == '-' || *p == '+') {
        neg = *p == '-';
        p++;
    }
    for (; *p >= '0' && *p <= '9'; p++) {
        whole = whole * 10 + (*p - '0');
        any = true;
        if (whole > 20000000L) {
            return false;
        }
    }
    if (*p == '.' || *p == ',') {
        for (p++; *p >= '0' && *p <= '9'; p++) {
            if (digits < 2) {
                frac = frac * 10 + (*p - '0');
                digits++;
            }
            any = true;
        }
    }
    while (*p == ' ') {
        p++;
    }
    if (!any || *p != '\0') {
        return false;
    }
    if (digits == 1) {
        frac *= 10;
    }
    const long v = whole * 100L + frac;
    *out = (int32_t)(neg ? -v : v);
    return true;
}

static void show_x100(int32_t v, char *buf, size_t len)
{
    const long a = v < 0 ? -(long)v : (long)v;
    if (a % 100 == 0) {
        snprintf(buf, len, "%s%ld", v < 0 ? "-" : "", a / 100);
    } else {
        snprintf(buf, len, "%s%ld.%02ld", v < 0 ? "-" : "", a / 100, a % 100);
    }
}

static void threshold_typed(const char *text)
{
    int32_t v = 0;
    if (!parse_x100(text, &v)) {
        ui_toast(TR("Type a number, like 5 or -3.5"));
        return;
    }
    rebind_with(set_threshold, v);
}

static void hysteresis_typed(const char *text)
{
    int32_t v = 0;
    if (!parse_x100(text, &v)) {
        ui_toast(TR("Type a number, like 0.5"));
        return;
    }
    rebind_with(set_hysteresis, v);
}

static void edit_threshold(void *ctx)
{
    char now[24];
    const bool hysteresis = (intptr_t)ctx != 0;
    const zsrc_mapping_t *m = &s_info.binding.in[ZSRC_ROLE_MAIN].map;
    show_x100(hysteresis ? m->hysteresis_x100 : m->threshold_x100, now, sizeof(now));
    ui_text_sheet_kind(hysteresis ? TR("Hysteresis") : TR("Threshold"),
                       hysteresis ? TR("How far back the reading must go before the zone closes again")
                                  : TR("In the sensor's own unit, like 5 for 5 °C"),
                       now, UI_TEXT_NUMBER, 12, hysteresis ? hysteresis_typed : threshold_typed);
}

static void states_typed(const char *text)
{
    static char typed[ZSRC_STATES_LEN];
    snprintf(typed, sizeof(typed), "%s", text);
    zone_hub_binding_t b;
    if (!S.be.zone_info(S.be.ctx, s_zone, &s_info)) {
        return;
    }
    b = s_info.binding;
    snprintf(b.in[ZSRC_ROLE_MAIN].map.states, sizeof(b.in[ZSRC_ROLE_MAIN].map.states), "%s", typed);
    const zone_hub_result_t r = S.be.zone_bind(S.be.ctx, s_zone, &b);
    if (r != ZONE_HUB_OK) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(bind_error(r));
    }
    ui_page_rebuild();
}

static void edit_states(void *ctx)
{
    (void)ctx;
    ui_text_sheet_kind(TR("States"), TR("Separated by commas: open,unlocked"),
                       s_info.binding.in[ZSRC_ROLE_MAIN].map.states, UI_TEXT_PLAIN,
                       ZSRC_STATES_LEN - 1u, states_typed);
}

static void on_reverse(bool on, void *ctx)
{
    (void)ctx;
    rebind_with(set_invert, on ? 1 : 0);
}

static void set_grace(zsrc_mapping_t *m, int32_t v)
{
    m->grace_s = (uint16_t)v;
}

/* "Unreachable after": how long its source may say the sensor is gone before the zone shows
 * Check (grace_s, 0 = the source's default). Choices in minutes; a value set another way (the
 * Panel Manager, the stepper before) stays offered as it is. */
static const uint16_t GRACE_S[] = {0u, 60u, 300u, 900u, 3600u};
#define GRACE_N (sizeof(GRACE_S) / sizeof(GRACE_S[0]))
static uint16_t s_grace_values[GRACE_N + 1u];
static char s_grace_titles[GRACE_N + 1u][24];

static void grace_words(uint16_t s, char *out, size_t len)
{
    if (s == 0u) {
        i18n_utf8_copy(out, len, TR("Source default"));
    } else if (s % 60u == 0u) {
        /* TR: a number of minutes in a number row; {0} is the number. At most 20 bytes */
        I18N_FORMAT(out, len, TRN("{0} min", "{0} min", (uint32_t)(s / 60u)), (unsigned)(s / 60u));
    } else {
        /* TR: a number of seconds in a number row; {0} is the number. At most 20 bytes */
        I18N_FORMAT(out, len, TRN("{0} s", "{0} s", (uint32_t)s), (unsigned)s);
    }
}

static void grace_chosen(int32_t index)
{
    if (index >= 0 && (size_t)index <= GRACE_N) {
        rebind_with(set_grace, (int32_t)s_grace_values[index]);
    }
}

static void pick_grace(void *ctx)
{
    (void)ctx;
    const uint16_t now = s_info.binding.in[ZSRC_ROLE_MAIN].map.grace_s;
    bool listed = false;
    for (size_t i = 0; i < GRACE_N; i++) {
        listed = listed || GRACE_S[i] == now;
    }
    /* The choices in order, today's value in its place when it isn't one of them. */
    size_t n = 0;
    for (size_t i = 0; i <= GRACE_N; i++) {
        if (!listed && (i == GRACE_N || now < GRACE_S[i])) {
            s_grace_values[n++] = now;
            listed = true;
        }
        if (i < GRACE_N) {
            s_grace_values[n++] = GRACE_S[i];
        }
    }
    static ui_choice_t choices[GRACE_N + 1u];
    int32_t selected = -1;
    for (size_t i = 0; i < n; i++) {
        grace_words(s_grace_values[i], s_grace_titles[i], sizeof(s_grace_titles[0]));
        choices[i] = (ui_choice_t){NULL, ui_tone_blue(), s_grace_titles[i], NULL};
        if (s_grace_values[i] == now) {
            selected = (int32_t)i;
        }
    }
    ui_pick_sheet(TR("Unreachable after"), TR("Shows Check when its source loses it"), choices, n,
                  selected, grace_chosen);
}

/* A tamper or low-battery input: pick its sensor, or none. */
static uint8_t s_other_role;

static void other_source_chosen(int32_t index)
{
    if (index == 0) { /* None */
        zone_hub_binding_t b;
        if (S.be.zone_info(S.be.ctx, s_zone, &s_info)) {
            b = s_info.binding;
            memset(&b.in[s_other_role], 0, sizeof(b.in[s_other_role]));
            const zone_hub_result_t r = S.be.zone_bind(S.be.ctx, s_zone, &b);
            if (r != ZONE_HUB_OK) { /* armed, or the store refused: the input stays */
                ui_beep(UI_BEEP_ERROR);
                ui_toast(bind_error(r));
            }
        }
        ui_page_rebuild();
        return;
    }
    s_role = s_other_role;
    s_new = false;
    source_chosen(index - 1);
}

static void pick_other(void *ctx)
{
    s_other_role = (uint8_t)(uintptr_t)ctx;
    ui_zone_source_t all[ZONE_HUB_MAX_SOURCES];
    const size_t n = S.be.zone_sources(S.be.ctx, all, ZONE_HUB_MAX_SOURCES);
    s_nsrcs = 0;
    for (size_t i = 0; i < n; i++) {
        if (all[i].usable) {
            s_srcs[s_nsrcs++] = all[i];
        }
    }
    static ui_choice_t choices[ZONE_HUB_MAX_SOURCES + 1];
    choices[0].glyph = UI_ICON_CIRCLE_SLASH;
    choices[0].tone = ui_tone_gray();
    choices[0].title = TR("None");
    choices[0].detail = s_other_role == ZSRC_ROLE_TAMPER ? TR("The main sensor reports its own tamper")
                                                         : TR("The main sensor reports its own battery");
    for (size_t i = 0; i < s_nsrcs; i++) {
        choices[i + 1u].glyph = kind_glyph(s_srcs[i].kind);
        choices[i + 1u].tone = ui_tone_blue();
        choices[i + 1u].title = kind_name(s_srcs[i].kind);
        choices[i + 1u].detail = kind_detail(s_srcs[i].kind);
    }
    const bool used = s_info.binding.in[s_other_role].addr.kind != ZSRC_NONE;
    ui_pick_sheet(s_other_role == ZSRC_ROLE_TAMPER ? TR("Tamper input") : TR("Low-battery input"),
                  TR("A separate input that reports it"), choices, s_nsrcs + 1u, used ? -1 : 0,
                  other_source_chosen);
}

static void unbind_confirmed(void)
{
    if (!ui_sources_zone_removed(s_zone)) {
        ui_beep(UI_BEEP_ERROR);
        ui_toast(TR("Disarm first"));
        return;
    }
    ui_page_back();
}

static void unbind_sensor(void *ctx)
{
    (void)ctx;
    ui_confirm(TR("Remove the sensor?"),
               TR("The zone stays, with no sensor: it never opens until you pick one again."),
               TR("Remove the sensor"), unbind_confirmed);
}

/* "Wired · GPIO 4": the terminal is a GPIO pin number (a value). */
static void wired_gpio(char *out, size_t len, uint32_t pin)
{
    char fact[32];
    /* TR: a wired contact's pin ({0} a number) after "Wired · ". At most 27 bytes */
    I18N_FORMAT(fact, sizeof(fact), TR("GPIO {0}"), (unsigned)pin);
    kind_and(out, len, ZSRC_WIRED, fact);
}

static void input_words(const zsrc_input_t *in, char *out, size_t len)
{
    uint8_t code;
    if (in->addr.kind == ZSRC_NONE) {
        i18n_utf8_copy(out, len, TR("None"));
    } else if (in->addr.kind == ZSRC_RF && zsrc_rf_address_parts(&in->addr, NULL, NULL, &code)) {
        char fact[32];
        char hex[8];
        snprintf(hex, sizeof(hex), "%X", (unsigned)code);
        /* TR: a 433 MHz sensor's code (hexadecimal {0}) after "433 MHz · ". At most 28 bytes */
        I18N_FORMAT(fact, sizeof(fact), TR("code {0}"), hex); /* which one it is (RF-021) */
        kind_and(out, len, ZSRC_RF, fact);
    } else if (in->addr.kind == ZSRC_WIRED) {
        wired_gpio(out, len, in->addr.id);
    } else if (in->addr.ref[0] != '\0') {
        snprintf(out, len, "%s · %.40s", kind_short(in->addr.kind), in->addr.ref);
    } else {
        i18n_utf8_copy(out, len, kind_short(in->addr.kind));
    }
}

void ui_sources_build_settings(lv_obj_t *body, int32_t arg, char *title, size_t tlen, char *note,
                               size_t nlen)
{
    s_zone = (uint16_t)arg;
    s_new = false;
    s_role = ZSRC_ROLE_MAIN;
    i18n_utf8_copy(title, tlen, TR("Sensor settings"));
    if (!ui_sources_available() || s_zone < 1u || s_zone > ALARM_MAX_ZONES ||
        !S.be.zone_info(S.be.ctx, s_zone, &s_info) || s_info.lost) {
        ui_paragraph(body, TR("This zone has no sensor."), ui_tone_gray());
        return;
    }
    char name[ALARM_NAME_LEN + 8];
    /* TR: the sensor settings page, the line under the title; {0} is the zone's name. At most 104 bytes */
    I18N_FORMAT(note, nlen, TR("{0} · changes apply at once"),
                ui_zone_name(S.edit, s_zone, name, sizeof(name)));
    const zsrc_mapping_t *m = &s_info.binding.in[ZSRC_ROLE_MAIN].map;
    char value[64];
    ui_section(body, TR("How it reads"));
    ui_row_link(body, UI_ICON_SLIDERS, ui_tone_blue(), TR("Open when"), NULL,
                m->mode < ZSRC_MAP_COUNT ? TR(MODE_NAMES[m->mode]) : "?", UI_ICON_CHEVRON_RIGHT,
                pick_mode, NULL);
    if (m->mode == ZSRC_MAP_ABOVE || m->mode == ZSRC_MAP_BELOW) {
        show_x100(m->threshold_x100, value, sizeof(value));
        ui_row_link(body, UI_ICON_THERMOMETER, ui_tone_blue(), TR("Threshold"), NULL, value,
                    UI_ICON_CHEVRON_RIGHT, edit_threshold, (void *)(intptr_t)0);
        show_x100(m->hysteresis_x100, value, sizeof(value));
        ui_row_link(body, UI_ICON_THERMOMETER, ui_tone_blue(), TR("Hysteresis"), NULL, value,
                    UI_ICON_CHEVRON_RIGHT, edit_threshold, (void *)(intptr_t)1);
    } else if (m->mode == ZSRC_MAP_STATES) {
        ui_row_link(body, UI_ICON_TAG, ui_tone_blue(), TR("States"), NULL,
                    m->states[0] != '\0' ? m->states : TR("None yet"), UI_ICON_CHEVRON_RIGHT,
                    edit_states, NULL);
    }
    if (s_info.binding.in[ZSRC_ROLE_MAIN].addr.kind == ZSRC_WIRED) {
        /* a wired contact in the words the zone page used: normally closed or open, or an
         * end-of-line loop (ZONE-005) */
        ui_row_link(body, UI_ICON_ARROW_LEFT_RIGHT, ui_tone_blue(), TR("Contact"), NULL,
                    contact_words(&s_info.binding.in[ZSRC_ROLE_MAIN]), UI_ICON_CHEVRON_RIGHT,
                    pick_contact, NULL);
    } else {
        ui_row_toggle_icon(
            body, UI_ICON_ARROW_LEFT_RIGHT, ui_tone_blue(), TR("Reverse open and closed"),
            TR("For a normally open contact, or a sensor that reports the other way round"),
            m->invert, on_reverse, NULL);
    }
    const uint16_t r = response_of(m);
    response_words(r, value, sizeof(value));
    ui_row_link(body, UI_ICON_TIMER, ui_tone_blue(), TR("Response time"), NULL, value,
                UI_ICON_CHEVRON_RIGHT, pick_response, NULL);
    if (hold_applies(&s_info.binding.in[ZSRC_ROLE_MAIN].addr)) {
        hold_words(m->hold_s, value, sizeof(value));
        ui_row_link(body, UI_ICON_CLOCK, ui_tone_blue(), TR("Hold"),
                    TR("How long the zone stays open after a sensor that only says \"open\" last spoke"),
                    value, UI_ICON_CHEVRON_RIGHT, pick_hold, NULL);
    }
    /* Two ways a sensor goes quiet, both on purpose: it misses its own check-in
     * (supervision_s, the zone's), or its source says it is gone (grace_s, the binding's). */
    ui_section(body, TR("When it goes quiet"));
    ui_installer_supervision_row(body, s_zone);
    grace_words(m->grace_s, value, sizeof(value));
    ui_row_link(body, UI_ICON_UNLINK, ui_tone_blue(), TR("Unreachable after"),
                TR("Shows Check when its source loses it"), value, UI_ICON_CHEVRON_RIGHT, pick_grace,
                NULL);
    ui_section(body, TR("Other inputs"));
    input_words(&s_info.binding.in[ZSRC_ROLE_TAMPER], value, sizeof(value));
    ui_row_link(body, UI_ICON_SHIELD_ALERT, ui_tone_amber(), TR("Tamper input"), NULL, value,
                UI_ICON_CHEVRON_RIGHT, pick_other, (void *)(uintptr_t)ZSRC_ROLE_TAMPER);
    input_words(&s_info.binding.in[ZSRC_ROLE_BATTERY], value, sizeof(value));
    ui_row_link(body, UI_ICON_BATTERY_WARNING, ui_tone_amber(), TR("Low-battery input"), NULL, value,
                UI_ICON_CHEVRON_RIGHT, pick_other, (void *)(uintptr_t)ZSRC_ROLE_BATTERY);
    ui_section(body, ""); /* apart */
    ui_row_action(body, UI_ICON_TRASH, TR("Remove the sensor, keep the zone"), ui_tone_red(),
                  unbind_sensor, NULL);
}

/* The zone right now: what the engine shows once the zone is saved, before that what the
 * sensor says. */
static void state_now(uint16_t zone, char *out, size_t len)
{
    zone_hub_info_t *info = &s_info;
    if (!S.be.zone_info(S.be.ctx, zone, info) || info->lost) {
        i18n_utf8_copy(out, len, info->lost ? TR("Sensor lost") : TR("No sensor"));
        return;
    }
    alarm_zone_status_t zs;
    memset(&zs, 0, sizeof(zs));
    if (S.cfg->zones[zone - 1u].type != ALARM_ZT_NOT_USED) {
        zs = S.snap.zone[zone - 1u];
    } else if (info->known && info->active) {
        zs.flags = ALARM_ZS_FAULT;
    }
    if (!info->known && S.cfg->zones[zone - 1u].type == ALARM_ZT_NOT_USED) {
        i18n_utf8_copy(out, len, TR("Not heard from yet"));
        return;
    }
    ui_line_t line;
    ui_zone_state(S.edit, zone, &zs, &line);
    i18n_utf8_copy(out, len, line.detail);
}

static void append(char *text, size_t len, const char *more)
{
    if (text[0] == '\0') {
        i18n_utf8_copy(text, len, more);
        return;
    }
    char first[320];
    i18n_utf8_copy(first, sizeof(first), text);
    /* TR: joins two sentences on a zone's source page. Languages that write no space between
     * sentences (Chinese, Japanese) drop it */
    I18N_FORMAT(text, len, TR_C("two sentences", "{0} {1}"), first, more);
}

/* What the source can promise, in words (ZSRC-008, ZSRC-009, ZSRC-010); true when it is a
 * warning (amber). */
static bool facts(char *text, size_t len, uint16_t zone, bool bound)
{
    text[0] = '\0';
    bool warn = false;
    const zsrc_input_t *in = &s_info.binding.in[ZSRC_ROLE_MAIN];
    if (!bound && s_info.lost) {
        append(text, len, TR("The panel couldn't read this zone's stored sensor, so the "
                                      "zone shows Check: pick its sensor again."));
        warn = true;
    } else if (!bound) {
        append(text, len, TR("No sensor feeds this zone, so it never opens."));
    } else if (!s_info.source_present) {
        append(text, len, TR("This software can't reach this kind of sensor: the zone "
                                      "shows Check until you pick another sensor or remove it."));
        warn = true;
    } else {
        const uint32_t caps = s_info.caps;
        if (s_info.check) {
            append(text, len, TR("The panel can't reach this sensor: the zone shows Check."));
            warn = true;
        }
        if ((caps & ZSRC_CAP_TEST) != 0u) {
            append(text, len, TR("For testing: nothing is wired, the service console opens "
                                          "and closes it."));
        } else {
            append(text, len, (caps & ZSRC_CAP_OFFLINE_OK) != 0u
                                           ? TR("Works without Wi-Fi.")
                                           : TR("Needs the network."));
            if (in->addr.kind == ZSRC_WIRED) {
                append(text, len, in->map.invert
                                               ? TR("A cut wire goes unnoticed: use a normally "
                                                    "closed contact where you can.")
                                               : TR("A cut wire shows as open."));
                warn = warn || in->map.invert;
            } else {
                append(text, len, (caps & ZSRC_CAP_SUPERVISED) != 0u
                                               ? TR("Tells the panel when it stops reporting.")
                                               : TR("Can't tell the panel when it stops working."));
                append(text, len, (caps & ZSRC_CAP_AUTH) != 0u
                                               ? TR("Uses authenticated radio messages.")
                                               : TR("Its radio signal can be copied."));
                if ((caps & ZSRC_CAP_BATTERY) != 0u) {
                    append(text, len, TR("Reports a low battery."));
                }
            }
            if (!zsrc_trusted_for(caps, S.edit->zones[zone - 1u].type)) {
                append(text, len, TR("Not trusted for fire, CO or panic zones: pick another "
                                              "sensor for this zone."));
                warn = true;
            }
        }
    }
    return warn;
}

#if ALARM_UI_ZWAVE
static void open_zwave_device(void *ctx)
{
    ui_page_open(PAGE_ZWAVE_DEVICE, (int32_t)(intptr_t)ctx);
}

static void dot_join(char *out, size_t len, const char *part)
{
    const size_t n = strlen(out);
    if (part[0] != '\0' && n < len) {
        snprintf(out + n, len - n, "%s%s", n > 0u ? " · " : "", part);
    }
}

/* The Source row's value for a Z-Wave input (screens.md §5 "Z-Wave · Zooz ZSE41"): the model's
 * first two words, its maker and model number - the whole name ("... tilt and shock sensor · tilt")
 * would push "Source" off the row - or "device 7" when the quirks table doesn't name it. */
static void zwave_source_words(const zsrc_address_t *addr, char *out, size_t len)
{
    uint32_t home;
    uint16_t generation, node;
    uint8_t endpoint, slot;
    ui_zw_device_page_t page;
    if (S.be.zw_device == NULL ||
        !zsrc_zw_address_parse(addr, &home, &generation, &node, &endpoint, &slot) ||
        !S.be.zw_device(S.be.ctx, node, generation, &page)) {
        i18n_utf8_copy(out, len, kind_short(ZSRC_ZWAVE));
        return;
    }
    char model[UI_ZW_MODEL_LEN];
    ui_zwave_short_model(page.dev.model, model, sizeof(model));
    if (model[0] == '\0') {
        char fact[32];
        /* TR: a Z-Wave device the panel has no model name for, after "Z-Wave · "; {0} is its
         * number. At most 28 bytes */
        I18N_FORMAT(fact, sizeof(fact), TR("device {0}"), (unsigned)node);
        kind_and(out, len, ZSRC_ZWAVE, fact);
    } else {
        snprintf(out, len, "%s · %s", kind_short(ZSRC_ZWAVE), model); /* the model is a value */
    }
}

/* screens.md §5 zone page: "Z-Wave device" · "Secure · Battery 92 % · Heard 2 min ago", opening
 * the device page. */
static void zwave_zone_device_row(lv_obj_t *body, const zsrc_address_t *addr)
{
    uint32_t home;
    uint16_t generation, node;
    uint8_t endpoint, slot;
    if (S.be.zw_device == NULL ||
        !zsrc_zw_address_parse(addr, &home, &generation, &node, &endpoint, &slot)) {
        return;
    }
    (void)home;
    (void)endpoint;
    (void)slot;
    ui_zw_device_page_t page;
    if (!S.be.zw_device(S.be.ctx, node, generation, &page)) {
        return;
    }
    const ui_zw_device_t *d = &page.dev;
    char line[96] = "";
    bool warn = false;
    switch (d->security) {
    case UI_ZW_SEC_S2_UNAUTH:
    case UI_ZW_SEC_S2_AUTH:
    case UI_ZW_SEC_S2_ACCESS: dot_join(line, sizeof(line), TR("Secure")); break;
    case UI_ZW_SEC_S0: dot_join(line, sizeof(line), TR("Older security")); break;
    case UI_ZW_SEC_UNCONFIRMED:
    case UI_ZW_SEC_UNKNOWN:
        dot_join(line, sizeof(line), TR("Security not confirmed"));
        warn = true;
        break;
    default:
        dot_join(line, sizeof(line), TR("Not encrypted"));
        warn = true;
        break;
    }
    if (d->battery_pct != UI_ZW_BATTERY_UNKNOWN) { /* a keypad woken by a call has one too */
        char batt[24];
        /* TR: a Z-Wave device's line on the zone page; {0} is the battery level in percent
         * (a number). At most 19 bytes */
        I18N_FORMAT(batt, sizeof(batt), TR("Battery {0} %"), (unsigned)d->battery_pct);
        dot_join(line, sizeof(line), batt);
        warn = warn || d->battery_low;
    }
    char heard[48];
    if (d->heard_s == UI_ZW_NEVER) {
        i18n_utf8_copy(heard, sizeof(heard), TR("Not heard yet")); /* At most 47 bytes */
    } else {
        char span[24];
        ui_zwave_span_words(d->heard_s, span, sizeof(span));
        /* TR: a Z-Wave or Zigbee device's line on the zone page; {0} is how long, like "2 min".
         * At most 24 bytes */
        I18N_FORMAT(heard, sizeof(heard), TR("Heard {0} ago"), span);
    }
    dot_join(line, sizeof(line), heard);
    warn = warn || d->unreachable;
    const uint32_t packed = (uint32_t)node | (uint32_t)generation << 16;
    /* the facts as the sub-line (detail), not the value: a long line there wrapped the title */
    ui_row_link(body, UI_ICON_RADIO_TOWER, warn ? ui_tone_amber() : ui_tone_green(),
                TR("Z-Wave device"), line, NULL, UI_ICON_CHEVRON_RIGHT, open_zwave_device,
                (void *)(intptr_t)(int32_t)packed);
}
#endif

void ui_sources_zone_rows(lv_obj_t *body, uint16_t zone)
{
    if (!ui_sources_available()) {
        return;
    }
    s_zone = zone;
    s_new = false;
    const bool bound = S.be.zone_info(S.be.ctx, zone, &s_info) && !s_info.lost;
    const zsrc_input_t *in = &s_info.binding.in[ZSRC_ROLE_MAIN];
    char value[96]; /* "Home Assistant · " + the source's 64-byte address words */
    if (!bound) {
        i18n_utf8_copy(value, sizeof(value), 
                 s_info.lost ? TR_C("zone page, Source row", "Lost") : TR("No sensor"));
    } else if (!s_info.source_present) {
        /* TR: the zone page's Source row, after the kind of sensor and " · ". At most 30 bytes */
        kind_and(value, sizeof(value), in->addr.kind, TR("missing"));
    } else if (in->addr.kind == ZSRC_WIRED) {
        wired_gpio(value, sizeof(value), in->addr.id);
#if ALARM_UI_ZWAVE
    } else if (in->addr.kind == ZSRC_ZWAVE) {
        zwave_source_words(&in->addr, value, sizeof(value));
#endif
#if ALARM_UI_ZIGBEE
    } else if (in->addr.kind == ZSRC_ZIGBEE &&
               ui_zigbee_source_words(&in->addr, value, sizeof(value))) {
        /* "Zigbee · Aqara door sensor" (docs/features/20, the BL-144 proposal's page 12) */
#endif
    } else if ((in->addr.ref[0] != '\0' || in->addr.kind == ZSRC_ZIGBEE ||
                in->addr.kind == ZSRC_RF) &&
               s_info.address[0] != '\0') {
        /* a named sensor elsewhere (a Home Assistant entity): say which one (ZSRC-009) */
        snprintf(value, sizeof(value), "%s · %s", kind_short(in->addr.kind), s_info.address);
    } else {
        i18n_utf8_copy(value, sizeof(value), kind_short(in->addr.kind));
    }
    const lv_color_t tone = !bound ? ui_tone_gray()
                            : (!s_info.source_present || s_info.check) ? ui_tone_amber()
                                                                        : ui_tone_blue();
    ui_row_link(body, bound ? kind_glyph(in->addr.kind) : UI_ICON_CIRCLE_SLASH, tone, TR("Source"),
                NULL, value, UI_ICON_CHEVRON_RIGHT, on_change_source, NULL);
#if ALARM_UI_ZWAVE
    if (bound && s_info.source_present && in->addr.kind == ZSRC_ZWAVE) {
        zwave_zone_device_row(body, &in->addr);
    }
#endif
#if ALARM_UI_ZIGBEE
    if (bound && s_info.source_present && in->addr.kind == ZSRC_ZIGBEE) {
        ui_zigbee_zone_device_row(body, &in->addr);
    }
#endif
    if (bound && s_info.source_present) {
        /* the row says what they are set to, so nothing needs opening to check them */
        const alarm_config_t *cfg = S.edit != NULL ? S.edit : S.cfg;
        const char *reads;
        if (in->addr.kind == ZSRC_WIRED) {
            reads = in->map.invert ? TR("Normally open") : TR("Normally closed");
        } else if (in->map.mode == ZSRC_MAP_BINARY && in->map.invert) {
            /* TR: the zone page's Sensor settings row, how it reads: a reversed on/off sensor */
            reads = TR("Open when off");
        } else {
            reads = in->map.mode < ZSRC_MAP_COUNT ? TR(MODE_SUMMARY[in->map.mode]) : "?";
        }
        char ms[16];
        response_words(response_of(&in->map), ms, sizeof(ms));
        /* TR: the zone page's Sensor settings row, whether the sensor must check in */
        const char *sup = cfg->zones[zone - 1u].supervision_s != 0u ? TR("supervised")
                                                                    : TR("not supervised");
        char detail[128];
        /* TR: the zone page's Sensor settings row: {0} how it reads ("Open when on", "Normally
         * closed"), {1} its response time ("350 ms"), {2} "supervised" or "not supervised". At
         * most 100 bytes */
        I18N_FORMAT(detail, sizeof(detail), TR("{0} · {1} · {2}"), reads, ms, sup);
        ui_row_link(body, UI_ICON_SLIDERS, ui_tone_blue(), TR("Sensor settings"), detail, NULL,
                    UI_ICON_CHEVRON_RIGHT, open_settings, NULL);
    }
}

void ui_sources_zone_status(lv_obj_t *body, uint16_t zone)
{
    s_state_row = NULL;
    if (!ui_sources_available() || zone < 1u || zone > ALARM_MAX_ZONES) {
        return;
    }
    s_zone = zone;
    s_new = false;
    const bool bound = S.be.zone_info(S.be.ctx, zone, &s_info) && !s_info.lost;
    char text[320];
    const bool warn = facts(text, sizeof(text), zone, bound);
    char detail[360];
    if (bound) {
        kind_and(detail, sizeof(detail), s_info.binding.in[ZSRC_ROLE_MAIN].addr.kind, text);
    } else {
        i18n_utf8_copy(detail, sizeof(detail), text);
    }
    const alarm_config_t *cfg = S.edit != NULL ? S.edit : S.cfg;
    const char *glyph =
        bound ? ui_device_class_glyph((alarm_device_class_t)cfg->zones[zone - 1u].device_class)
              : UI_ICON_CIRCLE_SLASH;
    const lv_color_t tone = !bound ? ui_tone_gray() : warn ? ui_tone_amber() : ui_tone_green();
    char now[96];
    state_now(zone, now, sizeof(now));
    /* not a button; its words change at most once a second (ui_sources_tick, UI-023) */
    s_state_row = ui_row_link(body, glyph, tone, now, detail, NULL, NULL, NULL, NULL);
    s_state_zone = zone;
    S.be.zone_info(S.be.ctx, zone, &s_info); /* state_now reused it */
}

bool ui_sources_zone_removed(uint16_t zone)
{
    if (!ui_sources_available() || S.be.zone_unbind == NULL) {
        return true;
    }
    return S.be.zone_unbind(S.be.ctx, zone) == ZONE_HUB_OK;
}

/* ---- life cycle ---------------------------------------------------------------------------------- */

void ui_sources_stop(void)
{
    if (s_learning && S.be.zone_learn != NULL) {
        S.be.zone_learn(S.be.ctx, s_kind, false);
    }
    s_learning = false;
    s_list = NULL;
    s_state_row = NULL;
}

static bool same(const zsrc_candidate_t *a, const zsrc_candidate_t *b)
{
    return a->zone == b->zone && a->changed == b->changed && a->best == b->best &&
           a->unusable == b->unusable &&
           strcmp(a->state, b->state) == 0 &&
           strcmp(a->name, b->name) == 0 && strcmp(a->detail, b->detail) == 0;
}

void ui_sources_tick(void)
{
    if ((s_list == NULL && s_state_row == NULL) || lv_tick_elaps(s_polled) < POLL_MS) {
        return;
    }
    s_polled = lv_tick_get();
    if (s_list != NULL) {
        size_t n = 0;
        read_candidates(s_fresh, &n);
        bool differ = n != s_ncands;
        for (size_t i = 0; i < n && !differ; i++) {
            differ = !same(&s_fresh[i], &s_cands[i]);
        }
        if (differ) {
            memcpy(s_cands, s_fresh, n * sizeof(*s_cands));
            s_ncands = n;
            fill();
        }
    }
    if (s_state_row != NULL) {
        char now[96];
        state_now(s_state_zone, now, sizeof(now));
        ui_set_label(s_state_row, "title_label", now);
    }
}
