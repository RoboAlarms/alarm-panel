/*
 * ui_internal.h - state and helpers shared by the UI's modules:
 *   ui_app.c        start, tick, snapshots, Home, code pad, lists, emergency, screen saver
 *   ui_forms.c      settings rows, sheets (text entry, choices, confirmation), keyboard
 *   ui_settings.c   Settings: sections, who may open them, page stack, display and sound,
 *                   users, about
 *   ui_installer.c  installer mode: options, zones, partition, codes, factory reset
 *   ui_sources.c    zone sources: where a zone's sensor is (picker, terminals, source rows)
 *   ui_net.c        Wi-Fi (status, scan, connecting), the address settings and the connection
 *                   check, and date and time; also the setup's steps
 *   ui_integrations.c  Integrations: the Home Assistant link and MQTT, each on its own page
 *   ui_zwave.c      Installer > Z-Wave: the radio, its devices and network (ALARM_UI_ZWAVE)
 *   ui_zigbee.c     Installer > Zigbee: the radio, its devices, adding them (ALARM_UI_ZIGBEE)
 *   ui_devices.c    Smart Home: the devices the panel controls, by area (ALARM_UI_DEVICES)
 *   ui_walk.c       the walk test: Settings > Walk test, its list, Home's card while one runs
 *   ui_automations.c, ui_auto_edit.c  Settings > Automations: the list, a page, the editors
 *                   (ALARM_UI_AUTOMATIONS; ui_automations.h)
 *   ui_wizard.c     first-boot setup
 * Everything runs on the LVGL thread.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm_ui.h"
#include "i18n/i18n.h"
#include "i18n/i18n_date.h"
#include "ui_app.h"
#include "ui_text.h"

/* ---- words on the screen (docs/features/37, I18N-003, I18N-008) ------------------------------
 * Every word the UI shows goes through these; S.catalog NULL is English, the text as written.
 * tools/i18n.py extracts their literal arguments, and `i18n.py check` fails on a word-like literal
 * in a module listed in components/core/i18n/po/MARKED that is in none of them nor in NOTR().
 *   TR("Saved"), TR_C("zone state", "Open")    a text; the context tells two meanings apart
 *   TRN("{0} trouble", "{0} troubles", n)      a count: one form per plural category
 *   TR_NOOP("...") / TR_NOOP_C(ctx, "...")     in a static table; TR(table[i]) where shown
 *   NOTR("...")                                text deliberately never translated: ids, product and
 *                                              protocol names, codes shown as values. Dates and times
 *                                              go through i18n_date with a TR_C("date pattern", ...)
 *                                              pattern (ui_app.c's clock functions are the example).
 * The rules (ui_app.c is the example to copy):
 *   - Mark text where it is chosen. A helper that receives text (ui_toast, ui_pad_ask, ui_confirm,
 *     rows, sheets, ui_set_str) takes it already translated and never translates it again.
 *   - Whole sentences only: never glue words into a sentence ("%s is %s", "Turn chime %s"). Pick
 *     one whole text per case instead: cond ? TR("Turn chime off") : TR("Turn chime on"), never
 *     TR(cond ? "a" : "b"). Independent facts on one line are joined with " · ", which is the
 *     same in every language: "%s · %s" of two translated facts is fine.
 *   - A text with a value in it: I18N_FORMAT(buf, sizeof(buf), TR("Entry through {0}"), name),
 *     {0}..{9}; never snprintf or lv_label_set_text_fmt with a translated pattern (no %s, %u in
 *     marked text). A copy into a fixed buffer: i18n_utf8_copy (it cuts on a whole character).
 *     Punctuation that joins values is text too and is marked: TR("{0}: {1}") (French writes
 *     " : "), TR("{0}, {1} +{2}"). Only a format with nothing but values ("%02d", "%s") stays
 *     snprintf. A percent is TR_C("percent", "{0}%").
 *   - A helper that returns translated text (ui_chime_note, ui_code_digits) is passed on as it
 *     is, never wrapped in TR() again.
 *   - A count: TRN (English: n == 1 the first, else the second), even when both English forms
 *     are the same ("{0} min ago"): other languages have more forms.
 *   - A short word ambiguous out of context ("Open", "Off", "Clear", "Bypass") gets a context
 *     naming what it is: TR_C("zone card button", "Bypass").
 *   - A note for the translator, on the marker's line or alone right above its statement (above
 *     a table declaration it reaches every element that has no note of its own):
 *     TR: where it appears ("Home, the line under the state headline"); "At most N bytes" when
 *     it lands in a buffer or a string subject (127). N is the budget of the text WITHOUT its
 *     {n} placeholders: destination size - 1 - the longest value(s) inserted ("Since {0}" into
 *     24 bytes, {0} = "10:41 PM" of 8 bytes: 15).
 *   - A marker that is a direct argument of ui_toast, ui_set_str, ui_pad_error, ui_pad_ask,
 *     ui_pad_ask_new, ui_confirm, ui_confirm_choice, ui_text_sheet(_kind) or ui_pick_sheet gets
 *     its note and budget from tools/i18n.py (CALL_NOTES): write none by hand there, only where
 *     the text goes through a placeholder value or a local buffer first. `i18n.py check --notes`
 *     lists the entries still without a note or a budget.
 *   - Names the household typed (zones, users, partitions) are never marked: they are values. */
#define TR(s)                    i18n_text(S.catalog, NULL, (s))
#define TR_C(ctx, s)             i18n_text(S.catalog, (ctx), (s))
#define TRN(one, other, n)       i18n_plural(S.catalog, NULL, (one), (other), (uint32_t)(n))
#define TRN_C(ctx, one, other, n) i18n_plural(S.catalog, (ctx), (one), (other), (uint32_t)(n))
#define TR_NOOP(s)               (s)
#define TR_NOOP_C(ctx, s)        (s)

/* The UI state is placed in PSRAM on a panel that has it, where LVGL's heap already lives:
 * internal RAM is the reserve the alarm's crypto and the network drivers draw on. The host
 * builds keep it an ordinary static. */
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define UI_SLOW_BSS EXT_RAM_BSS_ATTR
#else
#define UI_SLOW_BSS
#endif

/* Big tables only this thread uses (page scratch, the list rows) come from here the first time
 * they are needed, and are kept: from LVGL's heap, which is PSRAM on a panel that has it, and on
 * a board without PSRAM only once the touch UI runs, so a panel that shows just the status
 * screen (ui_status.c) doesn't hold them (docs/features/19). Zeroed; NULL when there is no room. */
void *ui_slow_alloc(size_t size);

#define UI_MAX_EVENT_ROWS   50u
/* Rows the trouble list keeps objects for. It is a page size, not a limit on what the panel
 * will show: troubles past it are reached with the pager, and the count above the list is
 * always the true total (docs/features/07 UI-030). */
#define UI_MAX_TROUBLE_ROWS 48u
#define UI_MAX_PAGES        7u

/* The XML marks every scrolling viewport (page_body, the lists, the setup body, the picker)
 * with user_1: they are clickable only so a drag can start anywhere on them, not because
 * they are controls. See components/ui/alarm_ui/layouts/1024x600/alarm_ui/components/base/box/box.xml. */
#define UI_FLAG_SCROLL_SURFACE LV_OBJ_FLAG_USER_1

enum { VIEW_HOME = 0, VIEW_ZONES, VIEW_ACTIVITY, VIEW_TROUBLES, VIEW_PANIC, VIEW_SETTINGS, VIEW_DEVICES };
enum { PANEL_ARM = 0, PANEL_DISARM, PANEL_PAD, PANEL_MEMORY, PANEL_WALK };
/* Settings sections (subject_settings_section): the numbers are the XML's ref_value, not the order
 * of the left column. */
