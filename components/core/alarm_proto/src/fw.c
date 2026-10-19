/*
 * fw.c - firmware update helpers: versions, manifest, install policy, serial upload framing,
 * Home Assistant update state (see proto_fw.h, docs/features/13).
 */
#include <stdio.h>
#include <string.h>

#include "alarm/alarm_codec.h"
#include "alarm_proto/proto_fw.h"
#include "fw_shared.h"
#include "i18n/i18n.h"

/* ---- versions ---------------------------------------------------------------------------- */

static bool parse_number(const char **p, uint16_t *out)
{
    const char *s = *p;
    uint32_t v = 0;
    if (*s < '0' || *s > '9') {
        return false;
    }
    if (*s == '0' && s[1] >= '0' && s[1] <= '9') {
        return false; /* no leading zeros */
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint32_t)(*s - '0');
        if (v > 65535u) {
            return false;
        }
        s++;
    }
    *out = (uint16_t)v;
    *p = s;
    return true;
}

static bool pre_char(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '-' || c == '.';
}

bool proto_version_parse(const char *text, proto_version_t *out)
{
    memset(out, 0, sizeof(*out));
    if (text == NULL) {
        return false;
    }
    const char *p = text;
    if (*p == 'v' || *p == 'V') {
        p++;
    }
    if (!parse_number(&p, &out->major) || *p++ != '.' || !parse_number(&p, &out->minor) ||
        *p++ != '.' || !parse_number(&p, &out->patch)) {
        return false;
    }
    if (*p == '-') {
        p++;
        size_t n = 0;
        while (pre_char(*p)) {
            if (n + 1u >= sizeof(out->pre)) {
                return false;
            }
            out->pre[n++] = *p++;
        }
        if (n == 0u) {
            return false;
        }
        out->pre[n] = '\0';
    }
    if (*p == '+') { /* build metadata: ignored for precedence */
        p++;
        if (!pre_char(*p)) {
            return false;
        }
        while (pre_char(*p)) {
            p++;
        }
    }
    return *p == '\0';
}

static bool all_digits(const char *s, size_t n)
{
    if (n == 0u) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return true;
}

/* Pre-release precedence: dot-separated identifiers, numbers numerically and below text. */
static int compare_pre(const char *a, const char *b)
{
    if (a[0] == '\0' || b[0] == '\0') {
        return (a[0] == '\0') - (b[0] == '\0'); /* a release outranks its pre-releases */
    }
    while (*a != '\0' || *b != '\0') {
        if (*a == '\0') {
            return -1; /* fewer identifiers: lower */
        }
        if (*b == '\0') {
            return 1;
        }
        const size_t la = strcspn(a, ".");
        const size_t lb = strcspn(b, ".");
        const bool na = all_digits(a, la);
        const bool nb = all_digits(b, lb);
        int c;
        if (na && nb) {
            c = (la != lb) ? (la < lb ? -1 : 1) : strncmp(a, b, la);
        } else if (na != nb) {
            c = na ? -1 : 1;
        } else {
            c = strncmp(a, b, la < lb ? la : lb);
            if (c == 0 && la != lb) {
                c = la < lb ? -1 : 1;
            }
        }
        if (c != 0) {
            return c < 0 ? -1 : 1;
        }
        a += la + (a[la] == '.' ? 1u : 0u);
        b += lb + (b[lb] == '.' ? 1u : 0u);
    }
    return 0;
}

int proto_version_compare(const proto_version_t *a, const proto_version_t *b)
{
    if (a->major != b->major) {
        return a->major < b->major ? -1 : 1;
    }
    if (a->minor != b->minor) {
        return a->minor < b->minor ? -1 : 1;
    }
    if (a->patch != b->patch) {
        return a->patch < b->patch ? -1 : 1;
    }
    return compare_pre(a->pre, b->pre);
}

