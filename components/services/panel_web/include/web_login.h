#pragma once
/* Panel Manager sign-in (docs/features/31 "Sign-in"): a user name and a password.
 *
 * Everyone signs in with the name shown on Settings > Users, the installer too ("Installer"
 * from the factory; until 2026-09-29 the installer used the panel ID). The installer, system masters, partition masters and standard
 * users can have one (2026-09-23); arm-only, guest and duress codes can't. Whoever manages a
 * user (alarm_config_user_manages: the installer everyone, a master everyone but the
 * installer) sets it, at the panel or in the Panel Manager as themselves; one set for somebody
 * else is temporary (changed at the first sign-in) unless the person setting it chooses that
 * the other keeps it - at the panel it always is. It is not the keypad code -
 * that stays a short number for the keypad, and a code seen over a shoulder at the door does
 * not open the panel from the network.
 *
 * A password is stored as PBKDF2-HMAC-SHA256 with its own random salt, and is bound to the
 * person it was set for (their slot and their code record): deleting the user, or anyone else
 * taking over the slot, ends it; changing their code or their name does not. A user whose code
 * can no longer sign in (arm-only, guest, duress) loses it. */
#include "alarm/alarm_config.h"
#include "alarm_service.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WEB_PASSWORD_MIN 12u
#define WEB_PASSWORD_MAX 64u

/* Loads the stored sign-ins. Call once, before the UI or the web service use them. */
esp_err_t web_login_init(void);
/* Watches user changes (deleted, code changed, authority changed, programming applied).
 * Register it with alarm_service_add_listener(). */
const alarm_listener_t *web_login_listener(void);

/* This panel's ID: 12 lowercase hex digits (the base MAC), shown on About. */
const char *web_login_panel_id(void);

/* May this user have a browser sign-in at all (the installer, a system or partition master,
 * a standard user)? */
bool web_login_eligible(const alarm_config_t *cfg, uint8_t user);
/* Is a sign-in set up for this user right now? cfg must be freshly read from the alarm
 * service (never a copy kept for a while: a stale code record would look like another
 * person, and the sign-in would be dropped). */
bool web_login_enabled(const alarm_config_t *cfg, uint8_t user);
/* Sets or replaces the password (hashing takes about a second). temporary: set by somebody
 * else, it signs in only to be changed. why explains a refusal. The caller checks who may.
 * true only once flash has it; refused, the password the user had stays theirs. */
bool web_login_set(uint8_t user, const char *password, bool temporary, char *why, size_t why_len);
/* Turns the user's sign-in off. The password stops at once, and so do the sessions it opened
 * (fail closed). false: flash refused the erase for now; it is tried again until flash takes
 * it (web_login_retry() or the next look at the sign-ins after a short wait, or the next
 * call), and the caller
 * reports the failure - a restart before then would bring the password back. */
bool web_login_clear(uint8_t user);
/* Is a flash write still owed (an erase flash refused, a deleted or changed user's sign-in not
 * yet brought in line)? Reads RAM only, any task. */
bool web_login_owed(void);
/* Brings the stored sign-ins in line with the users now, retrying what flash refused before
 * (no more often than its short wait). Writes flash: only on a task that may (an internal RAM
 * stack - the HTTPS worker, the start-up), never on an alarm_mem_task_create stack. panel_web
 * calls it once at start and every few seconds while web_login_owed(), so a refused erase is
 * retried with nobody at the browser (code review S01). */
void web_login_retry(void);

/* A temporary password, made up by the panel (16 random characters, about 79 bits) and
 * written to out (at least 20 bytes). It signs in only to be changed: until then the session
 * can do nothing else (panel_web.c). For someone away from the panel, set from the service
 * console at the owner's request. */
bool web_login_set_temporary(uint8_t user, char *out, size_t out_len, char *why, size_t why_len);
/* Has this user a browser password at all? */
bool web_login_has(uint8_t user);
/* Must this user's password be changed before anything else? */
bool web_login_temporary(uint8_t user);
/* The signed-in user changes their own password: the current one first, then the new one
 * under the same rules as on the panel. Stored only if the sign-in is still the one whose
 * current password was checked. false: why says why; *wrong_current tells a wrong current
 * password (counted by the sign-in gate) from a refused new one. fingerprint: the new sign-in's
 * (web_login_fingerprint()), for the session that changed it; all zero when false. */
bool web_login_change(uint8_t user, const char *current, const char *next, bool *wrong_current,
                      uint8_t fingerprint[32], char *why, size_t why_len);
/* Was why (from web_login_set() or web_login_change()) the panel's own trouble - memory, a busy
 * configuration, a flash that refused - rather than the person's? Answered 503, not 400. */
bool web_login_why_busy(const char *why);

/* Is this the signed-in user's own password (the recent password panel_web.c asks for before
 * a guarded change)? The same check as web_login_check(), as long, with no limiter inside: the
 * caller counts wrong ones. HTTP worker. */
bool web_login_verify(uint8_t user, const char *password);
/* Checks a sign-in; true with *user and verified, the fingerprint of the sign-in that matched
 * (for web_login_fingerprint_verified()). A wrong name and a wrong password take as long and
 * answer the same; a sign-in turned off or replaced during the check doesn't sign in. HTTP
 * worker. */
bool web_login_check(const char *username, const char *password, uint8_t *user,
                     uint8_t verified[32]);
/* What a signed-in session depends on besides the user record: changes when the password is
 * set, replaced or turned off, or when the user it belonged to is gone. cfg: the current
 * configuration. false (out all zero): the user has no sign-in now. HTTP worker. */
bool web_login_fingerprint(const alarm_config_t *cfg, uint8_t user, uint8_t out[32]);
/* web_login_fingerprint() for a session a password opens: true only while the user still has
 * the very sign-in web_login_check() or web_login_change() gave verified for; else false and out
 * all zero, and no session may open - one turned off or replaced in between (by the touchscreen
 * or the console) would otherwise leave a session that outlives it (code review S01). */
bool web_login_fingerprint_verified(const alarm_config_t *cfg, uint8_t user,
                                    const uint8_t verified[32], uint8_t out[32]);
