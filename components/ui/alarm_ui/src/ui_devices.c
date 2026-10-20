/*
 * ui_devices.c - Smart Home (docs/features/24, BL-045; drawn in mockup-next, proposals/
 * BL-045-devices.js): the devices the panel controls. The rail's Smart Home shows while there is a
 * device (HCTL-003); the view has the areas as tabs with Favorites first, and tiles of one or two
 * cells placed in a grid of four columns (twelve cells fill the view). A one-cell tile does what a
 * tap can do (switch, lock, run a scene); a two-cell tile carries its control: a dimmable light's
 * brightness, a thermostat's set point between minus and plus, a cover's Open, Stop and Close (Stop
 * where it can stop), a fan's speeds (where it has them: a fan without speeds switches as a switch
 * does; HCTL-004, BL-171). The state is words and colour, including what is under way (the chip dimmed),
 * what failed (amber; a tap tries again, nothing retries by itself: HCTL-012) and what is offline
 * (since when, as readable as any tile: only its chip and its controls dim; HCTL-006, BL-181). A tap stops a moving cover, or turns it back where it can't stop
 * (BL-179); a tap on a failed change of a value opens its sheet (HCTL-012). Nothing moves (UI-020):
 * no spinner, no sliding.
 *
 * An unlock or an opening asks for a code on the pad over the screen (HCTL-005): the backend says
 * which actions need one (UI_DEV_NEED_CODE) and checks the code with the engine itself.
 *
 * The layout (ui_dev_model.h: Favorites, the order, each tile's size) is the panel's, read
 * through the backend. The arrow, or a long press, opens the device's detail sheet (HCTL-004:
 * a switch, brightness, set point and modes, position, speeds, Lock and Unlock, Run, each only
 * where the device reports it). Arrange (the pencil, HCTL-008) edits the layout of the page you are
 * on: every tile gets a dashed edge and a grip and rests (a tap picks it, nothing is switched); a
 * picked tile is edged in blue and the header turns into its actions - the arrows (one place
 * earlier or later), Small and Wide, Remove (Favorites only) - with Add (the devices not on
 * Favorites, by area) beside Done. The edits are ui_dev_model.h's, kept in a working copy and
 * stored through dev_layout_set when Done is tapped (or the view is left). Holding a tile (the touch
 * driver's long press) lifts it: it is picked as a tap picks it, and while the finger stays down it
 * takes the place of the tile the finger is well inside, the others put again around it; held at
 * the grid's top or bottom edge, it scrolls the grid a step at a time. The arrows stay the way to
 * move a tile without dragging (WCAG 2.5.7); nothing slides (UI-020): the tiles are put.
 *
 * Rooms (BL-220; the mockup's proposals/BL-220-device-rooms.js, approved 2026-10-19): the tabs
 * after Favorites are the house's rooms (home_control/hc_home.h), kept by the alarm panel through
 * dev_home_get/set. A device from a source with areas follows its source into the room of that
 * area until a person puts it somewhere; a picked tile's Room, Arrange's Rooms and a room page's
 * Add change them, at once. There is no title over the page: the tabs start the row. A device the
 * household set to ask for a master code shows a key, and the backend judges the code
 * (UI_DEV_NEED_CODE); arranging may ask for one too (Settings > Smart Home, built here). A link
 * keypad keeps no rooms: its master sends each device's room as its area, in the rooms' order.
 *
 * Compiled only with ALARM_UI_DEVICES (sources.cmake).
 */
#include <stdio.h>
#include <string.h>

#include "home_control/hc_home.h"
#include "ui_backend/ui_dev_model.h"
#include "ui_internal.h"

#define POLL_VIEW_MS 1000u /* live words at most once a second (UI-023) */
#define POLL_IDLE_MS 5000u /* off the view: only whether the rail shows Smart Home */
#define AREA_MAX     (UI_DEVL_MAX + 1u) /* every device in an area of its own, and "Other" */
#define UI_DEVL_READ_MAX (2u * UI_DEVL_BLOB_MAX) /* a longer blob of a newer version still reads */
#define KEYS_MAX     4u
#define COLS         4u
#define DRAG_STEP_MS 350u /* a held tile at the grid's edge scrolls it a step this often: under 3 a
                           * second (WCAG 2.3.1's limit for flashes; the content jumps, it doesn't
                           * flash, but stays calm) */
/* The value of an action that carries none, as the console and the automations send it. Never 0:
 * ACT_ON's 0 is brightness 0, which leaves a light off (device_source.h; B030). */
#define NO_VALUE (-1)

/* A fan's keys: Off, Low, Medium, High, as a speed in percent */
static const int32_t FAN_SPEED[] = {0, 33, 67, 100};

/* Lucide glyphs (the icon fonts' ranges in globals.xml) */
#define G_LIGHT     "\xEE\x87\x82" /* U+E1C2 lightbulb */
#define G_PLUG      "\xEE\x8D\xBF" /* U+E37F plug */
#define G_LOCK      "\xEE\x84\x8B" /* U+E10B lock */
#define G_LOCK_OPEN "\xEE\x84\x8C" /* U+E10C lock-open */
#define G_THERMO    "\xEE\x86\x86" /* U+E186 thermometer */
#define G_GARAGE    "\xEE\x8F\xA6" /* U+E3E6 warehouse */
#define G_BLINDS    "\xEE\x8F\x80" /* U+E3C0 blinds */
#define G_FAN       "\xEE\x8D\xB9" /* U+E379 fan */
#define G_DROPLET   "\xEE\x82\xB4" /* U+E0B4 droplet */
#define G_SPARKLES  "\xEE\x90\x92" /* U+E412 sparkles */
#define G_UP        "\xEE\x81\x8A" /* U+E04A arrow-up */
#define G_STOP      "\xEE\x85\xA7" /* U+E167 square */
#define G_DOWN      "\xEE\x81\x82" /* U+E042 arrow-down */
#define G_SUN       "\xEE\x85\xB8" /* U+E178 sun */
#define G_POWER     "\xEE\x85\x80" /* U+E140 power */
#define G_FLAME     "\xEE\x83\x92" /* U+E0D2 flame */
#define G_SNOW      "\xEE\x85\xA5" /* U+E165 snowflake */
#define G_SUNSNOW   "\xEE\x8D\xB2" /* U+E372 sun-snow */
#define G_GRIP      "\xEE\x83\xAB" /* U+E0EB grip-vertical */
#define G_PLUS      "\xEE\x84\xBD" /* U+E13D plus */
#define G_PENCIL    "\xEE\x87\xB9" /* U+E1F9 pencil */
#define G_DOOR      "\xEE\x8F\x96" /* U+E3D6 door-open */
#define G_KEY       "\xEE\x92\xA3" /* U+E4A3 key-round */
#define G_SMART     "\xEE\x98\xBC" /* U+E63C house-wifi: Smart Home, a device's source */
#define G_NO_ROOM   "\xEE\x90\x82" /* U+E402 circle-slash */
#define G_TRASH     "\xEE\x86\x8E" /* U+E18E trash-2 */
#define G_LEFT      "\xEE\x81\x88" /* U+E048 arrow-left */
#define G_RIGHT     "\xEE\x81\x89" /* U+E049 arrow-right */
#define ROOM_ROWS   (HC_HOME_ROOMS + 3u) /* a picker: its source, the rooms, New room, No room */

typedef struct {
    lv_obj_t *obj;
    lv_obj_t *level; /* a light's brightness, or NULL */
    lv_obj_t *step;  /* a thermostat's stepper, or NULL */
    lv_obj_t *keys[KEYS_MAX];
    uint8_t act[KEYS_MAX]; /* each key's action and value, as the sheet's */
    int32_t val[KEYS_MAX];
    uint8_t nkeys;
    bool wide;
    bool picked; /* arranging: this tile is the one picked */
    bool still;  /* offline: pressed, it keeps its colour (style_device_still, BL-181) */
    char ref[DEV_REF_LEN];
    /* What the name and the words labels were given (BL-184): LVGL writes its "..." into a long
     * text's own copy, so the label's text is no longer what it was set to and can't tell whether
     * the device's changed; compared with these, a poll leaves a shortened label alone. */
    char name[DEV_NAME_LEN];
    char words[64];
} tile_t;

static UI_SLOW_BSS struct {
    ui_dev_t *devs;    /* UI_DEVL_MAX, from ui_slow_alloc */
    ui_devl_t *layout; /* the panel's layout */
    uint8_t *blob;     /* UI_DEVL_READ_MAX: the layout as stored */
    size_t n;
    tile_t tiles[UI_DEVL_MAX];
    size_t ntiles;
    uint8_t page[UI_DEVL_MAX];
    size_t npage;
    uint32_t page_sig; /* what the tiles on screen were built for */
    uint32_t areas_sig; /* the areas the tabs were built for */
    char areas[AREA_MAX][DEV_AREA_LEN];
    size_t nareas;
    bool tabs_built;
    char tab[DEV_AREA_LEN]; /* the open tab's area */
    bool tab_favorites;     /* ... or Favorites */
    uint32_t polled;
    bool ready;
    lv_obj_t *grid;
    lv_obj_t *tabs;
    lv_obj_t *tab_on;     /* the open tab's segment, NULL before the tabs are built */
    int32_t tabs_max;     /* the tabs' widest: the row up to the pencil (BL-220 R7) */
    /* the action waiting for a code (HCTL-005) */
    char ask_ref[DEV_REF_LEN];
    uint8_t ask_action;
    int32_t ask_value;
    /* Arrange (HCTL-008) */
    bool arranging;
    bool dirty;                /* the working layout differs from the stored one */
    char pick[DEV_REF_LEN];    /* the tile picked, "" none */
    lv_obj_t *pencil;          /* the pencil, the header, its actions */
    lv_obj_t *head;
    lv_obj_t *arr_head;
    lv_obj_t *arr_add;
    lv_obj_t *arr_prev;
    lv_obj_t *arr_next;
    lv_obj_t *arr_size;
    lv_obj_t *arr_remove;
    lv_obj_t *arr_done;
    /* a held tile being dragged (HCTL-008): the picked one */
    bool drag;
    bool drag_moved;              /* the finger has left where it lifted the tile */
    bool drag_scrollable;         /* the tile's own flags, given back on release */
    bool drag_chain;
    lv_point_t drag_from;         /* where the tile was lifted */
    char drag_block[DEV_REF_LEN]; /* under the finger after the last move: no target until left */
    uint32_t drag_step;           /* when the grid last scrolled a step */
    ui_choice_t add_choice[UI_DEVL_MAX]; /* Add's list */
    char add_detail[UI_DEVL_MAX][96];
    uint8_t add_dev[UI_DEVL_MAX];
    size_t nadd;
    /* Rooms and master codes (BL-220): the house's settings, kept by the alarm panel */
    hc_home_t *home;
    bool home_on;               /* this panel keeps them (dev_home_get/set): not a link keypad */
    bool home_drop;             /* the next poll may drop an empty room made from an area */
    uint8_t *home_blob;         /* HC_HOME_BLOB_MAX */
    char (*src)[DEV_AREA_LEN];  /* UI_DEVL_MAX: each device's source area, before its room */
    hc_home_seen_t seen[UI_DEVL_MAX];
    lv_obj_t *arr_room;
    lv_obj_t *arr_rooms;
    lv_obj_t *spacer;
    uint8_t room_kind[ROOM_ROWS]; /* a room sheet's rows: what each does, and for which room */
    uint8_t room_id[ROOM_ROWS];
    size_t nroom_rows;
    ui_choice_t room_choice[ROOM_ROWS];
    char room_detail[ROOM_ROWS][96];
    char room_ref[DEV_REF_LEN]; /* the device whose room is picked, "" none (Rooms' New room) */
    uint8_t room_edit;          /* the room whose sheet is open */
    char typed[HC_HOME_NAME_MAX + 1u]; /* a name typed that couldn't be used, to type again */
    char set_refs[UI_DEVL_MAX][DEV_REF_LEN]; /* Settings > Smart Home's switches, by row */
} D;

static void poll(void);
static void show(void);
static int room_of_tab(void);
static const char *no_room_tab(void);
static void arrange_pick(const char *ref);
static void note(void);
static void on_tile_hold(lv_event_t *e);
static void sheet_open(const ui_dev_t *d);

static bool on_view(void)
{
    return lv_subject_get_int(&subject_view) == VIEW_DEVICES;
}

static const ui_dev_t *find(const char *ref)
{
    for (size_t i = 0; i < D.n; i++) {
        if (strcmp(D.devs[i].ref, ref) == 0) {
            return &D.devs[i];
        }
    }
    return NULL;
}

/* ---- what a tile says --------------------------------------------------------------------- */

static bool is_on(const ui_dev_t *d)
{
    return d->state == UI_DEV_ST_ON;
}

/* A change that reads as done the moment it is made (BL-219, approved 2026-10-19): what a light, a
 * switch, a fan, a thermostat or a scene does. A lock, a garage door, a cover and a valve say they
 * are under way until they report: a security panel never says "Locked" or "Closed" before it is. */
static bool at_once(const ui_dev_t *d, uint8_t action)
{
    switch (d->kind) {
    case DEV_LIGHT:
    case DEV_SWITCH:
    case DEV_FAN:
        return action == ACT_ON || action == ACT_OFF || action == ACT_TOGGLE || action == ACT_SET_LEVEL;
    case DEV_CLIMATE: return action == ACT_SET_TEMP || action == ACT_SET_MODE;
    case DEV_SCENE:
    case DEV_SCRIPT: return action == ACT_RUN;
    default: return false;
    }
}

/* An action under way that the tile says so for (HCTL-006): not one that shows at once, even when
 * someone else - the console, an automation - started it: its tile shows the report until the next
 * one, as Home Assistant's own screens do (BL-219). */
static bool under_way(const ui_dev_t *d)
{
    return d->status == UI_DEV_PENDING && !at_once(d, d->action);
}

static bool entrance(const ui_dev_t *d)
{
    return d->kind == DEV_COVER && (d->caps & UI_DEV_CAP_ENTRANCE) != 0u;
}

static const char *glyph(const ui_dev_t *d)
{
    switch (d->kind) {
    case DEV_LIGHT: return G_LIGHT;
    case DEV_LOCK: return d->state == UI_DEV_ST_LOCKED || d->state == UI_DEV_ST_JAMMED ? G_LOCK : G_LOCK_OPEN;
    case DEV_CLIMATE: return G_THERMO;
    case DEV_COVER: return entrance(d) ? G_GARAGE : G_BLINDS;
    case DEV_FAN: return G_FAN;
    case DEV_VALVE: return G_DROPLET;
    case DEV_SCENE:
    case DEV_SCRIPT: return G_SPARKLES;
    default: return G_PLUG;
    }
}

/* Colour is the state: blue on, green locked or closed, amber unlocked or open, orange heating,
 * grey off (mockup-next's notes on n-devices). */
static lv_color_t tone(const ui_dev_t *d)
{
    if (d->status == UI_DEV_OFFLINE) {
        return ui_tone_gray();
    }
    switch (d->kind) {
    case DEV_LOCK:
        return d->state == UI_DEV_ST_LOCKED ? ui_tone_green()
               : d->state == UI_DEV_ST_UNKNOWN ? ui_tone_gray()
                                               : ui_tone_amber();
    case DEV_COVER:
        if (entrance(d)) {
            return d->state == UI_DEV_ST_CLOSED ? ui_tone_green()
                   : d->state == UI_DEV_ST_UNKNOWN ? ui_tone_gray()
                                                   : ui_tone_amber();
        }
        return d->state == UI_DEV_ST_CLOSED || d->state == UI_DEV_ST_UNKNOWN ? ui_tone_gray()
                                                                             : ui_tone_blue();
    case DEV_VALVE:
        return d->state == UI_DEV_ST_OPEN || d->state == UI_DEV_ST_OPENING ? ui_tone_blue()
                                                                           : ui_tone_gray();
    case DEV_CLIMATE:
        return d->state == UI_DEV_ST_HEAT ? ui_tone_orange()
               : d->state == UI_DEV_ST_COOL || d->state == UI_DEV_ST_AUTO ? ui_tone_blue()
                                                                          : ui_tone_gray();
    case DEV_SCENE:
    case DEV_SCRIPT: return ui_tone_violet();
    default: return is_on(d) ? ui_tone_blue() : ui_tone_gray();
    }
}

static bool amber(const ui_dev_t *d)
{
    return lv_color_eq(tone(d), ui_tone_amber());
}

/* "71°": a temperature in hundredths, rounded to a degree. */
static void degrees(int32_t hundredths, char *out, size_t len)
{
    const int32_t whole = (hundredths + (hundredths >= 0 ? 50 : -50)) / 100;
    /* TR: a temperature on a Smart Home tile, in the source's own unit */
    I18N_FORMAT(out, len, TR_C("temperature", "{0}°"), (int)whole);
}

static const char *fan_speed(int level)
{
    return level <= 0    ? TR_C("fan speed", "Off")
           : level <= 34 ? TR_C("fan speed", "Low")
           : level <= 67 ? TR_C("fan speed", "Medium")
                         : TR_C("fan speed", "High");
}

/* What a thermostat is doing, as its source says (dev_hvac_t), with the room's temperature ({0})
 * or without; NULL where its source doesn't say, or says something without words here (drying,
 * fan, defrosting). */
static const char *doing_words(uint8_t action, bool now)
{
    if (now) {
        /* TR: a thermostat's tile and sheet: what it is doing now, from its source (not its mode), and
         * the room's temperature; {0} is a temperature ("71°", up to 6 bytes). The mode shows on the
         * lit key. At most 40 bytes */
        switch (action) {
        case DEV_HVAC_HEATING: return TR_C("thermostat doing", "Heating · {0} now");
        case DEV_HVAC_COOLING: return TR_C("thermostat doing", "Cooling · {0} now");
        case DEV_HVAC_PREHEATING: return TR_C("thermostat doing", "Preheating · {0} now");
        case DEV_HVAC_IDLE: return TR_C("thermostat doing", "Idle · {0} now");
        case DEV_HVAC_OFF: return TR_C("thermostat doing", "Off · {0} now");
        default: return NULL;
        }
    }
    /* TR: a thermostat's tile and sheet: what it is doing now, from its source (not its mode), when
     * the room's temperature isn't known. The mode shows on the lit key. At most 40 bytes */
    switch (action) {
    case DEV_HVAC_HEATING: return TR_C("thermostat doing", "Heating");
    case DEV_HVAC_COOLING: return TR_C("thermostat doing", "Cooling");
    case DEV_HVAC_PREHEATING: return TR_C("thermostat doing", "Preheating");
    case DEV_HVAC_IDLE: return TR_C("thermostat doing", "Idle");
    case DEV_HVAC_OFF: return TR_C("thermostat doing", "Off");
    default: return NULL;
    }
}