bool proto_version_is_upgrade(const char *running, const char *candidate, bool allow_older)
{
    proto_version_t r;
    proto_version_t c;
    if (!proto_version_parse(candidate, &c)) {
        return allow_older;
    }
    if (!proto_version_parse(running, &r)) {
        return allow_older;
    }
    return allow_older || proto_version_compare(&c, &r) > 0;
}

/* ---- manifest ---------------------------------------------------------------------------- */

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool proto_fwi_parse_sha256(const char *hex, uint8_t out[32])
{
    if (strlen(hex) != 64u) {
        return false;
    }
    for (size_t i = 0; i < 32u; i++) {
        const int hi = hex_value(hex[2u * i]);
        const int lo = hex_value(hex[2u * i + 1u]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/* A download address must be http(s) with a host, printable, and from an https source https
 * too. The signature and the digest still guard the file; this keeps an answer from steering the
 * panel to plain http, a file: path or half an address. Another host is allowed (a CDN). */
static bool url_safe(const char *base_url, const char *url)
{
    const bool https = strncmp(url, "https://", 8) == 0;
    if (!https && strncmp(url, "http://", 7) != 0) {
        return false;
    }
    if (!https && base_url != NULL && strncmp(base_url, "https://", 8) == 0) {
        return false;
    }
    const char *host = url + (https ? 8 : 7);
    if (*host == '\0' || *host == '/') {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)url; *p != 0u; p++) {
        if (*p <= 0x20u || *p == 0x7fu) {
            return false;
        }
    }
    return true;
}

/* url relative to the manifest: "file.bin" -> same folder, "/x/file.bin" -> same host. The
 * result is always an absolute http(s) address (url_safe), or false. */
bool proto_fwi_resolve_url(const char *manifest_url, const char *url, char *out, size_t cap)
{
    int n;
    if (strstr(url, "://") != NULL || manifest_url == NULL || manifest_url[0] == '\0') {
        n = snprintf(out, cap, "%s", url);
    } else if (url[0] == '/') {
        const char *scheme = strstr(manifest_url, "://");
        const char *host_end = scheme != NULL ? strchr(scheme + 3, '/') : NULL;
        const size_t base = host_end != NULL ? (size_t)(host_end - manifest_url) : strlen(manifest_url);
        n = snprintf(out, cap, "%.*s%s", (int)base, manifest_url, url);
    } else {
        const char *slash = strrchr(manifest_url, '/');
        const size_t base = slash != NULL ? (size_t)(slash - manifest_url) + 1u : 0u;
        n = snprintf(out, cap, "%.*s%s", (int)base, manifest_url, url);
    }
    return n > 0 && (size_t)n < cap && url_safe(manifest_url, out);
}

typedef struct {
    char device[32];
    char chip_rev[8];
    char version[32];
    char url[256];
    int64_t size;
    char sha[72];
} artifact_raw_t;

static bool read_artifact(pj_reader_t *r, artifact_raw_t *a)
{
    memset(a, 0, sizeof(*a));
    if (!pj_enter_obj(r)) {
        return false;
    }
    char key[16];
    bool ok = true;
    while (pj_next_member(r, key, sizeof(key))) {
        if (strcmp(key, "device") == 0) {
            ok = pj_get_str(r, a->device, sizeof(a->device)) && ok;
        } else if (strcmp(key, "chip_rev") == 0) {
            ok = pj_get_str(r, a->chip_rev, sizeof(a->chip_rev)) && ok;
        } else if (strcmp(key, "version") == 0) {
            ok = pj_get_str(r, a->version, sizeof(a->version)) && ok;
        } else if (strcmp(key, "url") == 0) {
            ok = pj_get_str(r, a->url, sizeof(a->url)) && ok;
        } else if (strcmp(key, "size") == 0) {
            ok = pj_get_int(r, &a->size) && a->size >= 0 && a->size <= UINT32_MAX && ok;
        } else if (strcmp(key, "sha256") == 0) {
            ok = pj_get_str(r, a->sha, sizeof(a->sha)) && ok;
        }
    }
    return ok && !r->error;
}

alarm_result_t proto_fw_manifest_pick(const char *json, size_t len, const char *device,
                                      const char *chip_rev, const char *manifest_url,
                                      proto_fw_artifact_t *out)
{
    memset(out, 0, sizeof(*out));
    char version[32] = "";
    char notes[192] = "";
    artifact_raw_t found;
    bool have = false;
    bool bad = false;

    pj_reader_t r;
    pj_read_init(&r, json, len);
    if (!pj_enter_obj(&r)) {
        return ALARM_ERR_BAD_ARG;
    }
    char key[16];
    while (pj_next_member(&r, key, sizeof(key))) {
        if (strcmp(key, "version") == 0) {
            bad = !pj_get_str(&r, version, sizeof(version)) || bad;
        } else if (strcmp(key, "notes_url") == 0) {
            bad = !pj_get_str(&r, notes, sizeof(notes)) || bad;
        } else if (strcmp(key, "artifacts") == 0) {
            if (!pj_enter_arr(&r)) {
                return ALARM_ERR_BAD_ARG;
            }
            while (pj_next_element(&r)) {
                artifact_raw_t a;
                if (!read_artifact(&r, &a)) {
                    bad = true;
                    continue;
                }
                const bool rev_ok = chip_rev == NULL || a.chip_rev[0] == '\0' ||
                                    strcmp(a.chip_rev, chip_rev) == 0;
                if (!have && strcmp(a.device, device) == 0 && rev_ok) {
                    found = a;
                    have = true;
                }
            }
        }
    }
    if (!pj_finished(&r) || bad) {
        return ALARM_ERR_BAD_ARG;
    }
    if (!have) {
        return ALARM_ERR_DISABLED;
    }
    const char *v = found.version[0] != '\0' ? found.version : version;
    proto_version_t parsed;
    if (!proto_version_parse(v, &parsed) || found.url[0] == '\0' ||
        !proto_fwi_resolve_url(manifest_url, found.url, out->url, sizeof(out->url))) {
        return ALARM_ERR_BAD_ARG;
    }
    snprintf(out->version, sizeof(out->version), "%s", v);
    out->size = (uint32_t)found.size;
    if (found.sha[0] != '\0') {
        if (!proto_fwi_parse_sha256(found.sha, out->sha256)) {
            return ALARM_ERR_BAD_ARG;
        }
        out->has_sha256 = true;
    }
    snprintf(out->notes_url, sizeof(out->notes_url), "%s", notes);
    return ALARM_OK;
}

/* ---- language packs (OTA-016) ------------------------------------------------------------ */

typedef struct {
    char locale[16];
    char version[32];
    char url[256];
    int64_t size;
    char sha[72];
} language_raw_t;

static bool locale_valid(const char *s)
{
    const size_t n = strlen(s);
    if (n < 2u || n > 15u) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        const char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-')) {
            return false;
        }
    }
    return true;
}

