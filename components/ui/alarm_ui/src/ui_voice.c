/*
 * ui_voice.c - the voice at the touchscreen (docs/features/08 voice-first-build.md D03, D06, D09;
 * docs/features/07 UI-045): a zone's Spoken name row and its editor (Installer > Zones > zone), and
 * the words the Announcements page of Screen and sound shares with it (the voice's language, the
 * pack, the sample Preview plays).
 *
 * The editor is a form sheet (ui_form_sheet_open) with Cancel and Done over the zone page; a word's
 * actions, Add word, a group of words and Find a word's matches are pick sheets in its place, one
 * sheet at a time, and each comes back to the editor from its draft. The word lists come from a
 * lease on the active voice pack (ui_backend_t.voice_pack_acquire), held while the editor is open
 * and released however it closes: the editor owns each of its sheets (ui_sheet_owner), so one
 * closed or replaced from outside - Cancel, Settings left, the screen timed out, an alarm, the
 * remote banner's question - ends it at once; its own moves between its sheets let go first.
 * Ending stops the Preview it played (ui_backend_t.voice_preview_stop) and drops its draft; it
 * runs once however many ways it is reached.
 *
 * The words of the vocabulary are the pack's data, shown as recorded and never translated. Done
 * puts the words into the zone's draft (ui_installer_spoken_done); the approval follows the save
 * of the zone page (ui_installer.c), never a configuration by itself (D07).
 */
#include <stdio.h>
#include <string.h>

#include "ui_internal.h"
#include "voice_pack/vp_words.h"

/* The four groups of selectable words (vp_category_t 1-4), as the interface names them. */
/* TR: the spoken-name editor's Add word list: the groups of recorded words, and the line under a
 * word Find a word found. At most 47 bytes each */
static const char *const GROUP_TEXT[4] = {
    TR_NOOP_C("word group", "Position"), TR_NOOP_C("word group", "Rooms and places"),
    TR_NOOP_C("word group", "Things"), TR_NOOP_C("word group", "Numbers")};

/* What Add word shows under each group: three of its words, from the pack (data, in the voice's
 * language). The en-US voice's words are looked for first; another voice shows its first three. */
static const char *const GROUP_EXAMPLES[4] = {NOTR("Front Back Upstairs"), NOTR("Kitchen Garage Basement"),
                                              NOTR("Door Window Motion"), NOTR("One Nine Ten Twenty Hundred")};

/* The languages a voice can speak, named in the interface's language (the language page names
 * each language in itself instead: that list is for people looking for their own). */
static const struct {
    const char *tag;
    const char *name;
} VOICE_LOCALES[] = {
    /* TR: the voice's language, on the spoken-name editor and Screen and sound > Announcements.
     * At most 47 bytes */
    {NOTR("en-US"), TR_NOOP("English (United States)")},
};

/* The language the voice speaks without a pack: the only one this build's words are for (D07,
 * LIBRARY/en-US/vocabulary-schema-1). */
#define VOICE_DEFAULT_LOCALE NOTR("en-US")

/* A pick sheet's rows: a group (up to two pages of words) with a heading for each letter, or a
 * page of matches with Previous and Next. */
#define VOICE_CHOICES (2u * VP_WORDS_PAGE + 32u)
#define VOICE_SUMMARY_MAX 40u /* bytes of the zone row's phrase before it is cut with "..." */

typedef enum { MSG_NONE = 0, MSG_UNMATCHED, MSG_LONG } voice_msg_t;

typedef struct {
    bool active;
    uint16_t zone;
    const vp_pack_t *pack; /* the lease, NULL when the voice isn't ready */
    uint32_t pack_revision; /* the leased pack's, whose words the editor shows (0: none) */
    uint8_t state;         /* ui_voice_state_t when the editor opened */
    char locale[VP_LOCALE_SIZE];
    char name[ALARM_NAME_LEN + 8];
    uint16_t ids[VP_RECIPE_MAX];
    uint8_t n;
    voice_msg_t msg;
    char said[VP_MATCH_SPANS_MAX * 24u]; /* the parts of the name the panel can't say */
    uint8_t word;  /* the word whose actions are open */
    uint8_t group; /* the group open (1-4), 0 = Find a word */
    char query[32];
    uint16_t page;
    char note[UI_SHEET_NOTE_LENGTH];
    char title[48];
    char details[4][96];
    ui_choice_t choices[VOICE_CHOICES];
    uint16_t pick_ids[VOICE_CHOICES];
    char letters[32][8];
} voice_editor_t;

static voice_editor_t E UI_SLOW_BSS;

/* Whose Preview may still play: the one to stop as it closes (only Preview: automatic speech and
 * tones are never stopped from here). */
typedef enum { PREVIEW_NONE = 0, PREVIEW_EDITOR, PREVIEW_PAGE } preview_owner_t;
static uint8_t s_preview_owner;
static uint32_t s_voice_polled; /* the editor's once-a-second look at the voice */

static void editor_show(void);

/* ---- the voice ------------------------------------------------------------------------------ */

