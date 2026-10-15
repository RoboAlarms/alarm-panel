/*
 * backup_check.c - a decrypted backup checked in full (backup_check.h).
 */
#include "backup/backup_check.h"

#include <string.h>

#include "alarm/alarm_codec.h"

static bool is(const backup_entry_t *e, const char *ns, const char *key)
{
    return strcmp(e->ns, ns) == 0 && strcmp(e->key, key) == 0;
}

/* The same namespace and key again after the reader's position: a file that says two things
 * about one entry is not one this panel restores. */
static bool repeated(const backup_reader_t *at, const backup_entry_t *e)
{
    backup_reader_t r = *at;
    for (;;) {
        backup_entry_t other;
        bool done;
        if (backup_reader_next(&r, &other, &done) != BACKUP_OK) {
            return true;
        }
        if (done) {
            return false;
        }
        if (strcmp(other.ns, e->ns) == 0 && strcmp(other.key, e->key) == 0) {
            return true;
        }
    }
}

static backup_result_t refuse(backup_summary_t *s, const backup_entry_t *e)
{
    memcpy(s->bad_ns, e->ns, sizeof(s->bad_ns));
    memcpy(s->bad_key, e->key, sizeof(s->bad_key));
    return BACKUP_ERR_CONTENT;
}

backup_result_t backup_check(const uint8_t *payload, size_t len, const char *board,
                             backup_scratch_t *scratch, backup_summary_t *summary)
{
    if (payload == NULL || scratch == NULL || summary == NULL) {
        return BACKUP_ERR_ARG;
    }
    memset(summary, 0, sizeof(*summary));
    backup_reader_t r;
    backup_result_t res = backup_reader_init(&r, payload, len, &summary->meta);
    if (res != BACKUP_OK) {
        return res;
    }
    if (board != NULL && strcmp(board, summary->meta.board) != 0) {
        return BACKUP_ERR_BOARD;
    }
    bool config = false;
    for (;;) {
        backup_entry_t e;
        bool done;
        res = backup_reader_next(&r, &e, &done);
        if (res != BACKUP_OK) {
            return res;
        }
        if (done) {
            break;
        }
        summary->entries++;
        /* A retired entry (an older firmware's) is accepted and left out of the restore. */
        if ((!backup_policy_keeps(e.ns, e.key) && !backup_policy_retired(e.ns, e.key)) ||
            repeated(&r, &e)) {
            return refuse(summary, &e);
        }
        if (is(&e, "alarm", "config")) {
            if (e.type != BACKUP_T_BLOB ||
                alarm_config_decode(e.value, e.len, &scratch->cfg) != ALARM_OK) {
                return refuse(summary, &e);
            }
            config = true;
            for (size_t i = 0; i < ALARM_MAX_ZONES; i++) {
                if (scratch->cfg.zones[i].type != ALARM_ZT_NOT_USED) {
                    summary->zones++;
                }
            }
            for (size_t i = 0; i < ALARM_MAX_USERS; i++) {
                if (scratch->cfg.users[i].authority != ALARM_AUTH_NONE) {
                    summary->users++;
                }
            }
        } else if (is(&e, "zones", "bind")) {
            zone_hub_init(&scratch->hub, NULL);
            if (e.type != BACKUP_T_BLOB ||
                zone_hub_decode(&scratch->hub, e.value, e.len) != ZONE_HUB_OK) {
                return refuse(summary, &e);
            }
            summary->bindings = true;
        } else if (is(&e, "net", "ssid")) {
            summary->network = e.type == BACKUP_T_STR && e.len > 0u;
        } else if (is(&e, "zwave", "keys")) {
            summary->zwave = e.type == BACKUP_T_BLOB && e.len > 0u;
        }
    }
    /* A backup always carries the configuration: without it there is nothing to rebuild. */
    return config ? BACKUP_OK : BACKUP_ERR_CONTENT;
}

backup_result_t backup_restore_config(const uint8_t *payload, size_t len,
                                      const alarm_config_t *current, backup_scratch_t *scratch,
                                      uint8_t *out, size_t cap, size_t *out_len)
{
    if (payload == NULL || current == NULL || scratch == NULL || out == NULL ||
        out_len == NULL) {
        return BACKUP_ERR_ARG;
    }
    *out_len = 0u;
    backup_reader_t r;
    backup_meta_t meta;
    backup_result_t res = backup_reader_init(&r, payload, len, &meta);
    if (res != BACKUP_OK) {
        return res;
    }
    for (;;) {
        backup_entry_t e;
        bool done;
        res = backup_reader_next(&r, &e, &done);
        if (res != BACKUP_OK) {
            return res;
        }
        if (done) {
            return BACKUP_ERR_CONTENT; /* no configuration in the file */
        }
        if (!is(&e, "alarm", "config")) {
            continue;
        }
        if (e.type != BACKUP_T_BLOB ||
            alarm_config_decode(e.value, e.len, &scratch->cfg) != ALARM_OK) {
            return BACKUP_ERR_CONTENT;
        }
        alarm_config_voice_restore(current, &scratch->cfg);
        size_t n = 0u;
        if (alarm_config_encode(&scratch->cfg, out, cap, &n) != ALARM_OK) {
            return BACKUP_ERR_ARG;
        }
        *out_len = n;
        return BACKUP_OK;
    }
}
