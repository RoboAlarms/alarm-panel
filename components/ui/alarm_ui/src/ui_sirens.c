/*
 * ui_sirens.c - Installer > Sirens and outputs, and Installer > 433 MHz (docs/features/16, 25;
 * the mockup's BL-037 proposal): every siren the panel sounds, a wired output's page, a 433 MHz
 * siren's page and its pairing, and the 433 MHz sensors and sirens. A wired output "For
 * automations" (BL-224, docs/features/16 OUT-023) has a name, its terminal and relay board, and
 * Test it; while no automation uses it, its value in the list reads "Not used yet".
 *
 * Everything goes through ui_backend_t's out_* entries (NULL out_status: none of it shows). The
 * backend checks installer mode and the partitions itself; these pages ask only in installer
 * mode. Live words (Sounding, the receiver's state) are read once a second and change in place:
 * nothing is rebuilt by the tick, nothing blinks (UI-020, UI-023).
 */
#include <stdio.h>
#include <string.h>

#include "ui_internal.h"

#define POLL_MS 1000u
#define G_RADIO "\xEE\x85\x82" /* U+E142 radio (Zigbee's too) */

/* ---- the backend ----------------------------------------------------------------------------- */

static bool read_status(ui_out_status_t *st)
{
    memset(st, 0, sizeof(*st));
    return S.be.out_status != NULL && S.be.out_status(S.be.ctx, st);
}

static void read_wired(ui_out_wired_t w[UI_OUT_WIRED_MAX])
{
    memset(w, 0, sizeof(ui_out_wired_t) * UI_OUT_WIRED_MAX);
    if (S.be.out_wired != NULL) {
        S.be.out_wired(S.be.ctx, w);
    }
}

static void read_rf(ui_out_rf_t r[UI_OUT_RF_MAX])
{
    memset(r, 0, sizeof(ui_out_rf_t) * UI_OUT_RF_MAX);
    if (S.be.out_rf != NULL) {
        S.be.out_rf(S.be.ctx, r);
    }
}

static bool installer(void)
{
    return S.program;
}

static const alarm_config_t *cfg_now(void)
{
    return S.program && S.edit != NULL ? S.edit : S.cfg;
}

/* A change's answer in words, as a toast; true when it was done. */
static bool answered(uint8_t r)
{
    switch ((ui_out_result_t)r) {
    case UI_OUT_OK: return true;
    case UI_OUT_NOT_ALLOWED:
        /* TR: a toast: a siren's change or test is refused. At most 63 bytes */
        ui_toast(TR("Not while one of its partitions is armed"));
        break;
    case UI_OUT_BUSY:
        /* TR: a toast: a siren's test is refused while it sounds. At most 63 bytes */
        ui_toast(TR("Not while it sounds"));
        break;
    case UI_OUT_IN_USE:
        /* TR: a toast: a terminal chosen for an output is a zone's or another output's */
        ui_toast(TR("That terminal is in use: pick a free one"));
        break;
    case UI_OUT_NO_TRANSMITTER:
        /* TR: a toast: nothing can send to a 433 MHz siren */
        ui_toast(TR("Nothing can transmit on 433 MHz"));
        break;
    case UI_OUT_FULL:
        /* TR: a toast: every place for this kind of siren or output is taken */
        ui_toast(TR("No room for another one"));
        break;
    case UI_OUT_INVALID:
        /* TR: a toast: a siren's setting can't be taken (no partition, nothing to sound for) */
        ui_toast(TR("Not saved: it must sound for something, in a partition"));
        break;
    default:
        /* TR: a toast: a siren's or output's change couldn't be made */
        ui_toast(TR("Not saved: try again"));
        break;
    }
    return false;
}

/* ---- words ----------------------------------------------------------------------------------- */

static uint8_t parts_enabled(void)
{
    const alarm_config_t *cfg = cfg_now();
    uint8_t mask = 0;
    for (unsigned i = 0; cfg != NULL && i < ALARM_MAX_PARTITIONS && i < 8u; i++) {
        if (cfg->partitions[i].enabled) {
            mask = (uint8_t)(mask | (1u << i));
        }
    }
    return mask != 0u ? mask : 1u;
}

static void part_name(uint8_t p, char *out, size_t len)
{
    const alarm_config_t *cfg = cfg_now();
    if (cfg != NULL && p >= 1u && p <= ALARM_MAX_PARTITIONS && cfg->partitions[p - 1u].name[0] != '\0') {
        snprintf(out, len, "%s", cfg->partitions[p - 1u].name);
    } else {
        /* TR: the name of a partition nobody has named; {0} is its number */
        I18N_FORMAT(out, len, TR_C("default partition name", "Partition {0}"), (unsigned)p);
    }
}

/* "All", "Home", "Home, Shop", "None". */
static void parts_words(uint8_t parts, bool all_word, char *out, size_t len)
{
    const uint8_t all = parts_enabled();
    out[0] = '\0';
    parts &= all;
    if (parts == 0u) {
        /* TR: a siren's partitions: none */
        i18n_utf8_copy(out, len, TR_C("partitions", "None"));
        return;
    }
    if (all_word && parts == all && (all & (uint8_t)(all - 1u)) != 0u) {
        /* TR: a siren's partitions: every partition */
        i18n_utf8_copy(out, len, TR_C("partitions", "All"));
        return;
    }
    for (uint8_t i = 0; i < ALARM_MAX_PARTITIONS && i < 8u; i++) {
        if ((parts & (1u << i)) == 0u) {
            continue;
        }
        char name[ALARM_NAME_LEN];
        part_name((uint8_t)(i + 1u), name, sizeof(name));
        if (out[0] == '\0') {
            i18n_utf8_copy(out, len, name);
        } else {
            char joined[96];
            I18N_FORMAT(joined, sizeof(joined), TR_C("list of names", "{0}, {1}"), out, name);
            i18n_utf8_copy(out, len, joined);
        }
    }
}

/* "Home", "Home, also Shop" (a siren's line in the lists). */
static void scope_words(uint8_t parts, uint8_t also, char *out, size_t len)
{
    char own[64];
    parts_words(parts, true, own, sizeof(own));
    if ((also & parts_enabled() & (uint8_t)~parts) == 0u) {
        i18n_utf8_copy(out, len, own);
        return;
    }
    char other[64];
    parts_words(also & (uint8_t)~parts, false, other, sizeof(other));
    /* TR: a siren's partitions in a list's line: {0} its own, {1} the ones it also sounds for */
    I18N_FORMAT(out, len, TR("{0}, also {1}"), own, other);
}

static const char *fn_title(uint8_t fn)
{
    switch (fn) {
    /* TR: what a wired output is: a row's value, a choice. At most 31 bytes */
    case UI_OUT_FN_SIREN: return TR_C("wired output", "Siren");
    /* TR: what a wired output is: a row's value, a choice. At most 31 bytes */
    case UI_OUT_FN_STROBE: return TR_C("wired output", "Strobe");
    /* TR: what a wired output is: a row's value, a choice. At most 31 bytes */
    case UI_OUT_FN_SMOKE_RESET: return TR_C("wired output", "Smoke detector power");
    /* TR: what a wired output is: a row's value, a choice. At most 31 bytes */
    case UI_OUT_FN_ARMED: return TR_C("wired output", "Armed light");
    /* TR: what a wired output is (BL-224): a relay that automations switch, the porch or garden
     * lights. A choice, and the title of one with no name. At most 31 bytes */
    case UI_OUT_FN_AUTOMATION: return TR_C("wired output", "For automations");
    default: return "";
    }
}

static const char *fn_detail(uint8_t fn)
{
    switch (fn) {
    /* TR: under a choice of what a wired output is */
    case UI_OUT_FN_SIREN: return TR("Sounds with the alarm");
    /* TR: under a choice of what a wired output is */
    case UI_OUT_FN_STROBE: return TR("On from an audible alarm until disarmed");
    /* TR: under a choice of what a wired output is */
    case UI_OUT_FN_SMOKE_RESET: return TR("Cut briefly to reset 4-wire smoke detectors");
    /* TR: under a choice of what a wired output is */
    case UI_OUT_FN_ARMED: return TR("On while its partitions are armed");
    /* TR: under the choice "For automations" of what a wired output is */
    case UI_OUT_FN_AUTOMATION: return TR("Switched on and off by Settings > Automations");
    default: return "";
    }
}

static const char *fn_glyph(uint8_t fn)
{
    switch (fn) {
    case UI_OUT_FN_SIREN: return UI_ICON_SIREN;
    case UI_OUT_FN_STROBE: return UI_ICON_SUN;
    case UI_OUT_FN_SMOKE_RESET: return UI_ICON_ALARM_SMOKE;
    case UI_OUT_FN_AUTOMATION: return UI_ICON_POWER;
    default: return UI_ICON_LIGHTBULB;
    }
}