enum {
    SECTION_DISPLAY = 0, /* Screen and sound */
    SECTION_USERS,
    SECTION_WIFI,        /* Wi-Fi and network */
    SECTION_TIME,        /* Time and language (BL-131) */
    SECTION_INTEGRATIONS,
    SECTION_LOG,         /* retired (BL-131 finding 7: Activity is the log); its number stays
                          * so the XML's ref_values keep theirs. Never stored. */
    SECTION_INSTALLER, /* no line in the list since BL-189: the Installer page is under About */
    SECTION_ABOUT,
    SECTION_WALK,        /* docs/features/40 */
    SECTION_AUTOMATIONS = 9, /* after Integrations (docs/features/41; ALARM_UI_AUTOMATIONS) */
};

/* Who may open a Settings page (docs/features/14, "Who can change what"). */
typedef enum {
    ACCESS_ANYONE = 0,
    ACCESS_USERS,     /* users: a system or partition master, or the installer (2026-09-23) */
    ACCESS_SHARED,    /* Wi-Fi, time, event log: a master or the installer */
    ACCESS_SYSTEM,    /* integrations: the system master or the installer */
} ui_access_t;
/* subject_sheet: which sheet is over the screen (the XML's ref_value). SHEET_CLOCK and the ones
 * after it are sheets of their own (ui_sheet_open). */
enum { SHEET_NONE = 0, SHEET_TEXT, SHEET_PICK, SHEET_CONFIRM, SHEET_CLOCK, SHEET_NIGHT, SHEET_DEVICE, SHEET_TIME,
       SHEET_WINDOW, SHEET_PUT_OFF /* an automation's arming: an hour, or not tonight (ui_auto_warn.c) */,
       SHEET_FORM /* a sheet of rows (ui_form_sheet_open): "Arm the panel" (ui_auto_edit.c) */ };

typedef enum {
    MODE_READY = 0,
    MODE_NOT_READY,
    MODE_EXIT,
    MODE_AWAY,
    MODE_STAY,
    MODE_NIGHT,
    MODE_ENTRY,
    MODE_ALARM,
    MODE_FIRE,
    MODE_CO,
    MODE_AUX,
    MODE_MEMORY,
    MODE_WALK_TEST,
    MODE_PROGRAM,
} ui_mode_t;

/* What the code pad asks for. */
typedef enum {
    PAD_NONE = 0,
    PAD_ARM,
    PAD_DISARM,
    PAD_CHIME,
    PAD_BYPASS,
    PAD_ACK,
    PAD_CLEAR_MEMORY,
    PAD_EXTEND_WALK_TEST,
    PAD_CODE_CURRENT, /* "change your code" from the banner */
    PAD_CODE_NEW,
    PAD_CODE_CONFIRM,
    PAD_ASK,       /* a code for a callback (open a section, installer mode) */
    PAD_ASK_NEW,   /* a new code ... */
    PAD_ASK_AGAIN, /* ... and again; the callback gets it */
} pad_t;

/* Where a code pad shows. HOST_MODAL is the pad over the screen (UI-025): the actions that
 * aren't about arming ask there, so Home's state card can't be taken for what you are doing. */
typedef enum { HOST_HOME = 0, HOST_SETTINGS, HOST_WIZARD, HOST_MODAL, HOST_COUNT } pad_host_t;

/* Gets the code typed on the pad. Return true when done (the pad closes), false to keep the
 * pad open after reporting the problem with ui_pad_error(). */
typedef bool (*ui_code_fn)(const char *code);

/* Chime: the rail's Chime and the row in Settings both switch it here (UI-025). */
void ui_chime_toggle(void);
const char *ui_chime_note(void); /* returns translated text: pass it on, never wrap it in TR() */

typedef enum {
    PAGE_DISPLAY = 0,
    PAGE_USERS,
    PAGE_USER,     /* arg = user number */
    PAGE_INSTALLER,
    PAGE_OPTIONS,
    PAGE_ZONES,
    PAGE_ZONE,     /* arg = zone number */
    PAGE_PARTITION,
    PAGE_CODES,
    PAGE_WIFI,
    PAGE_NETWORK, /* Wi-Fi > the joined network: its address, DNS server, the check, Forget */
    PAGE_TIME,
    PAGE_RESET,
    PAGE_ABOUT,
    PAGE_SOFTWARE,
    PAGE_SD_CARD, /* About > SD card (BL-131: its state, details and Format) */
    PAGE_INTEGRATIONS,
    PAGE_HOME_ASSISTANT,
    PAGE_MQTT, /* the broker settings, its own entry in Integrations */
    PAGE_NTFY,      /* Integrations > Notifications > ntfy (docs/features/27) */
    PAGE_PUSHOVER,  /* Integrations > Notifications > Pushover (docs/features/28) */
    PAGE_ZONE_SOURCE, /* arg = zone | kind << 16 | (new zone) << 24 */
    /* Everything the Panel Manager can set, at the panel too (2026-09-23, docs/features/14). */
    PAGE_PARTITIONS,
    PAGE_PARTITION_EDIT, /* arg = partition number */
    PAGE_DEVICES,
    PAGE_DEVICE,         /* arg = device number */
    PAGE_TROUBLE_BEEPS,
    PAGE_PANIC_KEYS,
    PAGE_USER_PARTITIONS, /* arg = user number */
    PAGE_ZONE_SENSOR,     /* arg = zone number: how its sensor reads, its other inputs */
    PAGE_SET_CLOCK,       /* Time and language > the clock's row (a clock module, docs/features/35) */
    /* Installer > Z-Wave (docs/features/36 screens.md; ui_zwave.c, with ALARM_UI_ZWAVE) */
    PAGE_ZWAVE,           /* §3: the radio, the devices, the network */
    PAGE_ZWAVE_ADD,       /* §4: adding a device */
    PAGE_ZWAVE_REMOVE,    /* §7: removing one */
    PAGE_ZWAVE_DEVICE,    /* §6: arg = node | generation << 16 */
    PAGE_ZWAVE_REGION,    /* §8 */
    PAGE_ZWAVE_RADIO,     /* §8: the radio's details */
    PAGE_ZWAVE_SMART,     /* §9: waiting to join (SmartStart) */
    PAGE_ZWAVE_SMART_ENTRY, /* §9: one registered sensor (ui_zwave.c keeps which) */
    PAGE_WALK,            /* Settings > Walk test (docs/features/40; ui_walk.c) */
    PAGE_SAVER,           /* Screen and sound > Screen saver: when it starts, how bright */
    PAGE_NIGHT,           /* Screen and sound > Night hours (the hours open a sheet, SHEET_NIGHT) */
    PAGE_INSTALLER_LOST,  /* Installer, before the code: what to do about a lost installer code */
    /* Installer > Zigbee (docs/features/20 devices.md §9; ui_zigbee.c, with ALARM_UI_ZIGBEE) */
    PAGE_ZIGBEE,          /* the radio, the devices, the network (BL-144 page 2) */
    PAGE_ZIGBEE_ADD,      /* adding a device (page 3): arg 0 add, 1 from the zone picker, 2
                           * replace, 3 with an install code (ui_zigbee.c keeps which) */
    PAGE_ZIGBEE_ADDED,    /* a device that joined: "Use it for" (page 3), arg = its registry id */
    PAGE_ZIGBEE_DEVICE,   /* a device's page (page 5), arg = its registry id */
    PAGE_ZIGBEE_CODE,     /* adding with an install code (page 4) */
    PAGE_ZIGBEE_CODES,    /* install codes waiting (page 10) */
    PAGE_ZIGBEE_REMOVE,   /* removing a device (page 6), arg = its registry id */
    PAGE_ZIGBEE_RADIO,    /* the radio's details (page 7) */
    PAGE_ZIGBEE_CHANNEL,  /* the channel (page 8) */
    PAGE_AUTOMATIONS,     /* Settings > Automations: the list (docs/features/41; ui_automations.c) */
    PAGE_AUTOMATION,      /* one automation, arg = its slot (1..AUTO_MAX) */
    PAGE_FW_SERVER,       /* Installer > Updates > Update server (BL-186; ui_update.c) */
    PAGE_FW_WARNING,      /* ... the warning before a server whose key the panel does not trust */
    PAGE_ANNOUNCEMENTS,   /* Screen and sound > Announcements (BL-202, D06; ui_voice.c's words) */
} page_id_t;