/* A thermostat's mode as its neutral label (never "Heating" guessed from it), with the room's
 * temperature ({0}) or without; NULL for a mode that isn't known. */
static const char *mode_words(uint8_t state, bool now)
{
    if (now) {
        /* TR: a thermostat's tile and sheet when its source doesn't say what it is doing: its mode,
         * and the room's temperature; {0} is a temperature ("71°", up to 6 bytes). At most 40 bytes */
        switch (state) {
        case UI_DEV_ST_HEAT: return TR_C("thermostat mode", "Heat · {0} now");
        case UI_DEV_ST_COOL: return TR_C("thermostat mode", "Cool · {0} now");
        case UI_DEV_ST_AUTO: return TR_C("thermostat mode", "Auto · {0} now");
        case UI_DEV_ST_OFF: return TR_C("thermostat mode", "Off · {0} now");
        case UI_DEV_ST_ON: return TR_C("thermostat mode", "On · {0} now"); /* dry, fan only */
        default: return NULL;
        }
    }
    /* TR: a thermostat's tile and sheet when its source doesn't say what it is doing and the room's
     * temperature isn't known: its mode. At most 40 bytes */
    switch (state) {
    case UI_DEV_ST_HEAT: return TR_C("thermostat mode", "Heat");
    case UI_DEV_ST_COOL: return TR_C("thermostat mode", "Cool");
    case UI_DEV_ST_AUTO: return TR_C("thermostat mode", "Auto");
    case UI_DEV_ST_OFF: return TR_C("thermostat mode", "Off");
    case UI_DEV_ST_ON: return TR_C("device state", "On"); /* dry, fan only */
    default: return NULL;
    }
}

/* What a reported rest says (HCTL-004, BL-171): Idle while the reported mode can resume (Heat, Cool,
 * Auto, On), Off only while it is Off, whichever of the two its source calls its rest (some thermostat
 * integrations say off whenever the system rests, others idle in mode Off). Only off and
 * idle change, and only by the reported mode (d->state): never by the modes it has, a mode asked for
 * and pending, the temperature or the integration; an unknown mode keeps what was reported. Heating,
 * Cooling and Preheating, and an action without words, pass as reported. Presentation only. */
static uint8_t resting_action(uint8_t action, uint8_t state)
{
    if (action != DEV_HVAC_OFF && action != DEV_HVAC_IDLE) {
        return action;
    }
    switch (state) {
    case UI_DEV_ST_OFF: return DEV_HVAC_OFF;
    case UI_DEV_ST_HEAT:
    case UI_DEV_ST_COOL:
    case UI_DEV_ST_AUTO:
    case UI_DEV_ST_ON: return DEV_HVAC_IDLE;
    default: return action;
    }
}

/* A thermostat's words (HCTL-004, BL-171): what it is doing where its source says (hvac_action:
 * Heating, Cooling, Preheating; at rest Idle, or Off in mode Off: resting_action()); else its mode's
 * label (Heat, Cool, Auto, Off; On for a dry or fan-only mode) - never guessed from the mode, nor
 * from the temperature against the set point. With the room's temperature each pair is one sentence
 * ("Idle · 71° now"). The set point's label, the lit mode key and the colour say the mode (tone(),
 * the sheet). */
static void thermostat_words(const ui_dev_t *d, char *out, size_t len)
{
    const bool now = d->temperature != UI_DEV_TEMP_UNKNOWN;
    const char *w = doing_words(resting_action(d->hvac_action, d->state), now);
    if (w == NULL) {
        w = mode_words(d->state, now);
    }
    if (w == NULL) {
        if (!now) {
            if (len > 0) {
                out[0] = '\0';
            }
            return;
        }
        /* TR: a thermostat's tile: what it measures now ("71° now") */
        w = TR("{0} now");
    }
    if (now) {
        char t[16];
        degrees(d->temperature, t, sizeof(t));
        I18N_FORMAT(out, len, w, t);
    } else {
        i18n_utf8_copy(out, len, w);
    }
}

/* The state in words. One fact, or two joined with " · ". A thermostat's are its own. */
static void state_words(const ui_dev_t *d, char *out, size_t len)
{
    if (d->kind == DEV_CLIMATE) {
        thermostat_words(d, out, len);
        return;
    }
    char a[48];
    char b[48];
    a[0] = '\0';
    b[0] = '\0';
    switch (d->state) {
    case UI_DEV_ST_OFF: i18n_utf8_copy(a, sizeof(a), TR_C("device state", "Off")); break;
    case UI_DEV_ST_ON:
        /* TR: a light's, switch's or fan's tile and sheet: it is on, alone or before its
         * brightness or speed ("On · 40%"); also a thermostat's dry or fan-only mode. Every light,
         * switch and fan shows it. At most 23 bytes */
        i18n_utf8_copy(a, sizeof(a), TR_C("device state", "On"));
        if (d->kind == DEV_FAN && d->level >= 0) {
            i18n_utf8_copy(b, sizeof(b), fan_speed(d->level));
        } else if (d->level >= 0 && d->kind == DEV_LIGHT) {
            I18N_FORMAT(b, sizeof(b), TR_C("percent", "{0}%"), (int)d->level);
        }
        break;
    case UI_DEV_ST_LOCKED: i18n_utf8_copy(a, sizeof(a), TR_C("device state", "Locked")); break;
    case UI_DEV_ST_UNLOCKED: i18n_utf8_copy(a, sizeof(a), TR_C("device state", "Unlocked")); break;
    case UI_DEV_ST_JAMMED: i18n_utf8_copy(a, sizeof(a), TR_C("device state", "Jammed")); break;
    case UI_DEV_ST_LOCKING: i18n_utf8_copy(a, sizeof(a), TR_C("device state", "Locking…")); break;
    case UI_DEV_ST_UNLOCKING: i18n_utf8_copy(a, sizeof(a), TR_C("device state", "Unlocking…")); break;
    case UI_DEV_ST_CLOSED: i18n_utf8_copy(a, sizeof(a), TR_C("device state", "Closed")); break;
    case UI_DEV_ST_OPEN:
        i18n_utf8_copy(a, sizeof(a), TR_C("device state", "Open"));
        if (d->kind == DEV_COVER && d->level > 0 && d->level < 100) {
            I18N_FORMAT(b, sizeof(b), TR_C("percent", "{0}%"), (int)d->level);
        }
        break;
    case UI_DEV_ST_CLOSING: i18n_utf8_copy(a, sizeof(a), TR_C("device state", "Closing…")); break;
    case UI_DEV_ST_OPENING: i18n_utf8_copy(a, sizeof(a), TR_C("device state", "Opening…")); break;
    default:
        if (d->kind == DEV_SCENE || d->kind == DEV_SCRIPT) {
            char t[32];
            switch (ui_when_text(d->since_ms, t, sizeof(t))) { /* HCTL-006, BL-179 */
            case UI_WHEN_TODAY:
                /* TR: a scene's tile: when it last ran ("Ran at 11:05 PM") */
                I18N_FORMAT(a, sizeof(a), TR("Ran at {0}"), t);
                break;
            case UI_WHEN_DAY:
            case UI_WHEN_YEAR:
                /* TR: a scene's tile: the day it last ran, before today ("Ran on Sep 18", "Ran on
                 * Sep 18, 2025"); {0} is a short date, with the year when it was an earlier year,
                 * up to 31 bytes, and the whole sentence must fit in 47. At most 16 bytes */
                I18N_FORMAT(a, sizeof(a), TR("Ran on {0}"), t);
                break;
            default:
                i18n_utf8_copy(a, sizeof(a), TR_C("device kind", "Scene"));
                break;
            }
        } else {
            i18n_utf8_copy(a, sizeof(a), TR_C("device state", "Unknown"));
        }
        break;
    }
    if (a[0] != '\0' && b[0] != '\0') {
        char both[sizeof(a) + sizeof(b) + 4];
        snprintf(both, sizeof(both), "%s · %s", a, b);
        i18n_utf8_copy(out, len, both); /* cut on a whole character */
    } else {
        snprintf(out, len, "%s", a[0] != '\0' ? a : b);
    }
}

/* What an action under way says, and what it says when it failed (HCTL-006). */
static const char *action_words(uint8_t action, bool failed)
{
    switch (action) {
    case ACT_ON: return failed ? TR("Didn't turn on") : TR("Turning on…");
    case ACT_OFF: return failed ? TR("Didn't turn off") : TR("Turning off…");
    case ACT_TOGGLE: return failed ? TR("Didn't switch") : TR("Switching…");
    case ACT_LOCK: return failed ? TR("Didn't lock") : TR_C("device state", "Locking…");
    case ACT_UNLOCK: return failed ? TR("Didn't unlock") : TR_C("device state", "Unlocking…");
    case ACT_OPEN: return failed ? TR("Didn't open") : TR_C("device state", "Opening…");
    case ACT_CLOSE: return failed ? TR("Didn't close") : TR_C("device state", "Closing…");
    case ACT_STOP: return failed ? TR("Didn't stop") : TR("Stopping…");
    case ACT_RUN: return failed ? TR("Didn't run") : TR("Running…");
    case ACT_SET_LEVEL:
    case ACT_SET_TEMP:
    case ACT_SET_MODE: return failed ? TR("Didn't change") : TR("Changing…");
    default: return failed ? TR("Didn't respond") : NULL;
    }
}

static void tile_words(const ui_dev_t *d, char *out, size_t len)
{
    if (d->status == UI_DEV_OFFLINE) {
        char t[32];
        switch (ui_when_text(d->since_ms, t, sizeof(t))) { /* HCTL-006, BL-179 */
        case UI_WHEN_TODAY:
            /* TR: a device's tile while it can't be reached ("Offline since 9:12 PM") */
            I18N_FORMAT(out, len, TR("Offline since {0}"), t);
            break;
        case UI_WHEN_DAY:
        case UI_WHEN_YEAR:
            /* TR: a device's tile while it can't be reached, since an earlier day ("Offline since
             * Sep 18", "Offline since Sep 18, 2025"); {0} is a short date, with the year for an
             * earlier year, up to 31 bytes, and the whole sentence must fit in 47. At most 16
             * bytes */
            I18N_FORMAT(out, len, TR_C("since a date", "Offline since {0}"), t);
            break;
        default:
            i18n_utf8_copy(out, len, TR_C("device state", "Offline"));
            break;
        }
        return;
    }
    if (d->status == UI_DEV_FAILED && d->unsure != 0u) {
        /* TR: a device's tile and sheet: its last action got no answer (or the connection
         * dropped), so whether it happened isn't known; a tap tries it again, or opens its sheet
         * for a value change. At most 23 bytes */
        i18n_utf8_copy(out, len, TR_C("device state", "Not confirmed"));
        return;
    }
    const char *w = under_way(d) || d->status == UI_DEV_FAILED
                        ? action_words(d->action, d->status == UI_DEV_FAILED)
                        : NULL;
    if (w != NULL) {
        i18n_utf8_copy(out, len, w);
        return;
    }
    state_words(d, out, len);
}

/* ---- what a tap does ---------------------------------------------------------------------- */

static uint8_t tap_action(const ui_dev_t *d)
{
    switch (d->kind) {
    case DEV_LIGHT:
    case DEV_SWITCH:
    case DEV_FAN: return is_on(d) ? ACT_OFF : ACT_ON;
    case DEV_LOCK: return d->state == UI_DEV_ST_LOCKED ? ACT_UNLOCK : ACT_LOCK;
    case DEV_COVER:
        if (d->state == UI_DEV_ST_OPENING || d->state == UI_DEV_ST_CLOSING) {
            if ((d->caps & UI_DEV_CAP_NO_STOP) == 0u) {
                return ACT_STOP; /* it stops, or its source doesn't say (BL-171) */
            }
            /* it can't stop: turned back, as Home Assistant's cover toggle does (BL-179); reopening
             * goes through send(), so it asks for a code where opening does (HCTL-005) */
            return d->state == UI_DEV_ST_CLOSING ? ACT_OPEN : ACT_CLOSE;
        }
        return d->state == UI_DEV_ST_CLOSED ? ACT_OPEN : ACT_CLOSE;
    case DEV_VALVE: return d->state == UI_DEV_ST_OPEN ? ACT_CLOSE : ACT_OPEN;
    case DEV_SCENE:
    case DEV_SCRIPT: return ACT_RUN;
    default: return ACT_NONE; /* a thermostat: its stepper, or its sheet */
    }
}

/* ---- what was asked (BL-219, HCTL-006, HCTL-012) ------------------------------------------- */

/* A change a person makes that shows at once (at_once) reads as done the moment it is made, as Home
 * Assistant's own screens do (proposal BL-219, approved 2026-10-19): the tile, its sheet and the
 * page's note show what was asked until the device sends a new report, and from then on the report,
 * whatever it says. With no new report, what was asked stays until EXPECT_SETTLE_MS after the device
 * has answered (Home Assistant's toggle gives it 2 s), then the last report shows again. A refusal,
 * no answer ("Not confirmed") and offline show as before (HCTL-012): what was asked goes, and
 * nothing is sent again by itself. A touch while the last change is still on its way is the newest
 * wish: it shows at once and is sent once the device has answered the last one (it would refuse it
 * as busy before). Only this task reads and writes the table. */
#define EXPECT_MAX       8u
#define EXPECT_SETTLE_MS 2000u
#define EXPECT_LIFE_MS   60000u /* past any answer's own limit (HC_ACT_TIMEOUT_MS, 30 s) */

typedef struct {
    char ref[DEV_REF_LEN]; /* "" unused */
    uint8_t action;        /* what was asked last, and its value */
    int32_t value;
    uint8_t state;         /* what the tile shows meanwhile */
    int8_t level;
    uint8_t hvac_action;
    int32_t setpoint;
    int64_t since_ms;
    uint32_t sig;          /* the device's report as it was when it was asked (report_sig) */
    uint8_t was;           /* its status then */
    uint8_t status;        /* its status as last read: its tile shows it idle */
    bool moved;            /* its status has been other than `was` since */
    bool queued;           /* `action` waits behind the last change, not sent yet */
    bool calling;          /* `action` went out, its answer isn't back */
    uint32_t at;           /* lv_tick: asked, or sent */
    uint32_t idle_at;      /* lv_tick: answered and not busy since; 0 not yet */
} expect_t;

static UI_SLOW_BSS expect_t E[EXPECT_MAX];

static ui_dev_t *find_mut(const char *ref)
{
    for (size_t i = 0; i < D.n; i++) {
        if (strcmp(D.devs[i].ref, ref) == 0) {
            return &D.devs[i];
        }
    }
    return NULL;
}

static expect_t *expect_find(const char *ref)
{
    for (size_t i = 0; i < EXPECT_MAX; i++) {
        if (E[i].ref[0] != '\0' && strcmp(E[i].ref, ref) == 0) {
            return &E[i];
        }
    }
    return NULL;
}

static void expect_drop(expect_t *e)
{
    memset(e, 0, sizeof(*e));
}

/* What a report says about what a person changes: a new report is one where this differs. What a
 * thermostat is doing and the room's temperature change by themselves, so they aren't in it. */
static uint32_t report_sig(const ui_dev_t *d)
{
    const uint32_t parts[] = {d->state, (uint32_t)(uint8_t)d->level, (uint32_t)d->setpoint,
                              (uint32_t)(uint64_t)d->since_ms, (uint32_t)((uint64_t)d->since_ms >> 32)};
    uint32_t sig = 2166136261u;
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        sig = (sig ^ parts[i]) * 16777619u;
    }
    return sig;
}

/* What the tile shows once `action` is done, from what it shows now (d). */
static void expect_shape(expect_t *e, const ui_dev_t *d, uint8_t action, int32_t value)
{
    e->state = d->state;
    e->level = d->level;
    e->hvac_action = d->hvac_action;
    e->setpoint = d->setpoint;
    e->since_ms = d->since_ms;
    const int8_t pct = (int8_t)(value < 0 ? 0 : value > 100 ? 100 : value);
    switch (action) {
    case ACT_ON:
        if (d->kind == DEV_LIGHT && value >= 0) { /* a brightness with the "on": 0 is off (B030) */
            e->level = pct;
            e->state = pct > 0 ? UI_DEV_ST_ON : UI_DEV_ST_OFF;
            break;
        }
        e->state = UI_DEV_ST_ON;
        if (e->level == 0) {
            e->level = -1; /* on at its last level, which only its report knows: "On" */
        }
        break;
    case ACT_OFF: e->state = UI_DEV_ST_OFF; break;
    case ACT_TOGGLE:
        e->state = is_on(d) ? UI_DEV_ST_OFF : UI_DEV_ST_ON;
        if (e->state == UI_DEV_ST_ON && e->level == 0) {
            e->level = -1;
        }
        break;
    case ACT_SET_LEVEL:
        e->level = pct;
        e->state = pct > 0 ? UI_DEV_ST_ON : UI_DEV_ST_OFF;
        break;
    case ACT_SET_TEMP: e->setpoint = value; break;
    case ACT_SET_MODE:
        if (d->state != (uint8_t)value) {
            e->hvac_action = DEV_HVAC_UNKNOWN; /* the new mode's word until it reports (BL-171) */
        }
        e->state = (uint8_t)value;
        break;
    case ACT_RUN:
        if (S.be.epoch_ms != NULL && S.be.epoch_ms(S.be.ctx) > 0) {
            e->since_ms = S.be.epoch_ms(S.be.ctx); /* "Ran at" now */
        }
        break;
    default: break;
    }
}

/* What was asked, over the device's report: the tile, its sheet and the note read it. */
static void expect_lay(const expect_t *e, ui_dev_t *d)
{
    d->state = e->state;
    d->level = e->level;
    d->hvac_action = e->hvac_action;
    d->setpoint = e->setpoint;
    d->since_ms = e->since_ms;
    d->status = UI_DEV_IDLE;
    d->action = ACT_NONE;
    d->unsure = 0u;
}