/* false: the entry is malformed (skip it); the reader stays in step unless r->error. */
static bool read_language(pj_reader_t *r, language_raw_t *a)
{
    memset(a, 0, sizeof(*a));
    if (!pj_enter_obj(r)) {
        return false;
    }
    char key[16];
    bool ok = true;
    while (pj_next_member(r, key, sizeof(key))) {
        if (strcmp(key, "locale") == 0) {
            ok = pj_get_str(r, a->locale, sizeof(a->locale)) && ok;
        } else if (strcmp(key, "version") == 0) {
            ok = pj_get_str(r, a->version, sizeof(a->version)) && ok;
        } else if (strcmp(key, "url") == 0) {
            ok = pj_get_str(r, a->url, sizeof(a->url)) && ok;
        } else if (strcmp(key, "size") == 0) {
            ok = pj_get_int(r, &a->size) && a->size >= 0 && a->size <= UINT32_MAX && ok;
        } else if (strcmp(key, "sha256") == 0) {
            ok = pj_get_str(r, a->sha, sizeof(a->sha)) && ok;
        }
    }
    return ok && !r->error;
}

bool proto_fwi_read_languages(pj_reader_t *r, const char *base_url, proto_fw_language_t *out,
                              size_t max, size_t *n)
{
    if (!pj_enter_arr(r)) {
        return false;
    }
    while (pj_next_element(r)) {
        language_raw_t a;
        const bool read = read_language(r, &a);
        if (r->error) {
            return false;
        }
        if (!read || *n >= max || !locale_valid(a.locale) || a.url[0] == '\0') {
            continue;
        }
        proto_fw_language_t *l = &out[*n];
        memset(l, 0, sizeof(*l));
        if (a.sha[0] != '\0') {
            if (!proto_fwi_parse_sha256(a.sha, l->sha256)) {
                continue;
            }
            l->has_sha256 = true;
        }
        if (!proto_fwi_resolve_url(base_url, a.url, l->url, sizeof(l->url))) {
            continue;
        }
        snprintf(l->locale, sizeof(l->locale), "%s", a.locale);
        snprintf(l->version, sizeof(l->version), "%s", a.version);
        l->size = (uint32_t)a.size;
        (*n)++;
    }
    return !r->error;
}

