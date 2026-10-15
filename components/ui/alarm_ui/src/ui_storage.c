/* About > SD card (BL-131). All IO runs in the backend; this module only reads snapshots.
 * The page shows what the card's state allows: with no card, its state and "Check for a card";
 * with one, Safely remove, its details and Format last and apart. It is rebuilt when the card's
 * state changes (the shape of the page follows it). */
#include <stdio.h>
#include <string.h>
#include "ui_internal.h"

static storage_sd_t s_shown;
static lv_obj_t *s_status; /* the open page's status row, NULL when none */
static uint32_t s_polled, s_confirm_generation;

static void read_sd(storage_sd_t *st)
{
    memset(st, 0, sizeof(*st));
    st->state = STORAGE_ERROR;
    /* TR: About > SD card, when the panel has no SD storage. At most 95 bytes */
    i18n_utf8_copy(st->message, sizeof(st->message), TR("SD storage isn't available on this panel."));
    if (S.be.storage_sd) S.be.storage_sd(S.be.ctx, st);
}
static bool authorized(void)
{ return ui_settings_allowed(ACCESS_SYSTEM) && S.auth_user != 0u; }
static void enabled(lv_obj_t *row, bool on)
{
    if (!row) return;
    if (on) lv_obj_remove_state(row, LV_STATE_DISABLED);
    else lv_obj_add_state(row, LV_STATE_DISABLED);
}

/* "44.7 GB free of 59.6 GB", when the card's use is known. */
static bool free_of(const storage_sd_t *sd, char *out, size_t len)
{
    if (sd->state != STORAGE_READY || !sd->usage_valid || sd->total_bytes == 0u) return false;
    char free_text[32], total_text[32];
    ui_size_text(free_text, sizeof(free_text), sd->free_bytes);
    ui_size_text(total_text, sizeof(total_text), sd->total_bytes);
    /* TR: About > SD card, how much room is left; {0} is the free space and {1} the card's size,
     * for example "44.7 GB free of 59.6 GB". At most 63 bytes */
    I18N_FORMAT(out, len, TR("{0} free of {1}"), free_text, total_text);
    return true;
}

static lv_color_t state_tone(storage_state_t state)
{
    switch (state) {
    case STORAGE_READY: return ui_tone_green();
    case STORAGE_UNSUPPORTED: return ui_tone_amber();
    case STORAGE_ERROR: return ui_tone_red();
    case STORAGE_WORKING: return ui_tone_blue();
    default: return ui_tone_gray();
    }
}

static const char *state_words(storage_state_t state)
{
    return TR_C("storage state", storage_state_name(state));
}

/* About's row: the state as its value, the free space under the title. */
lv_obj_t *ui_storage_about_row(lv_obj_t *body, ui_click_fn fn, void *ctx)
{
    storage_sd_t sd;
    read_sd(&sd);
    char detail[96] = "";
    const bool known = free_of(&sd, detail, sizeof(detail));
    /* TR: Settings > About, the SD card's row. At most 47 bytes */
    return ui_row_link(body, UI_ICON_HARD_DRIVE, ui_tone_violet(), TR("SD card"),
                       known ? detail : NULL, state_words(sd.state), UI_ICON_CHEVRON_RIGHT, fn, ctx);
}

void ui_storage_stop(void)
{
    s_status = NULL;
}
void ui_storage_tick(void)
{
    if (!s_status || lv_tick_elaps(s_polled) < 1000u) return;
    s_polled = lv_tick_get();
    storage_sd_t st;
    read_sd(&st);
    if (memcmp(&st, &s_shown, sizeof(st)) != 0 && lv_subject_get_int(&subject_sheet) == SHEET_NONE &&
        S.pad == PAD_NONE) {
        ui_page_rebuild(); /* the page's shape follows the card: other rows now */
    }
}
static void request(storage_action_t action, uint32_t generation)
{
    storage_sd_t st;
    read_sd(&st);
    if (!storage_allowed(&st, action, generation, authorized(), !S.locked, false) ||
        !S.be.storage_action || !S.be.storage_action(S.be.ctx, action, generation, S.auth_user)) {
        ui_toast(TR("Card changed or busy. Check it and try again"));
    }
    s_polled = lv_tick_get() - 1000u; /* the tick shows the new state at once */
}
static void confirmed(void) { request(STORAGE_FORMAT, s_confirm_generation); }
static void action(void *ctx)
{
    const storage_action_t op = (storage_action_t)(intptr_t)ctx;
    storage_sd_t st;
    read_sd(&st);
    if (op != STORAGE_FORMAT) { request(op, st.generation); return; }
    if (!storage_allowed(&st, op, st.generation, authorized(), !S.locked, false)) return;
    s_confirm_generation = st.generation;
    char size[32], note[128];
    ui_size_text(size, sizeof(size), st.capacity_bytes);
    /* TR: the question before formatting the SD card; {0} is its size, for example "16 GB". At
     * most 96 bytes */
    I18N_FORMAT(note, sizeof(note), TR("Erase ALL files and partitions on this {0} card? This can't be undone. Panel settings stay safe."), size);
    ui_confirm(TR("Format SD card?"), note, TR("Erase and format"), confirmed);
}