/* After each read of the list (poll): what was asked stays, or the report wins. */
static void expect_read(void)
{
    for (size_t i = 0; i < EXPECT_MAX; i++) {
        expect_t *e = &E[i];
        if (e->ref[0] == '\0') {
            continue;
        }
        ui_dev_t *d = find_mut(e->ref);
        if (d == NULL || d->status == UI_DEV_OFFLINE || lv_tick_elaps(e->at) > EXPECT_LIFE_MS) {
            expect_drop(e); /* gone, or offline: its tile says so (HCTL-006, BL-181) */
            continue;
        }
        const uint32_t sig = report_sig(d);
        e->status = d->status;
        if (d->status != e->was) {
            e->moved = true;
        }
        if (e->queued) {
            /* the newest wish waits behind the last change: whatever that one does, this one is
             * what the person wants now; its report starts from here */
            e->sig = sig;
            e->was = d->status;
            e->moved = false;
        } else if (d->status == UI_DEV_FAILED && (e->was != UI_DEV_FAILED || e->moved)) {
            expect_drop(e); /* refused, or not confirmed: the tile says so (HCTL-012) */
            continue;
        } else if (sig != e->sig) {
            expect_drop(e); /* a new report: it wins, whatever it says */
            continue;
        } else if (e->calling || d->status == UI_DEV_PENDING) {
            e->idle_at = 0u;
        } else if (e->idle_at == 0u) {
            e->idle_at = lv_tick_get() | 1u;
        } else if (lv_tick_elaps(e->idle_at) >= EXPECT_SETTLE_MS) {
            expect_drop(e); /* answered, and no new report: the last report again */
            continue;
        }
        expect_lay(e, d);
    }
}

/* What a refusal says: the one outcome the screen doesn't show by itself. */
static void refusal(ui_dev_result_t r)
{
    if (r == UI_DEV_REFUSED) {
        /* TR: Smart Home: the panel couldn't send that to the device */
        ui_toast(TR("That can't be done right now"));
    } else if (r == UI_DEV_BUSY) {
        /* TR: Smart Home: a change was asked of a device that is still doing the last one */
        ui_toast(TR("It is still busy with the last change"));
    } else if (r == UI_DEV_UNREACHABLE) {
        /* TR: Smart Home: a change was asked of a device that can't be reached */
        ui_toast(TR("It can't be reached right now"));
    }
}

static bool code_entered(const char *code);

static void code_sent(const ui_call_t *c)
{
    if (!ui_pad_asks(code_entered)) {
        D.polled = lv_tick_get() - POLL_VIEW_MS; /* the pad closed meanwhile: the tile says */
        return;
    }
    if (c->dev == UI_DEV_CODE_REFUSED) {
        ui_beep(UI_BEEP_ERROR);
        if (c->r == ALARM_ERR_NOT_AUTHORIZED && D.home_on && hc_home_protected(D.home, D.ask_ref)) {
            /* TR: the code pad's line when a code that isn't a master's was typed for a device
             * that asks for a master code (BL-220). At most 63 bytes */
            ui_pad_error(TR("Only a master code can change it"));
        } else {
            ui_pad_error(ui_result_text(c->r));
        }
        return;
    }
    ui_pad_cancel();
    refusal(c->dev); /* the code was good, the change wasn't sent: say so */
    D.polled = lv_tick_get() - POLL_VIEW_MS; /* the tile says what happened, now */
}

/* The pad stays open; the answer closes it (code_sent). */
static bool code_entered(const char *code)
{
    if (!ui_call_dev_act(D.ask_ref, D.ask_action, D.ask_value, code, code_sent, 0u)) {
        ui_pad_error(ui_result_text(ALARM_ERR_UNAVAILABLE));
    }
    return false;
}

/* The answer to an action sent without a code: one that needs a code asks for it now. The tile
 * list may have changed meanwhile (LINK-054), so it is found again by its ref. */
static void sent(const ui_call_t *c)
{
    expect_t *e = expect_find(c->ref);
    if (e != NULL && e->calling) {
        e->calling = false;
        if (c->dev != UI_DEV_OK) {
            expect_drop(e); /* not sent: the report shows again, and the toast says why */
        }
    }
    if (c->dev == UI_DEV_NEED_CODE) {
        const ui_dev_t *d = find(c->ref);
        if (d == NULL) {
            return; /* it went from the list meanwhile */
        }
        snprintf(D.ask_ref, sizeof(D.ask_ref), "%s", d->ref);
        D.ask_action = c->action;
        D.ask_value = c->value;
        char title[64];
        const bool guarded = !hc_needs_code((dev_kind_t)d->kind, (dev_action_t)c->action, c->value);
        if (guarded) {
            /* TR: the code pad's title before a change to a device the household set to ask for a
             * master code (BL-220); {0} is its name */
            I18N_FORMAT(title, sizeof(title), TR("Change {0}"), d->name);
        } else if (c->action == ACT_UNLOCK) {
            /* TR: the code pad's title before an unlock; {0} is the lock's name */
            I18N_FORMAT(title, sizeof(title), TR("Unlock {0}"), d->name);
        } else {
            /* TR: the code pad's title before opening a garage door, a cover or a valve */
            I18N_FORMAT(title, sizeof(title), TR("Open {0}"), d->name);
        }
        ui_pad_ask(HOST_MODAL, c->action == ACT_UNLOCK ? G_LOCK_OPEN : glyph(d), title, NULL,
                   guarded ? tone(d) : ui_tone_amber(), code_entered);
        if (guarded) {
            char said[UI_SUBJECT_STRING_LENGTH];
            /* TR: under the code pad's title: why it asks (BL-220); {0} is the device's name. At
             * most 127 bytes */
            I18N_FORMAT(said, sizeof(said), TR("{0} asks for a master code"), d->name);
            ui_pad_ask_note(said);
        }
        return;
    }
    refusal(c->dev);
    poll();
    show();
}

/* Sends what was asked of a device that has nothing under way (BL-219). */
static void expect_send(expect_t *e)
{
    e->queued = false;
    e->calling = true;
    e->was = e->status;
    e->moved = false;
    e->idle_at = 0u;
    e->at = lv_tick_get();
    char ref[DEV_REF_LEN]; /* the answer may clear the entry: its ref is kept here */
    memcpy(ref, e->ref, sizeof(ref));
    if (!ui_call_dev_act(ref, e->action, e->value, NULL, sent, 0u)) {
        expect_t *still = expect_find(ref);
        if (still != NULL) {
            expect_drop(still);
        }
        refusal(UI_DEV_BUSY);
        poll();
        show();
    }
}

/* The newest wishes go out once their devices have answered the last change. */
static void expect_flush(void)
{
    for (size_t i = 0; i < EXPECT_MAX; i++) {
        expect_t *e = &E[i];
        if (e->ref[0] != '\0' && e->queued && !e->calling && e->status != UI_DEV_PENDING) {
            expect_send(e);
        }
    }
}

/* Sends an action; one that needs a code asks for it first (HCTL-005). One that shows at once
 * shows now, and goes out now or once the last change is answered (BL-219). */
static void send(const ui_dev_t *d, uint8_t action, int32_t value)
{
    if (S.be.dev_act == NULL || action == ACT_NONE) {
        return;
    }
    expect_t *e = at_once(d, action) ? expect_find(d->ref) : NULL;
    if (e == NULL && at_once(d, action)) {
        for (size_t i = 0; i < EXPECT_MAX && e == NULL; i++) {
            if (E[i].ref[0] == '\0') {
                e = &E[i];
                snprintf(e->ref, sizeof(e->ref), "%s", d->ref);
                e->sig = report_sig(d); /* nothing laid over it yet: d is the report */
                e->was = e->status = d->status;
            }
        }
    }
    if (e == NULL) { /* one that is under way until it reports, or the table is full */
        if (!ui_call_dev_act(d->ref, action, value, NULL, sent, 0u)) {
            refusal(UI_DEV_BUSY);
        }
        return;
    }
    expect_shape(e, d, action, value);
    e->action = action;
    e->value = value;
    e->queued = true;
    e->at = lv_tick_get();
    ui_dev_t *shown = find_mut(e->ref);
    if (shown != NULL) {
        expect_lay(e, shown);
    }
    show();
    expect_flush();
}

static const tile_t *tile_of(lv_event_t *e)
{
    const size_t i = (size_t)(uintptr_t)lv_event_get_user_data(e);
    return i < D.ntiles ? &D.tiles[i] : NULL;
}

static bool has_position(const ui_dev_t *d);

/* What its source says it can do (HCTL-004, BL-171): a cover's Stop, a fan's speed, a light's
 * brightness or a cover's position. Not known: what its kind has, as before. */
static bool can_do(const ui_dev_t *d, uint8_t action)
{
    if (action == ACT_STOP && d->kind == DEV_COVER) {
        return (d->caps & UI_DEV_CAP_NO_STOP) == 0u;
    }
    if (action == ACT_SET_LEVEL && d->kind == DEV_FAN) {
        return (d->caps & UI_DEV_CAP_NO_SPEED) == 0u;
    }
    if (action == ACT_SET_LEVEL && (d->kind == DEV_LIGHT || d->kind == DEV_COVER)) {
        return has_position(d);
    }
    return true;
}

static void on_tile(lv_event_t *e)
{
    const tile_t *t = tile_of(e);
    if (D.arranging) {
        if (t != NULL) {
            arrange_pick(t->ref); /* a tap picks the tile: nothing is switched */
        }
        return;
    }
    const ui_dev_t *d = t != NULL ? find(t->ref) : NULL;
    if (d == NULL || d->status == UI_DEV_OFFLINE || under_way(d)) {
        return;
    }
    if (d->status == UI_DEV_FAILED &&
        (d->action == ACT_SET_LEVEL || d->action == ACT_SET_TEMP || d->action == ACT_SET_MODE)) {
        /* a failed change of a value (brightness, position, speed, set point, mode): the value that
         * failed isn't kept (hc_device_t, ui_dev_t carry only the action), so its sheet opens, where
         * it is set again, rather than a retry with the value it has now (HCTL-012). Before the
         * check of what it can do: a refresh that took the ability away keeps it failed, and the
         * tap still sends nothing. */
        sheet_open(d);
        return;
    }
    if (d->status == UI_DEV_FAILED && d->action != ACT_NONE && can_do(d, d->action)) {
        /* a tap tries again: the same action (HCTL-012); an "on" is a plain on, never brightness 0
         * (B030). One its source now says it can't do (a Stop on a cover that can't stop) isn't
         * retried: the tap does its own, which turns a moving cover back (BL-179). */
        send(d, d->action, NO_VALUE);
        return;
    }
    send(d, tap_action(d), NO_VALUE); /* a light comes on at its last brightness (B030) */
}

static void set_text(lv_obj_t *parent, const char *name, const char *text);

/* Offline (HCTL-006, BL-181): the name and the words stay at full opacity; a control nobody can use
 * now rests. What fades is the control with what it holds (style_disabled, 40 %, given once here
 * where it is made), in its neutral grey: no tone, no lit key, the switch off (the callers). Each
 * part a finger takes is disabled too, so LVGL gives it no press, no pressed colour and no click;
 * the handlers refuse offline all the same. In one step either way: nothing fades (UI-020). */
static void rest_style(lv_obj_t *dim)
{
    if (dim != NULL) {
        lv_obj_remove_style(dim, &style_disabled, LV_STATE_DISABLED); /* once: a sheet reuses its switch */
        lv_obj_add_style(dim, &style_disabled, LV_STATE_DISABLED);
    }
}

/* A label's words in c, set only when it changes: a style change lays the label out again, and a
 * shortened one would be written and drawn anew at every poll (BL-184). */
static void words_color(lv_obj_t *label, lv_color_t c)
{
    if (label != NULL && !lv_color_eq(lv_obj_get_style_text_color(label, LV_PART_MAIN), c)) {
        lv_obj_set_style_text_color(label, c, 0);
    }
}

static void unusable(lv_obj_t *o, bool off)
{
    if (o == NULL) {
        return;
    }
    if (off) {
        lv_obj_remove_state(o, LV_STATE_PRESSED); /* under a finger as it went: no release comes */
    }
    lv_obj_set_state(o, LV_STATE_DISABLED, off);
}

/* ---- the detail sheet (HCTL-004) ---------------------------------------------------------- */

/* What a device's sheet shows: the switch beside the name for what switches, then the controls its
 * kind has, each only when the device reports it supports it (the Home Assistant catalog carries
 * no attributes yet, so a brightness, a position or a thermostat's modes show where they are
 * known). A change goes out at once (dev_act); the note says under way and failed like the tile
 * (HCTL-006, HCTL-012). Nothing moves (UI-020); its words are written when the poll finds them
 * changed, at most once a second (UI-023). */
static UI_SLOW_BSS struct {
    char ref[DEV_REF_LEN]; /* the device shown, "" when no sheet is open */
    uint32_t shape;        /* what the controls were built for: rebuilt when it changes */
    lv_obj_t *obj;
    lv_obj_t *body;
    lv_obj_t *sw;    /* on/off beside the name, or NULL */
    lv_obj_t *level; /* brightness or position (the row), or NULL */
    lv_obj_t *step;  /* a thermostat's set point, or NULL */
    lv_obj_t *keys[KEYS_MAX];
    uint8_t act[KEYS_MAX];
    int32_t val[KEYS_MAX];
    uint8_t nkeys;
} X;

static bool sheet_open_now(void)
{
    return X.ref[0] != '\0';
}

static const ui_dev_t *sheet_dev(void)
{
    return sheet_open_now() ? find(X.ref) : NULL;
}

static bool has_switch(const ui_dev_t *d)
{
    /* a fan without speeds switches as a switch does (HCTL-004, BL-171) */
    return d->kind == DEV_LIGHT || d->kind == DEV_SWITCH ||
           (d->kind == DEV_FAN && (d->caps & UI_DEV_CAP_NO_SPEED) != 0u);
}

static bool has_setpoint(const ui_dev_t *d)
{
    return d->kind == DEV_CLIMATE && d->setpoint != UI_DEV_TEMP_UNKNOWN;
}

static bool has_position(const ui_dev_t *d)
{
    return (d->kind == DEV_LIGHT || d->kind == DEV_COVER) && (d->caps & UI_DEV_CAP_LEVEL) != 0u;
}

static uint32_t sheet_shape(const ui_dev_t *d)
{
    return (uint32_t)d->kind | ((uint32_t)d->caps << 8) | ((uint32_t)d->modes << 16) |
           (has_setpoint(d) ? 0x1000000u : 0u);
}

/* Whether a change may be sent now: the device answers and has nothing under way (a change that
 * shows at once waits as the newest wish, BL-219). */
static const ui_dev_t *sheet_ready(void)
{
    const ui_dev_t *d = sheet_dev();
    return d != NULL && d->status != UI_DEV_OFFLINE && !under_way(d) ? d : NULL;
}

static void sheet_closed(void)
{
    X.ref[0] = '\0';
    X.obj = NULL;
}

static void on_sheet_switch(lv_event_t *e)
{
    const ui_dev_t *d = sheet_ready();
    if (d != NULL) {
        send(d, lv_obj_has_state(lv_event_get_current_target(e), LV_STATE_CHECKED) ? ACT_ON : ACT_OFF,
             NO_VALUE); /* on at its last brightness (B030) */
    }
}

static void pct_text(char *out, size_t len, int32_t v)
{
    /* TR: a level in percent; {0} is the number. At most 12 bytes */
    I18N_FORMAT(out, len, TR_C("percent", "{0}%"), (int)v);
}

/* While the finger moves the level shows; it is sent when the finger lifts. */
static void on_sheet_level(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_current_target(e);
    char text[16];
    pct_text(text, sizeof(text), lv_slider_get_value(slider));
    set_text(X.level, "value_label", text);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        const ui_dev_t *d = sheet_ready();
        if (d != NULL) {
            send(d, ACT_SET_LEVEL, lv_slider_get_value(slider));
        }
    }
}

static void on_sheet_step(lv_event_t *e)
{
    const ui_dev_t *d = sheet_ready();
    if (d == NULL || d->setpoint == UI_DEV_TEMP_UNKNOWN) {
        return;
    }
    const bool up = lv_event_get_current_target(e) == ui_find(X.step, "plus");
    send(d, ACT_SET_TEMP, d->setpoint + (up ? 100 : -100)); /* a degree */
}

static void on_sheet_key(lv_event_t *e)
{
    const ui_dev_t *d = sheet_ready();
    lv_obj_t *key = lv_event_get_current_target(e);
    for (size_t k = 0; d != NULL && k < X.nkeys; k++) {
        if (X.keys[k] == key) {
            send(d, X.act[k], X.val[k]);
            return;
        }
    }
}

static void sheet_key(lv_obj_t *box, const char *g, const char *text, uint8_t act, int32_t val)
{
    if (X.nkeys >= KEYS_MAX) {
        return;
    }
    lv_obj_t *k = device_key_create(box, g != NULL ? g : "", text);
    if (g == NULL) {
        lv_obj_add_flag(ui_find(k, "icon"), LV_OBJ_FLAG_HIDDEN);
    }
    rest_style(box); /* the row fades as one offline (BL-181) */
    lv_obj_add_event_cb(k, on_sheet_key, LV_EVENT_CLICKED, NULL);
    X.keys[X.nkeys] = k;
    X.act[X.nkeys] = act;
    X.val[X.nkeys] = val;
    X.nkeys++;
}

static void sheet_slider(const char *g, const char *title)
{
    lv_obj_t *row = row_slider_create(X.body, g, title, "");
    X.level = row;
    lv_obj_t *slider = ui_find(row, "slider");
    rest_style(slider); /* offline only the slider fades: its title stays clear (BL-181) */
    lv_obj_add_event_cb(slider, on_sheet_level, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, on_sheet_level, LV_EVENT_RELEASED, NULL);
}