bool ui_voice_present(void)
{
    if (S.be.voice_info == NULL || S.be.voice_pack_acquire == NULL || S.be.voice_pack_release == NULL ||
        S.be.voice_preview == NULL || S.be.voice_seen == NULL || S.be.voice_apply_approve == NULL) {
        return false;
    }
    if (ui_link_keypad()) {
        return false; /* a keypad has no voice: the alarm panel speaks (BL-048) */
    }
    ui_voice_info_t info;
    S.be.voice_info(S.be.ctx, &info);
    return info.state != UI_VOICE_OFF;
}

void ui_voice_info(ui_voice_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (S.be.voice_info != NULL) {
        S.be.voice_info(S.be.ctx, out);
    }
}

const char *ui_voice_locale_name(const char *locale)
{
    const char *tag = locale != NULL && locale[0] != '\0' ? locale : VOICE_DEFAULT_LOCALE;
    for (size_t i = 0; i < sizeof(VOICE_LOCALES) / sizeof(VOICE_LOCALES[0]); i++) {
        if (strcmp(VOICE_LOCALES[i].tag, tag) == 0) {
            return TR(VOICE_LOCALES[i].name);
        }
    }
    return tag; /* a language this build has no name for: its tag, as data */
}

/* "Voice language: English (United States)", from the editor's voice. */
static void language_line(char *buf, size_t len, const char *locale)
{
    /* TR: the spoken-name editor and its Add word lists: the language the recorded words are in;
     * {0} is the language's name (up to 47 bytes). At most 80 bytes */
    I18N_FORMAT(buf, len, TR("Voice language: {0}"), ui_voice_locale_name(locale));
}

static const char *word_text(const vp_pack_t *pack, uint16_t id)
{
    vp_clip_t clip;
    return pack != NULL && vp_find(pack, id, &clip) ? clip.text : NULL;
}

/* The words joined as they are said ("Front Door"), cut with "..." past max bytes. */
static void phrase_text(const vp_pack_t *pack, const uint16_t *ids, size_t n, char *buf, size_t len,
                        size_t max)
{
    buf[0] = '\0';
    size_t used = 0;
    for (size_t i = 0; i < n; i++) {
        const char *w = word_text(pack, ids[i]);
        if (w == NULL) {
            buf[0] = '\0'; /* a word this pack doesn't have: no phrase rather than half of one */
            return;
        }
        const size_t wl = strlen(w);
        if (used + (i > 0u ? 1u : 0u) + wl > max || used + wl + 5u > len) {
            snprintf(buf + used, len - used, "%s", NOTR("..."));
            return;
        }
        used += (size_t)snprintf(buf + used, len - used, "%s%s", i > 0u ? " " : "", w);
    }
}

/* Plays the words once, as the zone's name would sound (D03): the answer is the backend's
 * (QUIET at volume 0, in an alarm or on battery); nothing on the screen changes. */
static void preview_words(const uint16_t *ids, size_t n, uint8_t owner)
{
    if (S.be.voice_preview == NULL || n == 0u) {
        return;
    }
    s_preview_owner = owner;
    const uint8_t r = S.be.voice_preview(S.be.ctx, ids, n);
    if (r != UI_VOICE_PREVIEW_OK && r != UI_VOICE_PREVIEW_QUIET) {
        ui_beep(UI_BEEP_ERROR);
    }
}

/* Stops the Preview owner played, if it was the last to play one. */
static void preview_stop(uint8_t owner)
{
    if (s_preview_owner != owner) {
        return;
    }
    s_preview_owner = PREVIEW_NONE;
    if (S.be.voice_preview_stop != NULL) {
        S.be.voice_preview_stop(S.be.ctx);
    }
}

void ui_voice_page_preview_stop(void)
{
    preview_stop(PREVIEW_PAGE);
}

void ui_voice_preview_sample(void)
{
    if (S.be.voice_pack_acquire == NULL || S.be.voice_pack_release == NULL) {
        return;
    }
    const vp_pack_t *pack = S.be.voice_pack_acquire(S.be.ctx);
    if (pack == NULL) {
        ui_beep(UI_BEEP_ERROR);
        return;
    }
    /* A zone's name as the panel says it: "Front Door" with the en-US voice, else the voice's
     * first word. */
    vp_name_match_t m;
    if (!vp_match_name(pack, NOTR("Front door"), &m)) {
        vp_word_page_t wp;
        m.count = 0;
        if (vp_list_words(pack, 0, NULL, 0, &wp) && wp.count > 0u) {
            m.ids[0] = wp.ids[0];
            m.count = 1;
        }
    }
    preview_words(m.ids, m.count, PREVIEW_PAGE);
    S.be.voice_pack_release(S.be.ctx, pack);
}

/* ---- the zone's row ----------------------------------------------------------------------- */

/* Are the zone's words approved as they will be saved: the stored approval is this zone's (its
 * binding revision and this pack's revision) and the draft keeps the words, name and partition it
 * was approved with - or the editor's Done asked to approve them with the save (pending). */