static void value_or_dash(lv_obj_t *body, const char *title, const char *value, bool known)
{
    ui_row_value(body, title, known && value[0] != '\0' ? value : TR("Unavailable"));
}

void ui_storage_build(lv_obj_t *body)
{
    read_sd(&s_shown);
    const storage_sd_t *sd = &s_shown;
    s_polled = lv_tick_get();

    /* The state, its words and, for a card in use, its space with a bar. */
    char detail[96];
    if (!free_of(sd, detail, sizeof(detail))) {
        i18n_utf8_copy(detail, sizeof(detail), TR_C("storage", sd->message)); /* panel_storage.c's sentences */
    }
    const lv_color_t tone = state_tone(sd->state);
    s_status = ui_row_link(body, UI_ICON_HARD_DRIVE, tone, state_words(sd->state), detail, NULL, NULL,
                           NULL, NULL);
    if (sd->state == STORAGE_READY && sd->usage_valid && sd->total_bytes != 0u) {
        lv_obj_t *level = ui_find(s_status, "level");
        const uint64_t used = storage_free(sd->total_bytes, sd->free_bytes);
        lv_obj_remove_flag(level, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(level, (int32_t)(used * 100u / sd->total_bytes), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(level, tone, LV_PART_INDICATOR);
    }
    if (sd->state == STORAGE_UNSUPPORTED) {
        ui_paragraph(body, TR("Supports FAT12, FAT16 and FAT32. exFAT isn't supported."), ui_tone_gray());
    }

    const bool card = sd->state == STORAGE_READY || sd->state == STORAGE_UNSUPPORTED ||
                      (sd->state == STORAGE_ERROR && sd->capacity_bytes != 0u);
    const bool look = sd->state == STORAGE_NO_CARD || sd->state == STORAGE_EJECTED ||
                      sd->state == STORAGE_ERROR;
    const bool busy = sd->state == STORAGE_WORKING || sd->state == STORAGE_STARTING;
    const bool access = authorized() && S.be.storage_action != NULL;
    if (authorized()) {
        if (look) {
            lv_obj_t *row = ui_row_action(body, UI_ICON_REFRESH, TR("Check for a card"), ui_tone_blue(),
                                          action, (void *)(intptr_t)STORAGE_CHECK);
            enabled(row, storage_allowed(sd, STORAGE_CHECK, sd->generation, access, !S.locked, false));
        }
        if (card) {
            lv_obj_t *row = ui_row_action(body, UI_ICON_LOG_OUT, TR("Safely remove"), ui_tone_blue(),
                                          action, (void *)(intptr_t)STORAGE_EJECT);
            enabled(row, storage_allowed(sd, STORAGE_EJECT, sd->generation, access, !S.locked, false));
        }
    } else if (!busy) {
        ui_settings_unlock_row(body, ACCESS_SYSTEM, TR("Manage SD card"));
    }
    if (!card) return;

    ui_section(body, TR("Card details"));
    value_or_dash(body, TR_C("sd card detail", "Format"), sd->filesystem, sd->usage_valid);
    value_or_dash(body, TR_C("sd card detail", "Model"), sd->name, sd->name[0] != '\0');
    if (authorized()) {
        ui_section(body, ""); /* a gap: Format sits apart */
        lv_obj_t *row = ui_row_action(body, UI_ICON_TRASH, TR("Format the card"), ui_tone_red(), action,
                                      (void *)(intptr_t)STORAGE_FORMAT);
        enabled(row, storage_allowed(sd, STORAGE_FORMAT, sd->generation, access, !S.locked, false));
        ui_paragraph(body, TR("Erases everything on the card. Keep the power on until it finishes."),
                     ui_tone_gray());
    }
}