static lv_color_t fn_tone(uint8_t fn)
{
    return fn == UI_OUT_FN_SIREN || fn == UI_OUT_FN_SMOKE_RESET ? ui_tone_red()
           : fn == UI_OUT_FN_ARMED                              ? ui_tone_amber()
                                                                : ui_tone_blue();
}

/* A wired output's title: an output for automations by its name (BL-224), the others by what
 * they are. */
static const char *wired_title(const ui_out_wired_t *w)
{
    return w->function == UI_OUT_FN_AUTOMATION && w->name[0] != '\0' ? w->name : fn_title(w->function);
}

/* The outputs for automations that no automation names yet (bit n: channel n). 0 when this
 * firmware has no automations to ask (nothing is said then). */
static uint8_t auto_unused(const ui_out_wired_t w[UI_OUT_WIRED_MAX])
{
    static auto_rule_t *rules;
    uint8_t mask = 0u;
    for (size_t i = 0; i < UI_OUT_WIRED_MAX && i < 8u; i++) {
        if (w[i].function == UI_OUT_FN_AUTOMATION) {
            mask = (uint8_t)(mask | (1u << i));
        }
    }
    if (mask == 0u || S.be.auto_list == NULL ||
        (rules == NULL && (rules = ui_slow_alloc(AUTO_MAX * sizeof(*rules))) == NULL)) {
        return 0u;
    }
    const size_t n = S.be.auto_list(S.be.ctx, rules, AUTO_MAX);
    for (size_t r = 0; r < n && r < AUTO_MAX; r++) {
        for (size_t k = 0; k < rules[r].n && k < AUTO_ACTIONS; k++) {
            const auto_action_t *a = &rules[r].then[k];
            if (a->kind == AUTO_ACT_OUTPUT && a->output >= 1u && a->output <= UI_OUT_WIRED_MAX) {
                mask = (uint8_t)(mask & (uint8_t)~(1u << (a->output - 1u)));
            }
        }
    }
    return mask;
}

/* "GPIO 49" */
static void pin_words(uint8_t gpio, char *out, size_t len)
{
    /* TR: a wired terminal, by the pin printed beside it. At most 23 bytes */
    I18N_FORMAT(out, len, TR("GPIO {0}"), (unsigned)gpio);
}

/* "433 MHz siren 2" */
static void rf_name(uint8_t slot, char *out, size_t len)
{
    /* TR: a 433 MHz siren's name; {0} its number, 1 to 4. At most 31 bytes */
    I18N_FORMAT(out, len, TR("433 MHz siren {0}"), (unsigned)slot + 1u);
}

/* "2 min", "3 h", "2 days" - how long ago. */
static void ago_words(uint64_t ms, char *out, size_t len)
{
    const uint32_t min = (uint32_t)(ms / 60000u);
    if (min < 60u) {
        /* TR: how long ago a 433 MHz sensor was heard (minutes). At most 18 bytes */
        I18N_FORMAT(out, len, TRN("{0} min", "{0} min", min == 0u ? 1u : min), (unsigned)(min == 0u ? 1u : min));
    } else if (min < 48u * 60u) {
        /* TR: how long ago a 433 MHz sensor was heard (hours). At most 18 bytes */
        I18N_FORMAT(out, len, TRN("{0} h", "{0} h", min / 60u), (unsigned)(min / 60u));
    } else {
        /* TR: how long ago a 433 MHz sensor was heard (days). At most 18 bytes */
        I18N_FORMAT(out, len, TRN("{0} day", "{0} days", min / 1440u), (unsigned)(min / 1440u));
    }
}

/* ---- the pick sheets ------------------------------------------------------------------------- */

#define PICK_MAX 20u

typedef enum {
    PICK_NONE = 0,
    PICK_ADD_FN,      /* adding a wired output: what it is */
    PICK_ADD_PIN,     /* adding a wired output: its terminal */
    PICK_FN,
    PICK_PIN,
    PICK_SENSE,
    PICK_RELAY,
    PICK_PARTS,
    PICK_ALSO,
} pick_t;

static struct {
    uint8_t what;     /* pick_t */
    bool rf;          /* the page is a 433 MHz siren's */
    uint8_t slot;
    uint8_t add_fn;   /* adding: the function picked */
    size_t n;
    ui_choice_t choices[PICK_MAX];
    uint16_t value[PICK_MAX];
    char words[PICK_MAX][48];
} s_pick;

static void pick_start(pick_t what)
{
    s_pick.what = (uint8_t)what;
    s_pick.n = 0;
}

static void pick_add(const char *glyph, lv_color_t tone, const char *title, const char *detail,
                     uint16_t value)
{
    if (s_pick.n >= PICK_MAX) {
        return;
    }
    i18n_utf8_copy(s_pick.words[s_pick.n], sizeof(s_pick.words[0]), title);
    s_pick.choices[s_pick.n] = (ui_choice_t){.glyph = glyph, .tone = tone,
                                             .title = s_pick.words[s_pick.n], .detail = detail};
    s_pick.value[s_pick.n] = value;
    s_pick.n++;
}

static int32_t pick_index(uint16_t value)
{
    for (size_t i = 0; i < s_pick.n; i++) {
        if (s_pick.value[i] == value) {
            return (int32_t)i;
        }
    }
    return -1;
}

/* The partitions to choose from: all, then each alone (with more than one). */
static void add_parts(uint8_t exclude, bool none, const char *none_detail)
{
    const uint8_t all = (uint8_t)(parts_enabled() & (uint8_t)~exclude);
    if (none) {
        /* TR: a choice: a siren sounds for no other partition */
        pick_add(UI_ICON_BELL_OFF, ui_tone_gray(), TR_C("partitions", "None"), none_detail, 0u);
    }
    if (all == 0u) {
        return;
    }
    if ((all & (uint8_t)(all - 1u)) != 0u) {
        /* TR: a choice: every partition (that is not already the siren's own) */
        pick_add(UI_ICON_WAREHOUSE, ui_tone_violet(), exclude != 0u ? TR("All the others") : TR("All partitions"),
                 NULL, all);
    }
    for (uint8_t i = 0; i < ALARM_MAX_PARTITIONS && i < 8u; i++) {
        if ((all & (1u << i)) != 0u) {
            char name[ALARM_NAME_LEN];
            part_name((uint8_t)(i + 1u), name, sizeof(name));
            pick_add(UI_ICON_WAREHOUSE, ui_tone_violet(), name, NULL, (uint16_t)(1u << i));
        }
    }
}

/* ---- the installer menu's rows --------------------------------------------------------------- */

static struct {
    lv_obj_t *sirens;
    lv_obj_t *rf;
    uint32_t polled;
} s_menu;

static void open_page_cb(void *ctx)
{
    ui_page_open((page_id_t)(intptr_t)ctx, 0);
}

/* Zones whose sensor is on 433 MHz: their count, and the first max of them. */
static size_t rf_zones(uint16_t *out, size_t max)
{
    size_t n = 0u;
    if (S.be.zone_info == NULL) {
        return 0u;
    }
    for (uint16_t z = 1; z <= S.snap.zones_used && z <= ALARM_MAX_ZONES; z++) {
        zone_hub_info_t info;
        if (S.be.zone_info(S.be.ctx, z, &info) && info.binding.in[ZSRC_ROLE_MAIN].addr.kind == ZSRC_RF) {
            if (n < max && out != NULL) {
                out[n] = z;
            }
            n++;
        }
    }
    return n;
}

static size_t rf_sirens_used(const ui_out_rf_t r[UI_OUT_RF_MAX])
{
    size_t n = 0u;
    for (size_t i = 0; i < UI_OUT_RF_MAX; i++) {
        n += r[i].used ? 1u : 0u;
    }
    return n;
}

static size_t wired_sirens(const ui_out_wired_t w[UI_OUT_WIRED_MAX])
{
    size_t n = 0u;
    for (size_t i = 0; i < UI_OUT_WIRED_MAX; i++) {
        n += w[i].function == UI_OUT_FN_SIREN ? 1u : 0u;
    }
    return n;
}

