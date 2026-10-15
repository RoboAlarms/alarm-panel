/*
 * ui_automations.h - Settings > Automations (docs/features/41, BL-168), private to the UI.
 *
 * Two modules share it, each implementing its half and neither editing this file:
 *   (A) ui_automations.c   the list, an automation's page, the words and looks the editors share,
 *                          and the saves through ui_backend_t's auto_* hooks (U15)
 *   (B) ui_auto_edit.c     the editors: a new automation's steps, changing its trigger, window,
 *                          actions and "Turn it off after", the delete question (U16)
 * Compiled only with ALARM_UI_AUTOMATIONS (sources.cmake); every call from another module sits
 * under #if ALARM_UI_AUTOMATIONS. Everything runs on the LVGL thread.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "automation/automation.h"
#include "ui_internal.h"

#if ALARM_UI_AUTOMATIONS

/* ---- (A) ui_automations.c ------------------------------------------------------------------ */

void ui_automations_init(void);
void ui_automations_tick(void);
void ui_automations_language_changed(void);
/* The backend has the automations (a non-NULL auto_list): the Settings list shows the section
 * (subject_automations_avail). */
bool ui_automations_available(void);
/* PAGE_AUTOMATIONS (the list); PAGE_AUTOMATION with arg = the automation's slot (1..AUTO_MAX). */
void ui_automations_build(page_id_t id, int32_t arg, lv_obj_t *body, char *title, size_t tlen,
                          char *note, size_t nlen);

/* The words an automation is shown with, in the list, on its page and in the editors. */
void ui_auto_when_text(const auto_trigger_t *w, char *out, size_t len);
void ui_auto_then_text(const auto_action_t *a, char *out, size_t len);
void ui_auto_window_text(const auto_trigger_t *w, char *out, size_t len);
void ui_auto_off_after_text(uint16_t minutes, char *out, size_t len);

/* The looks: a device kind's glyph (dev_kind_t; ref for what the kind alone can't tell) and tone,
 * and a trigger's. */
const char *ui_auto_kind_glyph(uint8_t dev_kind, const char *ref);
lv_color_t ui_auto_kind_tone(uint8_t k);
const char *ui_auto_when_glyph(const auto_trigger_t *w);
lv_color_t ui_auto_when_tone(const auto_trigger_t *w);

/* Saves (r->uid 0 adds) and deletes through the backend, the expected gen being the one last
 * read. A refusal beeps and toasts its reason; true when stored. */
bool ui_auto_save(const auto_rule_t *r, uint16_t *uid_out);
/* The store's generation the page on screen was built from: the one ui_auto_save sends, so an
 * editor knows whether a refusal was CHANGED (AUT-008). */
uint16_t ui_auto_page_gen(void);
bool ui_auto_delete(uint8_t slot);

/* ---- (B) ui_auto_edit.c -------------------------------------------------------------------- */

void ui_auto_add_start(void);
void ui_auto_edit_when(uint8_t slot);
void ui_auto_edit_window(uint8_t slot);
void ui_auto_edit_action(uint8_t slot, uint8_t index);
void ui_auto_add_action(uint8_t slot);
void ui_auto_edit_off_after(uint8_t slot, uint8_t index);
void ui_auto_delete_ask(uint8_t slot);
void ui_auto_edit_stop(void); /* the editor's page or sheet is being dropped */

#endif /* ALARM_UI_AUTOMATIONS */
