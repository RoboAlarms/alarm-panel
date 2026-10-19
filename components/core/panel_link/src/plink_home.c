/*
 * plink_home.c - Smart Home on keypads, the pieces both sides share (see
 * include/panel_link/plink_home.h; docs/features/11 "The device list", "Device state"; LINK-044,
 * LINK-045, LINK-047).
 *
 * The blob and the records are built with plink_msg's catalog and record codecs, so what the
 * master lists is exactly what a keypad reads; a keypad walks a blob with the same decoder,
 * leniently: a field it doesn't know is skipped inside its section, a row it can't use keeps its
 * index.
 */
#include "panel_link/plink_home.h"

#include <string.h>

#include "alarm/alarm_codec.h" /* alarm_crc32 */

/* The registry's strings fit the wire's, NUL included, so a listed row is never cut but by the
 * encoder's own rules (a name or an area on a whole character). */
_Static_assert(DEV_REF_LEN == PLINK_REF_MAX + 1u, "a registry ref fits DEV_ACT's");
_Static_assert(DEV_NAME_LEN == PLINK_NAME_MAX + 1u, "a registry name fits the catalog's");
_Static_assert(DEV_AREA_LEN == PLINK_AREA_MAX + 1u, "a registry area fits the catalog's");
_Static_assert(DEV_STATE_LEN == PLINK_DEV_WORD_MAX + 1u, "a registry state word fits the record's");
_Static_assert((unsigned)HC_MAX_DEVICES <= PLINK_DEV_MAX, "the registry fits the v1 list");
_Static_assert(PLINK_DEV_BLOB_MAX >= PLINK_DEV_BLOB_HDR + PLINK_DEV_MAX * PLINK_DEV_CAT_MAX,
               "the longest list fits the blob");
/* A record's flags are the registry's facts; a record's modes are the registry's bits. */
_Static_assert(PLINK_DEV_MODE_OFF == DEV_MODE_OFF && PLINK_DEV_MODE_HEAT == DEV_MODE_HEAT &&
                   PLINK_DEV_MODE_COOL == DEV_MODE_COOL && PLINK_DEV_MODE_AUTO == DEV_MODE_AUTO,
               "DEV_ACT's modes are device_source.h's");

/* strlen, at most max. */
static size_t bounded_len(const char *s, size_t max)
{
    size_t n = 0u;
    while (s != NULL && n < max && s[n] != '\0') {
        n++;
    }
    return n;
}

/* in (at most in_cap bytes, NUL or not) into out (cap bytes, NUL-terminated). */
static void copy_str(char *out, size_t cap, const char *in, size_t in_cap)
{
    size_t n = bounded_len(in, in_cap);
    if (n > cap - 1u) {
        n = cap - 1u;
    }
    if (n > 0u) {
        memcpy(out, in, n);
    }
    out[n] = '\0';
}

static void cat_of(const hc_device_t *d, plink_dev_cat_t *out)
{
    memset(out, 0, sizeof(*out));
    out->kind = d->info.kind;
    out->source = d->info.source;
    copy_str(out->ref, sizeof(out->ref), d->info.ref, sizeof(d->info.ref));
    copy_str(out->name, sizeof(out->name), d->info.name, sizeof(d->info.name));
    copy_str(out->area, sizeof(out->area), d->info.area, sizeof(d->info.area));
}

/* ---- the master: from its registry ---------------------------------------------------------- */

bool plink_home_listed(const hc_device_t *d)
{
    if (d == NULL || d->info.ref[0] == '\0' ||
        memchr(d->info.ref, '\0', sizeof(d->info.ref)) == NULL) {
        return false; /* no ref, or one longer than the wire carries */
    }
    plink_dev_cat_t c;
    cat_of(d, &c);
    uint8_t buf[PLINK_DEV_CAT_MAX];
    size_t n = 0u;
    return plink_enc_dev_cat(&c, buf, sizeof(buf), &n);
}