/* The two rows' values: "4 sirens" (amber "Siren problem"), "3 sensors, 1 siren". */
static void menu_words(void)
{
    ui_out_status_t st;
    if (!read_status(&st)) {
        return;
    }
    ui_out_wired_t w[UI_OUT_WIRED_MAX];
    ui_out_rf_t r[UI_OUT_RF_MAX];
    read_wired(w);
    read_rf(r);
    char value[48];
    if (s_menu.sirens != NULL) {
        const size_t n = wired_sirens(w) + rf_sirens_used(r);
        if (st.trouble) {
            /* TR: the Sirens and outputs row's value: a siren is in trouble. At most 31 bytes */
            i18n_utf8_copy(value, sizeof(value), TR("Siren problem"));
        } else if (n == 0u) {
            /* TR: the Sirens and outputs row's value: none set up. At most 31 bytes */
            i18n_utf8_copy(value, sizeof(value), TR_C("sirens", "None"));
        } else {
            /* TR: the Sirens and outputs row's value. At most 31 bytes */
            I18N_FORMAT(value, sizeof(value), TRN("{0} siren", "{0} sirens", n), (unsigned)n);
        }
        lv_obj_t *l = ui_find(s_menu.sirens, "value_label");
        if (l != NULL && strcmp(lv_label_get_text(l), value) != 0) {
            lv_label_set_text(l, value);
        }
        if (l != NULL) {
            lv_obj_set_style_text_color(l, st.trouble ? ui_tone_amber() : ui_tone_gray(), 0);
        }
    }
    if (s_menu.rf != NULL) {
        lv_color_t color = ui_tone_gray();
        if (st.rf_receiver == UI_RF_RX_JAMMED) {
            /* TR: the 433 MHz row's value: something drowns the band. At most 31 bytes */
            i18n_utf8_copy(value, sizeof(value), TR_C("radio state", "Interference"));
            color = ui_tone_amber();
        } else if (st.rf_receiver != UI_RF_RX_WORKING && !st.rf_transmitter) {
            /* TR: the 433 MHz row's value: no receiver answers. At most 31 bytes */
            i18n_utf8_copy(value, sizeof(value), TR_C("radio state", "Not connected"));
        } else {
            const unsigned zones = (unsigned)rf_zones(NULL, 0u);
            const unsigned sirens = (unsigned)rf_sirens_used(r);
            char a[24];
            char b[24];
            /* TR: part of the 433 MHz row's value. At most 15 bytes */
            I18N_FORMAT(a, sizeof(a), TRN("{0} sensor", "{0} sensors", zones), zones);
            /* TR: part of the 433 MHz row's value. At most 15 bytes */
            I18N_FORMAT(b, sizeof(b), TRN("{0} siren", "{0} sirens", sirens), sirens);
            I18N_FORMAT(value, sizeof(value), TR_C("list of names", "{0}, {1}"), a, b);
        }
        lv_obj_t *l = ui_find(s_menu.rf, "value_label");
        if (l != NULL && strcmp(lv_label_get_text(l), value) != 0) {
            lv_label_set_text(l, value);
        }
        if (l != NULL) {
            lv_obj_set_style_text_color(l, color, 0);
        }
    }
}

void ui_sirens_rf_menu_row(lv_obj_t *body)
{
    ui_out_status_t st;
    if (!read_status(&st)) {
        return;
    }
    ui_out_rf_t r[UI_OUT_RF_MAX];
    read_rf(r);
    const bool any = st.rf_receiver == UI_RF_RX_WORKING || st.rf_receiver == UI_RF_RX_JAMMED ||
                     st.rf_transmitter || rf_sirens_used(r) != 0u || rf_zones(NULL, 0u) != 0u;
    if (!any) {
        return; /* nothing on 433 MHz here: no row (the proposal's menu) */
    }
    s_menu.rf = ui_row_link(body, G_RADIO, ui_tone_blue(), NOTR("433 MHz") /* a band */,
                            /* TR: the 433 MHz row's detail in the installer menu, and its page's note */
                            TR("Sensors and sirens on 433 MHz"), "-", UI_ICON_CHEVRON_RIGHT,
                            open_page_cb, (void *)(intptr_t)PAGE_RF);
    menu_words();
    s_menu.polled = lv_tick_get();
}

void ui_sirens_menu_row(lv_obj_t *body)
{
    ui_out_status_t st;
    if (!read_status(&st)) {
        return;
    }
    /* TR: a row in the installer menu (BL-037) */
    s_menu.sirens = ui_row_link(body, UI_ICON_SIREN, ui_tone_red(), TR("Sirens and outputs"),
                                /* TR: under "Sirens and outputs" in the installer menu, and its
                                 * page's note */
                                TR("What sounds and what switches when the alarm does"), "-",
                                UI_ICON_CHEVRON_RIGHT, open_page_cb, (void *)(intptr_t)PAGE_SIRENS);
    menu_words();
    s_menu.polled = lv_tick_get();
}

/* ---- Sirens and outputs (PAGE_SIRENS) -------------------------------------------------------- */

static struct {
    bool open;
    lv_obj_t *wired[UI_OUT_WIRED_MAX];
    lv_obj_t *rf[UI_OUT_RF_MAX];
    uint8_t unused; /* auto_unused() as the page was built */
    uint32_t polled;
} s_list;

static void open_wired(void *ctx)
{
    ui_page_open(PAGE_OUT_WIRED, (int32_t)(intptr_t)ctx);
}

static void open_rf_siren(void *ctx)
{
    ui_page_open(PAGE_OUT_RF, (int32_t)(intptr_t)ctx);
}

static const char *wired_state(const ui_out_wired_t *w, bool unused)
{
    if (w->function == UI_OUT_FN_AUTOMATION && unused && !w->on) {
        /* TR: an output for automations' state in a list while no automation uses it (BL-224).
         * At most 23 bytes */
        return TR_C("output state", "Not used yet");
    }
    if (w->trouble) {
        /* TR: a wired siren's state in a list: its supervision input is open. At most 23 bytes */
        return TR("Siren problem");
    }
    if (w->function == UI_OUT_FN_SIREN || w->function == UI_OUT_FN_STROBE) {
        /* TR: a siren's state in a list. At most 23 bytes */
        return w->on ? TR_C("siren state", "Sounding") : TR_C("siren state", "Ready");
    }
    /* TR: a wired output's state in a list. At most 23 bytes */
    return w->on ? TR_C("output state", "On") : TR_C("output state", "Off");
}

static void wired_line(const ui_out_wired_t *w, const char *where, char *out, size_t len)
{
    char pin[24];
    char scope[96];
    pin_words(w->gpio, pin, sizeof(pin));
    if (w->function == UI_OUT_FN_SMOKE_RESET || w->function == UI_OUT_FN_AUTOMATION) {
        /* TR: a wired output's line in a list: {0} "GPIO 49", {1} "J7 header" */
        I18N_FORMAT(out, len, TR("{0} on the {1}"), pin, where != NULL ? where : "");
        return;
    }
    scope_words(w->partitions, w->also, scope, sizeof(scope));
    /* TR: a wired output's line in a list: {0} "GPIO 49", {1} its partitions ("Home, also Shop") */
    I18N_FORMAT(out, len, TR("Terminal {0} · {1}"), pin, scope);
}

static void add_wired(void *ctx);
static void add_rf(void *ctx);