static bool zone_approved(const alarm_zone_config_t *draft, const alarm_zone_config_t *stored,
                          bool pending, uint32_t pack_revision)
{
    if (pending) {
        return true;
    }
    if (stored == NULL || memcmp(draft->spoken_words, stored->spoken_words, sizeof(draft->spoken_words)) != 0 ||
        strncmp(draft->name, stored->name, ALARM_NAME_LEN) != 0 || draft->partition != stored->partition) {
        return false;
    }
    return stored->voice_approved_revision != 0u &&
           stored->voice_approved_revision == stored->voice_binding_revision &&
           stored->voice_pack_revision == pack_revision;
}

static void open_editor(void *ctx);

void ui_voice_zone_row(lv_obj_t *body, uint16_t zone, const alarm_zone_config_t *draft,
                       const alarm_zone_config_t *stored, bool pending)
{
    if (!ui_voice_present() || draft == NULL) {
        return;
    }
    ui_voice_info_t info;
    ui_voice_info(&info);
    uint16_t ids[ALARM_ZONE_WORDS];
    const unsigned n = alarm_zone_word_count(draft);
    for (unsigned i = 0; i < n; i++) {
        ids[i] = alarm_zone_word(draft, i);
    }
    char phrase[VOICE_SUMMARY_MAX + 8u] = "";
    if (n > 0u && info.state == UI_VOICE_READY) {
        const vp_pack_t *pack = S.be.voice_pack_acquire(S.be.ctx);
        phrase_text(pack, ids, n, phrase, sizeof(phrase), VOICE_SUMMARY_MAX);
        S.be.voice_pack_release(S.be.ctx, pack);
    }
    const char *value = phrase;
    const char *detail = NULL;
    if (n == 0u) {
        /* TR: Installer > zone, the Spoken name row's value while the zone has no words yet. At
         * most 47 bytes */
        value = TR("Choose words");
    } else if (info.state == UI_VOICE_NONE) {
        /* TR: Installer > zone, the Spoken name row's value (its words below it) without a voice
         * pack. At most 47 bytes */
        value = TR("Voice pack not installed");
        detail = phrase;
    } else if (info.state != UI_VOICE_READY) {
        /* TR: Installer > zone, the Spoken name row's value (its words below it) while the voice
         * pack is installed but can't be used. At most 47 bytes */
        value = TR("Voice is unavailable");
        detail = phrase;
    } else if (!zone_approved(draft, stored, pending, info.pack_revision)) {
        /* TR: Installer > zone, the Spoken name row's value and the line under it: the words
         * are kept but nothing is said until the installer has looked at them. At most 47 bytes
         * each */
        value = TR("Review spoken name");
        /* TR: Installer > zone, the line under "Review spoken name". At most 95 bytes */
        detail = TR("The name, partition or sensor changed");
    } else if (!S.opt.voice_enabled) {
        /* TR: Installer > zone, the Spoken name row's value (its words below it) while
         * Announcements are off in Screen and sound. At most 47 bytes */
        value = TR("Announcements are off");
        detail = phrase;
    }
    /* TR: Installer > zone, the row of the words the panel says for the zone. At most 31 bytes */
    ui_row_link(body, UI_ICON_MIC, ui_tone_blue(), TR("Spoken name"), detail, value,
                UI_ICON_CHEVRON_RIGHT, open_editor, (void *)(uintptr_t)zone);
}

/* ---- the editor --------------------------------------------------------------------------- */

static bool voice_ready(void)
{
    return E.pack != NULL;
}

static bool draft_check(vp_recipe_check_t *chk)
{
    return voice_ready() && E.n > 0u && vp_check_recipe(E.pack, E.ids, E.n, chk);
}

/* The draft is too long to say (D02: over eight words or 10 s): saved, never approved. */
static bool draft_too_long(void)
{
    vp_recipe_check_t chk;
    if (!voice_ready() || E.n == 0u || draft_check(&chk)) {
        return false;
    }
    return chk.verdict == VP_RECIPE_TOO_LONG || chk.verdict == VP_RECIPE_TOO_MANY;
}

/* The editor ends (ui_sheet_owner's end, Done, ui_voice_close): its Preview stops, its lease is
 * released and its draft dropped. Never touches a sheet (it runs inside their changes); a second
 * call does nothing. */
static void editor_end(void)
{
    if (!E.active && E.pack == NULL) {
        return;
    }
    E.active = false;
    preview_stop(PREVIEW_EDITOR);
    if (E.pack != NULL && S.be.voice_pack_release != NULL) {
        S.be.voice_pack_release(S.be.ctx, E.pack);
    }
    E.pack = NULL;
    E.n = 0;
    memset(E.ids, 0, sizeof(E.ids));
}

/* The editor's own move from its sheet to its next (a word's actions, Add word, Find a word, back
 * to the editor): it lets go of the sheet on screen first, so closing it doesn't end the editor,
 * and takes the next one (editor_own) once it opened. */
static void editor_leave_sheet(void)
{
    ui_sheet_owner(NULL);
    ui_sheet_close();
}

static void editor_own(void)
{
    ui_sheet_owner(editor_end);
}