bool plink_home_list_build(const hc_device_t *rows, size_t n, uint8_t *blob, size_t cap,
                           size_t *len, uint32_t *crc)
{
    if (len != NULL) {
        *len = 0u;
    }
    if (crc != NULL) {
        *crc = 0u;
    }
    if (blob == NULL || cap < PLINK_DEV_BLOB_HDR || (rows == NULL && n > 0u)) {
        return false;
    }
    size_t pos = PLINK_DEV_BLOB_HDR;
    uint8_t count = 0u;
    for (size_t i = 0; i < n && count < PLINK_DEV_MAX; i++) {
        if (!plink_home_listed(&rows[i])) {
            continue;
        }
        plink_dev_cat_t c;
        cat_of(&rows[i], &c);
        size_t k = 0u;
        if (!plink_enc_dev_cat(&c, blob + pos, cap - pos, &k)) {
            return false; /* listed, so it encodes: no room */
        }
        pos += k;
        count++;
    }
    blob[0] = PLINK_DEV_CAT_COMPAT;
    blob[1] = count;
    if (len != NULL) {
        *len = pos;
    }
    if (crc != NULL) {
        *crc = plink_home_list_crc(blob, pos);
    }
    return true;
}

void plink_home_rec_of(const hc_device_t *d, plink_dev_rec_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (d == NULL) {
        return;
    }
    out->flags = (uint8_t)((d->info.online ? PLINK_DEVF_ONLINE : 0u) |
                           (d->pending != 0u ? PLINK_DEVF_PENDING : 0u) |
                           (d->failed ? PLINK_DEVF_FAILED : 0u) |
                           (hc_outcome_unknown(d) ? PLINK_DEVF_UNSURE : 0u));
    out->action = d->action;
    out->vflags = d->info.values.flags;
    out->level = d->info.values.level;
    out->modes = d->info.values.modes;
    out->hvac_action = d->info.values.hvac_action;
    out->setpoint = d->info.values.setpoint;
    out->temp = d->info.values.temp;
    out->since = d->info.values.since;
    out->caps = d->info.values.caps;
    copy_str(out->word, sizeof(out->word), d->info.state, sizeof(d->info.state));
}

void plink_home_recs_build(const hc_device_t *rows, size_t n, plink_dev_recs_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; rows != NULL && i < n && out->count < PLINK_DEV_MAX; i++) {
        if (!plink_home_listed(&rows[i])) {
            continue; /* as the blob: index for index */
        }
        plink_dev_rec_t r;
        plink_home_rec_of(&rows[i], &r);
        size_t k = 0u;
        if (plink_enc_dev_rec(&r, out->rec[out->count], PLINK_DEV_REC_MAX, &k)) {
            out->len[out->count] = (uint8_t)k;
        }
        out->count++;
    }
}

/* ---- both sides ----------------------------------------------------------------------------- */

uint32_t plink_home_list_crc(const uint8_t *blob, size_t len)
{
    const uint32_t crc = blob != NULL ? alarm_crc32(0u, blob, len) : 0u;
    return crc != 0u ? crc : 1u;
}

uint32_t plink_home_states_crc(const plink_dev_recs_t *recs)
{
    uint32_t crc = 0u;
    for (unsigned i = 0; recs != NULL && i < recs->count && i < PLINK_DEV_MAX; i++) {
        if (recs->len[i] > 0u) {
            const uint8_t index = (uint8_t)i;
            crc = alarm_crc32(crc, &index, 1u);
            crc = alarm_crc32(crc, recs->rec[i], recs->len[i]);
        }
    }
    return crc;
}

bool plink_home_rec(const plink_dev_recs_t *recs, uint8_t index, plink_dev_rec_t *out)
{
    if (recs == NULL || out == NULL || index >= recs->count || index >= PLINK_DEV_MAX ||
        recs->len[index] == 0u) {
        return false;
    }
    return plink_dec_dev_rec(recs->rec[index], recs->len[index], out, NULL);
}

void plink_home_values_of(const plink_dev_rec_t *rec, dev_values_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->hvac_action = DEV_HVAC_UNKNOWN;
    if (rec == NULL) {
        return;
    }
    out->flags = rec->vflags;
    out->level = rec->level;
    out->modes = rec->modes;
    out->hvac_action = rec->hvac_action;
    out->setpoint = rec->setpoint;
    out->temp = rec->temp;
    out->since = rec->since;
    out->caps = rec->caps;
}

bool plink_home_rec_put(plink_dev_recs_t *recs, uint8_t index, const uint8_t *sec, size_t len)
{
    if (recs == NULL || sec == NULL || index >= recs->count || index >= PLINK_DEV_MAX ||
        len == 0u || len != 1u + (size_t)sec[0] || len > PLINK_DEV_REC_MAX) {
        return false;
    }
    memcpy(recs->rec[index], sec, len);
    recs->len[index] = (uint8_t)len;
    return true;
}