static void list_build(lv_obj_t *body, char *note, size_t nlen)
{
    ui_out_status_t st;
    if (!read_status(&st)) {
        return;
    }
    i18n_utf8_copy(note, nlen, TR("What sounds and what switches when the alarm does"));
    ui_out_wired_t w[UI_OUT_WIRED_MAX];
    ui_out_rf_t r[UI_OUT_RF_MAX];
    read_wired(w);
    read_rf(r);
    char line[128];
    s_list.unused = auto_unused(w);
    ui_paragraph(body,
                 /* TR: the first paragraph on Sirens and outputs */
                 TR("The panel's own speaker always sounds too. Sirens follow it: steady for "
                    "burglary, the fire pattern for fire, the CO pattern for CO."),
                 ui_tone_gray());
    if (st.wired) {
        /* TR: a section on Sirens and outputs: outputs on the panel's own terminals */
        ui_section(body, TR("On the panel's terminals"));
        size_t used = 0u;
        for (size_t i = 0; i < UI_OUT_WIRED_MAX; i++) {
            if (w[i].function == UI_OUT_FN_NONE) {
                s_list.wired[i] = NULL;
                continue;
            }
            used++;
            wired_line(&w[i], st.where, line, sizeof(line));
            s_list.wired[i] = ui_row_link(body, fn_glyph(w[i].function), fn_tone(w[i].function),
                                          wired_title(&w[i]), line,
                                          wired_state(&w[i], (s_list.unused & (1u << i)) != 0u),
                                          UI_ICON_CHEVRON_RIGHT, open_wired, (void *)(intptr_t)i);
        }
        if (used < UI_OUT_WIRED_MAX) {
            /* TR: a row on Sirens and outputs */
            lv_obj_t *row = ui_row_link(body, UI_ICON_PLUS, ui_tone_green(), TR("Add a wired siren or output"),
                                        /* TR: under "Add a wired siren or output" */
                                        TR("A relay or MOSFET module on a free terminal"), NULL,
                                        UI_ICON_CHEVRON_RIGHT, add_wired, NULL);
            if (!installer()) {
                ui_row_disable(row);
            }
        }
    }
    if (st.rf_transmitter || rf_sirens_used(r) != 0u) {
        ui_section(body, NOTR("433 MHz") /* a band */);
        for (size_t i = 0; i < UI_OUT_RF_MAX; i++) {
            if (!r[i].used) {
                s_list.rf[i] = NULL;
                continue;
            }
            char name[48];
            char scope[96];
            rf_name((uint8_t)i, name, sizeof(name));
            scope_words(r[i].partitions, r[i].also, scope, sizeof(scope));
            /* TR: a 433 MHz siren's line in a list: {0} its partitions. At most 95 bytes */
            I18N_FORMAT(line, sizeof(line), TR("{0} · Not supervised"), scope);
            s_list.rf[i] = ui_row_link(body, UI_ICON_SIREN, ui_tone_red(), name, line,
                                       r[i].sounding ? TR_C("siren state", "Sounding")
                                                     : TR_C("siren state", "Ready"),
                                       UI_ICON_CHEVRON_RIGHT, open_rf_siren, (void *)(intptr_t)i);
        }
        if (rf_sirens_used(r) < UI_OUT_RF_MAX) {
            /* TR: a row on Sirens and outputs and on the 433 MHz page */
            lv_obj_t *row = ui_row_link(body, UI_ICON_PLUS, ui_tone_green(), TR("Add a 433 MHz siren"),
                                        /* TR: under "Add a 433 MHz siren" */
                                        TR("One that learns its panel like a key fob"), NULL,
                                        UI_ICON_CHEVRON_RIGHT, add_rf, NULL);
            if (!installer()) {
                ui_row_disable(row);
            }
        }
    }
    ui_paragraph(body,
                 /* TR: the last paragraph on Sirens and outputs */
                 TR("Zigbee and Z-Wave sirens are set on their device pages, under Zigbee and Z-Wave."),
                 ui_tone_gray());
    s_list.open = true;
    s_list.polled = lv_tick_get();
}

static void list_tick(void)
{
    if (!s_list.open || lv_tick_elaps(s_list.polled) < POLL_MS) {
        return;
    }
    s_list.polled = lv_tick_get();
    ui_out_wired_t w[UI_OUT_WIRED_MAX];
    ui_out_rf_t r[UI_OUT_RF_MAX];
    read_wired(w);
    read_rf(r);
    for (size_t i = 0; i < UI_OUT_WIRED_MAX; i++) {
        lv_obj_t *l = s_list.wired[i] != NULL ? ui_find(s_list.wired[i], "value_label") : NULL;
        if (l != NULL && w[i].function != UI_OUT_FN_NONE) {
            const char *t = wired_state(&w[i], (s_list.unused & (1u << i)) != 0u);
            if (strcmp(lv_label_get_text(l), t) != 0) {
                lv_label_set_text(l, t);
                lv_obj_set_style_text_color(l, w[i].trouble ? ui_tone_amber() : ui_tone_gray(), 0);
            }
        }
    }
    for (size_t i = 0; i < UI_OUT_RF_MAX; i++) {
        lv_obj_t *l = s_list.rf[i] != NULL ? ui_find(s_list.rf[i], "value_label") : NULL;
        if (l != NULL && r[i].used) {
            const char *t = r[i].sounding ? TR_C("siren state", "Sounding") : TR_C("siren state", "Ready");
            if (strcmp(lv_label_get_text(l), t) != 0) {
                lv_label_set_text(l, t);
            }
        }
    }
}

/* ---- adding a wired output: what it is, then its terminal ------------------------------------ */

static int free_wired_slot(void)
{
    ui_out_wired_t w[UI_OUT_WIRED_MAX];
    read_wired(w);
    for (size_t i = 0; i < UI_OUT_WIRED_MAX; i++) {
        if (w[i].function == UI_OUT_FN_NONE) {
            return (int)i;
        }
    }
    return -1;
}

static void pin_pick(uint8_t slot, pick_t what, uint8_t keep, bool none);

static void picked(int32_t index);

static void add_wired(void *ctx)
{
    (void)ctx;
    const int slot = free_wired_slot();
    if (slot < 0) {
        (void)answered(UI_OUT_FULL);
        return;
    }
    pick_start(PICK_ADD_FN);
    s_pick.slot = (uint8_t)slot;
    for (uint8_t fn = UI_OUT_FN_SIREN; fn <= UI_OUT_FN_AUTOMATION; fn++) { /* For automations last */
        pick_add(fn_glyph(fn), fn_tone(fn), fn_title(fn), fn_detail(fn), fn);
    }
    /* TR: a pick sheet's title: adding a wired output */
    ui_pick_sheet(TR("Add a wired output"), TR("What it switches"), s_pick.choices, s_pick.n, -1, picked);
}

/* A free terminal's choice sheet (keep: the channel's own pin stays on offer; none: "None"). */
static void pin_pick(uint8_t slot, pick_t what, uint8_t keep, bool none)
{
    uint8_t pins[16];
    size_t n = S.be.out_terminals != NULL ? S.be.out_terminals(S.be.ctx, slot, pins, 16u) : 0u;
    pick_start(what);
    s_pick.slot = slot;
    if (none) {
        /* TR: a choice: a wired siren has no supervision input */
        pick_add(UI_ICON_CIRCLE_SLASH, ui_tone_gray(), TR_C("supervision input", "None"),
                 /* TR: under "None" in a wired siren's supervision input choices */
                 TR("Its circuit isn't checked"), UI_OUT_NO_PIN);
    }
    for (size_t i = 0; i < n; i++) {
        if (what == PICK_SENSE && pins[i] == keep) {
            continue; /* its output pin can't be its own input */
        }
        char words[24];
        pin_words(pins[i], words, sizeof(words));
        pick_add(UI_ICON_PLUG, ui_tone_blue(), words, NULL, pins[i]);
    }
    if (s_pick.n == 0u) {
        /* TR: a toast: no wired terminal is free for an output */
        ui_toast(TR("No free terminal: every one is a zone's or an output's"));
        return;
    }
    ui_out_status_t st;
    (void)read_status(&st);
    char note[96];
    /* TR: a pick sheet's note: the free terminals; {0} where they are ("J7 header") */
    I18N_FORMAT(note, sizeof(note), TR("Free terminals on the {0}"), st.where != NULL ? st.where : "");
    ui_pick_sheet(what == PICK_SENSE ? TR("Supervision input") : TR("Terminal"), note, s_pick.choices,
                  s_pick.n, -1, picked);
}

/* ---- a wired output's page (PAGE_OUT_WIRED, arg = its slot) ---------------------------------- */

static struct {
    bool open;
    uint8_t slot;
    lv_obj_t *test;
    lv_obj_t *state;
    uint32_t polled;
} s_wired;

static bool wired_now(uint8_t slot, ui_out_wired_t *out)
{
    ui_out_wired_t w[UI_OUT_WIRED_MAX];
    read_wired(w);
    if (slot >= UI_OUT_WIRED_MAX || w[slot].function == UI_OUT_FN_NONE) {
        return false;
    }
    *out = w[slot];
    return true;
}

static void wired_send(const ui_out_wired_t *c)
{
    if (S.be.out_wired_set != NULL && answered(S.be.out_wired_set(S.be.ctx, s_wired.slot, c))) {
        ui_page_rebuild();
    }
}

typedef enum { FLAG_BURGLARY = 0, FLAG_FIRE, FLAG_SQUAWK } flag_t;

static void wired_flag(bool on, void *ctx)
{
    ui_out_wired_t c;
    if (!wired_now(s_wired.slot, &c)) {
        return;
    }
    switch ((flag_t)(uintptr_t)ctx) {
    case FLAG_BURGLARY: c.burglary = on; break;
    case FLAG_FIRE: c.fire = on; break;
    default: c.squawk = on; break;
    }
    if (!c.burglary && !c.fire) {
        /* TR: a toast: a siren's last alarm kind can't be turned off */
        ui_toast(TR("A siren sounds for something: remove it instead"));
        ui_page_rebuild();
        return;
    }
    wired_send(&c);
}