typedef struct {
    page_id_t id;
    int32_t arg;
} ui_page_t;

typedef struct {
    ui_backend_t be;
    ui_options_t opt;
    const i18n_catalog_t *catalog; /* NULL: English (G3 adds the setting) */
    unsigned text_cuts;            /* texts cut to fit a subject (I18N-023) */
    bool lang_pending;             /* a new language waits for the alarm or delay to end */
    uint32_t lang_gen;             /* the backend's lang_generation() last looked at */
    uint32_t asset_gen;            /* the backend's asset_generation() last looked at */
    char last_cut[64];             /* the start of the last one */
    lv_display_t *disp;
    lv_obj_t *main;
    lv_obj_t *saver;
    lv_obj_t *wizard;
    lv_obj_t *zone_list;
    lv_obj_t *event_list;
    lv_obj_t *trouble_list;
    lv_obj_t *page_body;
    lv_obj_t *pads[HOST_COUNT];

    alarm_snapshot_t snap;
    alarm_config_t *cfg;
    uint32_t snap_seq;
    uint32_t cfg_seq;
    bool have_snap;

    ui_mode_t mode;
    bool mode_valid;
    uint32_t count_total_ms;
    int32_t lock_shown_secs; /* the pad shows a lockout countdown at this second, 0 = no (KPD-008) */
    uint16_t entry_zone;
    uint8_t armed_user;
    int64_t armed_epoch_ms;
    bool armed_by_auto;     /* an automation armed it (D30): "By automation", never its maker */
    uint8_t armed_bypassed; /* zones the automation bypassed to arm (AUT-014), 0 = none or unknown */
    bool pulse_on;

    /* code pad */
    pad_t pad;
    pad_host_t pad_host;
    alarm_arm_level_t pad_level;
    uint16_t pad_zone;
    bool pad_unbypass;
    int32_t pad_return_view; /* view to go back to when the pad closes */
    bool chime_was_on;
    char digits[ALARM_CODE_MAX_LEN + 1];
    char code_current[ALARM_CODE_MAX_LEN + 1];
    char code_new[ALARM_CODE_MAX_LEN + 1];
    ui_code_fn pad_fn;
    const char *pad_ask_glyph; /* static: the icon beside the title */
    char pad_ask_title[64];
    char pad_ask_hint[96];
    char pad_ask_note[128]; /* plain words under the title over the screen; "" for none */
    lv_color_t pad_ask_tone;

    bool toast_on;
    uint32_t toast_since;

    bool panic_holding;
    bool panic_sent;
    alarm_panic_t panic;
    uint32_t panic_start;
    lv_obj_t *panic_arc;

    lv_obj_t *zone_cards[ALARM_MAX_ZONES];
    uint16_t zone_card_zone[ALARM_MAX_ZONES];
    uint16_t zone_cards_n;
    uint32_t zones_cfg_seq;
    bool zones_built;
    /* An empty Zones filter's check (BL-177, zones_empty_set): the filter last applied (-1 none:
     * the next one shows at once), whether the list is empty now and since when, and when the
     * check and the list last swapped (at most once a second from a zone's change) */
    int32_t zones_empty_filter;
    bool zones_empty_want;
    uint32_t zones_empty_since;
    uint32_t zones_empty_swapped;
    uint32_t events_shown_seq;
    lv_obj_t *trouble_rows[UI_MAX_TROUBLE_ROWS];
    uint8_t trouble_page; /* which page of the trouble list is on screen (0-based) */

    bool saver_on;
    uint8_t light;     /* ui_light_stage_t: awake, the saver (dimmed) or dark (UI-043) */
    bool night;        /* night hours now (UI-043) */
    int local_minute;  /* minutes after local midnight, -1 while the clock is not set */
    int light_sent;    /* the backlight last asked for, -1 none yet */
    int light_preview; /* a brightness slider moved: shown for a while, -1 none */
    uint32_t light_preview_at;
    bool locked; /* armed or in alarm: Settings, chime and codes wait (UI-022) */
    bool lit;    /* backlight switched on after the first frame */
    int last_minute;
    uint32_t net_polled;

    /* settings */
    ui_page_t pages[UI_MAX_PAGES];
    uint8_t depth;
    bool page_fresh;       /* the page being built was just opened (ui_page_open), not returned
                            * to or rebuilt: a page that starts something does it only then */
    uint8_t auth_user; /* whose code opened code-protected sections, 0 = nobody */
    char auth_code[ALARM_CODE_MAX_LEN + 1];
    bool program;          /* this keypad has the panel in installer mode */
    alarm_config_t *edit;  /* installer's working copy of the configuration */
    bool edit_dirty;
    uint32_t keepalive_at; /* last installer-mode keepalive */

    /* wizard */
    bool wizard_on;
    bool wizard_paused; /* an alarm took over the screen */

    /* the remote keypad (docs/features/31) */
    ui_remote_session_t remote;
    bool remote_paused;       /* the person at the panel paused it with the banner */
    lv_indev_t *remote_indev; /* presses from the network arrive here */
} ui_state_t;

extern ui_state_t S;

/* ---- ui_theme.c ---------------------------------------------------------------------------- */
void ui_theme_watch_saver(void); /* ui_app_start, once the screen saver is made */

/* ---- ui_app.c ------------------------------------------------------------------------------ */
void ui_app_theme_changed(void);
void ui_refresh(bool force);
void ui_toast(const char *text);
void ui_beep(ui_beep_t b);
void ui_set_int(lv_subject_t *s, int32_t v);
void ui_set_str(lv_subject_t *s, const char *v);
void ui_set_color(lv_subject_t *s, lv_color_t c);
lv_obj_t *ui_find(lv_obj_t *root, const char *name);
void ui_set_label(lv_obj_t *parent, const char *name, const char *text);
void ui_chip_tone(lv_obj_t *chip, const char *glyph, lv_color_t tone);
/* The region this keypad formats with: chosen, or the language's (docs/features/37 I18N-016). */
const i18n_region_t *ui_region(void);
/* The clock this keypad shows: its choice, or the region's (docs/features/37 I18N-016). */
bool ui_clock_24h(void);
/* A number of bytes with its unit, the region's decimal mark: "1.5 GiB", "1,5 Gio" (I18N-017). */
void ui_size_text(char *out, size_t len, uint64_t bytes);
/* S.opt.lang changed: every screen in the new language now, or once an alarm, entry time or
 * exit time on the screen ends (docs/features/37 I18N-014). */
void ui_language_apply(void);
/* A language's words: the backend's (an installed pack; NULL for English or one not loaded yet)
 * when it has a language set, else the built-in catalogs. ui_lang.c */