void ui_voice_close(void)
{
    if (E.active) {
        ui_sheet_close(); /* its own sheet (it owns the one on screen): ends it through the hook */
    }
    editor_end();
    preview_stop(PREVIEW_PAGE);
}

void ui_voice_tick(void)
{
    /* Installer mode ended under it (timed out, left elsewhere): zone words are the installer's
     * alone, and nothing may be armed with an editor still open - it closes, its draft dropped. */
    if (E.active && !S.program) {
        ui_voice_close();
    }
    /* Its pack changed or stopped being ready (an install, a fault): the Preview it played stops;
     * the editor keeps its lease and words until it closes. Once a second at most. */
    if (E.active && s_preview_owner == PREVIEW_EDITOR && lv_tick_elaps(s_voice_polled) >= 1000u) {
        s_voice_polled = lv_tick_get();
        ui_voice_info_t info;
        ui_voice_info(&info);
        if (info.state != UI_VOICE_READY || info.pack_revision != E.pack_revision) {
            preview_stop(PREVIEW_EDITOR);
        }
    }
}

static bool editor_done(void)
{
    if (!E.active) {
        return true;
    }
    vp_recipe_check_t chk;
    bool approve = draft_check(&chk);
    const uint16_t zone = E.zone;
    uint16_t ids[VP_RECIPE_MAX];
    const uint8_t n = E.n;
    memcpy(ids, E.ids, sizeof(ids));
    /* D07: the identity this Done saw - the configuration's generation and the zone's stored
     * binding now, and the pack whose words the editor showed. The approval that follows the
     * save is granted against these, never against what is current when it runs. */
    ui_voice_seen_t seen = {0};
    if (approve && (S.be.voice_seen == NULL || !S.be.voice_seen(S.be.ctx, zone, &seen))) {
        approve = false;
    }
    seen.pack_revision = E.pack_revision;
    editor_end();
    ui_installer_spoken_done(zone, ids, n, approve ? &seen : NULL);
    return true;
}

static void set_draft(const uint16_t *ids, uint8_t n)
{
    memcpy(E.ids, ids, (size_t)n * sizeof(ids[0]));
    E.n = n;
    E.msg = MSG_NONE;
}

static void back_to_editor(void)
{
    if (E.active) {
        editor_show();
    }
}

/* -- a word's actions -- */

enum { WORD_EARLIER = 0, WORD_LATER, WORD_REMOVE, WORD_ACTIONS };

static void word_action(int32_t index)
{
    const uint8_t i = E.word;
    if (E.active && i < E.n) {
        if (index == WORD_EARLIER && i > 0u) {
            const uint16_t t = E.ids[i - 1u];
            E.ids[i - 1u] = E.ids[i];
            E.ids[i] = t;
        } else if (index == WORD_LATER && i + 1u < E.n) {
            const uint16_t t = E.ids[i + 1u];
            E.ids[i + 1u] = E.ids[i];
            E.ids[i] = t;
        } else if (index == WORD_REMOVE) {
            memmove(&E.ids[i], &E.ids[i + 1u], (size_t)(E.n - i - 1u) * sizeof(E.ids[0]));
            E.n--;
        }
        E.msg = MSG_NONE;
    }
    back_to_editor();
}

static void on_word(lv_event_t *e)
{
    const uint8_t i = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (!E.active || i >= E.n) {
        return;
    }
    E.word = i;
    const char *w = word_text(E.pack, E.ids[i]);
    if (w != NULL) {
        snprintf(E.title, sizeof(E.title), "%s", w);
    } else {
        /* TR: the spoken-name editor: a word whose recording isn't available now; {0} is its
         * place (1-8). At most 40 bytes */
        I18N_FORMAT(E.title, sizeof(E.title), TR("Word {0}"), (unsigned)(i + 1u));
    }
    /* TR: the spoken-name editor, under a word's name on its actions: {0} its place, {1} how
     * many words there are. At most 60 bytes */
    I18N_FORMAT(E.note, sizeof(E.note), TR("Word {0} of {1}"), (unsigned)(i + 1u), (unsigned)E.n);
    /* TR: the spoken-name editor, a word's actions. At most 47 bytes */
    E.choices[WORD_EARLIER] = (ui_choice_t){UI_ICON_ARROW_UP, ui_tone_blue(), TR("Move earlier"), NULL};
    /* TR: the spoken-name editor, a word's actions. At most 47 bytes */
    E.choices[WORD_LATER] = (ui_choice_t){UI_ICON_ARROW_DOWN, ui_tone_blue(), TR("Move later"), NULL};
    /* TR: the spoken-name editor, a word's actions: takes the word out of the name. At most 47
     * bytes */
    E.choices[WORD_REMOVE] = (ui_choice_t){UI_ICON_TRASH, ui_tone_red(), TR_C("spoken word", "Remove"), NULL};
    editor_leave_sheet();
    ui_pick_sheet(E.title, E.note, E.choices, WORD_ACTIONS, -1, word_action);
    ui_pick_sheet_on_cancel(back_to_editor);
    editor_own();
    if (i == 0u) {
        ui_pick_sheet_dim(WORD_EARLIER);
    }
    if (i + 1u >= E.n) {
        ui_pick_sheet_dim(WORD_LATER);
    }
}

