/*
 * backup_service.h - the panel's backup file on the panel (docs/features/14 CFG-005, CFG-006):
 * making one from NVS, and restoring one onto this panel or a replacement board.
 *
 * The file's format, its encryption and what it holds are the portable components/core/backup;
 * this is the ESP-IDF half: NVS read and written through that component's policy, and its
 * crypto port on PSA (PBKDF2-HMAC-SHA256 and AES-256-GCM). The Panel Manager calls it for the
 * installer (components/services/panel_web).
 */
#ifndef BACKUP_SERVICE_H
#define BACKUP_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_types.h"
#include "backup/backup_check.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* This panel's kind (board_info()->id, a string that lives forever): a file made on another
 * kind is refused. Call once at start-up. */
void backup_service_set_board(const char *board_id);

/* A backup of this panel, encrypted with password (backup_password_ok). On ESP_OK *file is a
 * heap buffer of *len bytes the caller frees with free(). Logged as ALARM_EV_BACKUP_MADE by
 * src/user. *why says what went wrong otherwise (BACKUP_ERR_PASSWORD, ...); ESP_FAIL with
 * BACKUP_ERR_CONTENT: a namespace the policy keeps couldn't be read (logged by name), and no
 * file is made rather than one without it. */
esp_err_t backup_service_make(const char *password, const alarm_source_t *src, uint8_t user,
                              uint8_t **file, size_t *len, backup_result_t *why);

/* Restores file onto this panel: every partition disarmed and quiet, the file decrypted and
 * checked in full (backup_check) before anything is written; then ALARM_EV_BACKUP_RESTORED is
 * logged, the entries the policy keeps are replaced by the file's, and the panel restarts
 * about two seconds later (the caller has time to answer). ESP_ERR_INVALID_STATE: something is
 * armed or sounding; ESP_ERR_INVALID_ARG: the file was refused (*why, and *summary for what
 * it held); ESP_FAIL: writing failed part way (the panel restarts anyway: restore again).
 * summary may be NULL. */
esp_err_t backup_service_restore(const uint8_t *file, size_t len, const char *password,
                                 const alarm_source_t *src, uint8_t user,
                                 backup_summary_t *summary, backup_result_t *why);

/* The PSA port against the frozen format-1 file (components/core/backup/test): true when this
 * panel opens it. The console's `backup check`. */
bool backup_service_self_test(void);

esp_err_t backup_service_console_register(void);

#ifdef __cplusplus
}
#endif

#endif /* BACKUP_SERVICE_H */