const i18n_catalog_t *ui_lang_catalog(const char *locale);
/* ... and each module builds what it shows again, in it. */
void ui_forms_language_changed(void);
void ui_settings_language_changed(void);
void ui_wizard_language_changed(void);
void ui_clock_text(int64_t epoch_ms, char *buf, size_t len, bool with_ampm);
void ui_date_time_text(int64_t epoch_ms, char *buf, size_t len); /* "Sep 19, 10:41 PM" */
/* "10:41 PM" when the time is today (local), else the day: "Sep 19". */
void ui_time_or_day_text(int64_t epoch_ms, char *buf, size_t len);
/* When something happened, for a device's tile (HCTL-006, BL-179): UI_WHEN_TODAY with the time
 * ("8:30 PM"); UI_WHEN_DAY with the region's short date ("Sep 18") for an earlier day this year;
 * UI_WHEN_YEAR with the date and year ("Sep 18, 2025") for an earlier year. Days and years are the
 * panel's time zone's (local midnight). UI_WHEN_UNKNOWN, buf "", when the clock isn't set, epoch_ms
 * <= 0 or can't be converted, or it is more than a minute ahead of the panel's clock (up to a minute
 * ahead - two clocks a little apart - reads as now). buf: at least 32 bytes (a date pattern's result
 * is up to 31). */
typedef enum { UI_WHEN_UNKNOWN = 0, UI_WHEN_TODAY, UI_WHEN_DAY, UI_WHEN_YEAR } ui_when_t;
ui_when_t ui_when_text(int64_t epoch_ms, char *buf, size_t len);
void ui_date_time_long(int64_t epoch_ms, char *buf, size_t len); /* "Saturday, September 19 · 10:41 PM" */
void ui_options_changed(void); /* apply + save this keypad's preferences */
/* A brightness slider moved: the screen shows that level for a few seconds, then the level it
 * should have now (at night, the night's) - so the slider can be judged by eye. */
void ui_light_preview(uint8_t percent);
bool ui_clock_known(void); /* the panel knows the time (night hours need it) */
/* A time of day, minutes after midnight, on this keypad's clock: "10:30 PM" or "22:30". */
void ui_minute_text(int minute, char *buf, size_t len);
/* When a log entry happened (Activity, LOG-004): today "10:41 PM", "Yesterday, 9:01 PM", older
 * "Sep 18, 10:41 PM"; "Time unknown" or "Before a restart" while the clock wasn't set. */
void ui_entry_time(const elog_entry_t *e, char *buf, size_t len);
void ui_show_main(void);       /* main screen (leaves the wizard or the saver) */
/* "4 to 10 digits", from the shortest new code allowed. Returns translated text: pass it on,
 * never wrap it in TR() again. */
const char *ui_code_digits(void);

/* Code pad requests (any host). 'glyph' (a UI_ICON_*, static) is the icon beside the title:
 * the one on the menu row or card that asked, so every pad reads the same way. */
void ui_pad_ask(pad_host_t host, const char *glyph, const char *title, const char *hint,
                lv_color_t tone, ui_code_fn fn);
void ui_pad_ask_new(pad_host_t host, const char *glyph, const char *title, ui_code_fn fn);
/* After ui_pad_ask: plain words under its title, what the code will do (UI-025); "" for none.
 * Takes translated text, cut at 127 bytes. */
void ui_pad_ask_note(const char *note);
void ui_pad_error(const char *text);
void ui_pad_cancel(void);
/* Someone is in the middle of something at the panel: a code half typed, a sheet, Settings, the
 * setup, a panic button held. The screen saver waits for them, and so does a walk test started
 * elsewhere (WALK-023). */
bool ui_app_busy(void);
/* Wake the screen and show Home, where a walk test's card is (WALK-023). */
void ui_app_walk_show(void);
/* The status bar's left side again (a banner came or went). */
void ui_bar_banner_update(void);

/* ---- ui_forms.c ---------------------------------------------------------------------------- */
typedef void (*ui_text_fn)(const char *text);
typedef void (*ui_pick_fn)(int32_t index);
typedef void (*ui_confirm_fn)(void);

typedef struct {
    const char *glyph; /* UI_CHOICE_HEADING: a heading over the choices after it (title its words) */
    lv_color_t tone;
    const char *title;
    const char *detail;
} ui_choice_t;
/* A choice whose glyph is this is a heading (forms/section_title), not a choice: it can't be
 * picked, and keeps its index, so the pick's index still names the entry in the caller's table. */
extern const char UI_CHOICE_HEADING[];

/* Number setting shown in words. unit: UNIT_SECONDS, UNIT_MINUTES (value in seconds, shown in
 * minutes, 0 = zero_text), UNIT_PLAIN. */
typedef enum {
    UNIT_PLAIN = 0,
    UNIT_SECONDS,
    UNIT_MINUTES,
    UNIT_DIGITS,
    UNIT_TIMES,
    UNIT_MONTH,      /* 1..12 shown as "September" */
    UNIT_HOUR,       /* 0..23 shown as "14" or "2 PM", as this keypad's clock */
    UNIT_TWO_DIGITS, /* "05" (minutes) */
    UNIT_TIME_OF_DAY, /* minutes after midnight shown as "10:30 PM" or "22:30"; wraps at midnight */
} ui_unit_t;
typedef void (*ui_number_fn)(int32_t value, void *ctx);
typedef void (*ui_toggle_fn)(bool on, void *ctx);
typedef void (*ui_click_fn)(void *ctx);

void ui_forms_init(void);
/* The text fonts' chains changed (a language's fonts, the letters pack): the keys follow. */
void ui_forms_fonts_changed(void);
void ui_rows_reset(void); /* before a page is rebuilt: frees the row bindings */
size_t ui_rows_mark(void);            /* rows created after a mark ... */
void ui_rows_release(size_t mark);    /* ... can be dropped (lists refilled in place) */
lv_obj_t *ui_row_link(lv_obj_t *parent, const char *glyph, lv_color_t tone, const char *title,
                      const char *detail, const char *value, const char *trail, ui_click_fn fn,
                      void *ctx);
lv_obj_t *ui_row_toggle(lv_obj_t *parent, const char *title, const char *detail, bool on,
                        ui_toggle_fn fn, void *ctx);
/* a switch among rows with icons: it has one too, so the words start at the same place */
lv_obj_t *ui_row_toggle_icon(lv_obj_t *parent, const char *glyph, lv_color_t tone,
                             const char *title, const char *detail, bool on, ui_toggle_fn fn,
                             void *ctx);
lv_obj_t *ui_row_number(lv_obj_t *parent, const char *title, const char *detail, int32_t value,
                        int32_t min, int32_t max, int32_t step, ui_unit_t unit,
                        const char *zero_text, ui_number_fn fn, void *ctx);
lv_obj_t *ui_row_level(lv_obj_t *parent, const char *glyph, const char *title, int32_t value,
                       int32_t min, int32_t max, ui_number_fn fn, void *ctx, bool beep_on_release);
lv_obj_t *ui_row_action(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t tone,
                        ui_click_fn fn, void *ctx);
lv_obj_t *ui_row_value(lv_obj_t *parent, const char *title, const char *value);
lv_obj_t *ui_row_hold(lv_obj_t *parent, const char *text, ui_click_fn fn, void *ctx);
lv_obj_t *ui_section(lv_obj_t *parent, const char *text);
lv_obj_t *ui_paragraph(lv_obj_t *parent, const char *text, lv_color_t color);
/* A row that can't be used now, dimmed and deaf to taps (a list row, an action, a switch row):
 * what it says stays readable. */
void ui_row_disable(lv_obj_t *row);
/* A third line under a row's detail (a list row, a switch row), in the tone: what went wrong with
 * it ("Didn't work at 8:02 AM"). Made the first time, rewritten after; "" hides it. */
lv_obj_t *ui_row_sub(lv_obj_t *row, const char *text, lv_color_t tone);
/* A switch row's second line, rewritten in place as its state changes (alert: in the tone, else the
 * row's own colour); a list row (ui_row_link) takes it the same way. */