/* ---- asset packs (OPT-007) --------------------------------------------------------------- */

typedef struct {
    char kind[16];
    char version[32];
    char url[256];
    int64_t size;
    char sha[72];
} asset_raw_t;

static bool asset_kind_parse(const char *s, proto_fw_asset_kind_t *out)
{
    if (strcmp(s, "letters") == 0) {
        *out = PROTO_FW_ASSET_LETTERS;
    } else if (strcmp(s, "picture") == 0) {
        *out = PROTO_FW_ASSET_PICTURE;
    } else if (strcmp(s, "certificates") == 0) {
        *out = PROTO_FW_ASSET_CERTS;
    } else {
        return false; /* "web" and anything later: not known to this firmware yet */
    }
    return true;
}

/* false: the entry is malformed (skip it); the reader stays in step unless r->error. */
static bool read_asset(pj_reader_t *r, asset_raw_t *a)
{
    memset(a, 0, sizeof(*a));
    if (!pj_enter_obj(r)) {
        return false;
    }
    char key[16];
    bool ok = true;
    while (pj_next_member(r, key, sizeof(key))) {
        if (strcmp(key, "kind") == 0) {
            ok = pj_get_str(r, a->kind, sizeof(a->kind)) && ok;
        } else if (strcmp(key, "version") == 0) {
            ok = pj_get_str(r, a->version, sizeof(a->version)) && ok;
        } else if (strcmp(key, "url") == 0) {
            ok = pj_get_str(r, a->url, sizeof(a->url)) && ok;
        } else if (strcmp(key, "size") == 0) {
            ok = pj_get_int(r, &a->size) && a->size >= 0 && a->size <= UINT32_MAX && ok;
        } else if (strcmp(key, "sha256") == 0) {
            ok = pj_get_str(r, a->sha, sizeof(a->sha)) && ok;
        }
    }
    return ok && !r->error;
}

bool proto_fwi_read_assets(pj_reader_t *r, const char *base_url, proto_fw_asset_t *out, size_t max,
                           size_t *n)
{
    if (!pj_enter_arr(r)) {
        return false;
    }
    while (pj_next_element(r)) {
        asset_raw_t a;
        const bool read = read_asset(r, &a);
        if (r->error) {
            return false;
        }
        proto_fw_asset_kind_t kind;
        if (!read || *n >= max || !asset_kind_parse(a.kind, &kind) || a.url[0] == '\0') {
            continue;
        }
        proto_fw_asset_t *e = &out[*n];
        memset(e, 0, sizeof(*e));
        if (a.sha[0] != '\0') {
            if (!proto_fwi_parse_sha256(a.sha, e->sha256)) {
                continue;
            }
            e->has_sha256 = true;
        }
        if (!proto_fwi_resolve_url(base_url, a.url, e->url, sizeof(e->url))) {
            continue;
        }
        e->kind = kind;
        snprintf(e->version, sizeof(e->version), "%s", a.version);
        e->size = (uint32_t)a.size;
        (*n)++;
    }
    return !r->error;
}

