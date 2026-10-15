/*
 * fwsrv.c - talking to a RoboAlarms firmware server: the server's domain and address, the check
 * request and answer, language packs, signing keys and the trust record (see proto_fwsrv.h,
 * docs/features/13 OTA-017..OTA-025).
 */
#include <stdio.h>
#include <string.h>

#include "alarm/alarm_codec.h"
#include "alarm_proto/proto_fwsrv.h"
#include "alarm_proto/proto_link.h" /* proto_sha256 */
#include "fw_shared.h"

/* ---- the server's domain (OTA-017) ------------------------------------------------------- */

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool starts_with_ci(const char *s, size_t n, const char *prefix)
{
    const size_t pn = strlen(prefix);
    if (n < pn) {
        return false;
    }
    for (size_t i = 0; i < pn; i++) {
        if (lower(s[i]) != prefix[i]) {
            return false;
        }
    }
    return true;
}

/* s[0..n) already lower case: labels of [a-z0-9-], 1-63 each, no '-' at either end, at least
 * two, the last letters only. */
static bool domain_valid(const char *s, size_t n)
{
    size_t labels = 0;
    size_t start = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i < n && s[i] != '.') {
            const char c = s[i];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
                return false;
            }
            continue;
        }
        const size_t len = i - start;
        if (len < 1u || len > 63u || s[start] == '-' || s[i - 1u] == '-') {
            return false;
        }
        labels++;
        if (i == n) {
            for (size_t k = start; k < i; k++) {
                if (s[k] < 'a' || s[k] > 'z') {
                    return false; /* the top-level label is letters only */
                }
            }
        }
        start = i + 1u;
    }
    return labels >= 2u;
}

bool proto_fwsrv_domain(const char *typed, char *out, size_t cap)
{
    if (out != NULL && cap > 0u) {
        out[0] = '\0';
    }
    if (typed == NULL || out == NULL || cap == 0u) {
        return false;
    }
    const char *s = typed;
    size_t n = strlen(s);
    while (n > 0u && is_space(*s)) {
        s++;
        n--;
    }
    while (n > 0u && is_space(s[n - 1u])) {
        n--;
    }
    if (starts_with_ci(s, n, "https://")) {
        s += 8;
        n -= 8u;
    } else if (starts_with_ci(s, n, "http://")) {
        s += 7;
        n -= 7u;
    }
    if (n > 0u && s[n - 1u] == '/') {
        n--;
    }
    if (n == 0u || n > PROTO_FWSRV_DOMAIN_MAX - 1u || n >= cap) {
        return false;
    }
    char work[PROTO_FWSRV_DOMAIN_MAX];
    for (size_t i = 0; i < n; i++) {
        work[i] = lower(s[i]);
    }
    work[n] = '\0';
    if (!domain_valid(work, n)) {
        return false;
    }
    memcpy(out, work, n + 1u);
    return true;
}

/* "http://host[:port]" or "https://host[:port]", an optional trailing "/": host of
 * [A-Za-z0-9.-], port 1-65535. Returns the length without the "/", 0 when it isn't one. */
static size_t dev_address(const char *s)
{
    size_t i;
    if (strncmp(s, "http://", 7) == 0) {
        i = 7u;
    } else if (strncmp(s, "https://", 8) == 0) {
        i = 8u;
    } else {
        return 0u;
    }
    const size_t host = i;
    while ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
           (s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == '-') {
        i++;
    }
    if (i == host) {
        return 0u;
    }
    if (s[i] == ':') {
        i++;
        uint32_t port = 0;
        const size_t digits = i;
        while (s[i] >= '0' && s[i] <= '9' && i - digits < 5u) {
            port = port * 10u + (uint32_t)(s[i] - '0');
            i++;
        }
        if (i == digits || port < 1u || port > 65535u) {
            return 0u;
        }
    }
    const size_t len = i;
    if (s[i] == '/') {
        i++;
    }
    return s[i] == '\0' ? len : 0u;
}