/* -- adding a word -- */

static void word_picked(int32_t index)
{
    if (E.active && index >= 0 && (size_t)index < VOICE_CHOICES && E.pick_ids[index] != 0u &&
        E.n < VP_RECIPE_MAX) {
        E.ids[E.n++] = E.pick_ids[index];
        E.msg = MSG_NONE;
    }
    back_to_editor();
}

/* The first character of a word, as its heading ("B"): ASCII letters upper-cased. */
static void letter_of(const char *w, char *out, size_t len)
{
    size_t l = 1;
    const unsigned char c = (unsigned char)w[0];
    if (c >= 0xF0u) {
        l = 4;
    } else if (c >= 0xE0u) {
        l = 3;
    } else if (c >= 0xC0u) {
        l = 2;
    }
    if (l >= len) {
        l = len - 1u;
    }
    memcpy(out, w, l);
    out[l] = '\0';
    if (out[0] >= 'a' && out[0] <= 'z') {
        out[0] = (char)(out[0] - 'a' + 'A');
    }
}

static void open_group(uint8_t group)
{
    if (!voice_ready() || group < 1u || group > 4u) {
        back_to_editor();
        return;
    }
    E.group = group;
    size_t n = 0;
    size_t letters = 0;
    vp_word_page_t wp;
    for (uint16_t page = 0; n < VOICE_CHOICES; page++) {
        if (!vp_list_words(E.pack, group, NULL, page, &wp) || wp.count == 0u) {
            break;
        }
        for (uint8_t k = 0; k < wp.count && n + 2u <= VOICE_CHOICES; k++) {
            const char *w = word_text(E.pack, wp.ids[k]);
            if (w == NULL) {
                continue;
            }
            char letter[8];
            letter_of(w, letter, sizeof(letter));
            if ((letters == 0u || strcmp(E.letters[letters - 1u], letter) != 0) &&
                letters < sizeof(E.letters) / sizeof(E.letters[0])) {
                snprintf(E.letters[letters], sizeof(E.letters[0]), "%s", letter);
                E.pick_ids[n] = 0;
                E.choices[n++] = (ui_choice_t){UI_CHOICE_HEADING, ui_tone_blue(), E.letters[letters], NULL};
                letters++;
            }
            E.pick_ids[n] = wp.ids[k];
            E.choices[n++] = (ui_choice_t){NULL, ui_tone_blue(), w, NULL};
        }
        if ((uint16_t)(page + 1u) >= wp.pages) {
            break;
        }
    }
    language_line(E.note, sizeof(E.note), E.locale);
    editor_leave_sheet();
    ui_pick_sheet(TR_C("word group", GROUP_TEXT[group - 1u]), E.note, E.choices, n, -1, word_picked);
    ui_pick_sheet_on_cancel(back_to_editor);
    editor_own();
}

static void show_matches(void);

static void match_picked(int32_t index)
{
    if (!E.active || index < 0 || (size_t)index >= VOICE_CHOICES) {
        back_to_editor();
        return;
    }
    const uint16_t id = E.pick_ids[index];
    if (id == 0xFFFEu) { /* Previous page */
        E.page = E.page > 0u ? (uint16_t)(E.page - 1u) : 0u;
        show_matches();
    } else if (id == 0xFFFFu) { /* Next page */
        E.page++;
        show_matches();
    } else {
        word_picked(index);
    }
}