void ui_row_set_detail(lv_obj_t *row, const char *text, bool alert, lv_color_t tone);
/* One value to compare, large, the question above it (the Home Assistant pairing code). */
lv_obj_t *ui_big_value(lv_obj_t *parent, const char *label, const char *value);
/* Two actions side by side (left, right), half the width each. */
void ui_action_pair(lv_obj_t *parent, const char *left_glyph, const char *left_text,
                    lv_color_t left_tone, ui_click_fn left_fn, const char *right_glyph,
                    const char *right_text, lv_color_t right_tone, ui_click_fn right_fn, void *ctx);
void ui_forms_tick(void); /* hold buttons */

void ui_text_sheet(const char *title, const char *note, const char *initial, bool password,
                   uint32_t max_len, ui_text_fn done);
/* What is typed: words (names: a capital first), plain text (addresses, user names), a number
 * (the digits keyboard), a password (hidden). */
typedef enum { UI_TEXT_WORDS = 0, UI_TEXT_PLAIN, UI_TEXT_NUMBER, UI_TEXT_PASSWORD } ui_text_kind_t;
void ui_text_sheet_kind(const char *title, const char *note, const char *initial,
                        ui_text_kind_t kind, uint32_t max_len, ui_text_fn done);
/* After a text sheet opens: the words on its accept button ("Check"), for that sheet only. */
void ui_text_sheet_ok(const char *label);
/* After a text sheet opens: what its Cancel does, once the sheet is gone (the spoken-name editor's
 * Find a word goes back to the editor); nothing after Done. */
void ui_text_sheet_on_cancel(ui_confirm_fn fn);
void ui_pick_sheet(const char *title, const char *note, const ui_choice_t *choices, size_t n,
                   int32_t selected, ui_pick_fn fn);
/* After ui_pick_sheet: the choice at index dimmed and not pickable ("No devices to choose"). */
void ui_pick_sheet_dim(int32_t index);
/* After ui_pick_sheet: what its close button (Cancel) does, once the sheet is gone; nothing after
 * a pick. A sheet that opened it from another (the arm sheet's partition) goes back to it. */
void ui_pick_sheet_on_cancel(ui_confirm_fn fn);
/* The sheets' note (subject_sheet_note) holds more than the other string subjects' 127 bytes: a
 * confirmation may join two sentences of up to 95 bytes each, as a Zigbee device's Remove and
 * Forget do (I18N-023). ui_sheet_note_init gives it this buffer, before any screen binds it. */
#define UI_SHEET_NOTE_LENGTH 192
void ui_sheet_note_init(void);
void ui_confirm(const char *title, const char *text, const char *ok, ui_confirm_fn fn);
/* The same with a blue OK, for a question that destroys nothing ("Use it"); every other
 * confirmation's OK is red (subject_sheet_tone). */
void ui_confirm_primary(const char *title, const char *text, const char *ok, ui_confirm_fn fn);
/* A confirmation whose cancel button says and does something too ("Allow"). */
void ui_confirm_choice(const char *title, const char *text, const char *ok, ui_confirm_fn fn,
                       const char *cancel, ui_confirm_fn cancel_fn);
/* A sheet of its own (SHEET_CLOCK...): its OK runs ok, and the sheet closes when ok returns true
 * (false: it stays, ok said why); closed (may be NULL) runs once it is gone, however it closed. */
typedef bool (*ui_sheet_ok_fn)(void);
void ui_sheet_open(int32_t kind, ui_sheet_ok_fn ok, ui_confirm_fn closed);
/* After a sheet opens: who owns it (the spoken-name editor). end runs once, when that sheet is
 * closed (Cancel, Settings left, the screen timed out, an alarm) or another sheet replaces it (the
 * remote banner's question); a sheet's own pick, Cancel or Done end it only if their callback
 * didn't put up a sheet the same owner took again. NULL lets go without running it: an owner's
 * own move to its next sheet. A sheet that didn't open ends its owner at once. */
void ui_sheet_owner(ui_confirm_fn end);
void ui_sheet_close(void);
void ui_sheet_done(void);
void ui_sheet_cancel(void);
/* Rollers (lv_roller): a time of day as time_rollers.xml draws it (Hour, Minute in steps of
 * step minutes, and AM/PM on a 12-hour clock), for this keypad's clock. Fills the choices at
 * minute_of_day, without animation (UI-020); changed runs (user data ctx) when the person moves
 * one. ui_time_rollers_get: the minute of the day they show. */
void ui_time_rollers_setup(lv_obj_t *rollers, int32_t minute_of_day, int32_t step, lv_event_cb_t changed,
                           void *ctx);
int32_t ui_time_rollers_get(lv_obj_t *rollers);
/* A time of day and the days it holds for (time_sheet.xml, SHEET_TIME): title and note (may be
 * NULL) above, the rollers at minute (steps of step minutes) and seven day toggles in the region's
 * week order (ui_region()->first_day) with the short day names. days_mon: bit 0 Monday .. bit 6
 * Sunday. changed (may be NULL) runs on every roller move and every toggle, with what the sheet
 * shows; the caller answers with ui_time_sheet_result (after opening it too). ok as ui_sheet_open. */
typedef void (*ui_time_sheet_fn)(int32_t minute, uint8_t days_mon);
void ui_time_sheet_open(const char *title, const char *note, int32_t minute, uint8_t days_mon, int32_t step,
                        ui_time_sheet_fn changed, ui_sheet_ok_fn ok);
/* The line under the time sheet (the whole sentence, or in amber what is missing) and whether its
 * Done can be pressed. */
void ui_time_sheet_result(const char *text, bool amber, bool ok_enabled);
int32_t ui_time_sheet_minute(void);
uint8_t ui_time_sheet_days(void);
/* A window of time, From and To (window_sheet.xml, SHEET_WINDOW); it may pass midnight. changed (may
 * be NULL) runs on every roller move; any_time (NULL hides the button) runs once "Any time" has
 * closed the sheet. ok as ui_sheet_open. */
typedef void (*ui_window_sheet_fn)(int32_t from, int32_t to);
void ui_window_sheet_open(const char *title, int32_t from, int32_t to, int32_t step, ui_window_sheet_fn changed,
                          ui_sheet_ok_fn ok, ui_confirm_fn any_time);
/* The line under the window sheet and whether its Done can be pressed (not while From is To). */
void ui_window_sheet_result(const char *text, bool ok_enabled);
/* A sheet of rows (form_sheet.xml, SHEET_FORM): title, note (NULL: none), Cancel and Done, and its
 * body, emptied, for the caller to fill with rows; then ui_form_sheet_fit (and again when a row
 * comes or goes). ok as ui_sheet_open. The rows' own callbacks are the caller's: the page rows'
 * bindings (ui_row_number, ui_row_toggle) are dropped when the page under the sheet is rebuilt. */
lv_obj_t *ui_form_sheet_open(const char *title, const char *note, ui_sheet_ok_fn ok);
void ui_form_sheet_fit(void);
/* A sheet card's last child (its body) as tall as its contents, up to what the card's maximum
 * height leaves under its first child (the heading), then it scrolls (the device sheet, BL-184);
 * a refit keeps the body's scroll position, clamped to its new range. */
void ui_sheet_body_fit(lv_obj_t *body);
int32_t ui_window_sheet_from(void);
int32_t ui_window_sheet_to(void);
/* One roller's choices ("a\nb\nc") and selection, its rows as tall as the layout's roller_row_h;
 * wrap: the last choice is followed by the first again (hours, minutes). */
void ui_roller_fill(lv_obj_t *roller, const char *options, int32_t selected, bool wrap);
/* An event callback on an object that outlives a page (a sheet): added once, however often the
 * page that fills it is built. */