bool proto_fwsrv_base_url(const char *server, char *out, size_t cap)
{
    if (out != NULL && cap > 0u) {
        out[0] = '\0';
    }
    if (server == NULL || out == NULL || cap == 0u) {
        return false;
    }
    int n;
    if (strstr(server, "://") != NULL) {
        const size_t len = dev_address(server);
        if (len == 0u) {
            return false;
        }
        n = snprintf(out, cap, "%.*s", (int)len, server);
    } else {
        char domain[PROTO_FWSRV_DOMAIN_MAX];
        if (!proto_fwsrv_domain(server, domain, sizeof(domain))) {
            return false;
        }
        n = snprintf(out, cap, "https://firmware.%s", domain);
    }
    if (n <= 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool proto_fwsrv_channel_valid(const char *channel)
{
    static const char *const names[] = {"stable", "beta", "alpha", "dev"};
    if (channel == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(channel, names[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* ---- the check (OTA-018) ----------------------------------------------------------------- */

static void str_array(pj_writer_t *w, const char *key, const char *const *items, size_t count)
{
    pj_karr(w, key);
    for (size_t i = 0; items != NULL && i < count; i++) {
        pj_str(w, items[i]);
    }
    pj_arr_end(w);
}

void proto_fwsrv_check_request(pj_writer_t *w, const proto_fwsrv_facts_t *facts)
{
    pj_obj(w);
    pj_kstr(w, "channel",
            (facts->channel != NULL && facts->channel[0] != '\0') ? facts->channel : "stable");
    pj_kstr(w, "device", facts->device);
    if (facts->board_rev != NULL && facts->board_rev[0] != '\0') {
        pj_kstr(w, "board_rev", facts->board_rev);
    }
    pj_kstr(w, "chip_rev", facts->chip_rev);
    pj_kstr(w, "project", facts->project);
    pj_kstr(w, "version", facts->version);
    str_array(w, "peripherals", facts->peripherals, facts->peripheral_count);
    str_array(w, "languages", facts->languages, facts->language_count);
    pj_obj_end(w);
}

/* Relative URLs in a server's answers are relative to its address ("https://firmware.x.net"),
 * which has no path: give it its "/" so "a.bin" lands at the root, not in place of the host. */
static bool resolve(const char *base_url, const char *url, char *out, size_t cap)
{
    char base[256];
    if (base_url == NULL || base_url[0] == '\0') {
        return proto_fwi_resolve_url(NULL, url, out, cap);
    }
    const size_t n = strlen(base_url);
    const int w = snprintf(base, sizeof(base), "%s%s", base_url,
                           base_url[n - 1u] == '/' ? "" : "/");
    if (w <= 0 || (size_t)w >= sizeof(base)) {
        return false;
    }
    return proto_fwi_resolve_url(base, url, out, cap);
}

/* Copies a string that may be longer than cap - 1 bytes, cut at a UTF-8 character boundary. */
static void copy_cut(char *out, size_t cap, const char *in)
{
    size_t n = strlen(in);
    if (n >= cap) {
        n = cap - 1u;
        while (n > 0u && ((unsigned)(unsigned char)in[n] & 0xC0u) == 0x80u) {
            n--; /* in[n] continues a character: cut before that character */
        }
    }
    memcpy(out, in, n);
    out[n] = '\0';
}

/* A channel name: a known one is copied, null or an unknown one leaves "". false: malformed. */
static bool read_channel(pj_reader_t *r, char *out, size_t cap)
{
    out[0] = '\0';
    if (pj_peek(r) == PJ_NULL) {
        return pj_get_null(r);
    }
    if (pj_peek(r) != PJ_STRING) {
        pj_skip(r);
        return false;
    }
    char name[32];
    if (pj_get_str(r, name, sizeof(name)) && proto_fwsrv_channel_valid(name)) {
        copy_cut(out, cap, name);
    }
    return true;
}

static bool read_installed(pj_reader_t *r, proto_fwsrv_answer_t *out)
{
    if (pj_peek(r) != PJ_OBJECT) {
        pj_skip(r);
        return false;
    }
    (void)pj_enter_obj(r);
    char key[16];
    bool ok = true;
    while (pj_next_member(r, key, sizeof(key))) {
        if (strcmp(key, "channel") == 0) {
            ok = read_channel(r, out->installed_channel, sizeof(out->installed_channel)) && ok;
        } else if (strcmp(key, "withdrawn") == 0) {
            ok = pj_get_bool(r, &out->installed_withdrawn) && ok;
        }
    }
    return ok && !r->error;
}

static bool read_signing_key(pj_reader_t *r, proto_fwsrv_answer_t *out)
{
    if (pj_peek(r) == PJ_NULL) {
        return pj_get_null(r);
    }
    if (pj_peek(r) != PJ_OBJECT) {
        pj_skip(r);
        return false;
    }
    (void)pj_enter_obj(r);
    char key[16];
    char name[128] = "";
    char sha[72] = "";
    bool ok = true;
    while (pj_next_member(r, key, sizeof(key))) {
        if (strcmp(key, "name") == 0) {
            if (pj_peek(r) == PJ_STRING) {
                (void)pj_get_str(r, name, sizeof(name)); /* too long: no name, the digest counts */
            } else {
                ok = pj_get_str(r, name, sizeof(name)) && ok;
            }
        } else if (strcmp(key, "sha256") == 0) {
            ok = pj_get_str(r, sha, sizeof(sha)) && ok;
        }
    }
    if (!ok || r->error || !proto_fwi_parse_sha256(sha, out->key_sha256)) {
        return false; /* a signing key without its digest names nothing */
    }
    copy_cut(out->key_name, sizeof(out->key_name), name);
    out->has_key = true;
    return true;
}

typedef struct {
    char url[256];
    int64_t size;
    char sha[72];
} image_raw_t;

static bool read_image(pj_reader_t *r, image_raw_t *img, proto_fwsrv_answer_t *out)
{
    if (pj_peek(r) != PJ_OBJECT) {
        pj_skip(r);
        return false;
    }
    (void)pj_enter_obj(r);
    char key[16];
    bool ok = true;
    while (pj_next_member(r, key, sizeof(key))) {
        if (strcmp(key, "url") == 0) {
            ok = pj_get_str(r, img->url, sizeof(img->url)) && ok;
        } else if (strcmp(key, "size") == 0) {
            ok = pj_get_int(r, &img->size) && img->size >= 0 && img->size <= UINT32_MAX && ok;
        } else if (strcmp(key, "sha256") == 0) {
            ok = pj_get_str(r, img->sha, sizeof(img->sha)) && ok;
        } else if (strcmp(key, "signing_key") == 0) {
            ok = read_signing_key(r, out) && ok;
        }
    }
    return ok && !r->error;
}

static proto_fwsrv_reason_t reason_of(const char *s)
{
    if (s[0] == '\0' || strcmp(s, "up_to_date") == 0) {
        return PROTO_FWSRV_UP_TO_DATE;
    }
    if (strcmp(s, "unknown_device") == 0) {
        return PROTO_FWSRV_UNKNOWN_DEVICE;
    }
    if (strcmp(s, "needs_stepping_stone") == 0) {
        return PROTO_FWSRV_STEPPING_STONE;
    }
    if (strcmp(s, "held_back") == 0) {
        return PROTO_FWSRV_HELD_BACK;
    }
    return PROTO_FWSRV_OTHER;
}

alarm_result_t proto_fwsrv_check_answer(const char *json, size_t len, const char *base_url,
                                        proto_fwsrv_answer_t *out)
{
    memset(out, 0, sizeof(*out));
    image_raw_t img;
    memset(&img, 0, sizeof(img));
    char version[32] = "";
    char reason[32] = "";
    bool update = false;
    bool have_update = false;
    bool have_image = false;
    bool bad = false;

    if (json == NULL) {
        return ALARM_ERR_BAD_ARG;
    }
    pj_reader_t r;
    pj_read_init(&r, json, len);
    if (!pj_enter_obj(&r)) {
        return ALARM_ERR_BAD_ARG;
    }
    char key[16];
    while (pj_next_member(&r, key, sizeof(key))) {
        if (pj_peek(&r) == PJ_NULL &&
            (strcmp(key, "version") == 0 || strcmp(key, "critical") == 0 ||
             strcmp(key, "image") == 0 || strcmp(key, "reason") == 0)) {
            /* an offer field set to null is left out: refused later only if an update needs it */
            (void)pj_get_null(&r);
            continue;
        }
        if (strcmp(key, "update") == 0) {
            have_update = pj_get_bool(&r, &update);
            bad = !have_update || bad;
        } else if (strcmp(key, "installed") == 0) {
            bad = !read_installed(&r, out) || bad;
        } else if (strcmp(key, "version") == 0) {
            bad = !pj_get_str(&r, version, sizeof(version)) || bad;
        } else if (strcmp(key, "channel") == 0) {
            bad = !read_channel(&r, out->channel, sizeof(out->channel)) || bad;
        } else if (strcmp(key, "critical") == 0) {
            bad = !pj_get_bool(&r, &out->critical) || bad;
        } else if (strcmp(key, "notes_url") == 0) {
            if (pj_peek(&r) == PJ_STRING) {
                /* a notes page that doesn't fit is left out, not an error */
                if (!pj_get_str(&r, out->image.notes_url, sizeof(out->image.notes_url))) {
                    out->image.notes_url[0] = '\0';
                }
            } else if (pj_peek(&r) == PJ_NULL) {
                (void)pj_get_null(&r);
            } else {
                pj_skip(&r);
                bad = true;
            }
        } else if (strcmp(key, "image") == 0) {
            have_image = true;
            bad = !read_image(&r, &img, out) || bad;
        } else if (strcmp(key, "reason") == 0) {
            if (pj_peek(&r) == PJ_STRING) {
                if (!pj_get_str(&r, reason, sizeof(reason))) {
                    snprintf(reason, sizeof(reason), "?"); /* too long to be one it knows */
                }
            } else {
                pj_skip(&r);
                bad = true;
            }
        }
        if (r.error) {
            break;
        }
    }
    if (!pj_finished(&r) || bad || !have_update) {
        memset(out, 0, sizeof(*out));
        return ALARM_ERR_BAD_ARG;
    }
    if (!update) {
        /* Nothing to install: the offer's fields mean nothing. */
        const proto_fwsrv_answer_t keep = *out;
        memset(out, 0, sizeof(*out));
        memcpy(out->installed_channel, keep.installed_channel, sizeof(out->installed_channel));
        out->installed_withdrawn = keep.installed_withdrawn;
        out->reason = reason_of(reason);
        return ALARM_OK;
    }
    proto_version_t parsed;
    bool ok = have_image && img.url[0] != '\0' && proto_version_parse(version, &parsed) &&
              resolve(base_url, img.url, out->image.url, sizeof(out->image.url));
    if (ok && img.sha[0] != '\0') {
        ok = proto_fwi_parse_sha256(img.sha, out->image.sha256);
        out->image.has_sha256 = ok;
    }
    if (!ok) {
        memset(out, 0, sizeof(*out));
        return ALARM_ERR_BAD_ARG;
    }
    if (out->image.notes_url[0] != '\0') {
        /* shown as a link: the same http(s) rule as a download, or left out */
        char notes[sizeof(out->image.notes_url)];
        if (!resolve(base_url, out->image.notes_url, notes, sizeof(notes))) {
            notes[0] = '\0';
        }
        memcpy(out->image.notes_url, notes, sizeof(notes));
    }
    out->reason = PROTO_FWSRV_UPDATE;
    snprintf(out->image.version, sizeof(out->image.version), "%s", version);
    out->image.size = (uint32_t)img.size;
    return ALARM_OK;
}

/* The "languages" and "assets" lists of a check answer or a GET /api/v1/languages answer (either
 * output may be NULL with max 0), each entry's empty version filled from the answer's. false: not
 * an answer of either kind. */
static bool answer_lists(const char *json, size_t len, const char *base_url,
                         proto_fw_language_t *langs, size_t lmax, size_t *ln,
                         proto_fw_asset_t *assets, size_t amax, size_t *an)
{
    char version[32] = "";
    bool bad = false;
    bool answer = false; /* an "update" boolean (a check), a "languages" or an "assets" array */
    *ln = 0;
    *an = 0;
    char base[256];
    const char *rel_base = NULL;
    if (base_url != NULL && base_url[0] != '\0') {
        const size_t bn = strlen(base_url);
        const int w = snprintf(base, sizeof(base), "%s%s", base_url,
                               base_url[bn - 1u] == '/' ? "" : "/");
        if (w <= 0 || (size_t)w >= sizeof(base)) {
            return false;
        }
        rel_base = base;
    }

    if (json == NULL) {
        return false;
    }
    pj_reader_t r;
    pj_read_init(&r, json, len);
    if (!pj_enter_obj(&r)) {
        return false;
    }
    char key[16];
    while (pj_next_member(&r, key, sizeof(key))) {
        if (strcmp(key, "version") == 0) {
            bad = !pj_get_str(&r, version, sizeof(version)) || bad;
        } else if (strcmp(key, "update") == 0) {
            bool update;
            bad = !pj_get_bool(&r, &update) || bad;
            answer = true;
        } else if (strcmp(key, "languages") == 0) {
            if (!proto_fwi_read_languages(&r, rel_base, langs, lmax, ln)) {
                return false;
            }
            answer = true;
        } else if (strcmp(key, "assets") == 0) {
            /* newer than the languages: a value that is not a list (a server's empty list may come
             * as null) reads as none and never costs the languages beside it (OPT-007) */
            if (pj_peek(&r) != PJ_ARRAY) {
                pj_skip(&r);
            } else if (!proto_fwi_read_assets(&r, rel_base, assets, amax, an)) {
                return false;
            } else {
                answer = true;
            }
        }
    }
    if (!pj_finished(&r) || bad || !answer) {
        return false;
    }
    for (size_t i = 0; i < *ln; i++) {
        if (langs[i].version[0] == '\0') {
            snprintf(langs[i].version, sizeof(langs[i].version), "%s", version);
        }
    }
    for (size_t i = 0; i < *an; i++) {
        if (assets[i].version[0] == '\0') {
            snprintf(assets[i].version, sizeof(assets[i].version), "%s", version);
        }
    }
    return true;
}

size_t proto_fwsrv_languages(const char *json, size_t len, const char *base_url,
                             proto_fw_language_t *out, size_t max)
{
    size_t ln;
    size_t an;
    if (!answer_lists(json, len, base_url, out, max, &ln, NULL, 0, &an)) {
        return SIZE_MAX;
    }
    return ln;
}

size_t proto_fwsrv_assets(const char *json, size_t len, const char *base_url,
                          proto_fw_asset_t *out, size_t max)
{
    size_t ln;
    size_t an;
    if (!answer_lists(json, len, base_url, NULL, 0, &ln, out, max, &an)) {
        return SIZE_MAX;
    }
    return an;
}

/* Appends "<sep><name>=<value, percent-encoded>" at *n. false: it doesn't fit. */
static bool query_add(char *out, size_t cap, size_t *n, char sep, const char *name,
                      const char *value)
{
    static const char hex[] = "0123456789ABCDEF";
    const size_t nn = strlen(name);
    if (*n + 2u + nn >= cap) {
        return false;
    }
    out[(*n)++] = sep;
    memcpy(out + *n, name, nn);
    *n += nn;
    out[(*n)++] = '=';
    for (const char *p = value; *p != '\0'; p++) {
        const unsigned c = (unsigned)(unsigned char)*p;
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
        if (*n + (plain ? 1u : 3u) >= cap) {
            return false;
        }
        if (plain) {
            out[(*n)++] = (char)c;
        } else {
            out[(*n)++] = '%';
            out[(*n)++] = hex[c >> 4];
            out[(*n)++] = hex[c & 0x0Fu];
        }
    }
    out[*n] = '\0';
    return true;
}

static bool has_text(const char *s)
{
    return s != NULL && s[0] != '\0';
}

bool proto_fwsrv_languages_url(const char *base_url, const proto_fwsrv_facts_t *facts, char *out,
                               size_t cap)
{
    if (out == NULL || cap == 0u) {
        return false;
    }
    out[0] = '\0';
    if (!has_text(base_url) || facts == NULL || !has_text(facts->device) ||
        !has_text(facts->chip_rev) || !has_text(facts->version)) {
        return false;
    }
    size_t len = strlen(base_url);
    if (base_url[len - 1u] == '/') {
        len--;
    }
    const int w = snprintf(out, cap, "%.*s/api/v1/languages", (int)len, base_url);
    size_t n = w > 0 ? (size_t)w : 0u;
    const bool ok = w > 0 && n < cap && query_add(out, cap, &n, '?', "device", facts->device) &&
                    query_add(out, cap, &n, '&', "chip_rev", facts->chip_rev) &&
                    query_add(out, cap, &n, '&', "version", facts->version) &&
                    (!has_text(facts->project) ||
                     query_add(out, cap, &n, '&', "project", facts->project));
    if (!ok) {
        out[0] = '\0';
    }
    return ok;
}

/* ---- signing keys (OTA-020) -------------------------------------------------------------- */

size_t proto_fwsrv_keys(const char *json, size_t len, proto_fwsrv_key_t *out, size_t max)
{
    size_t n = 0;
    bool have_keys = false;
    if (json == NULL) {
        return SIZE_MAX;
    }
    pj_reader_t r;
    pj_read_init(&r, json, len);
    if (!pj_enter_obj(&r)) {
        return SIZE_MAX;
    }
    char key[16];
    while (pj_next_member(&r, key, sizeof(key))) {
        if (strcmp(key, "keys") != 0) {
            continue;
        }
        if (!pj_enter_arr(&r)) {
            return SIZE_MAX;
        }
        have_keys = true;
        while (pj_next_element(&r)) {
            if (!pj_enter_obj(&r)) {
                return SIZE_MAX;
            }
            char name[128] = "";
            char sha[72] = "";
            bool ok = true;
            while (pj_next_member(&r, key, sizeof(key))) {
                if (strcmp(key, "name") == 0) {
                    ok = pj_get_str(&r, name, sizeof(name)) && ok;
                } else if (strcmp(key, "sha256") == 0) {
                    ok = pj_get_str(&r, sha, sizeof(sha)) && ok;
                }
            }
            if (r.error) {
                return SIZE_MAX;
            }
            uint8_t digest[32];
            if (!ok || n >= max || name[0] == '\0' || !proto_fwi_parse_sha256(sha, digest)) {
                continue;
            }
            copy_cut(out[n].name, sizeof(out[n].name), name);
            memcpy(out[n].sha256, digest, sizeof(digest));
            n++;
        }
    }
    if (!pj_finished(&r) || !have_keys) {
        return SIZE_MAX;
    }
    return n;
}

#define SIGBLOCK_LEN       1216u
#define SIGBLOCK_CRC_AT    1196u
#define SIGBLOCK_KEY_AT    36u
#define SIGBLOCK_KEY_LEN   776u /* n[384], e, rinv[384], mdash */

bool proto_fwsrv_sigblock_key(const uint8_t *block, size_t len, uint8_t digest[32])
{
    if (block == NULL || len < SIGBLOCK_LEN || block[0] != 0xE7u || block[1] != 0x02u) {
        return false;
    }
    const uint32_t crc = (uint32_t)block[SIGBLOCK_CRC_AT] |
                         ((uint32_t)block[SIGBLOCK_CRC_AT + 1u] << 8) |
                         ((uint32_t)block[SIGBLOCK_CRC_AT + 2u] << 16) |
                         ((uint32_t)block[SIGBLOCK_CRC_AT + 3u] << 24);
    if (crc != alarm_crc32(0u, block, SIGBLOCK_CRC_AT)) {
        return false;
    }
    proto_sha256(block + SIGBLOCK_KEY_AT, SIGBLOCK_KEY_LEN, digest);
    return true;
}

void proto_fwsrv_fingerprint(const uint8_t digest[32], char out[PROTO_FWSRV_FINGERPRINT_LEN])
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; i < 8u; i++) {
        if (i > 0u && (i % 2u) == 0u) {
            out[o++] = ' ';
        }
        out[o++] = hex[(unsigned)digest[i] >> 4];
        out[o++] = hex[(unsigned)digest[i] & 15u];
    }
    out[o] = '\0';
}

bool proto_fwsrv_key_to_show(const proto_fwsrv_key_t *offered, bool has_offered,
                             const proto_fwsrv_key_t *keys, size_t n_keys,
                             const uint8_t (*trusted)[32], size_t n_trusted,
                             proto_fwsrv_key_t *out)
{
    memset(out, 0, sizeof(*out));
    if (keys == NULL) {
        n_keys = 0u;
    }
    if (trusted == NULL) {
        n_trusted = 0u;
    }
    if (has_offered && offered != NULL) {
        *out = *offered;
        out->name[sizeof(out->name) - 1u] = '\0';
        for (size_t i = 0; i < n_keys && out->name[0] == '\0'; i++) {
            if (memcmp(keys[i].sha256, out->sha256, 32u) == 0) {
                memcpy(out->name, keys[i].name, sizeof(out->name));
                out->name[sizeof(out->name) - 1u] = '\0';
            }
        }
        return true;
    }
    if (n_keys == 0u) {
        return false;
    }
    size_t pick = 0u;
    if (n_keys > 1u) {
        bool found = false;
        for (size_t i = 0; i < n_keys && !found; i++) {
            for (size_t t = 0; t < n_trusted && !found; t++) {
                if (memcmp(keys[i].sha256, trusted[t], 32u) == 0) {
                    pick = i;
                    found = true;
                }
            }
        }
    }
    *out = keys[pick];
    out->name[sizeof(out->name) - 1u] = '\0';
    return true;
}

/* ---- the trust record -------------------------------------------------------------------- */

/* Layout, version 1 (little-endian):
 *   0  magic 'T' (0x54)        1  version (1)        2  flags (bit 0: has_key; others 0)
 *   3  server length S (1..95), then S bytes         then the key name length N (0..47), N bytes
 *   with has_key: the key's 32-byte digest           last: CRC-32 (alarm_crc32) of all before
 * Strings carry no NUL. A new layout takes a new version; this reader refuses it. */
#define TRUST_MAGIC   0x54u
#define TRUST_VERSION 1u
#define TRUST_HAS_KEY 0x01u

static size_t bounded_len(const char *s, size_t cap)
{
    size_t n = 0;
    while (n < cap && s[n] != '\0') {
        n++;
    }
    return n;
}

size_t proto_fwsrv_trust_encode(const proto_fwsrv_trust_t *t, uint8_t *out, size_t cap)
{
    const size_t s = bounded_len(t->server, sizeof(t->server));
    const size_t k = bounded_len(t->key_name, sizeof(t->key_name));
    if (s == 0u || s >= sizeof(t->server) || k >= sizeof(t->key_name)) {
        return 0u;
    }
    const size_t total = 3u + 1u + s + 1u + k + (t->has_key ? 32u : 0u) + 4u;
    if (out == NULL || total > cap) {
        return 0u;
    }
    size_t o = 0;
    out[o++] = TRUST_MAGIC;
    out[o++] = TRUST_VERSION;
    out[o++] = t->has_key ? TRUST_HAS_KEY : 0u;
    out[o++] = (uint8_t)s;
    memcpy(out + o, t->server, s);
    o += s;
    out[o++] = (uint8_t)k;
    memcpy(out + o, t->key_name, k);
    o += k;
    if (t->has_key) {
        memcpy(out + o, t->key_sha256, 32u);
        o += 32u;
    }
    const uint32_t crc = alarm_crc32(0u, out, o);
    out[o++] = (uint8_t)(crc & 0xFFu);
    out[o++] = (uint8_t)((crc >> 8) & 0xFFu);
    out[o++] = (uint8_t)((crc >> 16) & 0xFFu);
    out[o++] = (uint8_t)((crc >> 24) & 0xFFu);
    return o;
}

static bool no_nul(const uint8_t *p, size_t n)
{
    return memchr(p, 0, n) == NULL;
}

bool proto_fwsrv_trust_decode(const uint8_t *in, size_t len, proto_fwsrv_trust_t *out)
{
    memset(out, 0, sizeof(*out));
    if (in == NULL || len < 3u + 1u + 1u + 1u + 4u || len > PROTO_FWSRV_TRUST_BLOB_MAX) {
        return false;
    }
    const size_t body = len - 4u;
    const uint32_t crc = (uint32_t)in[body] | ((uint32_t)in[body + 1u] << 8) |
                         ((uint32_t)in[body + 2u] << 16) | ((uint32_t)in[body + 3u] << 24);
    if (crc != alarm_crc32(0u, in, body) || in[0] != TRUST_MAGIC || in[1] != TRUST_VERSION ||
        (in[2] & (uint8_t)~TRUST_HAS_KEY) != 0u) {
        return false;
    }
    const bool has_key = (in[2] & TRUST_HAS_KEY) != 0u;
    size_t o = 3u;
    const size_t s = in[o++];
    if (s == 0u || s >= sizeof(out->server) || o + s + 1u > body || !no_nul(in + o, s)) {
        return false;
    }
    const uint8_t *server = in + o;
    o += s;
    const size_t k = in[o++];
    if (k >= sizeof(out->key_name) || o + k > body || !no_nul(in + o, k)) {
        return false;
    }
    const uint8_t *name = in + o;
    o += k;
    if (o + (has_key ? 32u : 0u) != body) {
        return false; /* cut short, or bytes this version doesn't have */
    }
    memcpy(out->server, server, s);
    memcpy(out->key_name, name, k);
    if (has_key) {
        memcpy(out->key_sha256, in + o, 32u);
    }
    out->has_key = has_key;
    return true;
}