/* Find a word's matches (D03, D09): a page of 64, Previous and Next as its last rows. */
static void show_matches(void)
{
    if (!voice_ready()) {
        back_to_editor();
        return;
    }
    vp_word_page_t wp;
    if (!vp_list_words(E.pack, 0, E.query, E.page, &wp)) {
        back_to_editor();
        return;
    }
    size_t n = 0;
    for (uint8_t k = 0; k < wp.count; k++) {
        vp_clip_t clip;
        if (!vp_find(E.pack, wp.ids[k], &clip)) {
            continue;
        }
        const char *group = clip.category >= 1u && clip.category <= 4u
                                ? TR_C("word group", GROUP_TEXT[clip.category - 1u])
                                : NULL;
        E.pick_ids[n] = wp.ids[k];
        E.choices[n++] = (ui_choice_t){NULL, ui_tone_blue(), clip.text, group};
    }
    const bool paged = wp.pages > 1u;
    int32_t prev = -1;
    int32_t next = -1;
    if (paged) {
        prev = (int32_t)n;
        E.pick_ids[n] = 0xFFFEu;
        /* TR: Find a word, the last two rows of a list longer than a page. At most 47 bytes each */
        E.choices[n++] = (ui_choice_t){UI_ICON_CHEVRON_LEFT, ui_tone_blue(), TR("Previous page"), NULL};
        next = (int32_t)n;
        E.pick_ids[n] = 0xFFFFu;
        /* TR: Find a word, the last row of a list longer than a page. At most 47 bytes */
        E.choices[n++] = (ui_choice_t){UI_ICON_CHEVRON_RIGHT, ui_tone_blue(), TR("Next page"), NULL};
    }
    const bool all = E.query[0] == '\0';
    if (wp.total == 0u) {
        /* TR: Find a word, the note when nothing matches; {0} is what was typed (up to 31
         * bytes). At most 120 bytes */
        I18N_FORMAT(E.note, sizeof(E.note), TR("No words match \"{0}\"."), E.query);
    } else if (all && paged) {
        /* TR: Find a word with nothing typed: every word, a page at a time; {0} the words on
         * this page, {1} all of them, {2} the page, {3} the pages. At most 120 bytes */
        I18N_FORMAT(E.note, sizeof(E.note), TR("All words: {0} of {1}, page {2} of {3}"), (unsigned)wp.count,
                    (unsigned)wp.total, (unsigned)(E.page + 1u), (unsigned)wp.pages);
    } else if (all) {
        /* TR: Find a word with nothing typed: every word; {0} how many. At most 120 bytes */
        I18N_FORMAT(E.note, sizeof(E.note), TRN("All words: {0}", "All words: {0}", wp.total),
                    (unsigned)wp.total);
    } else if (paged) {
        /* TR: Find a word: {0} what was typed (up to 31 bytes), {1} the matches on this page,
         * {2} all of them, {3} the page, {4} the pages. At most 120 bytes */
        I18N_FORMAT(E.note, sizeof(E.note),
                    TRN("{0}: {1} of {2} match, page {3} of {4}", "{0}: {1} of {2} matches, page {3} of {4}",
                        wp.total),
                    E.query, (unsigned)wp.count, (unsigned)wp.total, (unsigned)(E.page + 1u), (unsigned)wp.pages);
    } else {
        /* TR: Find a word: {0} what was typed (up to 31 bytes), {1} how many words match. At most
         * 120 bytes */
        I18N_FORMAT(E.note, sizeof(E.note), TRN("{0}: {1} match", "{0}: {1} matches", wp.total), E.query,
                    (unsigned)wp.total);
    }
    editor_leave_sheet();
    ui_pick_sheet(TR("Find a word"), E.note, E.choices, n, -1, match_picked);
    ui_pick_sheet_on_cancel(back_to_editor);
    editor_own();
    if (paged && E.page == 0u) {
        ui_pick_sheet_dim(prev);
    }
    if (paged && (uint16_t)(E.page + 1u) >= wp.pages) {
        ui_pick_sheet_dim(next);
    }
}

static void query_done(const char *text)
{
    if (!E.active) {
        return;
    }
    snprintf(E.query, sizeof(E.query), "%s", text);
    E.page = 0;
    show_matches();
}

static void find_word(void)
{
    editor_leave_sheet();
    ui_text_sheet_kind(TR("Find a word"), TR("Enter the start of a word, then tap Search."), E.query,
                       UI_TEXT_PLAIN, (uint32_t)sizeof(E.query) - 1u, query_done);
    /* TR: the button that searches, on Find a word's keyboard. At most 31 bytes */
    ui_text_sheet_ok(TR("Search"));
    ui_text_sheet_on_cancel(back_to_editor);
    editor_own();
}

enum { ADD_FIND = 0, ADD_GROUPS };

static void add_picked(int32_t index)
{
    if (!E.active) {
        return;
    }
    if (index == ADD_FIND) {
        find_word();
    } else if (index >= ADD_GROUPS && index < ADD_GROUPS + 4) {
        open_group((uint8_t)(index - ADD_GROUPS + 1));
    } else {
        back_to_editor();
    }
}

/* Three of a group's words, from the pack: the en-US voice's examples, else its first three. */
static void group_examples(uint8_t group, char *buf, size_t len)
{
    buf[0] = '\0';
    vp_name_match_t m;
    const char *w[5] = {NULL, NULL, NULL, NULL, NULL};
    size_t n = 0;
    if (vp_match_name(E.pack, GROUP_EXAMPLES[group - 1u], &m)) {
        for (uint8_t k = 0; k < m.count && n < 5u; k++) {
            vp_clip_t clip;
            if (vp_find(E.pack, m.ids[k], &clip) && clip.category == group) {
                w[n++] = clip.text;
            }
        }
    }
    if (group == VP_CAT_NUMBER && n == 5u) {
        /* TR: Add word, the line under Numbers: a range of number words and three more, as the
         * voice says them ("One to Nine, Ten, Twenty, Hundred..."). At most 95 bytes */
        I18N_FORMAT(buf, len, TR("{0} to {1}, {2}, {3}, {4}..."), w[0], w[1], w[2], w[3], w[4]);
        return;
    }
    if (n != 3u) {
        vp_word_page_t wp;
        n = 0;
        if (vp_list_words(E.pack, group, NULL, 0, &wp)) {
            for (uint8_t k = 0; k < wp.count && n < 3u; k++) {
                w[n] = word_text(E.pack, wp.ids[k]);
                n += w[n] != NULL;
            }
        }
    }
    if (n == 3u) {
        /* TR: Add word, the line under a group: three of its words, as the voice says them
         * ("Front, Back, Upstairs..."). At most 95 bytes */
        I18N_FORMAT(buf, len, TR("{0}, {1}, {2}..."), w[0], w[1], w[2]);
    }
}

