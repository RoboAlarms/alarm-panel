/*
 * backup_store.c - the panel's entries read for a backup and listed for a restore, through a
 * port (backup.h "the panel's entries"; docs/features/14 CFG-005, CFG-006).
 *
 * The one rule (S04): only "nothing there" is absence. A namespace that can't be opened, a
 * listing that fails part way and an entry that can't be read all fail the whole step, so a
 * backup never leaves out what it couldn't read and a restore never takes a namespace as
 * cleared when it couldn't list it.
 */
#include "backup/backup.h"

#include <string.h>

/* A namespace or key, cut to BACKUP_NAME_MAX (dst is zeroed already). */
static void copy_name(char dst[BACKUP_NAME_MAX + 1], const char *src)
{
    for (size_t i = 0; i < BACKUP_NAME_MAX && src[i] != 0; i++) {
        dst[i] = src[i];
    }
}

static bool port_ok(const backup_store_t *s)
{
    return s != NULL && s->first != NULL && s->next != NULL && s->end != NULL;
}

backup_store_status_t backup_collect(backup_writer_t *w, const backup_store_t *s, const char *ns,
                                     uint8_t *value, size_t cap, bool *fits)
{
    if (fits != NULL) {
        *fits = true;
    }
    if (w == NULL || !port_ok(s) || s->open == NULL || s->read == NULL || s->close == NULL ||
        ns == NULL || value == NULL || fits == NULL) {
        return BACKUP_STORE_FAILED;
    }
    backup_store_status_t st = s->open(s->ctx, ns);
    if (st == BACKUP_STORE_ABSENT) {
        return BACKUP_STORE_OK; /* nothing stored under it on this panel */
    }
    if (st != BACKUP_STORE_OK) {
        return BACKUP_STORE_FAILED;
    }
    char key[BACKUP_NAME_MAX + 1];
    uint8_t type = 0u;
    memset(key, 0, sizeof(key));
    st = s->first(s->ctx, ns, key, &type);
    while (st == BACKUP_STORE_OK) {
        key[BACKUP_NAME_MAX] = '\0';
        if (type != 0u && backup_policy_keeps(ns, key)) {
            uint32_t len = 0u;
            if (s->read(s->ctx, key, type, value, cap, &len) != BACKUP_STORE_OK) {
                st = BACKUP_STORE_FAILED;
                break;
            }
            backup_entry_t e;
            memset(&e, 0, sizeof(e));
            copy_name(e.ns, ns);
            copy_name(e.key, key);
            e.type = type;
            e.value = value;
            e.len = len;
            if (!backup_writer_add(w, &e)) {
                *fits = false;
                break;
            }
        }
        type = 0u;
        st = s->next(s->ctx, key, &type);
    }
    s->end(s->ctx);
    s->close(s->ctx);
    if (!*fits) {
        return BACKUP_STORE_OK; /* the writer refuses to finish: the caller says why */
    }
    return st == BACKUP_STORE_ABSENT ? BACKUP_STORE_OK : BACKUP_STORE_FAILED;
}

backup_store_status_t backup_erasable(const backup_store_t *s, const char *ns,
                                      char (*keys)[BACKUP_NAME_MAX + 1], size_t max, size_t *n)
{
    if (n != NULL) {
        *n = 0u;
    }
    if (!port_ok(s) || ns == NULL || keys == NULL || n == NULL || max == 0u) {
        return BACKUP_STORE_FAILED;
    }
    char key[BACKUP_NAME_MAX + 1];
    uint8_t type = 0u;
    memset(key, 0, sizeof(key));
    backup_store_status_t st = s->first(s->ctx, ns, key, &type);
    while (st == BACKUP_STORE_OK && *n < max) {
        key[BACKUP_NAME_MAX] = '\0';
        const bool config = strcmp(ns, "alarm") == 0 && strcmp(key, "config") == 0;
        if (backup_policy_keeps(ns, key) && !config) {
            memcpy(keys[*n], key, sizeof(key));
            (*n)++;
        }
        st = s->next(s->ctx, key, &type);
    }
    s->end(s->ctx);
    if (st == BACKUP_STORE_FAILED) {
        *n = 0u; /* nothing is taken as cleared from a listing that failed */
        return BACKUP_STORE_FAILED;
    }
    return BACKUP_STORE_OK; /* ABSENT: the end; OK: max reached, the caller asks again */
}