/* The manifest's "languages" and "assets" lists (either output may be NULL with max 0), each
 * entry's empty version filled from the manifest's. false: not a valid manifest. */
static bool manifest_lists(const char *json, size_t len, const char *manifest_url,
                           proto_fw_language_t *langs, size_t lmax, size_t *ln,
                           proto_fw_asset_t *assets, size_t amax, size_t *an)
{
    char version[32] = "";
    bool bad = false;
    *ln = 0;
    *an = 0;

    pj_reader_t r;
    pj_read_init(&r, json, len);
    if (!pj_enter_obj(&r)) {
        return false;
    }
    char key[16];
    while (pj_next_member(&r, key, sizeof(key))) {
        if (strcmp(key, "version") == 0) {
            bad = !pj_get_str(&r, version, sizeof(version)) || bad;
        } else if (strcmp(key, "notes_url") == 0) {
            char notes[192];
            bad = !pj_get_str(&r, notes, sizeof(notes)) || bad;
        } else if (strcmp(key, "languages") == 0) {
            if (!proto_fwi_read_languages(&r, manifest_url, langs, lmax, ln)) {
                return false;
            }
        } else if (strcmp(key, "assets") == 0) {
            /* newer than the languages: a value that is not a list (a server's empty list may come
             * as null) reads as none and never costs the languages beside it (OPT-007) */
            if (pj_peek(&r) != PJ_ARRAY) {
                pj_skip(&r);
            } else if (!proto_fwi_read_assets(&r, manifest_url, assets, amax, an)) {
                return false;
            }
        }
    }
    if (!pj_finished(&r) || bad) {
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

size_t proto_fw_manifest_languages(const char *json, size_t len, const char *manifest_url,
                                   proto_fw_language_t *out, size_t max)
{
    size_t ln;
    size_t an;
    if (!manifest_lists(json, len, manifest_url, out, max, &ln, NULL, 0, &an)) {
        return SIZE_MAX;
    }
    return ln;
}

size_t proto_fw_manifest_assets(const char *json, size_t len, const char *manifest_url,
                                proto_fw_asset_t *out, size_t max)
{
    size_t ln;
    size_t an;
    if (!manifest_lists(json, len, manifest_url, NULL, 0, &ln, out, max, &an)) {
        return SIZE_MAX;
    }
    return an;
}

/* ---- install policy ---------------------------------------------------------------------- */

bool proto_fw_install_allowed(const alarm_snapshot_t *snap, const char **why)
{
    const char *reason = NULL;
    if (snap->program) {
        reason = I18N_NOOP_C("update", "Installer mode is on");
    }
    for (unsigned p = 0; p < ALARM_MAX_PARTITIONS && reason == NULL; p++) {
        if ((snap->partitions & (1u << p)) == 0u) {
            continue;
        }
        const alarm_partition_status_t *ps = &snap->part[p];
        if (ps->state != ALARM_STATE_DISARMED) {
            reason = I18N_NOOP_C("update", "The system is armed");
        } else if (ps->bell != ALARM_PAT_OFF ||
                   (ps->sounder != ALARM_SOUND_NONE && ps->sounder != ALARM_SOUND_TROUBLE)) {
            reason = I18N_NOOP_C("update", "An alarm is sounding");
        } else if (ps->walk_test) {
            reason = I18N_NOOP_C("update", "A walk test is running");
        }
    }
    if (why != NULL) {
        *why = reason != NULL ? reason : "";
    }
    return reason == NULL;
}

/* ---- serial upload ----------------------------------------------------------------------- */

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

size_t proto_base64_decode(const char *in, size_t in_len, uint8_t *out, size_t cap)
{
    if (in_len % 4u != 0u) {
        return SIZE_MAX;
    }
    size_t n = 0;
    for (size_t i = 0; i < in_len; i += 4u) {
        int v[4];
        unsigned pad = 0;
        for (unsigned k = 0; k < 4u; k++) {
            const char c = in[i + k];
            if (c == '=') {
                if (i + 4u != in_len || k < 2u) {
                    return SIZE_MAX; /* padding only at the very end */
                }
                v[k] = 0;
                pad++;
            } else {
                if (pad != 0u) {
                    return SIZE_MAX; /* data after padding */
                }
                v[k] = b64_value(c);
                if (v[k] < 0) {
                    return SIZE_MAX;
                }
            }
        }
        const uint32_t word = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) |
                              ((uint32_t)v[2] << 6) | (uint32_t)v[3];
        const unsigned bytes = 3u - pad;
        if (n + bytes > cap) {
            return SIZE_MAX;
        }
        out[n++] = (uint8_t)(word >> 16);
        if (bytes > 1u) {
            out[n++] = (uint8_t)(word >> 8);
        }
        if (bytes > 2u) {
            out[n++] = (uint8_t)word;
        }
    }
    return n;
}

size_t proto_fw_line_decode(const char *line, uint32_t *seq, uint8_t *out, size_t cap)
{
    const char *p = line;
    uint32_t s = 0;
    if (*p < '0' || *p > '9') {
        return SIZE_MAX;
    }
    while (*p >= '0' && *p <= '9') {
        if (s > 429496728u) {
            return SIZE_MAX;
        }
        s = s * 10u + (uint32_t)(*p++ - '0');
    }
    if (*p++ != ' ') {
        return SIZE_MAX;
    }
    uint32_t crc = 0;
    for (unsigned i = 0; i < 8u; i++) {
        const int h = hex_value(*p++);
        if (h < 0) {
            return SIZE_MAX;
        }
        crc = (crc << 4) | (uint32_t)h;
    }
    if (*p++ != ' ') {
        return SIZE_MAX;
    }
    size_t len = strlen(p);
    while (len > 0u && (p[len - 1u] == '\n' || p[len - 1u] == '\r')) {
        len--;
    }
    const size_t n = proto_base64_decode(p, len, out, cap);
    if (n == SIZE_MAX || alarm_crc32(0u, out, n) != crc) {
        return SIZE_MAX;
    }
    *seq = s;
    return n;
}

/* ---- Home Assistant update entity -------------------------------------------------------- */

void proto_fw_state(pj_writer_t *w, const proto_fw_state_t *st)
{
    pj_obj(w);
    pj_kstr(w, "installed_version", st->installed);
    pj_kstr(w, "latest_version", st->latest != NULL && st->latest[0] != '\0' ? st->latest
                                                                             : st->installed);
    pj_kstr(w, "title", st->title);
    /* Left out when there is none: Home Assistant checks it as a URL and drops the whole
     * payload over a null (B019). */
    if (st->release_url != NULL && st->release_url[0] != '\0') {
        pj_kstr(w, "release_url", st->release_url);
    }
    pj_kbool(w, "in_progress", st->in_progress);
    if (st->percent >= 0 && st->percent <= 100) {
        pj_kint(w, "update_percentage", st->percent);
    } else {
        pj_knull(w, "update_percentage");
    }
    /* A master's link keypads (docs/features/11 "What people see"): Home Assistant's update
     * entity ignores the key; the integration reads it. */
    if (st->keypads != NULL && st->keypad_count > 0u) {
        const size_t n =
            st->keypad_count < PROTO_FW_KEYPADS_MAX ? st->keypad_count : PROTO_FW_KEYPADS_MAX;
        pj_karr(w, "keypads");
        for (size_t i = 0; i < n; i++) {
            const proto_fw_keypad_t *k = &st->keypads[i];
            pj_obj(w);
            pj_kuint(w, "device", k->device);
            if (k->version[0] != '\0') {
                pj_kstr(w, "version", k->version);
            } else {
                pj_knull(w, "version");
            }
            pj_kstr(w, "state", k->state);
            pj_obj_end(w);
        }
        pj_arr_end(w);
    }
    pj_obj_end(w);
}