void ui_event_once(lv_obj_t *obj, lv_event_cb_t cb, lv_event_code_t code, void *ctx);
/* The eye at the end of a password field: shows what was typed, or hides it again. */
void ui_sheet_eye(void);
/* The remote keypad's keyboard, into the text sheet on screen: false when none is open. */
bool ui_forms_remote_text(const char *utf8);
bool ui_forms_remote_key(ui_remote_key_t key);

/* ---- ui_settings.c ------------------------------------------------------------------------- */
void ui_settings_init(void);
void ui_settings_open(void);                 /* the gear */
void ui_settings_section(const char *which); /* a section in the left column */
void ui_settings_leave(void);                /* Settings closes: forget codes, leave installer */
void ui_settings_tick(void);
void ui_settings_refresh(void);              /* after a new snapshot */
void ui_page_open(page_id_t id, int32_t arg);
void ui_page_back(void);
void ui_page_rebuild(void);
/* The "Panel Manager" rows of a user who may sign in from a browser (ui_settings.c): allow,
 * change the password, turn off. username is what they type to sign in; heading puts a
 * "Panel Manager" section above them (not under the Panel Manager's own switch). */
void ui_login_rows(lv_obj_t *body, uint8_t user, const char *username, bool heading);
/* What a user types to sign in: their name on the Users screen, the installer's too
 * ("Installer" when blank) - the panel's own rule (web_login.c). */
const char *ui_login_name(const alarm_config_t *cfg, uint8_t user);
/* Stops a text area's cursor from ever blinking (UI-020); the preview asserts it. */
void ui_forms_steady_cursor(lv_obj_t *ta);
void ui_page_note(const char *note);
/* The code typed in Settings (or installer mode) opens pages needing 'access'. */
bool ui_settings_allowed(ui_access_t access);
/* The locked page of a section: why (the page note), and a button that asks for a code
 * (title: the pad's title). Opens the pad once by itself when the section was just chosen. */
void ui_settings_locked(lv_obj_t *body, ui_access_t access, const char *title, const char *why,
                        char *note, size_t nlen);
/* Remember a code typed in the installer section (it opens the shared sections too). */
void ui_settings_remember(uint8_t user, const char *code);
/* A row that asks for a code with 'access' (for a page that is partly open to anyone). */
void ui_settings_unlock_row(lv_obj_t *body, ui_access_t access, const char *title);

/* ---- ui_voice.c (docs/features/08 voice-first-build.md D03, D06, D09; UI-045) --------------- */
/* The build has a voice: every voice function of the backend, and voice_info not UI_VOICE_OFF.
 * Without one there is no Spoken name row and no Announcements page. */
bool ui_voice_present(void);
void ui_voice_info(ui_voice_info_t *out); /* zeros (UI_VOICE_OFF) without a backend */
/* The voice's language in the interface's language ("English (United States)"); locale "" is the
 * one this build's words are for. Translated: pass it on, never wrap it in TR() again. */
const char *ui_voice_locale_name(const char *locale);
/* Screen and sound > Announcements' Preview: a sample name, once (D06). */
void ui_voice_preview_sample(void);
/* The zone page's Spoken name row (D03): draft is the zone being edited, stored the saved one
 * (NULL: none), pending: the editor's Done asked to approve its words with the save. Tapping it
 * opens the editor; nothing without a voice. */
void ui_voice_zone_row(lv_obj_t *body, uint16_t zone, const alarm_zone_config_t *draft,
                       const alarm_zone_config_t *stored, bool pending);
/* Installer mode ended under the editor: it closes. Announcements open: the voice's state, once
 * a second, the page built again only when it changed. */
void ui_voice_tick(void);
/* Ends the editor at once, whatever it shows (before the screens' language changes): its sheet
 * closed, its draft dropped (nothing saved), its Preview stopped, its lease released, once. Stops
 * Announcements' Preview too. Idempotent. */
void ui_voice_close(void);
/* Stops the Preview Announcements played: another Settings page is built, Settings closes, or
 * its voice changed. Nothing when none of its own may still play. */
void ui_voice_page_preview_stop(void);

/* ---- ui_installer.c ------------------------------------------------------------------------ */
/* The spoken-name editor's Done (ui_voice.c): the words go into the zone's draft; seen (NULL: no
 * approval) is the identity Done saw, and asks the backend to save the page and approve them in
 * one transaction (voice_apply_approve) as the zone page is left. A draft that can't be said is
 * saved without approval. */
void ui_installer_spoken_done(uint16_t zone, const uint16_t *ids, size_t n,
                              const ui_voice_seen_t *seen);
/* The zone being edited in installer mode (NULL: not in it, or the zone isn't used). */
const alarm_zone_config_t *ui_installer_zone_draft(uint16_t zone);
bool ui_installer_build(page_id_t id, int32_t arg, lv_obj_t *body, char *title, size_t tlen,
                        char *note, size_t nlen);
void ui_installer_leave(void);   /* apply pending changes and leave installer mode */
void ui_installer_apply(void);   /* apply pending changes now */
void ui_installer_create_zone(uint16_t zone); /* a new zone in the working copy (defaults) */
/* ... with the type zsrc_suggest_zone_type(cls) suggests and device_class cls, instead of the
 * default Perimeter/door (docs/features/22 "Defaults from the device class"). */
void ui_installer_create_zone_for(uint16_t zone, alarm_device_class_t cls);
/* A zone's "Supervision" row, a picker (Sensor settings; the zone page without zone sources):
 * how long the sensor itself may stay silent, in the working copy, saved with the zone. */
void ui_installer_supervision_row(lv_obj_t *body, uint16_t zone);

/* ---- ui_sources.c (docs/features/22) ------------------------------------------------------------ */
bool ui_sources_available(void); /* the backend has zone sources */
/* "Wired", "Simulator", "No sensor"... for the zone list; NULL without zone sources. */
const char *ui_sources_short(uint16_t zone, char *buf, size_t len);
/* "Where is the sensor?": for a new zone (created once a sensor is picked) or an existing one. */
void ui_sources_pick(uint16_t zone, bool is_new);
/* A zone's sensor settings page (PAGE_ZONE_SENSOR): reading, contact or reverse, response,
 * supervision and grace, the tamper and low-battery inputs, removing the sensor but keeping the
 * zone. */
void ui_sources_build_settings(lv_obj_t *body, int32_t arg, char *title, size_t tlen, char *note,
                               size_t nlen);
/* The zone page's top row: the zone right now (live, at most once a second), the source and what
 * it promises as its detail. */
void ui_sources_zone_status(lv_obj_t *body, uint16_t zone);
/* The zone page's sensor rows: Source, a Z-Wave device, Sensor settings with what they are. */
void ui_sources_zone_rows(lv_obj_t *body, uint16_t zone);
/* PAGE_ZONE_SOURCE: a source's sensors (the terminals), live while it learns. */
void ui_sources_build_picker(lv_obj_t *body, int32_t arg, char *title, size_t tlen, char *note,
                             size_t nlen);
/* The zone no longer has a sensor; false when it couldn't be let go (something is armed). */
bool ui_sources_zone_removed(uint16_t zone);
void ui_sources_tick(void);
void ui_sources_stop(void); /* its page is being dropped (learning stops) */

/* ---- ui_net.c ------------------------------------------------------------------------------ */
void ui_net_build(lv_obj_t *body, bool setup); /* status + networks; setup: wizard layout */
/* PAGE_NETWORK: the joined network's page; the title is its name (set when one is joined). */
void ui_net_advanced_build(lv_obj_t *body, char *title, size_t tlen, char *note, size_t nlen);
/* Time and language's rows from ui_lang.c (docs/features/37 G3): Language, Region and how dates
 * and numbers look, with a master or installer code; and Clock (12 or 24 hours), this keypad's
 * and anyone's. */