static void sheet_build(const ui_dev_t *d)
{
    lv_obj_clean(X.body);
    X.sw = NULL;
    X.level = NULL;
    X.step = NULL;
    X.nkeys = 0;
    memset(X.keys, 0, sizeof(X.keys));
    X.shape = sheet_shape(d);
    lv_obj_set_flag(ui_find(X.obj, "sheet_switch"), LV_OBJ_FLAG_HIDDEN, !has_switch(d));
    X.sw = has_switch(d) ? ui_find(X.obj, "sheet_switch") : NULL;
    if (X.sw != NULL) {
        rest_style(X.sw);
        lv_obj_remove_event_cb(X.sw, on_sheet_switch);
        lv_obj_add_event_cb(X.sw, on_sheet_switch, LV_EVENT_VALUE_CHANGED, NULL);
    }
    if (has_setpoint(d)) {
        X.step = device_setpoint_create(X.body, "", "");
        rest_style(X.step);
        lv_obj_add_event_cb(ui_find(X.step, "minus"), on_sheet_step, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(ui_find(X.step, "plus"), on_sheet_step, LV_EVENT_CLICKED, NULL);
    }
    if (d->kind == DEV_LIGHT && has_position(d)) {
        sheet_slider(G_SUN, TR("Brightness"));
    }
    if (d->kind == DEV_COVER && has_position(d)) {
        sheet_slider(G_BLINDS, TR_C("cover", "Position"));
    }
    if (d->kind == DEV_CLIMATE && d->modes != 0u) {
        ui_section(X.body, TR_C("thermostat", "Mode"));
        lv_obj_t *box = device_keys_create(X.body);
        if ((d->modes & UI_DEV_MODE_OFF) != 0u) {
            /* TR: a key of the thermostat sheet's modes, up to four in a row, each with an icon:
             * the same word as the mode on its tile, but a key has about 100 px for it. At most 9
             * characters */
            sheet_key(box, G_POWER, TR_C("thermostat mode", "Off"), ACT_SET_MODE, UI_DEV_ST_OFF);
        }
        if ((d->modes & UI_DEV_MODE_HEAT) != 0u) {
            /* TR: a key of the thermostat sheet's modes, up to four in a row, each with an icon:
             * the same word as the mode on its tile, but a key has about 100 px for it. At most 9
             * characters */
            sheet_key(box, G_FLAME, TR_C("thermostat mode", "Heat"), ACT_SET_MODE, UI_DEV_ST_HEAT);
        }
        if ((d->modes & UI_DEV_MODE_COOL) != 0u) {
            /* TR: a key of the thermostat sheet's modes, up to four in a row, each with an icon:
             * the same word as the mode on its tile, but a key has about 100 px for it. At most 9
             * characters */
            sheet_key(box, G_SNOW, TR_C("thermostat mode", "Cool"), ACT_SET_MODE, UI_DEV_ST_COOL);
        }
        if ((d->modes & UI_DEV_MODE_AUTO) != 0u) {
            /* TR: a key of the thermostat sheet's modes, up to four in a row, each with an icon:
             * the same word as the mode on its tile, but a key has about 100 px for it. At most 9
             * characters */
            sheet_key(box, G_SUNSNOW, TR_C("thermostat mode", "Auto"), ACT_SET_MODE, UI_DEV_ST_AUTO);
        }
    }
    lv_obj_t *box = NULL;
    switch (d->kind) {
    case DEV_COVER:
        box = device_keys_create(X.body);
        sheet_key(box, G_UP, TR_C("cover", "Open"), ACT_OPEN, NO_VALUE);
        if ((d->caps & UI_DEV_CAP_NO_STOP) == 0u) { /* where it can stop (BL-171) */
            sheet_key(box, G_STOP, TR_C("cover", "Stop"), ACT_STOP, NO_VALUE);
        }
        sheet_key(box, G_DOWN, TR_C("cover", "Close"), ACT_CLOSE, NO_VALUE);
        break;
    case DEV_VALVE:
        box = device_keys_create(X.body);
        sheet_key(box, G_UP, TR_C("cover", "Open"), ACT_OPEN, NO_VALUE);
        sheet_key(box, G_DOWN, TR_C("cover", "Close"), ACT_CLOSE, NO_VALUE);
        break;
    case DEV_LOCK:
        box = device_keys_create(X.body);
        sheet_key(box, G_LOCK, TR_C("lock", "Lock"), ACT_LOCK, NO_VALUE);
        sheet_key(box, G_LOCK_OPEN, TR_C("lock", "Unlock"), ACT_UNLOCK, NO_VALUE);
        break;
    case DEV_FAN:
        if ((d->caps & UI_DEV_CAP_NO_SPEED) != 0u) {
            break; /* no speeds: the switch beside the name is all (BL-171) */
        }
        box = device_keys_create(X.body);
        sheet_key(box, NULL, fan_speed(0), ACT_OFF, NO_VALUE);
        for (size_t s = 1; s < 4u; s++) {
            sheet_key(box, NULL, fan_speed(FAN_SPEED[s]), ACT_SET_LEVEL, FAN_SPEED[s]);
        }
        break;
    case DEV_SCENE:
    case DEV_SCRIPT:
        box = device_keys_create(X.body);
        sheet_key(box, G_SPARKLES, TR_C("scene", "Run"), ACT_RUN, NO_VALUE);
        break;
    default: break;
    }
}

static void sheet_update(const ui_dev_t *d)
{
    bool refit = false; /* the header or the body changed height: the body fits the card again */
    if (sheet_shape(d) != X.shape) {
        sheet_build(d);
        refit = true;
    }
    /* the whole name, wrapped (BL-184): the subject holds it as given, so this compares it whole */
    if (strcmp(lv_subject_get_string(&subject_sheet_title), d->name) != 0) {
        ui_set_str(&subject_sheet_title, d->name);
        refit = true;
    }
    const lv_color_t c = tone(d);
    const bool offline = d->status == UI_DEV_OFFLINE;
    lv_obj_t *chip = ui_find(X.obj, "sheet_chip");
    ui_chip_tone(chip, glyph(d), c);
    /* Offline (BL-181): the card, its words and its sections at full opacity, the chip at 70 %,
     * the controls resting (unusable); Close works. An open sheet that loses its device stays
     * readable, and gets its controls back in one step when it answers again. */
    lv_obj_set_style_opa(chip, offline ? OPA_DEVICE_OFFLINE_CHIP : LV_OPA_COVER, 0);
    char words[64];
    tile_words(d, words, sizeof(words));
    if (strcmp(lv_subject_get_string(&subject_sheet_note), words) != 0) {
        ui_set_str(&subject_sheet_note, words); /* wrapped whole: it may take another line */
        refit = true;
    }
    const bool warn = d->status == UI_DEV_FAILED || (amber(d) && !under_way(d) && !offline);
    words_color(ui_find(X.obj, "sheet_state"), warn ? ui_tone_amber() : COLOR_TEXT_2);
    if (X.sw != NULL) {
        unusable(X.sw, offline);
        lv_obj_set_state(X.sw, LV_STATE_CHECKED, is_on(d) && !offline); /* offline: no state shown */
    }
    if (X.level != NULL) {
        lv_obj_t *slider = ui_find(X.level, "slider");
        unusable(slider, offline);
        /* offline, the row's chip and the fill in grey; else the row's own blue (row_slider.xml) */
        lv_obj_t *row_chip = lv_obj_get_child(lv_obj_get_child(X.level, 0), 0);
        if (offline) {
            ui_chip_tone(row_chip, NULL, ui_tone_gray());
            lv_obj_set_style_bg_color(slider, ui_tone_gray(), LV_PART_INDICATOR);
        } else {
            ui_chip_tone(row_chip, NULL, ui_tone_blue());
            lv_obj_remove_local_style_prop(slider, LV_STYLE_BG_COLOR, LV_PART_INDICATOR);
        }
        if (!lv_obj_has_state(slider, LV_STATE_PRESSED)) {
            const int32_t v = d->level >= 0 ? d->level : (is_on(d) ? 100 : 0);
            const int32_t shown = d->kind == DEV_LIGHT && !is_on(d) ? 0 : v;
            char text[16];
            lv_slider_set_value(slider, shown, LV_ANIM_OFF);
            pct_text(text, sizeof(text), shown);
            set_text(X.level, "value_label", text);
        }
    }
    if (X.step != NULL) {
        unusable(X.step, offline);
        unusable(ui_find(X.step, "minus"), offline);
        unusable(ui_find(X.step, "plus"), offline);
        char v[16];
        degrees(d->setpoint, v, sizeof(v));
        set_text(X.step, "value_label", v);
        set_text(X.step, "what_label",
                 d->state == UI_DEV_ST_HEAT   ? TR_C("thermostat", "Heat to")
                 : d->state == UI_DEV_ST_COOL ? TR_C("thermostat", "Cool to")
                                              : TR_C("thermostat", "Set to"));
        lv_obj_set_style_text_color(ui_find(X.step, "value_label"), d->state == UI_DEV_ST_OFF ? COLOR_TEXT_2 : c, 0);
    }
    for (size_t k = 0; k < X.nkeys; k++) {
        const uint8_t a = X.act[k];
        /* offline, no key is lit: the reported mode or state is kept and lights its key again
         * when the device answers (BL-181) */
        const bool lit = !offline &&
                         ((a == ACT_SET_MODE && d->state == (uint8_t)X.val[k]) ||
                          (d->kind == DEV_LOCK && ((a == ACT_LOCK && d->state == UI_DEV_ST_LOCKED) ||
                                                   (a == ACT_UNLOCK && d->state == UI_DEV_ST_UNLOCKED))) ||
                          (d->kind == DEV_VALVE && ((a == ACT_OPEN && d->state == UI_DEV_ST_OPEN) ||
                                                    (a == ACT_CLOSE && d->state == UI_DEV_ST_CLOSED))) ||
                          (d->kind == DEV_FAN &&
                           ((a == ACT_OFF && !is_on(d)) ||
                            (a == ACT_SET_LEVEL && is_on(d) && d->level >= 0 &&
                             ((X.val[k] == 33 && d->level <= 34) ||
                              (X.val[k] == 67 && d->level > 34 && d->level <= 67) ||
                              (X.val[k] == 100 && d->level > 67))))));
        lv_obj_t *key = X.keys[k];
        unusable(lv_obj_get_parent(key), offline);
        unusable(key, offline);
        if (lit) {
            lv_obj_set_style_bg_color(key, c, 0);
            lv_obj_set_style_bg_opa(key, LV_OPA_20, 0);
            lv_obj_set_style_text_color(key, c, 0);
        } else {
            lv_obj_remove_local_style_prop(key, LV_STYLE_BG_COLOR, 0);
            lv_obj_remove_local_style_prop(key, LV_STYLE_BG_OPA, 0);
            lv_obj_remove_local_style_prop(key, LV_STYLE_TEXT_COLOR, 0);
        }
    }
    if (refit) {
        /* a long name and a tall body: the body scrolls under the header, Close stays (BL-184) */
        ui_sheet_body_fit(X.body);
    }
}

static void sheet_open(const ui_dev_t *d)
{
    lv_obj_t *obj = ui_find(S.main, "device_sheet");
    if (obj == NULL) {
        return;
    }
    ui_sheet_open(SHEET_DEVICE, NULL, sheet_closed); /* closes whatever was open first */
    X.obj = obj;
    X.body = ui_find(obj, "device_body");
    X.shape = UINT32_MAX;
    i18n_utf8_copy(X.ref, sizeof(X.ref), d->ref);
    ui_set_str(&subject_sheet_title, d->name);
    ui_set_str(&subject_sheet_note, "");
    sheet_update(d);
    if (X.body != NULL) {
        /* a refit keeps the body's scroll (ui_sheet_body_fit): a sheet that opens starts at its top */
        lv_obj_scroll_to_y(X.body, 0, LV_ANIM_OFF);
    }
}

/* The arrow, or a long press on any tile. */
static void on_more(lv_event_t *e)
{
    const tile_t *t = tile_of(e);
    const ui_dev_t *d = t != NULL && !D.arranging ? find(t->ref) : NULL;
    if (d != NULL) {
        sheet_open(d);
    }
}

static void on_level(lv_event_t *e)
{
    const tile_t *t = tile_of(e);
    const ui_dev_t *d = t != NULL && !D.arranging ? find(t->ref) : NULL;
    if (d != NULL && t->level != NULL && d->status != UI_DEV_OFFLINE) {
        send(d, ACT_SET_LEVEL, lv_slider_get_value(t->level));
    }
}

static void on_step(lv_event_t *e)
{
    const tile_t *t = tile_of(e);
    const ui_dev_t *d = t != NULL && !D.arranging ? find(t->ref) : NULL;
    if (d == NULL || d->setpoint == UI_DEV_TEMP_UNKNOWN || d->status == UI_DEV_OFFLINE) {
        return;
    }
    const bool up = lv_event_get_current_target(e) == ui_find(t->step, "plus");
    send(d, ACT_SET_TEMP, d->setpoint + (up ? 100 : -100)); /* a degree */
}

static void on_key(lv_event_t *e)
{
    const tile_t *t = tile_of(e);
    const ui_dev_t *d = t != NULL && !D.arranging ? find(t->ref) : NULL;
    if (d == NULL || d->status == UI_DEV_OFFLINE) {
        return;
    }
    lv_obj_t *key = lv_event_get_current_target(e);
    size_t k = 0;
    while (k < t->nkeys && t->keys[k] != key) {
        k++;
    }
    if (k < t->nkeys) {
        send(d, t->act[k], t->val[k]); /* the key's own: a cover without Stop has two (BL-171) */
    }
}

/* ---- the tiles ---------------------------------------------------------------------------- */

static bool has_sheet(const ui_dev_t *d)
{
    switch (d->kind) {
    case DEV_SCENE:
    case DEV_SCRIPT:
    case DEV_SWITCH: return false;
    case DEV_LIGHT: return (d->caps & UI_DEV_CAP_LEVEL) != 0u;
    case DEV_FAN: return (d->caps & UI_DEV_CAP_NO_SPEED) == 0u; /* without speeds: as a switch */
    default: return true;
    }
}

static void add_key(tile_t *t, size_t index, const char *g, const char *text, uint8_t act,
                    int32_t val)
{
    if (t->nkeys >= KEYS_MAX) {
        return;
    }
    t->act[t->nkeys] = act;
    t->val[t->nkeys] = val;
    lv_obj_t *k = device_key_create(lv_obj_get_child(t->obj, -1), g != NULL ? g : "", text);
    if (g == NULL) {
        lv_obj_add_flag(ui_find(k, "icon"), LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_event_cb(k, on_key, LV_EVENT_CLICKED, (void *)(uintptr_t)index);
    t->keys[t->nkeys++] = k;
}

/* A tile's dashed edge while arranging (the picked one has its blue edge instead): dashes along
 * the sides and the corners' arcs, in the tile's own radius. */
static void on_tile_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    const tile_t *t = tile_of(e);
    if (t == NULL || t->picked) {
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const int32_t in = DEVICE_EDGE_W / 2;
    const int32_t r = lv_obj_get_style_radius(obj, LV_PART_MAIN);
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = COLOR_TEXT_3;
    line.width = DEVICE_EDGE_W;
    line.dash_width = DEVICE_DASH_W;
    line.dash_gap = DEVICE_DASH_GAP;
    const int32_t x1 = a.x1 + in;
    const int32_t x2 = a.x2 - in;
    const int32_t y1 = a.y1 + in;
    const int32_t y2 = a.y2 - in;
    const int32_t sides[4][4] = {{a.x1 + r, y1, a.x2 - r, y1},
                                 {a.x1 + r, y2, a.x2 - r, y2},
                                 {x1, a.y1 + r, x1, a.y2 - r},
                                 {x2, a.y1 + r, x2, a.y2 - r}};
    for (size_t i = 0; i < 4u; i++) {
        line.p1.x = (lv_value_precise_t)sides[i][0];
        line.p1.y = (lv_value_precise_t)sides[i][1];
        line.p2.x = (lv_value_precise_t)sides[i][2];
        line.p2.y = (lv_value_precise_t)sides[i][3];
        lv_draw_line(layer, &line);
    }
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.color = COLOR_TEXT_3;
    arc.width = DEVICE_EDGE_W;
    arc.radius = (uint16_t)r;
    const int32_t corners[4][4] = {{a.x1 + r, a.y1 + r, 180, 270},
                                   {a.x2 - r + 1, a.y1 + r, 270, 360},
                                   {a.x2 - r + 1, a.y2 - r + 1, 0, 90},
                                   {a.x1 + r, a.y2 - r + 1, 90, 180}};
    for (size_t i = 0; i < 4u; i++) {
        arc.center.x = corners[i][0];
        arc.center.y = corners[i][1];
        arc.start_angle = (lv_value_precise_t)corners[i][2];
        arc.end_angle = (lv_value_precise_t)corners[i][3];
        lv_draw_arc(layer, &arc);
    }
}

/* Nothing under o takes a touch: LVGL finds a clickable child even under a parent that isn't,
 * so a slider, a stepper's keys and a fan's or a cover's keys rest one by one. */
static void rest(lv_obj_t *o)
{
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    for (uint32_t c = 0; c < lv_obj_get_child_count(o); c++) {
        rest(lv_obj_get_child(o, (int32_t)c));
    }
}

/* Arranging: a grip in the corner where the arrow was, a dashed edge, the controls at rest. */
static void arrange_tile(tile_t *t, size_t index)
{
    lv_obj_add_style(t->obj, &style_device_arrange, 0);
    lv_obj_t *top = ui_find(t->obj, "top");
    lv_obj_t *more = ui_find(t->obj, "more");
    for (uint32_t c = 0; c < lv_obj_get_child_count(t->obj); c++) {
        lv_obj_t *child = lv_obj_get_child(t->obj, (int32_t)c);
        rest(child);
        if (child != top && child != more) { /* the controls, dimmed */
            lv_obj_set_style_opa(child, LV_OPA_40, 0);
        }
    }
    lv_obj_t *grip = lv_label_create(t->obj);
    lv_obj_add_flag(grip, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_text_font(grip, icons_20, 0);
    lv_obj_set_style_text_color(grip, COLOR_TEXT_2, 0);
    lv_label_set_text(grip, G_GRIP);
    lv_obj_align(grip, LV_ALIGN_TOP_RIGHT, -DEVICE_GRIP_X, 0);
    lv_obj_set_style_translate_y(grip, DEVICE_GRIP_Y, 0);
    lv_obj_add_event_cb(t->obj, on_tile_draw, LV_EVENT_DRAW_POST, (void *)(uintptr_t)index);
}

static void tile_create(size_t index, const ui_dev_t *d, bool wide)
{
    tile_t *t = &D.tiles[index];
    memset(t, 0, sizeof(*t));
    snprintf(t->ref, sizeof(t->ref), "%s", d->ref);
    snprintf(t->name, sizeof(t->name), "%s", d->name);
    t->wide = wide;
    t->obj = device_tile_create(D.grid, glyph(d), t->name, t->words);
    lv_obj_add_event_cb(t->obj, on_tile, LV_EVENT_SHORT_CLICKED, (void *)(uintptr_t)index);
    lv_obj_add_event_cb(t->obj, on_more, LV_EVENT_LONG_PRESSED, (void *)(uintptr_t)index);
    /* arranging: a long press lifts the tile and the finger drags it (HCTL-008) */
    lv_obj_add_event_cb(t->obj, on_tile_hold, LV_EVENT_LONG_PRESSED, (void *)(uintptr_t)index);
    lv_obj_add_event_cb(t->obj, on_tile_hold, LV_EVENT_PRESSING, (void *)(uintptr_t)index);
    lv_obj_add_event_cb(t->obj, on_tile_hold, LV_EVENT_RELEASED, (void *)(uintptr_t)index);
    lv_obj_add_event_cb(t->obj, on_tile_hold, LV_EVENT_PRESS_LOST, (void *)(uintptr_t)index);
    /* a remote pointer that lets go mid-press resets the indev: LVGL sends only this (B004) */
    lv_obj_add_event_cb(t->obj, on_tile_hold, LV_EVENT_INDEV_RESET, (void *)(uintptr_t)index);
    lv_obj_add_event_cb(ui_find(t->obj, "more"), on_more, LV_EVENT_CLICKED, (void *)(uintptr_t)index);
    lv_obj_t *top = ui_find(t->obj, "top");
    if (D.home_on && !D.arranging && hc_home_protected(D.home, d->ref)) {
        /* BL-220: it asks for a master code before a change - a key beside the arrow */
        lv_obj_t *key = lv_label_create(t->obj);
        lv_obj_add_flag(key, LV_OBJ_FLAG_FLOATING);
        lv_obj_remove_flag(key, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_text_font(key, icons_20, 0);
        lv_obj_set_style_text_color(key, COLOR_TEXT_3, 0);
        lv_label_set_text(key, G_KEY);
        lv_obj_align(key, LV_ALIGN_TOP_RIGHT, -(DEVICE_GRIP_X + 44), 0);
        lv_obj_set_style_translate_y(key, DEVICE_GRIP_Y, 0);
        if (wide) {
            lv_obj_set_style_pad_right(top, lv_obj_get_style_pad_right(top, LV_PART_MAIN) + 32, 0);
        }
    }
    if (!wide) {
        /* one cell: the chip over the words, the words at the bottom */
        lv_obj_set_flex_flow(top, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_flex_grow(top, 1);
        lv_obj_set_style_pad_right(top, 0, 0);
        lv_obj_t *words = ui_find(t->obj, "words");
        lv_obj_set_flex_grow(words, 0);
        lv_obj_set_width(words, lv_pct(100));
        if (D.arranging) {
            arrange_tile(t, index);
        }
        return;
    }
    lv_obj_set_width(t->obj, DEVICE_WIDE_W);
    switch (d->kind) {
    case DEV_LIGHT:
        if ((d->caps & UI_DEV_CAP_LEVEL) != 0u) {
            lv_obj_t *box = device_level_create(t->obj, 0);
            rest_style(box); /* offline the bar fades with its icon (BL-181) */
            t->level = ui_find(box, "level");
            lv_obj_add_event_cb(t->level, on_level, LV_EVENT_RELEASED, (void *)(uintptr_t)index);
        }
        break;
    case DEV_CLIMATE:
        t->step = device_stepper_create(t->obj, "", ""); /* hidden without a set point (tile_update) */
        rest_style(t->step);
        lv_obj_add_event_cb(ui_find(t->step, "minus"), on_step, LV_EVENT_CLICKED, (void *)(uintptr_t)index);
        lv_obj_add_event_cb(ui_find(t->step, "plus"), on_step, LV_EVENT_CLICKED, (void *)(uintptr_t)index);
        break;
    case DEV_COVER:
        rest_style(device_keys_create(t->obj));
        add_key(t, index, G_UP, TR_C("cover", "Open"), ACT_OPEN, NO_VALUE);
        if ((d->caps & UI_DEV_CAP_NO_STOP) == 0u) { /* where it can stop (BL-171) */
            add_key(t, index, G_STOP, TR_C("cover", "Stop"), ACT_STOP, NO_VALUE);
        }
        add_key(t, index, G_DOWN, TR_C("cover", "Close"), ACT_CLOSE, NO_VALUE);
        break;
    case DEV_FAN:
        if ((d->caps & UI_DEV_CAP_NO_SPEED) != 0u) {
            break; /* no speeds: a tap switches it, as a switch (BL-171) */
        }
        rest_style(device_keys_create(t->obj));
        add_key(t, index, NULL, fan_speed(FAN_SPEED[0]), ACT_OFF, NO_VALUE);
        for (size_t s = 1; s < 4u; s++) {
            add_key(t, index, NULL, fan_speed(FAN_SPEED[s]), ACT_SET_LEVEL, FAN_SPEED[s]);
        }
        break;
    default: break;
    }
    if (D.arranging) {
        arrange_tile(t, index);
    }
}

static void set_text(lv_obj_t *parent, const char *name, const char *text)
{
    lv_obj_t *l = ui_find(parent, name);
    if (l != NULL && strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
}

/* A tile's label given text only when it differs from what it was given last (cache), not from
 * what it shows: a shortened label shows its own "..." (BL-184). */
static void tile_text(tile_t *t, const char *name, char *cache, size_t len, const char *text)
{
    if (strcmp(cache, text) != 0) {
        i18n_utf8_copy(cache, len, text);
        lv_label_set_text(ui_find(t->obj, name), cache);
    }
}

/* The name's lines (BL-184): one on a one-cell tile and on a wide tile with a slider, keys or a
 * thermostat's stepper under it; two on a wide tile without one (a thermostat holding a range hides
 * its stepper, so the name gets its second line back). Whole lines of font_label, so LVGL ends the
 * last one with "..."; the name, the state and the control never overlap. */
static void tile_name_lines(tile_t *t)
{
    const bool control = t->level != NULL || t->nkeys > 0u ||
                         (t->step != NULL && !lv_obj_has_flag(t->step, LV_OBJ_FLAG_HIDDEN));
    const int32_t h = t->wide && !control ? DEVICE_NAME_TWO_H : DEVICE_NAME_H;
    lv_obj_t *title = ui_find(t->obj, "title_label");
    if (lv_obj_get_style_height(title, LV_PART_MAIN) != h) {
        lv_obj_set_height(title, h);
    }
}

static void tile_update(tile_t *t, const ui_dev_t *d)
{
    const lv_color_t c = tone(d);
    const bool offline = d->status == UI_DEV_OFFLINE;
    const bool pending = under_way(d);
    lv_obj_t *chip = ui_find(t->obj, "chip");
    ui_chip_tone(chip, glyph(d), c);
    /* Under way, the chip at 40 %. Offline (BL-181), the tile stays at full opacity, its name and
     * since when as readable as any tile's; the chip at 70 % and the controls resting say it can't
     * be used, and pressed it keeps its colour: a tap does nothing (a long press opens its sheet). */
    lv_obj_set_style_opa(chip, pending ? LV_OPA_40 : offline ? OPA_DEVICE_OFFLINE_CHIP : LV_OPA_COVER, 0);
    if (t->still != offline) {
        t->still = offline;
        if (offline) {
            lv_obj_add_style(t->obj, &style_device_still, LV_STATE_PRESSED);
        } else {
            lv_obj_remove_style(t->obj, &style_device_still, LV_STATE_PRESSED);
        }
    }
    tile_text(t, "title_label", t->name, sizeof(t->name), d->name);
    char words[sizeof(t->words)];
    tile_words(d, words, sizeof(words));
    tile_text(t, "detail_label", t->words, sizeof(t->words), words);
    const bool warn = d->status == UI_DEV_FAILED || (amber(d) && !pending && !offline);
    words_color(ui_find(t->obj, "detail_label"), warn ? ui_tone_amber() : COLOR_TEXT_2);
    lv_obj_set_flag(ui_find(t->obj, "more"), LV_OBJ_FLAG_HIDDEN, offline || !has_sheet(d) || D.arranging);
    if (D.arranging && t->picked != (strcmp(D.pick, t->ref) == 0)) {
        t->picked = !t->picked;
        if (t->picked) {
            lv_obj_add_style(t->obj, &style_device_picked, 0);
        } else {
            lv_obj_remove_style(t->obj, &style_device_picked, 0);
        }
        lv_obj_invalidate(t->obj);
    }
    if (t->level != NULL) {
        unusable(lv_obj_get_parent(t->level), offline);
        unusable(t->level, offline);
    }
    if (t->level != NULL && !lv_obj_has_state(t->level, LV_STATE_PRESSED)) {
        const int32_t v = d->level >= 0 ? d->level : (is_on(d) ? 100 : 0);
        lv_slider_set_value(t->level, is_on(d) ? v : 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(t->level, is_on(d) && !offline ? ui_tone_blue() : ui_tone_gray(),
                                  LV_PART_INDICATOR);
    }
    if (t->step != NULL) {
        unusable(t->step, offline);
        unusable(ui_find(t->step, "minus"), offline);
        unusable(ui_find(t->step, "plus"), offline);
        /* no single set point (it holds a range): no - and +, as on its sheet; hidden, not made
         * again, so a change of mode doesn't rebuild the grid (on_step ignores it too) */
        lv_obj_set_flag(t->step, LV_OBJ_FLAG_HIDDEN, d->setpoint == UI_DEV_TEMP_UNKNOWN);
        char v[16];
        if (d->setpoint != UI_DEV_TEMP_UNKNOWN) {
            degrees(d->setpoint, v, sizeof(v));
        } else {
            snprintf(v, sizeof(v), "%s", "–");
        }
        set_text(t->step, "value_label", v);
        set_text(t->step, "what_label",
                 d->state == UI_DEV_ST_HEAT   ? TR_C("thermostat", "Heat to")
                 : d->state == UI_DEV_ST_COOL ? TR_C("thermostat", "Cool to")
                                              : TR_C("thermostat", "Set to"));
    }
    tile_name_lines(t); /* after the stepper came or went: one line of name or two */
    int active = -1;
    if (d->kind == DEV_FAN && t->nkeys > 0u && !offline) { /* offline none is lit (BL-181) */
        active = !is_on(d) ? 0 : d->level < 0 ? -1 : d->level <= 34 ? 1 : d->level <= 67 ? 2 : 3;
    } /* a cover's keys are actions, not a state: none is lit (n-devices-outside) */
    for (size_t k = 0; k < t->nkeys; k++) {
        lv_obj_t *key = t->keys[k];
        unusable(lv_obj_get_parent(key), offline);
        unusable(key, offline);
        if ((int)k == active) {
            lv_obj_set_style_bg_color(key, c, 0);
            lv_obj_set_style_bg_opa(key, LV_OPA_20, 0);
            lv_obj_set_style_text_color(key, c, 0);
        } else {
            lv_obj_remove_local_style_prop(key, LV_STYLE_BG_COLOR, 0);
            lv_obj_remove_local_style_prop(key, LV_STYLE_BG_OPA, 0);
            lv_obj_remove_local_style_prop(key, LV_STYLE_TEXT_COLOR, 0);
        }
    }
}

static tile_t *tile_for(const char *ref)
{
    for (size_t i = 0; i < D.ntiles; i++) {
        if (strcmp(D.tiles[i].ref, ref) == 0) {
            return &D.tiles[i];
        }
    }
    return NULL;
}

/* Places the tiles in four columns of device_cell_w, rows of device_cell_h, in the page's order
 * (a drag reorders the page, not the tiles); a one-cell tile fills a gap a wide one left at a row's
 * end. Nothing slides: they are put. */
static void place(void)
{
    uint8_t rows[UI_DEVL_MAX]; /* the cells taken, a bit each */
    memset(rows, 0, sizeof(rows));
    for (size_t p = 0; p < D.npage; p++) {
        const tile_t *t = tile_for(D.devs[D.page[p]].ref);
        if (t == NULL) {
            continue;
        }
        const unsigned w = t->wide ? 2u : 1u;
        const unsigned mask = w == 2u ? 3u : 1u;
        for (size_t r = 0; r < UI_DEVL_MAX; r++) {
            unsigned c = 0;
            while (c + w <= COLS && (rows[r] & (mask << c)) != 0u) {
                c++;
            }
            if (c + w <= COLS) {
                rows[r] = (uint8_t)(rows[r] | (mask << c));
                lv_obj_set_pos(t->obj, (int32_t)(c * (DEVICE_CELL_W + DEVICE_GAP)),
                               (int32_t)(r * (DEVICE_CELL_H + DEVICE_GAP)));
                break;
            }
        }
    }
}

/* ---- Arrange (HCTL-008) ------------------------------------------------------------------- */

static const char *page_area(void)
{
    return D.tab_favorites ? NULL : D.tab;
}

static size_t pick_place(void)
{
    for (size_t p = 0; p < D.npage; p++) {
        if (strcmp(D.devs[D.page[p]].ref, D.pick) == 0) {
            return p;
        }
    }
    return D.npage;
}

/* The header: the page's name or the picked tile's, the actions that apply, the arrows that can move. */
static void arrange_chrome(void)
{
    if (D.pick[0] != '\0' && pick_place() == D.npage) {
        D.pick[0] = '\0'; /* the device left the page */
    }
    const bool a = D.arranging;
    const bool picked = a && D.pick[0] != '\0';
    lv_obj_add_flag(D.head, LV_OBJ_FLAG_HIDDEN); /* no title over the page (BL-220 R7) */
    lv_obj_set_flag(D.spacer, LV_OBJ_FLAG_HIDDEN, a);
    lv_obj_set_flag(D.tabs, LV_OBJ_FLAG_HIDDEN, a);
    lv_obj_set_flag(D.pencil, LV_OBJ_FLAG_HIDDEN, a);
    lv_obj_set_flag(D.arr_head, LV_OBJ_FLAG_HIDDEN, !a);
    /* Add: to Favorites, or (BL-220) into the room whose page this is */
    lv_obj_set_flag(D.arr_add, LV_OBJ_FLAG_HIDDEN,
                    !(a && !picked && (D.tab_favorites || room_of_tab() >= 0)));
    lv_obj_set_flag(D.arr_rooms, LV_OBJ_FLAG_HIDDEN, !(a && !picked && D.home_on));
    lv_obj_set_flag(D.arr_room, LV_OBJ_FLAG_HIDDEN, !(picked && D.home_on));
    lv_obj_set_flag(D.arr_prev, LV_OBJ_FLAG_HIDDEN, !picked);
    lv_obj_set_flag(D.arr_next, LV_OBJ_FLAG_HIDDEN, !picked);
    lv_obj_set_flag(D.arr_size, LV_OBJ_FLAG_HIDDEN, !picked);
    lv_obj_set_flag(D.arr_remove, LV_OBJ_FLAG_HIDDEN, !(picked && D.tab_favorites));
    lv_obj_set_flag(D.arr_done, LV_OBJ_FLAG_HIDDEN, !a);
    if (!a) {
        return;
    }
    char title[DEV_NAME_LEN + 40];
    const ui_dev_t *pd = picked ? find(D.pick) : NULL;
    if (pd != NULL) {
        i18n_utf8_copy(title, sizeof(title), pd->name);
    } else if (D.tab_favorites) {
        /* TR: Smart Home's title while arranging Favorites */
        i18n_utf8_copy(title, sizeof(title), TR("Arrange Favorites"));
    } else {
        const char *room = D.tab;
        if (room[0] == '\0') {
            room = no_room_tab();
        }
        /* TR: Smart Home's title while arranging a room's page; {0} is the room. At most 63
         * bytes */
        I18N_FORMAT(title, sizeof(title), TR("Arrange {0}"), room);
    }
    lv_label_set_text(lv_obj_get_child(D.arr_head, 0), title);
    if (pd != NULL) {
        const size_t p = pick_place();
        lv_obj_set_flag(D.arr_prev, LV_OBJ_FLAG_CLICKABLE, p > 0u);
        lv_obj_set_flag(D.arr_next, LV_OBJ_FLAG_CLICKABLE, p + 1u < D.npage);
        lv_obj_set_style_opa(D.arr_prev, p > 0u ? LV_OPA_COVER : LV_OPA_40, 0);
        lv_obj_set_style_opa(D.arr_next, p + 1u < D.npage ? LV_OPA_COVER : LV_OPA_40, 0);
        const bool wide = ui_devl_wide(D.layout, pd);
        for (uint32_t i = 0; i < 2u; i++) {
            lv_obj_t *seg = lv_obj_get_child(D.arr_size, (int32_t)i);
            if (seg == NULL) {
                continue;
            }
            if ((i == 1u) == wide) {
                lv_obj_add_style(seg, &style_segment_active, 0);
            } else {
                lv_obj_remove_style(seg, &style_segment_active, 0);
            }
        }
    }
    note();
}

/* Something changed in the working layout: put the tiles again (show() keeps the grid's scroll and
 * the picked tile in view while arranging). */
static void arrange_changed(void)
{
    D.dirty = true;
    D.page_sig = 0;
    show();
    arrange_chrome();
}

static void arrange_pick(const char *ref)
{
    snprintf(D.pick, sizeof(D.pick), "%s", ref);
    for (size_t i = 0; i < D.ntiles; i++) {
        const ui_dev_t *d = find(D.tiles[i].ref);
        if (d != NULL) {
            tile_update(&D.tiles[i], d); /* the blue edge */
        }
    }
    arrange_chrome();
}

static void arrange_begin(void)
{
    if (!D.ready || D.n == 0u || D.arranging) {
        return;
    }
    D.arranging = true;
    D.dirty = false;
    D.pick[0] = '\0';
    D.page_sig = 0;
    show();
    arrange_chrome();
}

/* A code of a master (system or partition) or the installer's (BL-220 D6). */
static bool master_user(uint8_t user)
{
    if (user == 0u || user > ALARM_MAX_USERS || S.cfg == NULL) {
        return false;
    }
    const alarm_authority_t a = S.cfg->users[user - 1u].authority;
    return a == ALARM_AUTH_MASTER || a == ALARM_AUTH_PARTITION_MASTER || a == ALARM_AUTH_INSTALLER;
}

static bool arrange_code_entered(const char *code);

static void arrange_code_checked(const ui_call_t *c)
{
    if (!ui_pad_asks(arrange_code_entered)) {
        return; /* the pad closed meanwhile */
    }
    if (c->r != ALARM_OK) {
        ui_beep(UI_BEEP_ERROR);
        ui_pad_error(ui_result_text(c->r));
        return;
    }
    if (!master_user(c->user)) {
        ui_beep(UI_BEEP_ERROR);
        /* TR: the code pad's line when a code that isn't a master's was typed to arrange Smart
         * Home (BL-220). At most 63 bytes */
        ui_pad_error(TR("Only a master code can arrange it"));
        return;
    }
    ui_pad_cancel();
    arrange_begin();
}

static bool arrange_code_entered(const char *code)
{
    if (!ui_call_authenticate(code, arrange_code_checked, 0u)) {
        ui_pad_error(ui_result_text(ALARM_ERR_UNAVAILABLE));
    }
    return false;
}

/* The pencil: Arrange, after a master's code where Settings > Smart Home asks for one (BL-220). */
static void on_arrange_start(lv_event_t *e)
{
    (void)e;
    if (!D.ready || D.n == 0u || D.arranging) {
        return;
    }
    if (D.home_on && hc_home_arrange_code(D.home)) {
        /* TR: the code pad's title before arranging Smart Home when it asks for a master code */
        ui_pad_ask(HOST_MODAL, G_SMART, TR("Arrange Smart Home"), NULL, ui_tone_blue(),
                   arrange_code_entered);
        /* TR: under that pad's title (BL-220). At most 127 bytes */
        ui_pad_ask_note(TR("Arranging asks for a master code"));
        return;
    }
    arrange_begin();
}

static void arrange_end(void)
{
    if (!D.arranging) {
        return;
    }
    if (D.dirty) {
        const size_t len = ui_devl_encode(D.layout, D.blob, UI_DEVL_READ_MAX);
        if (len == 0u || S.be.dev_layout_set == NULL || !S.be.dev_layout_set(S.be.ctx, D.blob, len)) {
            /* TR: Smart Home: the arranged layout couldn't be stored; it is lost at the next start */
            ui_toast(TR("The arrangement couldn't be saved"));
        }
    }
    D.arranging = false;
    D.dirty = false;
    D.drag = false; /* its tile is made again */
    D.pick[0] = '\0';
    D.page_sig = 0;
    show();
    arrange_chrome();
}

static void on_arrange_done(lv_event_t *e)
{
    (void)e;
    arrange_end();
}

static void on_arrange_move(lv_event_t *e)
{
    const bool later = lv_event_get_current_target(e) == D.arr_next;
    if (D.arranging && D.pick[0] != '\0' &&
        ui_devl_move(D.layout, D.devs, D.n, page_area(), D.pick, later ? 1 : -1)) {
        arrange_changed();
    }
}

static void on_arrange_size(lv_event_t *e)
{
    const bool wide = (uintptr_t)lv_event_get_user_data(e) == 1u;
    if (D.arranging && D.pick[0] != '\0' &&
        ui_devl_set_size(D.layout, D.devs, D.n, D.pick, wide ? UI_DEVL_SIZE_WIDE : UI_DEVL_SIZE_SMALL)) {
        arrange_changed();
    }
}

static void on_arrange_remove(lv_event_t *e)
{
    (void)e;
    if (D.arranging && D.pick[0] != '\0' && D.tab_favorites &&
        ui_devl_remove(D.layout, D.devs, D.n, D.pick)) {
        D.pick[0] = '\0';
        arrange_changed();
    }
}

/* ---- Dragging a held tile (HCTL-008) ----
 * A long press lifts the tile (LV_EVENT_LONG_PRESSED, which LVGL sends only while nothing scrolls:
 * a swipe that starts without a hold still scrolls the grid). LVGL hands a moving finger to the
 * first scrollable object the pressed one chains to (lv_indev_find_scroll_obj); the lifted tile
 * neither scrolls nor chains until it is released, so the grid stays still under the drag, and
 * the tile, press-locked as every object is, keeps getting LV_EVENT_PRESSING wherever the finger
 * goes. Each one moves the tile to the place of the tile the finger is well inside (DRAG_INSET in
 * from its edges); the tile under the finger right after a move is no target until the finger
 * leaves it, so a finger held still moves the tile once at most, however wide and small tiles
 * fall (at the grid's edges, only as often as the grid steps under it). */
/* The edges are a third of a tile: a finger on the last row's tiles is outside them (on a panel,
 * half a tile kept stepping under a finger held on that row), and one in the top edge is still
 * well inside the first row's tiles, so the held tile drops there once the grid is at the top. */
#define DRAG_INSET (DEVICE_CELL_H / 4)                /* "well inside" a tile */
#define DRAG_EDGE  (DEVICE_CELL_H / 3)                /* the grid's edges that scroll it */
#define DRAG_STEP  ((DEVICE_CELL_H + DEVICE_GAP) / 2) /* one step of that scroll */

static uint32_t page_signature(void);

/* The tile whose area, inset in from each edge (0: the whole of it), holds pt. */
static const tile_t *tile_at(const lv_point_t *pt, int32_t inset)
{
    for (size_t i = 0; i < D.ntiles; i++) {
        lv_area_t a;
        lv_obj_get_coords(D.tiles[i].obj, &a);
        a.x1 += inset;
        a.y1 += inset;
        a.x2 -= inset;
        a.y2 -= inset;
        if (lv_area_is_point_on(&a, pt, 0)) {
            return &D.tiles[i];
        }
    }
    return NULL;
}

static void drag_block_at(const lv_point_t *pt)
{
    const tile_t *under = tile_at(pt, 0);
    snprintf(D.drag_block, sizeof(D.drag_block), "%s", under != NULL ? under->ref : "");
}

static void drag_end(void)
{
    if (!D.drag) {
        return;
    }
    D.drag = false;
    tile_t *t = tile_for(D.pick);
    if (t != NULL) {
        lv_obj_set_flag(t->obj, LV_OBJ_FLAG_SCROLLABLE, D.drag_scrollable);
        lv_obj_set_flag(t->obj, LV_OBJ_FLAG_SCROLL_CHAIN, D.drag_chain);
    }
}

/* Held at the grid's top or bottom edge (once it has moved from where it was lifted), the tile
 * scrolls the grid a step every DRAG_STEP_MS, so that a place off the screen comes into reach:
 * steps, not a slide (UI-020), and none past the ends. */
static void drag_scroll(const lv_point_t *pt)
{
    if (!D.drag_moved || lv_tick_elaps(D.drag_step) < DRAG_STEP_MS) {
        return;
    }
    lv_area_t g;
    lv_obj_get_coords(D.grid, &g);
    int32_t dy = 0;
    if (pt->y < g.y1 + DRAG_EDGE && lv_obj_get_scroll_top(D.grid) > 0) {
        dy = DRAG_STEP; /* what is above comes down into view */
    } else if (pt->y > g.y2 - DRAG_EDGE && lv_obj_get_scroll_bottom(D.grid) > 0) {
        dy = -DRAG_STEP;
    }
    if (dy != 0) {
        lv_obj_scroll_by_bounded(D.grid, 0, dy, LV_ANIM_OFF);
        D.drag_step = lv_tick_get();
    }
}

/* The finger at pt: the lifted tile takes the place of the tile it is well inside. */
static void drag_follow(const lv_point_t *pt)
{
    if (D.drag_block[0] != '\0') {
        const tile_t *b = tile_for(D.drag_block);
        lv_area_t a;
        if (b != NULL) {
            lv_obj_get_coords(b->obj, &a);
        }
        if (b == NULL || !lv_area_is_point_on(&a, pt, 0)) {
            D.drag_block[0] = '\0'; /* the finger left it */
        }
    }
    lv_area_t g;
    lv_obj_get_coords(D.grid, &g);
    if (!lv_area_is_point_on(&g, pt, 0)) {
        return;
    }
    const tile_t *over = tile_at(pt, DRAG_INSET);
    if (over == NULL || strcmp(over->ref, D.pick) == 0 || strcmp(over->ref, D.drag_block) == 0) {
        return;
    }
    size_t to = 0;
    while (to < D.npage && strcmp(D.devs[D.page[to]].ref, over->ref) != 0) {
        to++;
    }
    if (to == D.npage || !ui_devl_move_to(D.layout, D.devs, D.n, page_area(), D.pick, to)) {
        return;
    }
    /* the same tiles put again in the new order: made again, they would end the press */
    D.dirty = true;
    D.npage = ui_devl_page(D.layout, D.devs, D.n, page_area(), D.page, UI_DEVL_MAX);
    place();
    lv_obj_update_layout(D.grid);
    D.page_sig = page_signature();
    drag_block_at(pt);
    arrange_chrome(); /* the arrows: first or last now */
}

static void on_tile_hold(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    const tile_t *t = tile_of(e);
    lv_indev_t *indev = lv_indev_active();
    if (code == LV_EVENT_LONG_PRESSED) {
        drag_end(); /* a drag whose release never came can't block this lift */
        if (!D.arranging || t == NULL || indev == NULL) {
            return; /* outside Arrange a long press opens the device's sheet (on_more) */
        }
        arrange_pick(t->ref); /* lifted: picked as a tap picks it */
        D.drag = true;
        D.drag_moved = false;
        D.drag_scrollable = lv_obj_has_flag(t->obj, LV_OBJ_FLAG_SCROLLABLE);
        D.drag_chain = lv_obj_has_flag(t->obj, LV_OBJ_FLAG_SCROLL_CHAIN);
        lv_obj_remove_flag(t->obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
        lv_indev_get_point(indev, &D.drag_from);
        snprintf(D.drag_block, sizeof(D.drag_block), "%s", t->ref);
        D.drag_step = lv_tick_get();
        return;
    }
    if (!D.drag) {
        return;
    }
    if (code == LV_EVENT_PRESSING && indev != NULL) {
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);
        if (LV_ABS(pt.x - D.drag_from.x) > DRAG_INSET || LV_ABS(pt.y - D.drag_from.y) > DRAG_INSET) {
            D.drag_moved = true;
        }
        drag_scroll(&pt);
        drag_follow(&pt);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST ||
               code == LV_EVENT_INDEV_RESET) {
        drag_end(); /* it stays picked; Done stores it */
    }
}

static void build_size(void)
{
    lv_obj_clean(D.arr_size);
    /* TR: Smart Home, arranging: the picked tile's size, two buttons. At most 15 bytes */
    static const char *const SIZES[2] = {TR_NOOP_C("tile size", "Small"),
                                         TR_NOOP_C("tile size", "Wide")};
    for (uint32_t i = 0; i < 2u; i++) {
        lv_obj_t *seg = segment_create(D.arr_size, TR_C("tile size", SIZES[i]));
        lv_obj_add_event_cb(seg, on_arrange_size, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
}

/* Add: the devices not on Favorites, by area; a tap puts one at the end and the list stays open
 * (it is made again without it) for the next. */
static void add_open(int32_t scroll_y);

static void on_add_pick(int32_t index)
{
    if (!D.arranging) {
        return;
    }
    lv_obj_t *list = ui_find(S.main, "pick_list");
    const int32_t y = list != NULL ? lv_obj_get_scroll_y(list) : 0;
    if (D.arranging && index >= 0 && (size_t)index < D.nadd &&
        ui_devl_add(D.layout, D.devs, D.n, D.devs[D.add_dev[index]].ref)) {
        arrange_changed();
    }
    D.nadd = 0;
    size_t left = 0; /* is there another to add? */
    for (size_t i = 0; i < D.n; i++) {
        left += ui_devl_favorite(D.layout, &D.devs[i]) ? 0u : 1u;
    }
    if (left > 0u) {
        add_open(y);
    }
}

static void add_open(int32_t scroll_y)
{
    D.nadd = 0;
    for (size_t a = 0; a < D.nareas; a++) {
        for (size_t i = 0; i < D.n && D.nadd < UI_DEVL_MAX; i++) {
            const ui_dev_t *d = &D.devs[i];
            if (strcmp(d->area, D.areas[a]) != 0 || ui_devl_favorite(D.layout, d)) {
                continue;
            }
            char st[48];
            tile_words(d, st, sizeof(st));
            char *detail = D.add_detail[D.nadd];
            if (d->area[0] != '\0') {
                char both[sizeof(st) + DEV_AREA_LEN + 4];
                snprintf(both, sizeof(both), "%s · %s", d->area, st);
                i18n_utf8_copy(detail, sizeof(D.add_detail[0]), both);
            } else {
                i18n_utf8_copy(detail, sizeof(D.add_detail[0]), st);
            }
            ui_choice_t *c = &D.add_choice[D.nadd];
            c->glyph = glyph(d);
            c->tone = tone(d);
            c->title = d->name;
            c->detail = detail;
            D.add_dev[D.nadd++] = (uint8_t)i;
        }
    }
    const char *note;
    if (D.nadd > 0u) {
        /* TR: Smart Home, arranging, the Add sheet's note: a tap on a device adds its tile at the
         * end of Favorites (the last place in their order); the sheet stays open for the next. At
         * most 127 bytes */
        note = TR("Tap a device to add it at the end"); /* HCTL-008 (BL-177) */
    } else {
        /* TR: Smart Home, arranging, the Add sheet's note when every device is on Favorites
         * already. At most 127 bytes */
        note = TR("Every device is on Favorites already");
    }
    /* TR: Smart Home, arranging: the title of the sheet that adds devices to Favorites. At most 47
     * bytes */
    ui_pick_sheet(TR("Add to Favorites"), note, D.add_choice, D.nadd, -1, on_add_pick);
    lv_obj_t *list = ui_find(S.main, "pick_list");
    for (size_t i = 0; list != NULL && i < D.nadd; i++) {
        lv_obj_t *trail = ui_find(lv_obj_get_child(list, (int32_t)i), "trail_label");
        if (trail != NULL) { /* a plus, not the check of a chosen one */
            lv_label_set_text(trail, G_PLUS);
            lv_obj_remove_flag(trail, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (list != NULL) {
        lv_obj_scroll_to_y(list, scroll_y, LV_ANIM_OFF);
    }
}

static void room_add_open(int32_t scroll_y);

static void on_arrange_add(lv_event_t *e)
{
    (void)e;
    if (D.arranging && D.tab_favorites) {
        add_open(0);
    } else if (D.arranging) {
        room_add_open(0);
    }
}

/* ---- Rooms (BL-220): the house's, kept by the alarm panel (home_control/hc_home.h) ---------- */

enum {
    ROOM_SOURCE = 1, /* the room from its source */
    ROOM_IN,         /* a room */
    ROOM_NEW,
    ROOM_NONE,
    ROOM_RENAME,
    ROOM_EARLIER,
    ROOM_LATER,
    ROOM_REMOVE,
};

static void home_load(void)
{
    if (!D.home_on) {
        return;
    }
    const size_t len = S.be.dev_home_get(S.be.ctx, D.home_blob, HC_HOME_BLOB_MAX);
    if (len == 0u || !hc_home_decode(D.home, D.home_blob, len)) {
        hc_home_clear(D.home); /* the defaults: no rooms, nothing asks for a code */
    }
}

/* Stores the settings as they are now; a failure says so and goes back to what is stored. */
static bool home_save(void)
{
    const size_t len = hc_home_encode(D.home, D.home_blob, HC_HOME_BLOB_MAX);
    if (len == 0u || !S.be.dev_home_set(S.be.ctx, D.home_blob, len)) {
        /* TR: Smart Home: a change to the rooms or the master codes couldn't be stored */
        ui_toast(TR("The change couldn't be saved"));
        home_load();
        return false;
    }
    return true;
}

/* Each device's room as its area, so the pages and the tabs are the rooms (a poll's last step);
 * the source's area is kept beside it. New areas get their rooms here (hc_home_adopt). */
static void home_apply(void)
{
    for (size_t i = 0; i < D.n; i++) {
        snprintf(D.src[i], DEV_AREA_LEN, "%s", D.devs[i].area);
        D.seen[i].ref = D.devs[i].ref;
        D.seen[i].area = D.src[i];
    }
    if (!D.home_on) {
        return;
    }
    const bool drop = D.home_drop;
    D.home_drop = false;
    if (hc_home_adopt(D.home, D.seen, D.n, drop)) {
        (void)home_save();
    }
    for (size_t i = 0; i < D.n; i++) {
        const char *name = hc_home_room_name(D.home, D.devs[i].ref, D.src[i]);
        if (name != D.src[i]) {
            snprintf(D.devs[i].area, sizeof(D.devs[i].area), "%s", name);
        }
    }
}

/* The room whose page is open, its index in the rooms; -1 Favorites, Other or a keypad. */
static int room_of_tab(void)
{
    if (!D.home_on || D.tab_favorites || D.tab[0] == '\0') {
        return -1;
    }
    return hc_home_room_named(D.home, D.tab);
}

/* The tab of the devices without a room: "Other" beside rooms, "All devices" with none (R6). */
static const char *no_room_tab(void)
{
    if (D.nareas <= 1u) {
        /* TR: Smart Home's tab for every device while there are no rooms (BL-220). At most 23
         * bytes */
        return TR_C("smart home", "All devices");
    }
    /* TR: Smart Home's tab for the devices in no room. At most 23 bytes */
    return TR_C("smart home", "Other");
}

static size_t room_count(uint8_t id)
{
    const int r = hc_home_room_index(D.home, id);
    size_t k = 0;
    for (size_t i = 0; r >= 0 && i < D.n; i++) {
        k += hc_home_room_of(D.home, D.devs[i].ref, D.src[i]) == r ? 1u : 0u;
    }
    return k;
}

static void devices_words(char *out, size_t len, size_t n)
{
    /* TR: Smart Home's rooms: how many devices a room has */
    I18N_FORMAT(out, len, TRN("{0} device", "{0} devices", n), (unsigned)n);
}

/* The rooms changed: the devices' rooms, the tabs and the page again, now. */
static void rooms_changed(void)
{
    D.tabs_built = false;
    D.page_sig = 0;
    poll();
    show();
    if (D.arranging) {
        arrange_chrome();
    }
}

static size_t room_row(size_t k, uint8_t kind, uint8_t id, const char *g, lv_color_t tone,
                       const char *title, const char *detail)
{
    if (k >= ROOM_ROWS) {
        return k;
    }
    D.room_kind[k] = kind;
    D.room_id[k] = id;
    ui_choice_t *c = &D.room_choice[k];
    c->glyph = g;
    c->tone = tone;
    c->title = title;
    c->detail = NULL;
    if (detail != NULL) {
        i18n_utf8_copy(D.room_detail[k], sizeof(D.room_detail[0]), detail);
        c->detail = D.room_detail[k];
    }
    D.nroom_rows = k + 1u;
    return k + 1u;
}

/* A name a person typed: usable, or a toast that says why not (BL-220 R2). */
static bool name_usable(const char *text, uint8_t self)
{
    switch (hc_home_name_check(D.home, text, self)) {
    case HC_HOME_NAME_OK: return true;
    case HC_HOME_NAME_EMPTY:
        /* TR: Smart Home: a room's name was left empty. At most 63 bytes */
        ui_toast(TR("Type a name for the room"));
        return false;
    case HC_HOME_NAME_TAKEN: {
        char said[UI_SUBJECT_STRING_LENGTH];
        /* TR: Smart Home: another room has that name; {0} is the name. At most 127 bytes */
        I18N_FORMAT(said, sizeof(said), TR("There is already a room called {0}"), text);
        ui_toast(said);
        return false;
    }
    default:
        /* TR: Smart Home: the rooms are at their limit (24). At most 63 bytes */
        ui_toast(TR("No more rooms can be added"));
        return false;
    }
}

static void rooms_open(void);
static void room_sheet_open(uint8_t id);
static void new_room_sheet(void);

/* New room's name: from a tile's Room, the tile goes in it; from Rooms, its empty page opens. */
static void new_room_typed(const char *text)
{
    snprintf(D.typed, sizeof(D.typed), "%s", text != NULL ? text : "");
    if (!D.home_on || !name_usable(D.typed, 0u)) {
        if (hc_home_name_check(D.home, D.typed, 0u) != HC_HOME_NAME_FULL) {
            new_room_sheet(); /* to type it again */
        }
        return;
    }
    const uint8_t id = hc_home_add(D.home, D.typed);
    if (id == 0u) {
        return;
    }
    if (D.room_ref[0] != '\0') {
        (void)hc_home_place(D.home, D.room_ref, id);
    }
    if (!home_save()) {
        return;
    }
    if (D.room_ref[0] == '\0') {
        const int r = hc_home_room_index(D.home, id);
        D.tab_favorites = false; /* the new room's page, to add devices to */
        snprintf(D.tab, sizeof(D.tab), "%s", D.home->rooms[r].name);
        D.pick[0] = '\0';
    }
    rooms_changed();
}

static void new_room_sheet(void)
{
    char note[UI_SUBJECT_STRING_LENGTH] = "";
    const ui_dev_t *d = D.room_ref[0] != '\0' ? find(D.room_ref) : NULL;
    if (d != NULL) {
        /* TR: the line under "New room" when it is made from a tile's Room; {0} is the device's
         * name. At most 127 bytes */
        I18N_FORMAT(note, sizeof(note), TR("{0} goes in it"), d->name);
    }
    /* TR: Smart Home: the title of the keyboard that names a new room (BL-220). At most 47 bytes */
    ui_text_sheet_kind(TR("New room"), note, D.typed, UI_TEXT_WORDS, HC_HOME_NAME_MAX, new_room_typed);
    /* TR: the keyboard's button that makes the new room. At most 15 bytes */
    ui_text_sheet_ok(TR_C("new room", "Add"));
}

static void on_room_pick(int32_t index)
{
    if (!D.home_on || index < 0 || (size_t)index >= D.nroom_rows) {
        return;
    }
    bool changed = false;
    switch (D.room_kind[index]) {
    case ROOM_SOURCE: changed = hc_home_follow(D.home, D.room_ref); break;
    case ROOM_IN: changed = hc_home_place(D.home, D.room_ref, D.room_id[index]); break;
    case ROOM_NONE: changed = hc_home_place(D.home, D.room_ref, 0u); break;
    case ROOM_NEW:
        D.typed[0] = '\0';
        new_room_sheet();
        return;
    default: return;
    }
    if (changed && home_save()) {
        rooms_changed();
    }
}

/* A picked tile's Room (BL-220 R1): its source's room, the rooms, New room, No room. */
static void room_pick_open(void)
{
    const ui_dev_t *d = find(D.room_ref);
    if (d == NULL || !D.home_on) {
        return;
    }
    const size_t i = (size_t)(d - D.devs);
    const char *src = D.src[i];
    const int now = hc_home_room_of(D.home, d->ref, src);
    const bool placed = hc_home_placed(D.home, d->ref);
    size_t k = 0;
    int32_t sel = -1;
    char line[96];
    if (src[0] != '\0') {
        const int r = hc_home_room_for_area(D.home, src);
        /* TR: Smart Home, a tile's Room: the line under "Room from its source"; {0} is the room its
         * source puts it in. At most 95 bytes */
        I18N_FORMAT(line, sizeof(line), TR("{0} now · Moves when its source moves it"),
                    r >= 0 ? D.home->rooms[r].name : src);
        sel = placed ? -1 : (int32_t)k;
        /* TR: Smart Home, a tile's Room: back to the room the device's source gives it. At most
         * 47 bytes */
        k = room_row(k, ROOM_SOURCE, 0u, G_SMART, placed ? ui_tone_gray() : ui_tone_blue(),
                     TR("Room from its source"), line);
    }
    for (size_t r = 0; r < D.home->nrooms; r++) {
        const bool here = placed && now == (int)r;
        devices_words(line, sizeof(line), room_count(D.home->rooms[r].id));
        if (here) {
            sel = (int32_t)k;
        }
        k = room_row(k, ROOM_IN, D.home->rooms[r].id, G_DOOR, here ? ui_tone_blue() : ui_tone_gray(),
                     D.home->rooms[r].name, line);
    }
    if (D.home->nrooms < HC_HOME_ROOMS) {
        /* TR: Smart Home, a tile's Room: the line under "New room"; {0} is the device's name. At
         * most 95 bytes */
        I18N_FORMAT(line, sizeof(line), TR("Name it, and {0} goes in it"), d->name);
        k = room_row(k, ROOM_NEW, 0u, G_PLUS, ui_tone_blue(), TR("New room"), line);
    }
    if (now < 0 && (placed || src[0] == '\0')) {
        sel = (int32_t)k;
    }
    /* TR: Smart Home, a tile's Room: the device in no room, and the line under it. At most 47
     * bytes each */
    k = room_row(k, ROOM_NONE, 0u, G_NO_ROOM, ui_tone_gray(), TR("No room"), TR("Shown in Other"));
    char title[96];
    /* TR: Smart Home: the title of a tile's Room; {0} is the device's name. At most 63 bytes */
    I18N_FORMAT(title, sizeof(title), TR("Room for {0}"), d->name);
    /* TR: the line under that title. At most 127 bytes */
    ui_pick_sheet(title, TR("Where it shows besides Favorites"), D.room_choice, k, sel, on_room_pick);
}

static void on_arrange_room(lv_event_t *e)
{
    (void)e;
    if (D.arranging && D.pick[0] != '\0') {
        snprintf(D.room_ref, sizeof(D.room_ref), "%s", D.pick);
        room_pick_open();
    }
}

static void room_renamed(const char *text)
{
    const int r = hc_home_room_index(D.home, D.room_edit);
    snprintf(D.typed, sizeof(D.typed), "%s", text != NULL ? text : "");
    if (r < 0) {
        return;
    }
    if (!name_usable(D.typed, D.room_edit)) {
        char title[96];
        /* TR: Smart Home: the title of the keyboard that renames a room; {0} is its name. At most
         * 63 bytes */
        I18N_FORMAT(title, sizeof(title), TR("Rename {0}"), D.home->rooms[r].name);
        ui_text_sheet_kind(title, NULL, D.typed, UI_TEXT_WORDS, HC_HOME_NAME_MAX, room_renamed);
        return;
    }
    const bool open = !D.tab_favorites && hc_home_same_name(D.tab, D.home->rooms[r].name);
    if (hc_home_rename(D.home, D.room_edit, D.typed) && home_save()) {
        if (open) {
            snprintf(D.tab, sizeof(D.tab), "%s", D.home->rooms[r].name);
        }
        rooms_changed();
    }
    room_sheet_open(D.room_edit);
}

static void room_removed(void)
{
    const int r = hc_home_room_index(D.home, D.room_edit);
    if (r < 0) {
        return;
    }
    if (!D.tab_favorites && hc_home_same_name(D.tab, D.home->rooms[r].name)) {
        D.tab_favorites = true; /* its page goes with it */
        D.pick[0] = '\0';
    }
    if (hc_home_remove(D.home, D.room_edit, D.seen, D.n) && home_save()) {
        rooms_changed();
    }
    rooms_open();
}

static void on_room_sheet_pick(int32_t index)
{
    const int r = hc_home_room_index(D.home, D.room_edit);
    if (r < 0 || index < 0 || (size_t)index >= D.nroom_rows) {
        return;
    }
    char title[96];
    switch (D.room_kind[index]) {
    case ROOM_RENAME:
        snprintf(D.typed, sizeof(D.typed), "%s", D.home->rooms[r].name);
        I18N_FORMAT(title, sizeof(title), TR("Rename {0}"), D.home->rooms[r].name);
        ui_text_sheet_kind(title, NULL, D.typed, UI_TEXT_WORDS, HC_HOME_NAME_MAX, room_renamed);
        return;
    case ROOM_EARLIER:
    case ROOM_LATER:
        if (hc_home_move(D.home, D.room_edit, D.room_kind[index] == ROOM_EARLIER ? -1 : 1) &&
            home_save()) {
            rooms_changed();
        }
        room_sheet_open(D.room_edit);
        return;
    case ROOM_REMOVE: {
        char note[UI_SHEET_NOTE_LENGTH];
        char count[48];
        devices_words(count, sizeof(count), room_count(D.room_edit));
        /* TR: Smart Home: the question before removing a room; {0} is its name. At most 63 bytes */
        I18N_FORMAT(title, sizeof(title), TR("Remove {0}?"), D.home->rooms[r].name);
        /* TR: under that question; {0} is how many devices ("2 devices"). At most 191 bytes */
        I18N_FORMAT(note, sizeof(note),
                    TR("Its {0} stay on the panel and show in Other. Favorites doesn't change."),
                    count);
        /* TR: the button that removes the room. At most 23 bytes */
        ui_confirm(title, note, TR_C("room", "Remove"), room_removed);
        return;
    }
    default: return;
    }
}

/* One room's sheet: Rename, Earlier, Later, Remove room (BL-220 R2); X goes back to Rooms. */
static void room_sheet_open(uint8_t id)
{
    const int r = hc_home_room_index(D.home, id);
    if (r < 0) {
        rooms_open();
        return;
    }
    D.room_edit = id;
    size_t k = 0;
    /* TR: Smart Home, a room's sheet: its rows and the lines under them. At most 47 bytes each */
    k = room_row(k, ROOM_RENAME, id, G_PENCIL, ui_tone_blue(), TR("Rename"), NULL);
    k = room_row(k, ROOM_EARLIER, id, G_LEFT, ui_tone_blue(), TR("Earlier"), TR("One tab to the left"));
    k = room_row(k, ROOM_LATER, id, G_RIGHT, ui_tone_blue(), TR("Later"), TR("One tab to the right"));
    k = room_row(k, ROOM_REMOVE, id, G_TRASH, ui_tone_red(), TR("Remove room"),
                 TR("Its devices go to Other"));
    char count[48];
    char note[UI_SUBJECT_STRING_LENGTH];
    devices_words(count, sizeof(count), room_count(id));
    /* TR: Smart Home, a room's sheet: the line under its name; {0} how many devices ("2 devices"),
     * {1} its tab's place, {2} how many rooms. At most 127 bytes */
    I18N_FORMAT(note, sizeof(note), TR("{0} · Tab {1} of {2}"), count, (unsigned)(r + 1),
                (unsigned)D.home->nrooms);
    ui_pick_sheet(D.home->rooms[r].name, note, D.room_choice, k, -1, on_room_sheet_pick);
    if (r == 0) {
        ui_pick_sheet_dim(1);
    }
    if (r + 1 == (int)D.home->nrooms) {
        ui_pick_sheet_dim(2);
    }
    ui_pick_sheet_on_cancel(rooms_open);
}

static void on_rooms_pick(int32_t index)
{
    if (index < 0 || (size_t)index >= D.nroom_rows) {
        return;
    }
    if (D.room_kind[index] == ROOM_IN) {
        room_sheet_open(D.room_id[index]);
    } else if (D.room_kind[index] == ROOM_NEW) {
        D.room_ref[0] = '\0';
        D.typed[0] = '\0';
        new_room_sheet();
    }
}

/* Arrange's Rooms (BL-220 R2): the rooms in the tabs' order, and New room. */
static void rooms_open(void)
{
    if (!D.home_on) {
        return;
    }
    size_t k = 0;
    D.nroom_rows = 0;
    char line[96];
    for (size_t r = 0; r < D.home->nrooms; r++) {
        devices_words(line, sizeof(line), room_count(D.home->rooms[r].id));
        k = room_row(k, ROOM_IN, D.home->rooms[r].id, G_DOOR, ui_tone_blue(), D.home->rooms[r].name,
                     line);
    }
    if (D.home->nrooms < HC_HOME_ROOMS) {
        /* TR: Smart Home, Rooms: the line under "New room". At most 95 bytes */
        k = room_row(k, ROOM_NEW, 0u, G_PLUS, ui_tone_blue(), TR("New room"),
                     TR("An empty room, to add devices to"));
    }
    /* TR: Smart Home, arranging: the title of the rooms' sheet, and the line under it, with rooms
     * or without. At most 127 bytes each */
    ui_pick_sheet(TR("Rooms"),
                  D.home->nrooms > 0u ? TR("The tabs beside Favorites, in their order")
                                      : TR("No rooms yet"),
                  D.room_choice, k, -1, on_rooms_pick);
}

static void on_arrange_rooms(lv_event_t *e)
{
    (void)e;
    if (D.arranging) {
        rooms_open();
    }
}

static void on_room_add_pick(int32_t index)
{
    lv_obj_t *list = ui_find(S.main, "pick_list");
    const int32_t y = list != NULL ? lv_obj_get_scroll_y(list) : 0;
    const int r = room_of_tab();
    if (!D.arranging || r < 0) {
        return;
    }
    if (index >= 0 && (size_t)index < D.nadd &&
        hc_home_place(D.home, D.devs[D.add_dev[index]].ref, D.home->rooms[r].id) && home_save()) {
        rooms_changed();
    }
    room_add_open(y);
}

/* Add on a room's page (BL-220 D5): the devices in other rooms or none, each with where it is
 * now; a tap moves one in, at the end, and the list stays open for the next. */
static void room_add_open(int32_t scroll_y)
{
    const int r = room_of_tab();
    if (r < 0) {
        return;
    }
    D.nadd = 0;
    for (size_t i = 0; i < D.n && D.nadd < UI_DEVL_MAX; i++) {
        const ui_dev_t *d = &D.devs[i];
        if (hc_home_room_of(D.home, d->ref, D.src[i]) == r) {
            continue;
        }
        char st[48];
        tile_words(d, st, sizeof(st));
        char both[sizeof(st) + DEV_AREA_LEN + 4];
        snprintf(both, sizeof(both), "%s · %s", d->area[0] != '\0' ? d->area : no_room_tab(), st);
        i18n_utf8_copy(D.add_detail[D.nadd], sizeof(D.add_detail[0]), both);
        ui_choice_t *c = &D.add_choice[D.nadd];
        c->glyph = glyph(d);
        c->tone = tone(d);
        c->title = d->name;
        c->detail = D.add_detail[D.nadd];
        D.add_dev[D.nadd++] = (uint8_t)i;
    }
    char title[96];
    /* TR: Smart Home, arranging a room: the title of the sheet that moves devices into it; {0} is
     * the room. At most 63 bytes */
    I18N_FORMAT(title, sizeof(title), TR("Add to {0}"), D.home->rooms[r].name);
    /* TR: the line under that title, with devices to move or without. At most 127 bytes each */
    ui_pick_sheet(title,
                  D.nadd > 0u ? TR("Tap a device to move it here")
                              : TR("Every device is in this room already"),
                  D.add_choice, D.nadd, -1, on_room_add_pick);
    lv_obj_t *list = ui_find(S.main, "pick_list");
    for (size_t i = 0; list != NULL && i < D.nadd; i++) {
        lv_obj_t *trail = ui_find(lv_obj_get_child(list, (int32_t)i), "trail_label");
        if (trail != NULL) {
            lv_label_set_text(trail, G_PLUS);
            lv_obj_remove_flag(trail, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (list != NULL) {
        lv_obj_scroll_to_y(list, scroll_y, LV_ANIM_OFF);
    }
}

/* ---- Settings > Smart Home (BL-220 R5): what asks for a master code ---- */

bool ui_devices_settings_available(void)
{
    return D.ready && D.home_on && D.n > 0u;
}

static void on_arrange_code_toggle(bool on, void *ctx)
{
    (void)ctx;
    if (D.home_on && hc_home_set_arrange_code(D.home, on)) {
        (void)home_save();
    }
}

static void on_protect_toggle(bool on, void *ctx)
{
    const size_t i = (size_t)(uintptr_t)ctx;
    if (D.home_on && i < UI_DEVL_MAX && D.set_refs[i][0] != '\0' &&
        hc_home_protect(D.home, D.set_refs[i], on) && home_save()) {
        D.page_sig = 0; /* the key on its tile */
    }
}

static void protect_row(lv_obj_t *body, size_t *row, size_t i)
{
    const ui_dev_t *d = &D.devs[i];
    char detail[160];
    const char *where = d->area[0] != '\0' ? d->area : no_room_tab();
    const char *also = NULL;
    if (d->kind == DEV_LOCK) {
        /* TR: Settings > Smart Home: under a lock, beside its room. At most 63 bytes */
        also = TR("Unlocking asks for a code anyway");
    } else if (d->kind == DEV_VALVE || entrance(d) || d->kind == DEV_COVER) {
        /* TR: Settings > Smart Home: under a garage door, a cover or a valve. At most 63 bytes */
        also = TR("Opening asks for a code anyway");
    }
    if (also != NULL) {
        snprintf(detail, sizeof(detail), "%s · %s", where, also);
    } else {
        i18n_utf8_copy(detail, sizeof(detail), where);
    }
    snprintf(D.set_refs[*row], sizeof(D.set_refs[0]), "%s", d->ref);
    ui_row_toggle_icon(body, glyph(d), tone(d), d->name, detail, hc_home_protected(D.home, d->ref),
                       on_protect_toggle, (void *)(uintptr_t)*row);
    (*row)++;
}

void ui_devices_settings_build(lv_obj_t *body, char *note, size_t nlen)
{
    /* TR: Settings > Smart Home, the line under the title (BL-220). At most 127 bytes */
    i18n_utf8_copy(note, nlen, TR("What asks for a master code"));
    if (!D.ready || !D.home_on) {
        return;
    }
    home_load();
    poll();
    /* TR: Settings > Smart Home: a switch and the line under it. At most 63 bytes each */
    ui_row_toggle_icon(body, G_PENCIL, ui_tone_blue(), TR("Arranging asks for a master code"),
                       TR("Moving tiles, their sizes, Favorites and rooms"),
                       hc_home_arrange_code(D.home), on_arrange_code_toggle, NULL);
    /* TR: Settings > Smart Home, a section heading over a switch for each device */
    ui_section(body, TR("Devices that ask for a master code before a change"));
    memset(D.set_refs, 0, sizeof(D.set_refs));
    size_t row = 0;
    for (size_t r = 0; r < D.home->nrooms; r++) { /* by room, in the tabs' order */
        for (size_t i = 0; i < D.n && row < UI_DEVL_MAX; i++) {
            if (hc_home_room_of(D.home, D.devs[i].ref, D.src[i]) == (int)r) {
                protect_row(body, &row, i);
            }
        }
    }
    for (size_t i = 0; i < D.n && row < UI_DEVL_MAX; i++) {
        if (hc_home_room_of(D.home, D.devs[i].ref, D.src[i]) < 0) {
            protect_row(body, &row, i);
        }
    }
}

/* ---- the page: tabs, note, tiles ---------------------------------------------------------- */

static uint32_t hash(uint32_t h, const char *s)
{
    while (*s != '\0') {
        h = (h ^ (uint8_t)*s++) * 16777619u;
    }
    return (h ^ 0xFFu) * 16777619u;
}

static void on_tab(lv_event_t *e)
{
    const size_t i = (size_t)(uintptr_t)lv_event_get_user_data(e);
    D.tab_favorites = i == 0u;
    if (i > 0u && i <= D.nareas) {
        snprintf(D.tab, sizeof(D.tab), "%s", D.areas[i - 1u]);
    }
    D.tabs_built = false;
    D.page_sig = 0;
    show();
    lv_obj_scroll_to_y(D.grid, 0, LV_ANIM_OFF);
}

/* The areas, in the registry's order; the devices without one last, as "Other". With the house's
 * rooms (BL-220): every room in the tabs' order, empty ones too, before them. */
static bool collect_areas(void)
{
    D.nareas = 0;
    bool none = false;
    for (size_t r = 0; D.home_on && r < D.home->nrooms && D.nareas < AREA_MAX - 1u; r++) {
        snprintf(D.areas[D.nareas++], DEV_AREA_LEN, "%s", D.home->rooms[r].name);
    }
    for (size_t i = 0; i < D.n; i++) {
        const char *a = D.devs[i].area;
        if (a[0] == '\0') {
            none = true;
            continue;
        }
        size_t k = 0;
        while (k < D.nareas && strcmp(D.areas[k], a) != 0) {
            k++;
        }
        if (k == D.nareas && D.nareas < AREA_MAX - 1u) { /* AREA_MAX leaves room for "Other" */
            snprintf(D.areas[D.nareas++], DEV_AREA_LEN, "%s", a);
        }
    }
    if (none) {
        D.areas[D.nareas++][0] = '\0';
    }
    uint32_t sig = 2166136261u ^ (uint32_t)D.nareas;
    for (size_t k = 0; k < D.nareas; k++) {
        sig = hash(sig, D.areas[k]);
    }
    const bool changed = sig != D.areas_sig;
    D.areas_sig = sig;
    return changed;
}

static void build_tabs(void)
{
    if (!D.tab_favorites) {
        size_t k = 0;
        while (k < D.nareas && strcmp(D.areas[k], D.tab) != 0) {
            k++;
        }
        if (k == D.nareas) {
            D.tab_favorites = true; /* its area is gone */
        }
    }
    lv_obj_clean(D.tabs);
    D.tab_on = NULL;
    lv_obj_t *active = NULL;
    for (size_t i = 0; i <= D.nareas; i++) {
        /* TR: Smart Home's first tab: the devices picked for it */
        const char *name = i == 0u ? TR_C("smart home", "Favorites")
                           : D.areas[i - 1u][0] == '\0' ? no_room_tab()
                                                          : D.areas[i - 1u];
        lv_obj_t *seg = segment_create(D.tabs, name);
        lv_obj_set_style_pad_left(seg, SPACE_MD, 0);
        lv_obj_set_style_pad_right(seg, SPACE_MD, 0);
        /* a room's long name (47 bytes) ends in "..." on its tab, which stays a part of the row
         * (BL-220): the whole name is in Rooms and Rename */
        lv_obj_t *words = lv_obj_get_child(seg, 0);
        lv_point_t size;
        lv_text_get_size(&size, name, lv_obj_get_style_text_font(words, LV_PART_MAIN),
                         lv_obj_get_style_text_letter_space(words, LV_PART_MAIN), 0, LV_COORD_MAX,
                         LV_TEXT_FLAG_NONE);
        if (size.x > DEVICE_WIDE_W) { /* LVGL writes its "..." only into a label of a set width */
            lv_label_set_long_mode(words, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_width(words, DEVICE_WIDE_W);
        }
        const bool on = i == 0u ? D.tab_favorites : !D.tab_favorites && strcmp(D.areas[i - 1u], D.tab) == 0;
        if (on) {
            lv_obj_add_style(seg, &style_segment_active, 0);
            active = seg;
            D.tab_on = seg;
        }
        lv_obj_add_event_cb(seg, on_tab, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
    if (active != NULL) {
        lv_obj_update_layout(D.tabs);
        lv_obj_scroll_to_view(active, LV_ANIM_OFF);
    }
    D.tabs_built = true;
}

/* The line under Arrange's title: what a tap and a hold do. Outside Arrange nothing (BL-220 R7, the
 * owner 2026-10-19: no title and no note over the page - the tabs take the row, and each tile says
 * its own state, a failure in amber, an offline time; HCTL-006's note went with it). */
static void note(void)
{
    if (D.arranging) {
        /* TR: Smart Home, arranging a page with nothing picked: holding a tile and dragging it moves
         * it, a tap picks it for its size and Remove. One line beside Add and Done: keep it as
         * short. At most 127 bytes */
        const char *tap = !D.home_on && D.tab_favorites
                              ? TR("Hold a tile to drag it, or tap it to resize or remove it")
                          : !D.home_on
                              /* TR: the same on a room's page, where tiles can't be removed */
                              ? TR("Hold a tile to drag it, or tap it to resize it")
                              /* TR: the same where a tile's room can be changed too (BL-220). At
                               * most 127 bytes */
                              : TR("Hold a tile to drag it, or tap it to change it");
        /* TR: Smart Home, arranging with a tile picked: holding and dragging it, or the arrows,
         * move it. One line beside the arrows, Small, Wide, Remove and Done: keep it as short.
         * At most 127 bytes */
        const char *picked = TR("Hold to drag, or use arrows");
        ui_set_str(&subject_devices_note, D.pick[0] != '\0' ? picked : tap);
        return;
    }
    ui_set_str(&subject_devices_note, "");
}

/* What the tiles on screen are built for: the page, its devices in order, their sizes. */
static uint32_t page_signature(void)
{
    uint32_t sig = 2166136261u ^ (D.tab_favorites ? 1u : 2u) ^ (D.arranging ? 0x77u : 0u);
    for (size_t p = 0; p < D.npage; p++) {
        const ui_dev_t *d = &D.devs[D.page[p]];
        sig = hash(sig, d->ref) ^ (ui_devl_wide(D.layout, d) ? 0x5Au : 0xA5u);
        sig = (sig ^ (uint32_t)d->caps) * 16777619u; /* a slider, Stop, speeds (BL-171) */
    }
    return sig;
}

/* Draws the open page: the tabs when the areas changed, the tiles when the page's devices or
 * sizes did, else only their words and colours. */
static void show(void)
{
    if (!D.ready || !on_view()) {
        return;
    }
    if (collect_areas() || !D.tabs_built) {
        build_tabs();
    }
    if (!D.arranging) {
        /* BL-220 R7: no title beside the tabs - they may take the row, up to the pencil */
        lv_obj_t *row = lv_obj_get_parent(D.tabs);
        const int32_t room = lv_obj_get_content_width(row) - lv_obj_get_width(D.pencil) -
                             lv_obj_get_style_pad_column(row, LV_PART_MAIN) * 2;
        if (room > 0 && room != D.tabs_max) {
            D.tabs_max = room;
            lv_obj_set_style_max_width(D.tabs, room, 0);
        }
    }
    D.npage = ui_devl_page(D.layout, D.devs, D.n, D.tab_favorites ? NULL : D.tab, D.page,
                           UI_DEVL_MAX);
    const uint32_t sig = page_signature();
    if (sig != D.page_sig) {
        const int32_t y = lv_obj_get_scroll_y(D.grid);
        lv_obj_clean(D.grid);
        D.drag = false; /* the held tile is gone */
        D.ntiles = 0;
        for (size_t p = 0; p < D.npage; p++) {
            const ui_dev_t *d = &D.devs[D.page[p]];
            tile_create(D.ntiles++, d, ui_devl_wide(D.layout, d));
        }
        place();
        D.page_sig = sig;
        if (D.arranging) {
            /* Made again, the grid would be back at its top: while arranging (an arrow, Small,
             * Wide, Remove, Add, or a device's change) it keeps its scroll, and the picked tile
             * is brought into view where it moved out of it (HCTL-008). At once, no animation:
             * nothing moves by itself (UI-020). */
            lv_obj_update_layout(D.grid);
            lv_obj_scroll_by_bounded(D.grid, 0, lv_obj_get_scroll_y(D.grid) - y, LV_ANIM_OFF);
            const tile_t *t = D.pick[0] != '\0' ? tile_for(D.pick) : NULL;
            if (t != NULL) {
                lv_obj_scroll_to_view(t->obj, LV_ANIM_OFF);
            }
        }
    }
    for (size_t i = 0; i < D.ntiles; i++) {
        const ui_dev_t *d = find(D.tiles[i].ref);
        if (d != NULL) {
            tile_update(&D.tiles[i], d);
        }
    }
    note();
    if (sheet_open_now()) {
        const ui_dev_t *sd = sheet_dev();
        if (sd != NULL) {
            sheet_update(sd);
        } else {
            ui_sheet_close(); /* the device went away */
        }
    }
}

static void load_layout(void)
{
    size_t len = 0;
    if (S.be.dev_layout_get != NULL) {
        len = S.be.dev_layout_get(S.be.ctx, D.blob, UI_DEVL_READ_MAX); /* bytes after the entries are ignored */
    }
    if (len == 0u || !ui_devl_decode(D.layout, D.blob, len)) {
        ui_devl_clear(D.layout); /* the defaults: everything on Favorites */
    }
}

static void poll(void)
{
    D.polled = lv_tick_get();
    D.n = D.ready ? S.be.dev_list(S.be.ctx, D.devs, UI_DEVL_MAX) : 0u;
    if (D.n > UI_DEVL_MAX) {
        D.n = UI_DEVL_MAX;
    }
    /* The panel's own Z-Wave outputs are listed for the automation editors (docs/features/41
     * phase B); their Smart Home tiles wait for their own approved mockup (BL-169 B3). The list
     * holds the whole registry (UI_DEVL_MAX = HC_MAX_DEVICES), so dropping them here costs Home
     * Assistant's devices nothing. */
    size_t kept = 0u;
    for (size_t i = 0; i < D.n; i++) {
        if (D.devs[i].source != (uint8_t)DSRC_ZWAVE) {
            D.devs[kept++] = D.devs[i];
        }
    }
    D.n = kept;
    home_apply(); /* each device's room as its area (BL-220) */
    expect_read(); /* what was asked, until the report says otherwise (BL-219) */
    /* HCTL-003: Smart Home on the rail while a device is listed - a link keypad's are its alarm
     * panel's, through the link (BL-216, proposal P1 approved 2026-10-16) */
    ui_set_int(&subject_devices_avail, D.n > 0u ? 1 : 0);
    if (D.n == 0u && on_view()) {
        ui_set_int(&subject_view, VIEW_HOME); /* the last device went away */
    }
}

void ui_devices_init(void)
{
    D.tab_favorites = true;
    D.grid = ui_find(S.main, "device_grid");
    D.tabs = ui_find(S.main, "device_areas");
    if (S.be.dev_list == NULL || D.grid == NULL || D.tabs == NULL) {
        return; /* no devices on this panel: the rail's Smart Home stays hidden */
    }
    /* The note is live words on one line: a longer one ends in "..." rather than pushing the
     * tiles down while someone looks at them (UI-020). */
    D.head = ui_find(S.main, "device_header");
    D.arr_head = ui_find(S.main, "device_arrange_header");
    D.tabs_max = lv_obj_get_style_max_width(D.tabs, LV_PART_MAIN); /* show() widens it to the row */
    for (size_t h = 0; h < 2u; h++) {
        lv_obj_t *header = h == 0u ? D.head : D.arr_head;
        /* both lines of the arranging header are one line each: a long name ends in "..." */
        for (int32_t line = (h == 0u ? 1 : 0); header != NULL && line < 2; line++) {
            lv_obj_t *label = lv_obj_get_child(header, line);
            lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_height(label,
                              lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN)));
        }
    }
    D.pencil = ui_find(S.main, "device_arrange");
    D.arr_add = ui_find(S.main, "device_arrange_add");
    D.arr_prev = ui_find(S.main, "device_arrange_prev");
    D.arr_next = ui_find(S.main, "device_arrange_next");
    D.arr_size = ui_find(S.main, "device_arrange_size");
    D.arr_remove = ui_find(S.main, "device_arrange_remove");
    D.arr_done = ui_find(S.main, "device_arrange_done");
    D.arr_room = ui_find(S.main, "device_arrange_room");
    D.arr_rooms = ui_find(S.main, "device_arrange_rooms");
    D.spacer = ui_find(S.main, "device_spacer");
    if (D.head == NULL || D.arr_head == NULL || D.pencil == NULL || D.arr_add == NULL ||
        D.arr_prev == NULL || D.arr_next == NULL || D.arr_size == NULL || D.arr_remove == NULL ||
        D.arr_done == NULL || D.arr_room == NULL || D.arr_rooms == NULL || D.spacer == NULL) {
        return;
    }
    lv_obj_add_flag(D.head, LV_OBJ_FLAG_HIDDEN); /* no title over the page (BL-220 R7) */
    lv_obj_add_event_cb(D.arr_room, on_arrange_room, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(D.arr_rooms, on_arrange_rooms, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(D.pencil, on_arrange_start, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(D.arr_add, on_arrange_add, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(D.arr_prev, on_arrange_move, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(D.arr_next, on_arrange_move, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(D.arr_remove, on_arrange_remove, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(D.arr_done, on_arrange_done, LV_EVENT_CLICKED, NULL);
    build_size();
    D.devs =ui_slow_alloc(sizeof(ui_dev_t) * UI_DEVL_MAX);
    D.layout = ui_slow_alloc(sizeof(ui_devl_t));
    D.blob = ui_slow_alloc(UI_DEVL_READ_MAX);
    D.src = ui_slow_alloc(sizeof(*D.src) * UI_DEVL_MAX);
    if (D.devs == NULL || D.layout == NULL || D.blob == NULL || D.src == NULL) {
        return;
    }
    /* BL-220: the house's rooms and master codes, where this panel keeps them (not a keypad) */
    if (S.be.dev_home_get != NULL && S.be.dev_home_set != NULL) {
        D.home = ui_slow_alloc(sizeof(hc_home_t));
        D.home_blob = ui_slow_alloc(HC_HOME_BLOB_MAX);
        D.home_on = D.home != NULL && D.home_blob != NULL;
    }
    D.ready = true;
    home_load();
    load_layout();
    poll();
}

void ui_devices_tick(void)
{
    if (!D.ready) {
        return;
    }
    if (D.arranging && !on_view()) {
        arrange_end(); /* the view was left: what was arranged is kept */
    }
    const uint32_t every = on_view() ? POLL_VIEW_MS : POLL_IDLE_MS;
    if (!D.drag && lv_tick_elaps(D.polled) >= every) { /* a held tile: after its release */
        poll();
        show();
        expect_flush();
    }
}

void ui_devices_shown(void)
{
    if (!D.ready) {
        return;
    }
    load_layout(); /* Arrange may have changed it */
    home_load();
    D.home_drop = true; /* an empty room made from an area may go now (hc_home_adopt) */
    D.tabs_built = false;
    D.page_sig = 0;
    poll();
    show();
}

void ui_devices_language_changed(void)
{
    if (sheet_open_now()) {
        ui_sheet_close(); /* its keys' words are made when it opens */
    }
    D.tabs_built = false;
    D.page_sig = 0; /* the keys' words */
    if (D.arr_size != NULL) {
        build_size();
    }
    show();
    if (D.arranging) {
        arrange_chrome();
    }
}
