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
 * Compiled only with ALARM_UI_DEVICES (sources.cmake).
 */
#include <stdio.h>
#include <string.h>

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
    int32_t tab_on_w;     /* its width with its whole name (before tab_cap) */
    int32_t tab_cap;      /* the width its name is shortened to with "…", 0 whole (note_make_room) */
    int32_t tabs_max;     /* the layout's widest for the tabs (its style_max_width) */
    bool note_two_lines;  /* the note wraps to a second line (note_make_room's last resort) */
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
} D;

static void poll(void);
static void show(void);
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

/* The page's note for a failed action (HCTL-006, HCTL-012, BL-171): what didn't happen where the
 * source refused it; "Couldn't confirm" where no answer came, the connection dropped with it in
 * flight or the source sent it and couldn't confirm it (d->unsure), never "didn't" for an
 * outcome that isn't known. {0} is the device's name. */
static const char *failed_sentence(const ui_dev_t *d)
{
    const char *definite;
    const char *unknown;
    switch (d->action) {
    case ACT_ON:
        /* TR: Smart Home's note: the last action on a device was refused and it didn't turn on; {0}
         * is its name, shortened with "…" to fit. The note may hold two facts. At most 40 bytes */
        definite = TR("{0} didn't turn on");
        /* TR: Smart Home's note: a device was told to turn on but no answer came (or the connection
         * dropped), so whether it did isn't known; {0} is its name, shortened with "…" to fit. The
         * note may hold two facts. At most 40 bytes */
        unknown = TR("Couldn't confirm {0} turned on");
        break;
    case ACT_OFF:
        /* TR: Smart Home's note: the last action on a device was refused and it didn't turn off;
         * {0} is its name, shortened with "…" to fit. The note may hold two facts. At most 40 bytes
         */
        definite = TR("{0} didn't turn off");
        /* TR: Smart Home's note: a device was told to turn off but no answer came (or the
         * connection dropped), so whether it did isn't known; {0} is its name, shortened with "…"
         * to fit. The note may hold two facts. At most 40 bytes */
        unknown = TR("Couldn't confirm {0} turned off");
        break;
    case ACT_TOGGLE:
        /* TR: Smart Home's note: the last action on a device was refused and it didn't switch; {0}
         * is its name, shortened with "…" to fit. The note may hold two facts. At most 40 bytes */
        definite = TR("{0} didn't switch");
        /* TR: Smart Home's note: a device was told to switch but no answer came (or the connection
         * dropped), so whether it did isn't known; {0} is its name, shortened with "…" to fit. The
         * note may hold two facts. At most 40 bytes */
        unknown = TR("Couldn't confirm {0} switched");
        break;
    case ACT_LOCK:
        /* TR: Smart Home's note: the last action on a lock was refused and it didn't lock; {0} is
         * its name, shortened with "…" to fit. The note may hold two facts. At most 40 bytes */
        definite = TR("{0} didn't lock");
        /* TR: Smart Home's note: a lock was told to lock but no answer came (or the connection
         * dropped), so whether it did isn't known; {0} is its name, shortened with "…" to fit. The
         * note may hold two facts. At most 40 bytes */
        unknown = TR("Couldn't confirm {0} locked");
        break;
    case ACT_UNLOCK:
        /* TR: Smart Home's note: the last action on a lock was refused and it didn't unlock; {0} is
         * its name, shortened with "…" to fit. The note may hold two facts. At most 40 bytes */
        definite = TR("{0} didn't unlock");
        /* TR: Smart Home's note: a lock was told to unlock but no answer came (or the connection
         * dropped), so whether it did isn't known; {0} is its name, shortened with "…" to fit. The
         * note may hold two facts. At most 40 bytes */
        unknown = TR("Couldn't confirm {0} unlocked");
        break;
    case ACT_OPEN:
        /* TR: Smart Home's note: the last action on a device was refused and it didn't open; {0} is
         * its name, shortened with "…" to fit. The note may hold two facts. At most 40 bytes */
        definite = TR("{0} didn't open");
        /* TR: Smart Home's note: a device was told to open but no answer came (or the connection
         * dropped), so whether it did isn't known; {0} is its name, shortened with "…" to fit. The
         * note may hold two facts. At most 40 bytes */
        unknown = TR("Couldn't confirm {0} opened");
        break;
    case ACT_CLOSE:
        /* TR: Smart Home's note: the last action on a device was refused and it didn't close; {0}
         * is its name, shortened with "…" to fit. The note may hold two facts. At most 40 bytes */
        definite = TR("{0} didn't close");
        /* TR: Smart Home's note: a device was told to close but no answer came (or the connection
         * dropped), so whether it did isn't known; {0} is its name, shortened with "…" to fit. The
         * note may hold two facts. At most 40 bytes */
        unknown = TR("Couldn't confirm {0} closed");
        break;
    case ACT_STOP:
        /* TR: Smart Home's note: the last action on a device was refused and it didn't stop; {0} is
         * its name, shortened with "…" to fit. The note may hold two facts. At most 40 bytes */
        definite = TR("{0} didn't stop");
        /* TR: Smart Home's note: a device was told to stop but no answer came (or the connection
         * dropped), so whether it did isn't known; {0} is its name, shortened with "…" to fit. The
         * note may hold two facts. At most 40 bytes */
        unknown = TR("Couldn't confirm {0} stopped");
        break;
    case ACT_RUN:
        /* TR: Smart Home's note: the last action on a scene or script was refused and it didn't
         * run; {0} is its name, shortened with "…" to fit. The note may hold two facts. At most 40
         * bytes */
        definite = TR("{0} didn't run");
        /* TR: Smart Home's note: a scene or script was told to run but no answer came (or the
         * connection dropped), so whether it did isn't known; {0} is its name, shortened with "…"
         * to fit. The note may hold two facts. At most 40 bytes */
        unknown = TR("Couldn't confirm {0} ran");
        break;
    case ACT_SET_LEVEL:
    case ACT_SET_TEMP:
    case ACT_SET_MODE:
        /* TR: Smart Home's note: the last action on a device was refused and it didn't change a
         * value (brightness, position, speed, set point, mode); {0} is its name, shortened with "…"
         * to fit. The note may hold two facts. At most 40 bytes */
        definite = TR("{0} didn't change");
        /* TR: Smart Home's note: a device was told to change a value (brightness, position, speed,
         * set point, mode) but no answer came (or the connection dropped), so whether it did isn't
         * known; {0} is its name, shortened with "…" to fit. The note may hold two facts. At most
         * 40 bytes */
        unknown = TR("Couldn't confirm {0} changed");
        break;
    default:
        /* TR: Smart Home's note: the last action on a device failed; {0} is its name, shortened
         * with "…" to fit. The note may hold two facts. At most 40 bytes */
        definite = TR("{0} didn't respond");
        /* TR: Smart Home's note: a device was told to do something but no answer came (or the
         * connection dropped), so the result isn't known; {0} is its name, shortened with "…" to
         * fit. The note may hold two facts. At most 40 bytes */
        unknown = TR("Couldn't confirm the result for {0}");
        break;
    }
    return d->unsure != 0u ? unknown : definite;
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
    const char *w = d->status == UI_DEV_PENDING || d->status == UI_DEV_FAILED
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

static bool code_entered(const char *code)
{
    alarm_result_t why = ALARM_OK;
    const ui_dev_result_t r =
        S.be.dev_act(S.be.ctx, D.ask_ref, D.ask_action, D.ask_value, code, &why);
    if (r == UI_DEV_CODE_REFUSED) {
        ui_beep(UI_BEEP_ERROR);
        ui_pad_error(ui_result_text(why));
        return false;
    }
    refusal(r); /* the code was good, the change wasn't sent: say so */
    D.polled = lv_tick_get() - POLL_VIEW_MS; /* the tile says what happened, now */
    return true;
}

/* Sends an action; one that needs a code asks for it first (HCTL-005). */
static void send(const ui_dev_t *d, uint8_t action, int32_t value)
{
    if (S.be.dev_act == NULL || action == ACT_NONE) {
        return;
    }
    alarm_result_t why = ALARM_OK;
    const ui_dev_result_t r = S.be.dev_act(S.be.ctx, d->ref, action, value, NULL, &why);
    if (r == UI_DEV_NEED_CODE) {
        snprintf(D.ask_ref, sizeof(D.ask_ref), "%s", d->ref);
        D.ask_action = action;
        D.ask_value = value;
        char title[64];
        if (action == ACT_UNLOCK) {
            /* TR: the code pad's title before an unlock; {0} is the lock's name */
            I18N_FORMAT(title, sizeof(title), TR("Unlock {0}"), d->name);
        } else {
            /* TR: the code pad's title before opening a garage door, a cover or a valve */
            I18N_FORMAT(title, sizeof(title), TR("Open {0}"), d->name);
        }
        ui_pad_ask(HOST_MODAL, action == ACT_UNLOCK ? G_LOCK_OPEN : glyph(d), title, NULL,
                   ui_tone_amber(), code_entered);
        return;
    }
    refusal(r);
    poll();
    show();
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
    if (d == NULL || d->status == UI_DEV_OFFLINE || d->status == UI_DEV_PENDING) {
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

/* Whether a change may be sent now: the device answers and has nothing under way. */
static const ui_dev_t *sheet_ready(void)
{
    const ui_dev_t *d = sheet_dev();
    return d != NULL && d->status != UI_DEV_OFFLINE && d->status != UI_DEV_PENDING ? d : NULL;
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
    const bool warn = d->status == UI_DEV_FAILED || (amber(d) && d->status != UI_DEV_PENDING && !offline);
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
    const bool pending = d->status == UI_DEV_PENDING;
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
    lv_obj_set_flag(D.head, LV_OBJ_FLAG_HIDDEN, a);
    lv_obj_set_flag(D.tabs, LV_OBJ_FLAG_HIDDEN, a);
    lv_obj_set_flag(D.pencil, LV_OBJ_FLAG_HIDDEN, a);
    lv_obj_set_flag(D.arr_head, LV_OBJ_FLAG_HIDDEN, !a);
    lv_obj_set_flag(D.arr_add, LV_OBJ_FLAG_HIDDEN, !(a && !picked && D.tab_favorites));
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
            /* TR: Smart Home, the page of the devices without a room: its tab. At most 23 bytes */
            room = TR_C("smart home", "Other");
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

static void on_arrange_start(lv_event_t *e)
{
    (void)e;
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

static void on_arrange_add(lv_event_t *e)
{
    (void)e;
    if (D.arranging) {
        add_open(0);
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

/* The areas, in the registry's order; the devices without one last, as "Other". */
static bool collect_areas(void)
{
    D.nareas = 0;
    bool none = false;
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
    D.tab_on_w = 0;
    D.tab_cap = 0; /* new segments: every name whole */
    lv_obj_t *active = NULL;
    for (size_t i = 0; i <= D.nareas; i++) {
        /* TR: Smart Home's first tab: the devices picked for it */
        const char *name = i == 0u ? TR_C("smart home", "Favorites")
                           : D.areas[i - 1u][0] == '\0'
                               /* TR: Smart Home's tab for the devices in no area */
                               ? TR_C("smart home", "Other")
                               : D.areas[i - 1u];
        lv_obj_t *seg = segment_create(D.tabs, name);
        lv_obj_set_style_pad_left(seg, SPACE_MD, 0); /* room for the note beside the tabs */
        lv_obj_set_style_pad_right(seg, SPACE_MD, 0);
        const bool on = i == 0u ? D.tab_favorites : !D.tab_favorites && strcmp(D.areas[i - 1u], D.tab) == 0;
        if (on) {
            lv_obj_add_style(seg, &style_segment_active, 0);
            active = seg;
            D.tab_on = seg;
            /* its width with the whole name, measured from its words (what note_make_room's
             * shortening gives back) */
            lv_obj_t *label = lv_obj_get_child(seg, 0);
            lv_point_t size;
            lv_text_get_size(&size, name, lv_obj_get_style_text_font(label, LV_PART_MAIN),
                             lv_obj_get_style_text_letter_space(label, LV_PART_MAIN), 0, LV_COORD_MAX,
                             LV_TEXT_FLAG_NONE);
            D.tab_on_w = size.x + lv_obj_get_style_pad_left(seg, LV_PART_MAIN) +
                         lv_obj_get_style_pad_right(seg, LV_PART_MAIN) +
                         2 * lv_obj_get_style_border_width(seg, LV_PART_MAIN);
        }
        lv_obj_add_event_cb(seg, on_tab, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
    if (active != NULL) {
        lv_obj_update_layout(D.tabs);
        lv_obj_scroll_to_view(active, LV_ANIM_OFF);
    }
    D.tabs_built = true;
}

/* One fact of the page's note with a device's name in it: the sentence whole, the name shortened
 * with "…" where both don't fit len - 1 bytes, so the outcome is never cut - also where a
 * translation repeats the name (I18N-010, BL-171). */
static void fact_with_name(char *out, size_t len, const char *pattern, const char *name)
{
    i18n_format_fit(out, len, pattern, name);
}

/* A fact of the note: a sentence with {0} for a device's name, or (name NULL) words as they are. */
typedef struct {
    const char *pattern;
    const char *name;
} note_fact_t;

#define NOTE_FACT     60u /* bytes of one fact; two and " · " fit the note's 127 */
#define NOTE_NAME_MIN 12u /* a name's bytes kept on the line before a second fact gives way */

/* The note's facts joined with " · ", each name given at most `room` bytes. */
static void note_join(char *out, size_t len, const note_fact_t *f, size_t nf, size_t room)
{
    char facts[2][NOTE_FACT];
    for (size_t i = 0; i < nf && i < 2u; i++) {
        if (f[i].name == NULL) {
            i18n_utf8_copy(facts[i], sizeof(facts[0]), f[i].pattern);
            continue;
        }
        const size_t words = i18n_format_fit(facts[i], sizeof(facts[0]), f[i].pattern, "");
        const size_t cap = words + room + 1u < sizeof(facts[0]) ? words + room + 1u : sizeof(facts[0]);
        fact_with_name(facts[i], cap, f[i].pattern, f[i].name);
    }
    if (nf >= 2u) {
        snprintf(out, len, "%s · %s", facts[0], facts[1]);
    } else {
        i18n_utf8_copy(out, len, nf == 1u ? facts[0] : "");
    }
}

static lv_obj_t *note_label(void)
{
    return D.head != NULL ? lv_obj_get_child(D.head, 1) : NULL;
}

/* How wide text is drawn in the note's label (its font and letter spacing), on one line. */
static int32_t note_text_width(lv_obj_t *label, const char *text)
{
    lv_point_t size;
    lv_text_get_size(&size, text, lv_obj_get_style_text_font(label, LV_PART_MAIN),
                     lv_obj_get_style_text_letter_space(label, LV_PART_MAIN), 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    return size.x;
}

/* The tabs beside the header in one row (the layout's flex row): what they give up, the header
 * gets (its flex_grow). */
static bool tabs_beside_note(void)
{
    return D.tabs != NULL && D.head != NULL && lv_obj_get_parent(D.tabs) == lv_obj_get_parent(D.head);
}

/* The tabs' own padding and border: what they take beside their segments. */
static int32_t tabs_frame(void)
{
    return lv_obj_get_style_pad_left(D.tabs, LV_PART_MAIN) + lv_obj_get_style_pad_right(D.tabs, LV_PART_MAIN) +
           2 * lv_obj_get_style_border_width(D.tabs, LV_PART_MAIN);
}

/* The tabs' width at the layout's widest: every segment with its whole name (the open tab's as
 * measured when it was built, whatever note_make_room shortened it to), capped by the layout's
 * style_max_width. */
static int32_t tabs_full_width(void)
{
    const uint32_t n = lv_obj_get_child_count(D.tabs);
    int32_t w = tabs_frame();
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *seg = lv_obj_get_child(D.tabs, (int32_t)i);
        w += seg == D.tab_on && D.tab_on_w > 0 ? D.tab_on_w : lv_obj_get_width(seg);
    }
    if (n > 1u) {
        w += (int32_t)(n - 1u) * lv_obj_get_style_pad_column(D.tabs, LV_PART_MAIN);
    }
    return w < D.tabs_max ? w : D.tabs_max;
}

/* The note's one line with the tabs at the layout's widest (what a narrower strip made by
 * note_make_room gives back), 0 while the label isn't laid out yet. */
static int32_t note_room_full(lv_obj_t *label)
{
    const int32_t w = lv_obj_get_content_width(label);
    if (w <= 0 || !tabs_beside_note() || lv_obj_has_flag(D.tabs, LV_OBJ_FLAG_HIDDEN)) {
        return w > 0 ? w : 0;
    }
    const int32_t back = lv_obj_get_width(D.tabs) - tabs_full_width();
    return back < 0 ? w + back : w;
}

/* Whether text fits the note's one line as drawn with the tabs at their widest: its label's font
 * and width. True while the label isn't laid out yet (the next refresh measures it). */
static bool note_fits(const char *text)
{
    lv_obj_t *label = note_label();
    const int32_t w = label != NULL ? note_room_full(label) : 0;
    return w <= 0 || note_text_width(label, text) <= w;
}

/* The open tab's whole width (its segment's padding and border beside its name). */
static int32_t tab_frame(lv_obj_t *seg)
{
    return lv_obj_get_style_pad_left(seg, LV_PART_MAIN) + lv_obj_get_style_pad_right(seg, LV_PART_MAIN) +
           2 * lv_obj_get_style_border_width(seg, LV_PART_MAIN);
}

/* The open tab's name shortened with "…" so its segment is w px wide, or (w 0) whole again; set
 * only when it changes. True when it changed. */
static bool tab_cap_set(int32_t w)
{
    lv_obj_t *label = D.tab_on != NULL ? lv_obj_get_child(D.tab_on, 0) : NULL;
    if (label == NULL || w == D.tab_cap) {
        return false;
    }
    D.tab_cap = w;
    if (w > 0) {
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(label, w - tab_frame(D.tab_on),
                        lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN)));
    } else {
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_size(label, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    }
    return true;
}

/* The narrowest the open tab's name is shortened to: as wide as Favorites, the first tab (or, when
 * Favorites is the one open, its padding and "…"). */
static int32_t tab_floor(void)
{
    lv_obj_t *first = lv_obj_get_child(D.tabs, 0);
    if (first != NULL && first != D.tab_on) {
        return lv_obj_get_width(first);
    }
    lv_obj_t *label = lv_obj_get_child(D.tab_on, 0);
    return tab_frame(D.tab_on) + note_text_width(label, "\xE2\x80\xA6");
}

/* UI-020, HCTL-012 (BL-171, addendum 6): the note's outcome is never cut. Its shortest form (one
 * fact, the name down to "…") that is still wider than the line takes the room it needs from the
 * tabs, which scroll as they do when a house has many rooms; the tiles stay where they are. The
 * open tab stays in view: where the other tabs alone can't give the room (the open room's name is
 * long), the open tab's name is shortened with "…" too, never narrower than Favorites, and the tabs
 * never grow past the layout's widest. An open tab wider than the tabs can ever be is shortened the
 * same way, so it is always whole in view. Only where even that leaves too little (a language far
 * longer than any we have) does the note wrap to a second line. Everything goes back once the note
 * fits again. Measured after the layout; a style is set only when it changes, so a steady note
 * moves nothing (UI-020). */
static void note_make_room(const char *text, bool fits)
{
    lv_obj_t *label = note_label();
    if (label == NULL || !tabs_beside_note()) {
        return;
    }
    const int32_t frame = tabs_frame();
    int32_t want = D.tabs_max;
    int32_t cap = 0;
    bool wrap = false;
    if (D.tab_on != NULL && D.tab_on_w > D.tabs_max - frame) {
        cap = D.tabs_max - frame; /* never wider than the tabs can be */
    }
    if (!fits) {
        const int32_t room = note_room_full(label);
        const int32_t full = tabs_full_width();
        want = full - (note_text_width(label, text) - room);
        if (D.tab_on == NULL) {
            if (want < frame) {
                want = frame;
                wrap = true;
            }
        } else {
            const int32_t open = cap > 0 ? cap : D.tab_on_w;
            if (want < frame + open) {
                int32_t least = tab_floor();
                least = least < open ? least : open;
                cap = want - frame;
                if (cap < least) {
                    cap = least;
                    wrap = true;
                }
                want = frame + cap;
            }
        }
        want = want < D.tabs_max ? want : D.tabs_max;
    }
    if (cap >= D.tab_on_w) {
        cap = 0; /* its whole name fits */
    }
    bool moved = tab_cap_set(cap);
    if (lv_obj_get_style_max_width(D.tabs, LV_PART_MAIN) != want) {
        lv_obj_set_style_max_width(D.tabs, want, LV_PART_MAIN);
        moved = true;
    }
    if (moved) {
        lv_obj_update_layout(D.tabs);
    }
    /* measured as drawn: a rounding the arithmetic missed wraps rather than cutting */
    wrap = wrap || (!fits && note_text_width(label, text) > lv_obj_get_content_width(label));
    if (wrap != D.note_two_lines) {
        D.note_two_lines = wrap;
        lv_label_set_long_mode(label, wrap ? LV_LABEL_LONG_MODE_WRAP : LV_LABEL_LONG_MODE_DOTS);
        const int32_t line = lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN));
        lv_obj_set_height(label, wrap ? 2 * line + lv_obj_get_style_text_line_space(label, LV_PART_MAIN)
                                      : line);
        moved = true;
    }
    if (moved && D.tab_on != NULL) {
        lv_obj_update_layout(D.tabs);
        lv_obj_scroll_to_view(D.tab_on, LV_ANIM_OFF);
    }
}

/* The note under the title: what needs you, then what is on (mockup-next n-devices): at most two
 * facts, on one line rather than pushing the tiles down (UI-020). What a fact says about a device
 * is never the part cut: its name shortens first, to NOTE_NAME_MIN bytes, then the second fact
 * gives way, then the name goes down to "…", and what still doesn't fit takes its room from the
 * tabs (note_make_room) (BL-171). Live words: written at most once a second (UI-023). */
static void note(void)
{
    if (D.arranging) {
        /* TR: Smart Home, arranging a page with nothing picked: holding a tile and dragging it moves
         * it, a tap picks it for its size and Remove. One line beside Add and Done: keep it as
         * short. At most 127 bytes */
        const char *tap = D.tab_favorites
                              ? TR("Hold a tile to drag it, or tap it to resize or remove it")
                              /* TR: the same on a room's page, where tiles can't be removed */
                              : TR("Hold a tile to drag it, or tap it to resize it");
        /* TR: Smart Home, arranging with a tile picked: holding and dragging it, or the arrows,
         * move it. One line beside the arrows, Small, Wide, Remove and Done: keep it as short.
         * At most 127 bytes */
        const char *picked = TR("Hold to drag, or use arrows");
        ui_set_str(&subject_devices_note, D.pick[0] != '\0' ? picked : tap);
        return;
    }
    note_fact_t f[2] = {{NULL, NULL}, {NULL, NULL}};
    size_t nf = 0;
    size_t lights = 0;
    size_t offline = 0;
    for (int pass = 0; pass < 4 && nf < 2u; pass++) {
        for (size_t p = 0; p < D.npage && nf < 2u; p++) {
            const ui_dev_t *d = &D.devs[D.page[p]];
            if (pass == 0 && d->status != UI_DEV_OFFLINE && d->state == UI_DEV_ST_JAMMED) {
                /* TR: Smart Home's note: a lock reports a jam; {0} is its name, shortened with "…"
                 * to fit. At most 40 bytes */
                f[nf].pattern = TR("{0} jammed");
                f[nf++].name = d->name;
            } else if (pass == 1 && d->status != UI_DEV_OFFLINE && d->kind == DEV_LOCK &&
                       d->state == UI_DEV_ST_UNLOCKED) {
                /* TR: Smart Home's note: a lock is unlocked; {0} is its name, shortened with "…" to
                 * fit. At most 40 bytes */
                f[nf].pattern = TR("{0} unlocked");
                f[nf++].name = d->name;
            } else if (pass == 1 && d->status != UI_DEV_OFFLINE && entrance(d) &&
                       d->state == UI_DEV_ST_OPEN) {
                /* TR: Smart Home's note: a garage door is open; {0} is its name, shortened with "…"
                 * to fit. At most 40 bytes */
                f[nf].pattern = TR_C("smart home note", "{0} open");
                f[nf++].name = d->name;
            } else if (pass == 2 && d->status == UI_DEV_OFFLINE) {
                /* TR: Smart Home's note: a device can't be reached; {0} is its name, shortened with
                 * "…" to fit. At most 40 bytes */
                f[nf].pattern = TR("{0} offline");
                f[nf++].name = d->name;
            } else if (pass == 3 && d->status == UI_DEV_FAILED) {
                /* what didn't happen, or that it isn't known whether it did (HCTL-012) */
                f[nf].pattern = failed_sentence(d);
                f[nf++].name = d->name;
            }
        }
    }
    for (size_t p = 0; p < D.npage; p++) {
        const ui_dev_t *d = &D.devs[D.page[p]];
        lights += d->kind == DEV_LIGHT && d->status != UI_DEV_OFFLINE && is_on(d) ? 1u : 0u;
        offline += d->status == UI_DEV_OFFLINE ? 1u : 0u;
    }
    char count[NOTE_FACT];
    if (nf < 2u && lights > 0u) {
        /* TR: Smart Home's note: how many lights are on */
        I18N_FORMAT(count, sizeof(count), TRN("{0} light on", "{0} lights on", lights), (unsigned)lights);
        f[nf].pattern = count;
        f[nf++].name = NULL;
    }
    char text[UI_SUBJECT_STRING_LENGTH] = "";
    bool fits = true;
    if (D.npage > 0u && offline == D.npage) {
        /* TR: Smart Home's note when no device on the page can be reached (the link is down) */
        i18n_utf8_copy(text, sizeof(text), TR("Your devices can't be reached right now"));
    } else if (nf == 0u) {
        /* TR: Smart Home's note when nothing needs attention and no light is on */
        i18n_utf8_copy(text, sizeof(text), TR("Nothing needs you"));
    } else {
        const bool named = f[0].name != NULL || (nf > 1u && f[1].name != NULL);
        fits = false;
        for (size_t n = nf; n > 0u && !fits; n--) {
            const size_t least = n > 1u ? NOTE_NAME_MIN : 3u; /* 3: "…" alone */
            for (size_t room = NOTE_FACT - 1u;; room--) {
                note_join(text, sizeof(text), f, n, room);
                fits = note_fits(text);
                if (fits || !named || room <= least) {
                    break;
                }
            }
        }
    }
    /* what didn't fit on the line takes its room from the tabs, never from the outcome */
    note_make_room(text, fits);
    ui_set_str(&subject_devices_note, text);
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
    /* a keypad controls no devices: its rail has no Smart Home (BL-048) */
    ui_set_int(&subject_devices_avail, D.n > 0u && !ui_link_keypad() ? 1 : 0);
    if ((D.n == 0u || ui_link_keypad()) && on_view()) {
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
    D.tabs_max = lv_obj_get_style_max_width(D.tabs, LV_PART_MAIN); /* note_make_room gives it back */
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
    if (D.head == NULL || D.arr_head == NULL || D.pencil == NULL || D.arr_add == NULL ||
        D.arr_prev == NULL || D.arr_next == NULL || D.arr_size == NULL || D.arr_remove == NULL ||
        D.arr_done == NULL) {
        return;
    }
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
    if (D.devs == NULL || D.layout == NULL || D.blob == NULL) {
        return;
    }
    D.ready = true;
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
    }
}

void ui_devices_shown(void)
{
    if (!D.ready) {
        return;
    }
    load_layout(); /* Arrange may have changed it */
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