static void on_add(lv_event_t *e)
{
    (void)e;
    if (!E.active || !voice_ready() || E.n >= VP_RECIPE_MAX) {
        return;
    }
    /* TR: Add word, the first row: search for a word by its first letters. At most 47 bytes */
    E.choices[ADD_FIND] = (ui_choice_t){UI_ICON_SEARCH, ui_tone_blue(), TR("Find a word"), NULL};
    static const char *const glyphs[4] = {UI_ICON_COMPASS, UI_ICON_HOUSE, UI_ICON_PACKAGE, UI_ICON_HASH};
    for (uint8_t g = 1; g <= 4u; g++) {
        group_examples(g, E.details[g - 1u], sizeof(E.details[0]));
        E.choices[ADD_GROUPS + g - 1u] = (ui_choice_t){glyphs[g - 1u], ui_tone_blue(),
                                                       TR_C("word group", GROUP_TEXT[g - 1u]),
                                                       E.details[g - 1u][0] != '\0' ? E.details[g - 1u] : NULL};
    }
    language_line(E.note, sizeof(E.note), E.locale);
    editor_leave_sheet();
    ui_pick_sheet(TR("Add word"), E.note, E.choices, ADD_GROUPS + 4u, -1, add_picked);
    ui_pick_sheet_on_cancel(back_to_editor);
    editor_own();
}

/* -- Use zone name, Preview -- */

/* The unmatched parts of the name, each as it was typed ("2nd", "2nd, B2"). */
static void unmatched_text(const vp_name_match_t *m)
{
    E.said[0] = '\0';
    size_t used = 0;
    const size_t name_len = strlen(E.name);
    for (uint8_t k = 0; k < m->span_count; k++) {
        const vp_span_t *sp = &m->span[k];
        if (sp->start >= name_len) {
            break;
        }
        const size_t l = sp->start + sp->length <= name_len ? sp->length : name_len - sp->start;
        char part[ALARM_NAME_LEN + 8];
        snprintf(part, sizeof(part), "%.*s", (int)l, E.name + sp->start);
        char joined[sizeof(E.said)];
        if (used == 0u) {
            snprintf(joined, sizeof(joined), "%s", part);
        } else {
            /* TR: the spoken-name editor: two parts of a zone's name the panel can't say, joined
             * ("2nd, B2"). At most 60 bytes */
            I18N_FORMAT(joined, sizeof(joined), TR("{0}, {1}"), E.said, part);
        }
        i18n_utf8_copy(E.said, sizeof(E.said), joined);
        used = strlen(E.said);
    }
}

static void on_use_name(lv_event_t *e)
{
    (void)e;
    if (!E.active || !voice_ready()) {
        return;
    }
    vp_name_match_t m;
    if (vp_match_name(E.pack, E.name, &m)) {
        set_draft(m.ids, m.count); /* only the draft, until Done */
    } else if (m.result == VP_MATCH_TOO_MANY || m.result == VP_MATCH_TOO_LONG) {
        E.msg = MSG_LONG; /* the draft stays as it is */
    } else if (m.result == VP_MATCH_UNMATCHED && m.span_count > 0u) {
        unmatched_text(&m);
        E.msg = MSG_UNMATCHED;
    } else {
        i18n_utf8_copy(E.said, sizeof(E.said), E.name); /* nothing in it can be said */
        E.msg = MSG_UNMATCHED;
    }
    editor_show();
}

static void on_preview(lv_event_t *e)
{
    (void)e;
    vp_recipe_check_t chk;
    if (E.active && draft_check(&chk)) {
        preview_words(E.ids, E.n, PREVIEW_EDITOR);
    }
}

/* -- the sheet -- */

static lv_obj_t *action_row(lv_obj_t *body, const char *glyph, const char *text, lv_event_cb_t cb,
                            bool enabled)
{
    lv_obj_t *row = row_button_create(body, glyph, text, ui_tone_blue());
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
    if (!enabled) {
        ui_row_disable(row);
    }
    return row;
}

/* The editor from its draft (D03; drawings bl-202-editor-*): the words in order, Add word, Use
 * zone name, Preview, why any of them can't be used, and the voice's language. */