static void wired_open_pick(void *ctx)
{
    ui_out_wired_t c;
    if (!wired_now(s_wired.slot, &c)) {
        return;
    }
    const pick_t what = (pick_t)(intptr_t)ctx;
    s_pick.rf = false;
    s_pick.slot = s_wired.slot;
    switch (what) {
    case PICK_FN:
        pick_start(PICK_FN);
        for (uint8_t fn = UI_OUT_FN_SIREN; fn <= UI_OUT_FN_ARMED; fn++) { /* not For automations */
            pick_add(fn_glyph(fn), fn_tone(fn), fn_title(fn), fn_detail(fn), fn);
        }
        /* TR: a pick sheet's title: what a wired output is */
        ui_pick_sheet(TR("What it is"), NULL, s_pick.choices, s_pick.n, pick_index(c.function), picked);
        break;
    case PICK_PIN: pin_pick(s_wired.slot, PICK_PIN, c.gpio, false); break;
    case PICK_SENSE: pin_pick(s_wired.slot, PICK_SENSE, c.gpio, true); break;
    case PICK_RELAY:
        pick_start(PICK_RELAY);
        /* TR: a choice: the relay board's input switches the relay on when high */
        pick_add(NULL, ui_tone_blue(), TR("Switches on high"), TR("The terminal goes high to switch it on"), 0u);
        /* TR: a choice: the relay board's input switches the relay on when low */
        pick_add(NULL, ui_tone_blue(), TR("Switches on low"), TR("Most relay boards: the terminal goes low"), 1u);
        /* TR: a pick sheet's title: how a wired output's relay board switches */
        ui_pick_sheet(TR("Relay board"), NULL, s_pick.choices, s_pick.n, c.active_low ? 1 : 0, picked);
        break;
    case PICK_PARTS:
        pick_start(PICK_PARTS);
        add_parts(0u, false, NULL);
        /* TR: a pick sheet's title: a siren's own partitions */
        ui_pick_sheet(TR_C("siren", "Partitions"), TR("Their alarms and its chirps"), s_pick.choices,
                      s_pick.n, pick_index(c.partitions), picked);
        break;
    default:
        pick_start(PICK_ALSO);
        /* TR: under "None" in a siren's Also sounds for */
        add_parts(c.partitions, true, TR("Only its own partitions"));
        /* TR: a pick sheet's title: other partitions whose alarms a siren also sounds for */
        ui_pick_sheet(TR("Also sounds for"), TR("Other partitions' alarms, never their chirps"),
                      s_pick.choices, s_pick.n, pick_index(c.also), picked);
        break;
    }
}

static void wired_remove(void *ctx);
static lv_obj_t *link_row(lv_obj_t *body, const char *glyph, lv_color_t tone, const char *title,
                          const char *detail, const char *value, ui_click_fn fn, void *ctx);

static void wired_test(void *ctx)
{
    (void)ctx;
    ui_out_wired_t c;
    const bool automation = wired_now(s_wired.slot, &c) && c.function == UI_OUT_FN_AUTOMATION;
    if (S.be.out_wired_test != NULL && answered(S.be.out_wired_test(S.be.ctx, s_wired.slot))) {
        /* TR: a toast: an output for automations' test started (BL-224) */
        ui_toast(automation ? TR("On for 2 seconds")
                            /* TR: a toast: a wired siren's test started */
                            : TR("Sounding for 2 seconds"));
    }
}

static void wired_renamed(const char *text)
{
    ui_out_wired_t c;
    if (!wired_now(s_wired.slot, &c)) {
        return;
    }
    memset(c.name, 0, sizeof(c.name));
    i18n_utf8_copy(c.name, sizeof(c.name), text);
    wired_send(&c);
}

static void wired_rename(void *ctx)
{
    (void)ctx;
    ui_out_wired_t c;
    if (!wired_now(s_wired.slot, &c)) {
        return;
    }
    /* TR: a text sheet's title and note: naming an output for automations (BL-224) */
    ui_text_sheet(TR("Output name"), TR("Say what it switches: Garden Lights, Porch Light"), c.name,
                  false, UI_OUT_NAME_LEN - 1u, wired_renamed);
}

/* An output for automations' page (BL-224, the mockup's bl-224-output-add): its name, terminal and
 * relay board, Test it, Remove it; no partitions, no chirps. */
static void auto_build(const ui_out_wired_t *c, const char *pin, lv_obj_t *body)
{
    ui_section(body, TR("How it is wired"));
    link_row(body, UI_ICON_TAG, ui_tone_blue(), TR("Name"), NULL,
             c->name[0] != '\0' ? c->name
                                /* TR: an output for automations' Name row with no name. At most 23 bytes */
                                : TR_C("output name", "None"),
             wired_rename, NULL);
    link_row(body, UI_ICON_PLUG, ui_tone_blue(), TR("Terminal"), NULL, pin, wired_open_pick,
             (void *)(intptr_t)PICK_PIN);
    link_row(body, UI_ICON_ZAP, ui_tone_blue(), TR("Relay board"), TR("Most switch on low"),
             c->active_low ? TR("Switches on low") : TR("Switches on high"), wired_open_pick,
             (void *)(intptr_t)PICK_RELAY);
    ui_section(body, TR_C("siren section", "Check it"));
    /* TR: a row on an output for automations' page (BL-224): switch it on briefly */
    s_wired.test = link_row(body, UI_ICON_POWER, ui_tone_blue(), TR("Test it"),
                            TR("Switches it on for 2 seconds"), NULL, wired_test, NULL);
    link_row(body, UI_ICON_TRASH, ui_tone_red(), TR("Remove it"), TR("Its terminal goes back to the zones"),
             NULL, wired_remove, NULL);
    s_wired.open = true;
}

static void wired_remove_ok(void)
{
    ui_out_wired_t c;
    memset(&c, 0, sizeof(c));
    c.function = UI_OUT_FN_NONE;
    c.sense = UI_OUT_NO_PIN;
    if (S.be.out_wired_set != NULL && answered(S.be.out_wired_set(S.be.ctx, s_wired.slot, &c))) {
        ui_page_back();
    }
}

static void wired_remove(void *ctx)
{
    (void)ctx;
    /* TR: a confirmation's title and text: removing a wired siren or output */
    ui_confirm(TR("Remove it?"), TR("It stops switching, and its terminal goes back to the zones."),
               TR("Remove"), wired_remove_ok);
}

static lv_obj_t *link_row(lv_obj_t *body, const char *glyph, lv_color_t tone, const char *title,
                          const char *detail, const char *value, ui_click_fn fn, void *ctx)
{
    lv_obj_t *row = ui_row_link(body, glyph, tone, title, detail, value, UI_ICON_CHEVRON_RIGHT, fn, ctx);
    if (!installer()) {
        ui_row_disable(row);
    }
    return row;
}

static lv_obj_t *toggle_row(lv_obj_t *body, const char *glyph, lv_color_t tone, const char *title,
                            const char *detail, bool on, ui_toggle_fn fn, void *ctx)
{
    lv_obj_t *row = ui_row_toggle_icon(body, glyph, tone, title, detail, on, fn, ctx);
    if (!installer()) {
        ui_row_disable(row);
    }
    return row;
}

static void scope_rows(lv_obj_t *body, uint8_t parts, uint8_t also, ui_click_fn fn)
{
    char value[96];
    parts_words(parts, true, value, sizeof(value));
    /* TR: a row on a siren's page: its own partitions */
    link_row(body, UI_ICON_WAREHOUSE, ui_tone_violet(), TR_C("siren", "Partitions"),
             /* TR: under a siren's Partitions */
             TR("Their alarms and its chirps"), value, fn, (void *)(intptr_t)PICK_PARTS);
    parts_words(also & (uint8_t)~parts, false, value, sizeof(value));
    /* TR: a row on a siren's page: other partitions whose alarms it also sounds for (OUT-015) */
    link_row(body, UI_ICON_LAYERS, ui_tone_violet(), TR("Also sounds for"),
             /* TR: under a siren's Also sounds for */
             TR("Other partitions' alarms, never their chirps"), value, fn, (void *)(intptr_t)PICK_ALSO);
}