void ui_lang_rows(lv_obj_t *body);
void ui_lang_clock_row(lv_obj_t *body);
/* First-time setup's language step (I18N-012): only when there is more than one language. */
bool ui_lang_offered(void);
void ui_lang_setup_build(lv_obj_t *body, void (*rebuild)(void));
/* Settings > Time and language (BL-131): what the clock says and the 12/24-hour clock for anyone;
 * unlocked, the time zone, the language and the time server too. */
void ui_time_build(lv_obj_t *body, bool unlocked, char *note, size_t nlen);
/* PAGE_SET_CLOCK: only the way through a code (the page is locked without one): once a code
 * opened it, Set the date and time opens over Time and language (docs/features/35). */
void ui_clock_build(lv_obj_t *body, char *note, size_t nlen);
/* The setup's date and time step (RTC-015): the calendar and the rollers, narrower, in body. */
void ui_clock_picker(lv_obj_t *body);
void ui_clock_fields_start(void);   /* the calendar and rollers start from the panel's time */
bool ui_clock_set_chosen(void);     /* sets the clock; false (beep + toast) when it can't */
bool ui_clock_setup_needed(void);   /* a module that can keep a time, unset, no internet */
void ui_net_tick(void);
bool ui_net_connected(void);
void ui_net_stop(void);

/* ---- optional pages (docs/features/33 step 6) ---------------------------------------------------
 * Each is compiled only with its feature (sources.cmake: ALARM_UI_NOTIFY, _INTEGRATIONS,
 * _STORAGE, _PHOTO_SAVER); without it the calls below are empty and its rows are not shown. */
#ifndef ALARM_UI_NOTIFY
#define ALARM_UI_NOTIFY 1
#endif
#ifndef ALARM_UI_INTEGRATIONS
#define ALARM_UI_INTEGRATIONS 1
#endif
#ifndef ALARM_UI_STORAGE
#define ALARM_UI_STORAGE 1
#endif
#ifndef ALARM_UI_PHOTO_SAVER
#define ALARM_UI_PHOTO_SAVER 1
#endif
#ifndef ALARM_UI_ZWAVE
#define ALARM_UI_ZWAVE 0 /* only builds with the Z-Wave zone source have its pages */
#endif
#ifndef ALARM_UI_ZIGBEE
#define ALARM_UI_ZIGBEE 0 /* only builds with the Zigbee zone source have its pages */
#endif
#ifndef ALARM_UI_DEVICES
#define ALARM_UI_DEVICES 0 /* only builds with the devices registry have Smart Home */
#endif
#ifndef ALARM_UI_AUTOMATIONS
#define ALARM_UI_AUTOMATIONS 0 /* only builds with the automations service have their pages */
#endif

/* ---- ui_auto_warn.c (an automation's arming warning at the wall, docs/features/41 AUT-012,
 * AUT-013, D19, D20, D26-D28): the status bar's banner, Home's card above the arming tiles with
 * its Put off, the choice and the code, and the quiet hold note. Without the feature, or with a
 * backend that has no auto_warnings, nothing shows. */
#if ALARM_UI_AUTOMATIONS
void ui_auto_warn_init(void);
void ui_auto_warn_tick(void);             /* reads the warnings once a second */
void ui_auto_warn_language_changed(void); /* every word it wrote, again */
bool ui_auto_warn_banner(void);           /* a warning is on: the bar may show banner 5 */
#else
static inline bool ui_auto_warn_banner(void) { return false; }
#endif

/* ---- ui_devices.c (Smart Home, docs/features/24, BL-045) ----------------------------------- *
 * Without the feature the rail's Smart Home stays hidden (subject_devices_avail 0) and nothing
 * here exists: every call from another module sits under #if ALARM_UI_DEVICES. */
#if ALARM_UI_DEVICES
void ui_devices_init(void);
void ui_devices_tick(void);             /* reads the devices: once a second on the view, else every few */
void ui_devices_shown(void);            /* the view was opened: its tiles, now */
void ui_devices_language_changed(void); /* every word it wrote, again */
#endif

/* ---- ui_integrations.c --------------------------------------------------------------------- */
#if ALARM_UI_INTEGRATIONS
void ui_integrations_build(page_id_t id, lv_obj_t *body, char *title, size_t tlen, char *note,
                           size_t nlen);
void ui_integrations_tick(void); /* keeps the connection status current */
void ui_integrations_stop(void); /* its page is being dropped */
#else
static inline void ui_integrations_build(page_id_t id, lv_obj_t *body, char *title, size_t tlen,
                                         char *note, size_t nlen)
{
    (void)id; (void)body; (void)title; (void)tlen; (void)note; (void)nlen;
}
static inline void ui_integrations_tick(void) {}
static inline void ui_integrations_stop(void) {}
#endif

/* ---- ui_notify.c (Integrations > Notifications: ntfy, Pushover) ---------------------------- */
#if ALARM_UI_NOTIFY
void ui_notify_build(page_id_t id, lv_obj_t *body, char *title, size_t tlen, char *note,
                     size_t nlen);
bool ui_notify_available(void);           /* this panel can send notifications at all */
void ui_notify_list_rows(lv_obj_t *body); /* the rows under the Notifications heading */
void ui_notify_tick(void);                /* keeps "last sent" and the state current */
void ui_notify_page_left(void);           /* the QR code never outlives its page */
void ui_notify_stop(void);                /* its page is being dropped */
#else
static inline void ui_notify_build(page_id_t id, lv_obj_t *body, char *title, size_t tlen,
                                   char *note, size_t nlen)
{
    (void)id; (void)body; (void)title; (void)tlen; (void)note; (void)nlen;
}
static inline bool ui_notify_available(void) { return false; }
static inline void ui_notify_list_rows(lv_obj_t *body) { (void)body; }
static inline void ui_notify_tick(void) {}
static inline void ui_notify_page_left(void) {}
static inline void ui_notify_stop(void) {}
#endif

/* ---- ui_saver.c ---------------------------------------------------------------------------- */
#if ALARM_UI_PHOTO_SAVER
void ui_saver_prepare(void); /* before the screen saver shows: its picture (decoded once) */
#else
static inline void ui_saver_prepare(void) {} /* the plain saver: clock and state */
#endif

/* ---- ui_storage.c -------------------------------------------------------------------------- */
#if ALARM_UI_STORAGE
void ui_storage_build(lv_obj_t *body); /* PAGE_SD_CARD */
lv_obj_t *ui_storage_about_row(lv_obj_t *body, ui_click_fn fn, void *ctx); /* About's SD card row */
void ui_storage_tick(void);
void ui_storage_stop(void);
#else
static inline void ui_storage_build(lv_obj_t *body) { (void)body; }
static inline lv_obj_t *ui_storage_about_row(lv_obj_t *body, ui_click_fn fn, void *ctx)
{
    (void)body; (void)fn; (void)ctx;
    return NULL;
}
static inline void ui_storage_tick(void) {}
static inline void ui_storage_stop(void) {}
#endif

/* ---- ui_zwave.c (Installer > Z-Wave, docs/features/36) ------------------------------------- *
 * Declared only with the feature: every call from another module sits under #if ALARM_UI_ZWAVE. */
/* A sensor's state in the screen's language (ui_sources.c): the words a source reports its state
 * in ("Wet", "Clear", "Mains lost") are marked there; anything else - a value such as "21.5 °C" -
 * comes back as it is. */