static void editor_show(void)
{
    ui_sheet_owner(NULL); /* the sheet it replaces is the editor's own */
    /* TR: the spoken-name editor's title (a sheet over the zone page). At most 47 bytes */
    lv_obj_t *body = ui_form_sheet_open(TR("Spoken name"), E.name, editor_done);
    if (body == NULL) {
        editor_end();
        return;
    }
    editor_own();
    const bool ready = voice_ready();
    const bool full = E.n >= VP_RECIPE_MAX;
    const bool too_long = draft_too_long();
    /* TR: the spoken-name editor, the heading over the words. At most 63 bytes */
    ui_section(body, TR("Words, in the order they are said"));
    if (E.n == 0u) {
        /* TR: the spoken-name editor before it has words. At most 127 bytes */
        ui_paragraph(body, TR("No words yet. Add words, or use the zone name."), ui_tone_gray());
    }
    for (uint8_t i = 0; i < E.n; i++) {
        const char *w = word_text(E.pack, E.ids[i]);
        char fallback[48];
        if (w == NULL) {
            /* TR: the spoken-name editor: a word whose recording isn't available now; {0} is its
             * place (1-8). At most 40 bytes */
            I18N_FORMAT(fallback, sizeof(fallback), TR("Word {0}"), (unsigned)(i + 1u));
            w = fallback;
        }
        char place[8];
        snprintf(place, sizeof(place), "%u", (unsigned)(i + 1u));
        lv_obj_t *row = list_row_create(body, UI_ICON_AUDIO_LINES, ui_tone_blue(), w, "", place,
                                        UI_ICON_CHEVRON_RIGHT);
        lv_obj_set_flag(ui_find(row, "detail_label"), LV_OBJ_FLAG_HIDDEN, true);
        lv_obj_set_flag(ui_find(row, "level"), LV_OBJ_FLAG_HIDDEN, true);
        lv_obj_add_event_cb(row, on_word, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
    /* TR: the spoken-name editor's buttons. At most 47 bytes each */
    action_row(body, UI_ICON_PLUS, TR("Add word"), on_add, ready && !full);
    if (full) {
        /* TR: the spoken-name editor under a dimmed Add word, with eight words. At most 95 bytes */
        ui_paragraph(body, TR("Eight words is the most."), ui_tone_gray());
    }
    /* TR: the spoken-name editor, the heading over Use zone name: another way to choose the
     * words. At most 31 bytes */
    ui_section(body, TR_C("spoken name editor", "Or"));
    /* TR: the spoken-name editor's button that makes the words from the zone's name. At most 47
     * bytes */
    action_row(body, UI_ICON_TEXT_CURSOR, TR("Use zone name"), on_use_name, ready);
    if (E.msg == MSG_UNMATCHED) {
        char text[UI_SHEET_NOTE_LENGTH];
        /* TR: the spoken-name editor after Use zone name: a part of the zone's name has no
         * recorded word; {0} is that part (up to 60 bytes). At most 127 bytes */
        I18N_FORMAT(text, sizeof(text),
                    TR("The panel can’t say \"{0}\". The words stay as they are. Choose words yourself, or "
                       "rename the zone."),
                    E.said);
        ui_paragraph(body, text, ui_tone_amber());
    }
    action_row(body, UI_ICON_VOLUME, TR_C("voice", "Preview"), on_preview,
               ready && E.n > 0u && !too_long);
    if (E.state == UI_VOICE_NONE) {
        /* TR: the spoken-name editor without a voice pack: why Add word, Use zone name and Preview
         * can't be used. At most 95 bytes */
        ui_paragraph(body, TR("Voice pack not installed."), ui_tone_amber());
    } else if (!ready) {
        /* TR: the spoken-name editor and Screen and sound > Announcements, while the voice pack
         * is installed but can't be used. At most 95 bytes */
        ui_paragraph(body, TR("Voice is unavailable."), ui_tone_amber());
    }
    if (too_long || E.msg == MSG_LONG) {
        /* TR: the spoken-name editor: the words take more than eight words or 10 seconds to say.
         * At most 95 bytes */
        ui_paragraph(body, TR("This name is too long to say. Remove a word."), ui_tone_amber());
    }
    char lang[96];
    language_line(lang, sizeof(lang), E.locale);
    ui_paragraph(body, lang, ui_tone_gray());
    ui_form_sheet_fit();
}

static void open_editor(void *ctx)
{
    const uint16_t zone = (uint16_t)(uintptr_t)ctx;
    const alarm_zone_config_t *zc = ui_installer_zone_draft(zone);
    if (zc == NULL || !ui_voice_present()) {
        return;
    }
    editor_end();
    memset(&E, 0, sizeof(E));
    ui_voice_info_t info;
    ui_voice_info(&info);
    E.state = info.state;
    snprintf(E.locale, sizeof(E.locale), "%s", info.locale);
    E.pack = info.state == UI_VOICE_READY ? S.be.voice_pack_acquire(S.be.ctx) : NULL;
    E.pack_revision = E.pack != NULL ? E.pack->pack_revision : 0u;
    if (E.pack != NULL && E.locale[0] == '\0') {
        snprintf(E.locale, sizeof(E.locale), "%s", E.pack->locale);
    }
    E.zone = zone;
    char name[ALARM_NAME_LEN + 8];
    i18n_utf8_copy(E.name, sizeof(E.name), ui_zone_name(S.edit, zone, name, sizeof(name)));
    const unsigned n = alarm_zone_word_count(zc);
    for (unsigned i = 0; i < n && i < VP_RECIPE_MAX; i++) {
        E.ids[i] = alarm_zone_word(zc, i);
    }
    E.n = (uint8_t)(n < VP_RECIPE_MAX ? n : VP_RECIPE_MAX);
    E.active = true;
    editor_show();
}