static void wired_build(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note, size_t nlen)
{
    s_wired.slot = (uint8_t)arg;
    ui_out_wired_t c;
    ui_out_status_t st;
    if (!read_status(&st) || !wired_now(s_wired.slot, &c)) {
        /* TR: a page's title: a wired output that was removed meanwhile */
        i18n_utf8_copy(title, tlen, TR("Wired output"));
        ui_paragraph(body, TR("It was removed."), ui_tone_gray());
        return;
    }
    i18n_utf8_copy(title, tlen, wired_title(&c));
    char pin[24];
    pin_words(c.gpio, pin, sizeof(pin));
    /* TR: a wired output page's note: {0} "GPIO 49", {1} "J7 header" */
    I18N_FORMAT(note, nlen, TR("On terminal {0} · {1}"), pin, st.where != NULL ? st.where : "");
    if (c.function == UI_OUT_FN_AUTOMATION) {
        auto_build(&c, pin, body);
        return;
    }
    char value[64];
    if (c.function == UI_OUT_FN_SIREN) {
        /* TR: a section heading on a siren's page: which alarms sound it */
        ui_section(body, TR_C("siren section", "It sounds for"));
        /* TR: a switch on a siren's page */
        toggle_row(body, UI_ICON_SIREN, ui_tone_red(), TR_C("siren sounds for", "Burglary and panic"),
                   /* TR: under "Burglary and panic" on a siren's page */
                   TR("A silent panic stays silent"), c.burglary, wired_flag, (void *)(uintptr_t)FLAG_BURGLARY);
        /* TR: a switch on a siren's page */
        toggle_row(body, UI_ICON_FLAME, ui_tone_red(), TR_C("siren sounds for", "Fire and CO"),
                   /* TR: under "Fire and CO" on a wired siren's page */
                   TR("The fire pattern for fire, the CO pattern for CO"), c.fire, wired_flag,
                   (void *)(uintptr_t)FLAG_FIRE);
    } else if (c.function != UI_OUT_FN_SMOKE_RESET) {
        /* TR: a section heading on a wired output's page */
        ui_section(body, TR_C("output section", "It follows"));
    }
    if (c.function != UI_OUT_FN_SMOKE_RESET) {
        if (c.function == UI_OUT_FN_ARMED) {
            parts_words(c.partitions, true, value, sizeof(value));
            link_row(body, UI_ICON_WAREHOUSE, ui_tone_violet(), TR_C("siren", "Partitions"),
                     /* TR: under an armed light's Partitions */
                     TR("On while any of them is armed"), value, wired_open_pick, (void *)(intptr_t)PICK_PARTS);
        } else {
            scope_rows(body, c.partitions, c.also, wired_open_pick);
        }
    }
    /* TR: a section heading on a wired output's page */
    ui_section(body, TR("How it is wired"));
    /* TR: a row on a wired output's page: what it is */
    link_row(body, fn_glyph(c.function), ui_tone_blue(), TR("What it is"), NULL, fn_title(c.function),
             wired_open_pick, (void *)(intptr_t)PICK_FN);
    /* TR: a row on a wired output's page: the terminal it drives */
    link_row(body, UI_ICON_PLUG, ui_tone_blue(), TR("Terminal"), NULL, pin, wired_open_pick,
             (void *)(intptr_t)PICK_PIN);
    /* TR: a row on a wired output's page: how its relay board switches */
    link_row(body, UI_ICON_ZAP, ui_tone_blue(), TR("Relay board"),
             /* TR: under "Relay board" */
             TR("Most switch on low"), c.active_low ? TR("Switches on low") : TR("Switches on high"),
             wired_open_pick, (void *)(intptr_t)PICK_RELAY);
    if (c.function == UI_OUT_FN_SIREN) {
        /* TR: a switch on a siren's page */
        toggle_row(body, UI_ICON_BELL, ui_tone_blue(), TR("Chirp when arming and disarming"),
                   /* TR: under "Chirp when arming and disarming" */
                   TR("One chirp to arm, two to disarm"), c.squawk, wired_flag, (void *)(uintptr_t)FLAG_SQUAWK);
        if (c.sense == UI_OUT_NO_PIN) {
            i18n_utf8_copy(value, sizeof(value), TR_C("supervision input", "None"));
        } else {
            pin_words(c.sense, value, sizeof(value));
        }
        /* TR: a row on a wired siren's page: the terminal that checks its circuit */
        link_row(body, UI_ICON_CLOCK_ALERT, ui_tone_amber(), TR("Supervision input"),
                 /* TR: under "Supervision input" */
                 TR("Its fault contact: open is a siren trouble"), value, wired_open_pick,
                 (void *)(intptr_t)PICK_SENSE);
        if (c.trouble) {
            /* TR: a paragraph on a wired siren's page while its supervision input is open */
            ui_paragraph(body, TR("Its supervision input is open: check the siren and its wiring."),
                         ui_tone_amber());
        }
    }
    /* TR: a section heading on a siren's page */
    ui_section(body, TR_C("siren section", "Check it"));
    if (c.function == UI_OUT_FN_SIREN || c.function == UI_OUT_FN_STROBE) {
        /* TR: a row on a siren's page: sound it briefly */
        s_wired.test = link_row(body, UI_ICON_BELL_RING, ui_tone_blue(), TR("Test it"),
                                TR("Sounds it for 2 seconds"), NULL, wired_test, NULL);
    }
    /* TR: a row on a wired output's page */
    link_row(body, UI_ICON_TRASH, ui_tone_red(), TR("Remove it"),
             /* TR: under "Remove it" on a wired output's page */
             TR("Its terminal goes back to the zones"), NULL, wired_remove, NULL);
    s_wired.open = true;
}

/* ---- a 433 MHz siren (PAGE_OUT_RF, PAGE_OUT_RF_PAIR; arg = its slot) ------------------------- */

static struct {
    bool open;
    uint8_t slot;
} s_rfs;

static bool rf_now(uint8_t slot, ui_out_rf_t *out)
{
    ui_out_rf_t r[UI_OUT_RF_MAX];
    read_rf(r);
    if (slot >= UI_OUT_RF_MAX || !r[slot].used) {
        return false;
    }
    *out = r[slot];
    return true;
}

static void rf_send(const ui_out_rf_t *c)
{
    if (S.be.out_rf_set != NULL && answered(S.be.out_rf_set(S.be.ctx, s_rfs.slot, c))) {
        ui_page_rebuild();
    }
}

static void rf_flag(bool on, void *ctx)
{
    ui_out_rf_t c;
    if (!rf_now(s_rfs.slot, &c)) {
        return;
    }
    switch ((flag_t)(uintptr_t)ctx) {
    case FLAG_BURGLARY: c.burglary = on; break;
    case FLAG_FIRE: c.fire = on; break;
    default: c.squawk = on; break;
    }
    if (!c.burglary && !c.fire) {
        ui_toast(TR("A siren sounds for something: remove it instead"));
        ui_page_rebuild();
        return;
    }
    rf_send(&c);
}

static void rf_open_pick(void *ctx)
{
    ui_out_rf_t c;
    if (!rf_now(s_rfs.slot, &c)) {
        return;
    }
    s_pick.rf = true;
    s_pick.slot = s_rfs.slot;
    if ((pick_t)(intptr_t)ctx == PICK_PARTS) {
        pick_start(PICK_PARTS);
        add_parts(0u, false, NULL);
        ui_pick_sheet(TR_C("siren", "Partitions"), TR("Their alarms and its chirps"), s_pick.choices,
                      s_pick.n, pick_index(c.partitions), picked);
    } else {
        pick_start(PICK_ALSO);
        add_parts(c.partitions, true, TR("Only its own partitions"));
        ui_pick_sheet(TR("Also sounds for"), TR("Other partitions' alarms, never their chirps"),
                      s_pick.choices, s_pick.n, pick_index(c.also), picked);
    }
}

static void rf_test(void *ctx)
{
    (void)ctx;
    if (S.be.out_rf_test != NULL && answered(S.be.out_rf_test(S.be.ctx, s_rfs.slot))) {
        /* TR: a toast: a 433 MHz siren's test was sent */
        ui_toast(TR("Sent: it should sound for 2 seconds"));
    }
}

static void rf_pair(void *ctx)
{
    (void)ctx;
    if (S.be.out_rf_pair != NULL && answered(S.be.out_rf_pair(S.be.ctx, s_rfs.slot))) {
        /* TR: a toast: the pairing signal goes out to a 433 MHz siren */
        ui_toast(TR("Sending the pairing signal three times"));
    }
}

static void open_pair(void *ctx)
{
    (void)ctx;
    ui_page_open(PAGE_OUT_RF_PAIR, s_rfs.slot);
}

static void rf_remove_ok(void)
{
    if (S.be.out_rf_clear != NULL && answered(S.be.out_rf_clear(S.be.ctx, s_rfs.slot))) {
        ui_page_back();
    }
}

static void rf_remove(void *ctx)
{
    (void)ctx;
    /* TR: a confirmation's title and text: removing a 433 MHz siren */
    ui_confirm(TR("Remove it?"), TR("It gets the stop signal, and the panel no longer sounds it."),
               TR("Remove"), rf_remove_ok);
}

static void add_rf(void *ctx)
{
    (void)ctx;
    uint8_t slot = 0u;
    if (S.be.out_rf_add != NULL && answered(S.be.out_rf_add(S.be.ctx, &slot))) {
        ui_page_open(PAGE_OUT_RF_PAIR, slot);
    }
}

static void rf_note(uint8_t slot, char *note, size_t nlen)
{
    ui_out_rf_t c;
    if (rf_now(slot, &c)) {
        char addr[12];
        snprintf(addr, sizeof(addr), "%05lX", (unsigned long)c.address);
        /* TR: a 433 MHz siren page's note: {0} the panel's address for it ("9E3B5") */
        I18N_FORMAT(note, nlen, TR("Address {0} · Not supervised"), addr);
    }
}