const char *ui_sensor_state_words(const char *state);
/* A service's answer - marked I18N_NOOP_C(ctx, ...), or one of ui_why_words.h's with a number - in
 * the screen's language (ui_app.c). One buffer: use the result before calling again. */
const char *ui_answer(const char *ctx, const char *why);
#if ALARM_UI_ZWAVE
bool ui_zwave_available(void);          /* the panel runs a Z-Wave service (ui_backend_t zw_*) */
/* A Z-Wave input's words in the screen's language (ui_sources.c, docs/features/37): "door contact",
 * "input 2 · water"; as a title "Door contact", "Input 2 · water". endpoint 0 is the device
 * itself, slot a zsrc_zw_slot_t, device_class what a zone of it starts with (a contact's door or
 * window). The same words as the zone source's English (zsrc_zw_input_words). */
void ui_zw_input_words(uint8_t endpoint, uint8_t slot, uint8_t device_class, bool title, char *out,
                       size_t len);
void ui_zwave_menu_row(lv_obj_t *body); /* the installer menu's row (screens.md §2) */
/* PAGE_ZWAVE and the pages it leads to. */
void ui_zwave_build(page_id_t id, int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                    size_t nlen);
void ui_zwave_tick(void); /* the pages' live words, read once a second */
/* The Troubles list (screens.md section 10, ZW-019): true when the sensor radio trouble is the
 * Z-Wave radio's (jammed, silent, unusable or another system's); the row is then titled and
 * worded for Z-Wave, and opens the Z-Wave page (ui_zwave_trouble_link). */
bool ui_zwave_trouble_line(alarm_sys_trouble_t t, ui_line_t *line);
void ui_zwave_trouble_link(lv_obj_t *row, bool link); /* the row opens the Z-Wave page, or not */
void ui_zwave_stop(void); /* its page is being dropped */
/* "45 min", "2 h 30 min", "3 days": for the zone page's "Z-Wave device" row (ui_sources.c). */
void ui_zwave_span_words(uint32_t seconds, char *out, size_t len);
/* "Zooz ZSE41" of "Zooz ZSE41 open/close sensor": the zone page's Source row, the device list. */
void ui_zwave_short_model(const char *model, char *out, size_t len);
#endif

/* ---- ui_zigbee.c (Installer > Zigbee, docs/features/20 devices.md §9, ZB-027) --------------- *
 * Declared only with the feature: every call from another module sits under #if ALARM_UI_ZIGBEE. */
#if ALARM_UI_ZIGBEE
bool ui_zigbee_available(void);          /* the panel runs a Zigbee service (ui_backend_t zb_*) */
void ui_zigbee_menu_row(lv_obj_t *body); /* the installer menu's row, after Z-Wave's */
/* PAGE_ZIGBEE and the pages it leads to. */
void ui_zigbee_build(page_id_t id, int32_t arg, lv_obj_t *body, char *title, size_t tlen, char *note,
                     size_t nlen);
void ui_zigbee_tick(void); /* the pages' live words, read once a second */
void ui_zigbee_stop(void); /* its page is being dropped */
/* The zone page (ui_sources.c, the proposal's page 12): a Zigbee input's Source words ("Zigbee ·
 * Aqara door sensor"; false: no such device, the caller says it its own way), and the row of its
 * device (battery, signal, when heard) that opens the device page. */
bool ui_zigbee_source_words(const zsrc_address_t *addr, char *out, size_t len);
void ui_zigbee_zone_device_row(lv_obj_t *body, const zsrc_address_t *addr);
/* The zone editor's sensor picker (page 11): its first row, "Add a new Zigbee device". */
void ui_zigbee_picker_row(lv_obj_t *body);
/* The Troubles list (page 13): true when this trouble's row opens the Zigbee page (the sensor radio
 * trouble while the Zigbee radio has the problem); its second line then says it can be tapped. */
bool ui_zigbee_trouble_line(alarm_sys_trouble_t t, ui_line_t *line);
void ui_zigbee_trouble_link(lv_obj_t *row, bool link); /* the row opens the Zigbee page, or not */
#endif

/* ---- ui_walk.c (the walk test, docs/features/40) ------------------------------------------ */
bool ui_walk_running(void); /* a partition is in walk test: the screen stays lit (WALK-022) */
/* Every tick and snapshot: Home's walk test card (its words at most once a second, WALK-013), a
 * test started elsewhere (WALK-023), the end warning (WALK-004) and the status bar's banner. */
void ui_walk_home(void);
/* The status bar's walk test banner (bar banner 4): true to show it, its words in
 * subject_walk_banner. A tap: ui_walk_banner_pressed(). */
bool ui_walk_banner(void);
void ui_walk_banner_pressed(void);
/* End on Home's card: a question, not a code (WALK-025). */
void ui_walk_end_ask(void);
/* Installer > Walk test: leaves installer mode and starts the test, which the engine keeps apart
 * (CORE-PROG-002/003); with several partitions it opens Settings > Walk test to pick one. */
void ui_walk_from_installer(bool unsaved); /* unsaved: leaving showed "Not saved" */
/* PAGE_WALK, opened with a code that may start one (WALK-001): the start, or the test running
 * with its list; or without a code while ui_walk_viewable(): the test or its summary, to follow
 * and to end (WALK-009, WALK-025). */
bool ui_walk_viewable(void); /* this keypad's partition is testing or has a summary */
void ui_walk_build(lv_obj_t *body, char *note, size_t nlen);
void ui_walk_tick(void);    /* the open page follows the test, at most once a second */

/* ---- ui_update.c --------------------------------------------------------------------------- */

void ui_update_build(lv_obj_t *body); /* "Software update" on the About page */
void ui_update_tag(void);             /* About's "Update" tag in the Settings list (BL-131) */
lv_obj_t *ui_update_about_row(lv_obj_t *body, ui_click_fn fn, void *ctx); /* About's Software update row */
void ui_update_tick(void);            /* keeps it current */
void ui_update_stop(void);            /* its page is being dropped */
/* Installer > Updates (BL-186, in ui_update.c): the menu's group, the server pages, and the
 * check's end when its pages are left. */
void ui_fwsrv_menu(lv_obj_t *body);
bool ui_fwsrv_build(page_id_t id, lv_obj_t *body, char *title, size_t tlen, char *note, size_t nlen);
void ui_fwsrv_stop(void);
/* The status bar's update banner (BL-188): 0 none, 1 blue (bar banner 6), 2 amber (7). The words
 * are in subject_update_banner; ui_update_banner_tick reads the run once a second. */
uint8_t ui_update_banner(void);
void ui_update_banner_tick(void);

/* ---- ui_wizard.c --------------------------------------------------------------------------- */
void ui_wizard_init(void);
void ui_wizard_start(bool can_exit); /* can_exit: the panel is already set up, offer "Leave setup" */
void ui_wizard_next(void);
void ui_wizard_back(void);
void ui_wizard_exit(void);   /* "Leave setup": back to Home, nothing else changed */
void ui_wizard_tick(void);
void ui_wizard_pause(void);  /* an alarm needs the main screen */
void ui_wizard_resume(void);

/* ---- ui_tz.c ------------------------------------------------------------------------------- */
typedef struct {
    const char *name;   /* "Eastern Time" */
    const char *places; /* "New York, Toronto" */
    const char *posix;  /* "EST5EDT,M3.2.0,M11.1.0" */
} ui_tz_t;
extern const ui_tz_t UI_TIMEZONES[];
extern const size_t UI_TIMEZONE_COUNT;
int ui_tz_find(const char *posix); /* index, -1 when not in the list */
void ui_tz_pick(ui_pick_fn fn);    /* choice sheet with every zone */

/* ---- ui_automations.c, ui_auto_edit.c (Settings > Automations, docs/features/41) ----------- */
#include "ui_automations.h"