/* ---- a keypad: the blob it pulled ----------------------------------------------------------- */

bool plink_home_list_next(const uint8_t *blob, size_t len, plink_home_walk_t *w, uint8_t *index,
                          plink_dev_cat_t *cat, bool *usable)
{
    if (w == NULL || blob == NULL || len < PLINK_DEV_BLOB_HDR ||
        blob[0] != PLINK_DEV_CAT_COMPAT || blob[1] > PLINK_DEV_MAX || w->index >= blob[1]) {
        return false;
    }
    const size_t pos = w->pos < PLINK_DEV_BLOB_HDR ? PLINK_DEV_BLOB_HDR : w->pos;
    if (pos >= len) {
        return false;
    }
    plink_dev_cat_t c;
    size_t used = 0u;
    const bool ok = plink_dec_dev_cat(blob + pos, len - pos, &c, &used);
    if (used == 0u) {
        return false; /* not whole: the blob is damaged */
    }
    if (index != NULL) {
        *index = w->index;
    }
    if (cat != NULL) {
        *cat = c; /* zeros for a row it can't use */
    }
    if (usable != NULL) {
        *usable = ok;
    }
    w->pos = pos + used;
    w->index++;
    return true;
}

bool plink_home_list_check(const uint8_t *blob, size_t len, uint8_t *count)
{
    if (count != NULL) {
        *count = 0u;
    }
    if (blob == NULL || len < PLINK_DEV_BLOB_HDR || blob[0] != PLINK_DEV_CAT_COMPAT ||
        blob[1] > PLINK_DEV_MAX) {
        return false; /* a newer compat is never guessed at */
    }
    plink_home_walk_t w = {0u, 0u};
    while (plink_home_list_next(blob, len, &w, NULL, NULL, NULL)) {
    }
    if (w.index != blob[1]) {
        return false;
    }
    if (count != NULL) {
        *count = w.index;
    }
    return true;
}

bool plink_home_list_find(const uint8_t *blob, size_t len, const char *ref, uint8_t *index)
{
    if (ref == NULL || ref[0] == '\0') {
        return false;
    }
    plink_home_walk_t w = {0u, 0u};
    plink_dev_cat_t cat;
    uint8_t i = 0u;
    bool usable = false;
    while (plink_home_list_next(blob, len, &w, &i, &cat, &usable)) {
        if (usable && strcmp(cat.ref, ref) == 0) {
            if (index != NULL) {
                *index = i;
            }
            return true;
        }
    }
    return false;
}

uint32_t plink_home_ref_key(const char *ref)
{
    const uint32_t crc = alarm_crc32(0u, ref, bounded_len(ref, PLINK_REF_MAX));
    return crc != 0u ? crc : 1u;
}

void plink_home_list_keys(const uint8_t *blob, size_t len, uint32_t keys[PLINK_DEV_MAX])
{
    if (keys == NULL) {
        return;
    }
    memset(keys, 0, PLINK_DEV_MAX * sizeof(keys[0]));
    plink_home_walk_t w = {0u, 0u};
    plink_dev_cat_t cat;
    uint8_t i = 0u;
    bool usable = false;
    while (plink_home_list_next(blob, len, &w, &i, &cat, &usable)) {
        if (usable) {
            keys[i] = plink_home_ref_key(cat.ref);
        }
    }
}

void plink_home_carry(const uint32_t old_keys[PLINK_DEV_MAX], const plink_dev_recs_t *old,
                      const uint32_t new_keys[PLINK_DEV_MAX], uint8_t new_count,
                      plink_dev_recs_t *out)
{
    if (out == NULL || out == old) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->count = (uint8_t)(new_count <= PLINK_DEV_MAX ? new_count : PLINK_DEV_MAX);
    if (old_keys == NULL || old == NULL || new_keys == NULL) {
        return;
    }
    for (unsigned i = 0; i < out->count; i++) {
        for (unsigned j = 0; new_keys[i] != 0u && j < old->count && j < PLINK_DEV_MAX; j++) {
            if (old_keys[j] == new_keys[i] && old->len[j] > 0u) {
                memcpy(out->rec[i], old->rec[j], old->len[j]);
                out->len[i] = old->len[j];
                break;
            }
        }
    }
}