static void rf_build(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note, size_t nlen)
{
    s_rfs.slot = (uint8_t)arg;
    rf_name(s_rfs.slot, title, tlen);
    ui_out_rf_t c;
    ui_out_status_t st;
    if (!read_status(&st) || !rf_now(s_rfs.slot, &c)) {
        ui_paragraph(body, TR("It was removed."), ui_tone_gray());
        return;
    }
    rf_note(s_rfs.slot, note, nlen);
    ui_paragraph(body,
                 st.rf_transmitter
                     /* TR: a paragraph on a 433 MHz siren's page */
                     ? TR("Not supervised: the panel can't hear this siren. Test it now and then.")
                     /* TR: a paragraph on a 433 MHz siren's page while nothing can transmit */
                     : TR("Nothing can transmit on 433 MHz now: this siren won't sound. It is a "
                          "siren trouble."),
                 ui_tone_amber());
    ui_section(body, TR_C("siren section", "It sounds for"));
    toggle_row(body, UI_ICON_SIREN, ui_tone_red(), TR_C("siren sounds for", "Burglary and panic"),
               TR("A silent panic stays silent"), c.burglary, rf_flag, (void *)(uintptr_t)FLAG_BURGLARY);
    toggle_row(body, UI_ICON_FLAME, ui_tone_red(), TR_C("siren sounds for", "Fire and CO"),
               /* TR: under "Fire and CO" on a 433 MHz siren's page: it has one sound */
               TR("Its one sound"), c.fire, rf_flag, (void *)(uintptr_t)FLAG_FIRE);
    scope_rows(body, c.partitions, c.also, rf_open_pick);
    toggle_row(body, UI_ICON_BELL, ui_tone_blue(), TR("Chirp when arming and disarming"),
               /* TR: under "Chirp when arming and disarming" on a 433 MHz siren's page */
               TR("Its own arm and disarm sounds"), c.squawk, rf_flag, (void *)(uintptr_t)FLAG_SQUAWK);
    ui_section(body, TR_C("siren section", "Check it"));
    link_row(body, UI_ICON_BELL_RING, ui_tone_blue(), TR("Test it"), TR("Sounds it for 2 seconds"), NULL,
             rf_test, NULL);
    /* TR: a row on a 433 MHz siren's page */
    link_row(body, UI_ICON_SEND, ui_tone_blue(), TR("Pair it again"),
             /* TR: under "Pair it again" */
             TR("Its learning mode first"), NULL, open_pair, NULL);
    link_row(body, UI_ICON_TRASH, ui_tone_red(), TR("Remove it"),
             /* TR: under "Remove it" on a 433 MHz siren's page */
             TR("Sends it the stop signal first"), NULL, rf_remove, NULL);
    s_rfs.open = true;
}

static void pair_build(int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note, size_t nlen)
{
    s_rfs.slot = (uint8_t)arg;
    /* TR: the pairing page's title */
    i18n_utf8_copy(title, tlen, TR("Pair a 433 MHz siren"));
    rf_name(s_rfs.slot, note, nlen);
    ui_out_rf_t c;
    if (!rf_now(s_rfs.slot, &c)) {
        ui_paragraph(body, TR("It was removed."), ui_tone_gray());
        return;
    }
    /* TR: the pairing page's first step */
    ui_paragraph(body, TR("1. Put the siren in its learning mode: its manual says how, usually by "
                          "holding its button until it beeps."),
                 ui_tone_gray());
    /* TR: the pairing page's second step */
    ui_paragraph(body, TR("2. Send the pairing signal. Most sirens beep to say they learned it."),
                 ui_tone_gray());
    /* TR: a button on the pairing page */
    lv_obj_t *send = ui_row_action(body, UI_ICON_SEND, TR("Send the pairing signal"), ui_tone_blue(),
                                   rf_pair, NULL);
    if (!installer()) {
        ui_row_disable(send);
    }
    /* TR: the pairing page's third step */
    ui_paragraph(body, TR("3. Test it. If it stays quiet, send the pairing signal again."),
                 ui_tone_gray());
    link_row(body, UI_ICON_BELL_RING, ui_tone_blue(), TR("Test it"), TR("Sounds it for 2 seconds"), NULL,
             rf_test, NULL);
    /* TR: a row on the pairing page: the siren's settings */
    link_row(body, UI_ICON_SLIDERS, ui_tone_blue(), TR("What it sounds for"),
             /* TR: under "What it sounds for" on the pairing page */
             TR("Burglary and fire in its partitions, for now"), NULL, open_rf_siren,
             (void *)(intptr_t)s_rfs.slot);
}

/* ---- the 433 MHz page (PAGE_RF) -------------------------------------------------------------- */

static struct {
    bool open;
    lv_obj_t *rx;
    lv_obj_t *tx;
    uint32_t polled;
} s_rf;

/* The receiver row: its state as the row's value, what it heard as its line. */
static void rx_words(const ui_out_status_t *st, const char **state, char *detail, size_t dlen,
                     lv_color_t *tone)
{
    *tone = ui_tone_blue();
    detail[0] = '\0';
    switch ((ui_rf_rx_t)st->rf_receiver) {
    case UI_RF_RX_WORKING:
        /* TR: the 433 MHz receiver's state, a row's value. At most 23 bytes */
        *state = TR_C("radio state", "Working");
        if (st->rf_heard_ms != 0u && st->now_ms >= st->rf_heard_ms) {
            char ago[24];
            ago_words(st->now_ms - st->rf_heard_ms, ago, sizeof(ago));
            /* TR: under the receiver row: {0} "2 min" */
            I18N_FORMAT(detail, dlen, TR("Heard a sensor {0} ago"), ago);
        } else {
            /* TR: under the receiver row */
            i18n_utf8_copy(detail, dlen, TR("Nothing heard since the panel started"));
        }
        break;
    case UI_RF_RX_JAMMED:
        *state = TR_C("radio state", "Interference");
        /* TR: under the receiver row */
        i18n_utf8_copy(detail, dlen, TR("Something nearby drowns 433 MHz: sensors may not be heard"));
        *tone = ui_tone_amber();
        break;
    case UI_RF_RX_OFF:
        /* TR: the 433 MHz receiver's state, a row's value. At most 23 bytes */
        *state = TR_C("radio state", "Off");
        /* TR: under the receiver row */
        i18n_utf8_copy(detail, dlen, TR("The panel was told none is fitted"));
        *tone = ui_tone_gray();
        break;
    default:
        *state = TR_C("radio state", "Not connected");
        /* TR: under the receiver row */
        i18n_utf8_copy(detail, dlen, TR("No 433 MHz receiver answers"));
        *tone = ui_tone_gray();
        break;
    }
}

static void open_zone(void *ctx)
{
    ui_page_open(PAGE_ZONE, (int32_t)(intptr_t)ctx);
}

static void open_zones(void *ctx)
{
    (void)ctx;
    ui_page_open(PAGE_ZONES, 0);
}

static void rf_page_build(lv_obj_t *body, char *title, size_t tlen, char *note, size_t nlen)
{
    i18n_utf8_copy(title, tlen, NOTR("433 MHz"));
    i18n_utf8_copy(note, nlen, TR("Sensors and sirens on 433 MHz"));
    ui_out_status_t st;
    if (!read_status(&st)) {
        return;
    }
    const char *state = "";
    char d[128];
    lv_color_t tone;
    rx_words(&st, &state, d, sizeof(d), &tone);
    /* TR: the 433 MHz page's receiver row; its state is the row's value */
    s_rf.rx = ui_row_link(body, G_RADIO, tone, TR("Receiver"), d, state, NULL, NULL, NULL);
    s_rf.tx = ui_row_link(body, UI_ICON_SEND, st.rf_transmitter ? ui_tone_blue() : ui_tone_gray(),
                          /* TR: the 433 MHz page's transmitter row; its state is the row's value */
                          TR("Transmitter"),
                          st.rf_transmitter
                              /* TR: under the transmitter row */
                              ? TR("433 MHz sirens can be sounded")
                              /* TR: under the transmitter row */
                              : TR("433 MHz sirens can't be sounded"),
                          /* TR: the transmitter's state, a row's value. At most 23 bytes */
                          st.rf_transmitter ? TR_C("transmitter state", "Ready")
                                            : TR_C("transmitter state", "None"),
                          NULL, NULL, NULL);
    /* TR: a row on the 433 MHz page */
    link_row(body, UI_ICON_PLUS, ui_tone_green(), TR("Add a sensor"),
             /* TR: under "Add a sensor" on the 433 MHz page */
             TR("In Zones: add a zone, pick 433 MHz sensor, trigger it"), NULL, open_zones, NULL);
    ui_out_rf_t r[UI_OUT_RF_MAX];
    read_rf(r);
    if (rf_sirens_used(r) < UI_OUT_RF_MAX) {
        /* TR: a row on the 433 MHz page */
        link_row(body, UI_ICON_PLUS, ui_tone_green(), TR("Add a siren"),
                 TR("One that learns its panel like a key fob"), NULL, add_rf, NULL);
    }
    /* TR: a section on the 433 MHz page */
    ui_section(body, TR_C("433 MHz section", "Sensors"));
    uint16_t zones[32];
    const size_t n = rf_zones(zones, 32u);
    if (n == 0u) {
        /* TR: the 433 MHz page without 433 MHz zones */
        ui_paragraph(body, TR("No zone uses a 433 MHz sensor yet."), ui_tone_gray());
    }
    const alarm_config_t *cfg = cfg_now();
    for (size_t i = 0; i < n && i < 32u; i++) {
        zone_hub_info_t info;
        if (!S.be.zone_info(S.be.ctx, zones[i], &info)) {
            continue;
        }
        const alarm_zone_config_t *zc = cfg != NULL ? &cfg->zones[zones[i] - 1u] : NULL;
        char name[ALARM_NAME_LEN + 8];
        if (zc != NULL && zc->name[0] != '\0') {
            snprintf(name, sizeof(name), "%s", zc->name);
        } else {
            /* TR: a zone nobody has named; {0} its number */
            I18N_FORMAT(name, sizeof(name), TR_C("default zone name", "Zone {0}"), (unsigned)zones[i]);
        }
        char heard[48];
        if (info.heard_at != 0u && st.now_ms >= info.heard_at) {
            char ago[24];
            ago_words(st.now_ms - info.heard_at, ago, sizeof(ago));
            /* TR: when a 433 MHz sensor was last heard: {0} "2 min" */
            I18N_FORMAT(heard, sizeof(heard), TR("Heard {0} ago"), ago);
        } else {
            /* TR: a 433 MHz sensor not heard since the panel started */
            i18n_utf8_copy(heard, sizeof(heard), TR("Not heard yet"));
        }
        char line[160];
        /* TR: a 433 MHz sensor's line: {0} its zone number, {1} its code ("433 MHz · A2B3C code
         * 6"), {2} when it was heard */
        I18N_FORMAT(line, sizeof(line), TR("Zone {0} · {1} · {2}"), (unsigned)zones[i], info.address, heard);
        const bool problem = info.check || info.unreachable;
        lv_obj_t *row = ui_row_link(body,
                                    zc != NULL ? ui_device_class_glyph((alarm_device_class_t)zc->device_class)
                                               : UI_ICON_DOOR_CLOSED,
                                    problem ? ui_tone_amber() : ui_tone_green(), name, line, NULL,
                                    UI_ICON_CHEVRON_RIGHT, open_zone, (void *)(intptr_t)zones[i]);
        if (!installer()) {
            ui_row_disable(row);
        }
    }
    if (rf_sirens_used(r) != 0u) {
        /* TR: a section on the 433 MHz page */
        ui_section(body, TR_C("433 MHz section", "Sirens"));
        for (size_t i = 0; i < UI_OUT_RF_MAX; i++) {
            if (!r[i].used) {
                continue;
            }
            char name[48];
            char scope[96];
            char line[128];
            rf_name((uint8_t)i, name, sizeof(name));
            scope_words(r[i].partitions, r[i].also, scope, sizeof(scope));
            I18N_FORMAT(line, sizeof(line), TR("{0} · Not supervised"), scope);
            link_row(body, UI_ICON_SIREN, ui_tone_red(), name, line,
                     r[i].sounding ? TR_C("siren state", "Sounding") : TR_C("siren state", "Ready"),
                     open_rf_siren, (void *)(intptr_t)i);
        }
    }
    s_rf.open = true;
    s_rf.polled = lv_tick_get();
}

static void rf_tick(void)
{
    if (!s_rf.open || s_rf.rx == NULL || lv_tick_elaps(s_rf.polled) < POLL_MS) {
        return;
    }
    s_rf.polled = lv_tick_get();
    ui_out_status_t st;
    if (!read_status(&st)) {
        return;
    }
    const char *state = "";
    char d[128];
    lv_color_t tone;
    rx_words(&st, &state, d, sizeof(d), &tone);
    lv_obj_t *l = ui_find(s_rf.rx, "value_label");
    if (l != NULL && strcmp(lv_label_get_text(l), state) != 0) {
        lv_label_set_text(l, state);
    }
    ui_row_set_detail(s_rf.rx, d, !lv_color_eq(tone, ui_tone_blue()), tone);
}

/* ---- the picks' answers ------------------------------------------------------------------------ */

static void picked(int32_t index)
{
    if (index < 0 || (size_t)index >= s_pick.n) {
        return;
    }
    const uint16_t v = s_pick.value[index];
    switch ((pick_t)s_pick.what) {
    case PICK_ADD_FN:
        s_pick.add_fn = (uint8_t)v;
        pin_pick(s_pick.slot, PICK_ADD_PIN, UI_OUT_NO_PIN, false);
        return;
    case PICK_ADD_PIN: {
        ui_out_wired_t c;
        memset(&c, 0, sizeof(c));
        c.function = s_pick.add_fn;
        c.gpio = (uint8_t)v;
        c.sense = UI_OUT_NO_PIN;
        c.partitions = ((parts_enabled() & 0x01u) != 0u) ? 0x01u : parts_enabled();
        c.burglary = true;
        c.fire = true;
        if (S.be.out_wired_set != NULL && answered(S.be.out_wired_set(S.be.ctx, s_pick.slot, &c))) {
            ui_page_open(PAGE_OUT_WIRED, s_pick.slot);
        }
        return;
    }
    default:
        break;
    }
    if (s_pick.rf) {
        ui_out_rf_t c;
        if (!rf_now(s_pick.slot, &c)) {
            return;
        }
        if (s_pick.what == PICK_PARTS) {
            c.partitions = (uint8_t)v;
            c.also = (uint8_t)(c.also & (uint8_t)~c.partitions);
        } else {
            c.also = (uint8_t)v;
        }
        rf_send(&c);
        return;
    }
    ui_out_wired_t c;
    if (!wired_now(s_pick.slot, &c)) {
        return;
    }
    switch ((pick_t)s_pick.what) {
    case PICK_FN:
        c.function = (uint8_t)v;
        if (c.partitions == 0u) { /* from For automations: its partitions as a new output's */
            c.partitions = ((parts_enabled() & 0x01u) != 0u) ? 0x01u : parts_enabled();
        }
        break;
    case PICK_PIN: c.gpio = (uint8_t)v; break;
    case PICK_SENSE: c.sense = (uint8_t)v; break;
    case PICK_RELAY: c.active_low = v != 0u; break;
    case PICK_PARTS:
        c.partitions = (uint8_t)v;
        c.also = (uint8_t)(c.also & (uint8_t)~c.partitions);
        break;
    default: c.also = (uint8_t)v; break;
    }
    if (c.function == UI_OUT_FN_SIREN && !c.burglary && !c.fire) {
        c.burglary = true;
        c.fire = true;
    }
    wired_send(&c);
}

/* ---- the module's entry points ------------------------------------------------------------------ */

bool ui_sirens_available(void)
{
    ui_out_status_t st;
    return read_status(&st);
}

void ui_sirens_build(page_id_t id, int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                     size_t nlen)
{
    ui_sirens_stop();
    switch (id) {
    case PAGE_SIRENS:
        i18n_utf8_copy(title, tlen, TR("Sirens and outputs"));
        list_build(body, note, nlen);
        break;
    case PAGE_OUT_WIRED: wired_build(arg, body, title, tlen, note, nlen); break;
    case PAGE_OUT_RF: rf_build(arg, body, title, tlen, note, nlen); break;
    case PAGE_OUT_RF_PAIR: pair_build(arg, body, title, tlen, note, nlen); break;
    default: rf_page_build(body, title, tlen, note, nlen); break;
    }
}

void ui_sirens_tick(void)
{
    if (lv_subject_get_int(&subject_view) != VIEW_SETTINGS) {
        return;
    }
    if ((s_menu.sirens != NULL || s_menu.rf != NULL) && lv_tick_elaps(s_menu.polled) >= POLL_MS) {
        s_menu.polled = lv_tick_get();
        menu_words();
    }
    list_tick();
    rf_tick();
}

void ui_sirens_stop(void)
{
    s_menu.sirens = NULL;
    s_menu.rf = NULL;
    memset(&s_list, 0, sizeof(s_list));
    memset(&s_wired, 0, sizeof(s_wired));
    memset(&s_rfs, 0, sizeof(s_rfs));
    memset(&s_rf, 0, sizeof(s_rf));
}
