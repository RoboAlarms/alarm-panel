/*
 * zw_fwcat.c - which firmware a Z-Wave device could be updated to. See zw_fwcat.h.
 */
#include "zwave_zones/zw_fwcat.h"

#include <stdio.h>
#include <string.h>

#include "alarm_proto/proto_json.h"

const char *zwfc_region(uint8_t code)
{
    switch (code) {
    case 0x00u:
    case 0x0Bu: return "europe";
    case 0x01u:
    case 0x09u: return "usa";
    case 0x02u: return "australia/new zealand";
    case 0x03u: return "hong kong";
    case 0x05u: return "india";
    case 0x06u: return "israel";
    case 0x07u: return "russia";
    case 0x08u: return "china";
    case 0x20u: return "japan";
    case 0x21u: return "korea";
    default: return NULL;
    }
}

size_t zwfc_request(const zwfc_device_t *dev, const char *region, char *out, size_t cap)
{
    if (dev == NULL || out == NULL) {
        return 0u;
    }
    pj_writer_t w;
    pj_init(&w, out, cap);
    pj_obj(&w);
    if (region != NULL) {
        pj_kstr(&w, "region", region);
    }
    pj_karr(&w, "devices");
    pj_obj(&w);
    char s[16];
    snprintf(s, sizeof s, "0x%04x", (unsigned)dev->manufacturer);
    pj_kstr(&w, "manufacturerId", s);
    snprintf(s, sizeof s, "0x%04x", (unsigned)dev->product_type);
    pj_kstr(&w, "productType", s);
    snprintf(s, sizeof s, "0x%04x", (unsigned)dev->product_id);
    pj_kstr(&w, "productId", s);
    snprintf(s, sizeof s, "%u.%u", (unsigned)dev->fw_major, (unsigned)dev->fw_minor);
    pj_kstr(&w, "firmwareVersion", s);
    if (dev->has_sdk) {
        snprintf(s, sizeof s, "%u.%u.%u", (unsigned)dev->sdk_major, (unsigned)dev->sdk_minor,
                 (unsigned)dev->sdk_patch);
        pj_kstr(&w, "sdkVersion", s);
    }
    pj_obj_end(&w);
    pj_arr_end(&w);
    pj_obj_end(&w);
    return pj_finish(&w);
}

/* "0x0346" (case-insensitive, 1..4 hex digits). */
static bool hex16(const char *s, uint16_t *out)
{
    if (s[0] != '0' || (s[1] != 'x' && s[1] != 'X') || s[2] == '\0') {
        return false;
    }
    uint32_t v = 0u;
    size_t n = 0u;
    for (const char *p = &s[2]; *p != '\0'; p++, n++) {
        const char c = *p;
        const int d = c >= '0' && c <= '9'   ? c - '0'
                      : c >= 'a' && c <= 'f' ? c - 'a' + 10
                      : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                             : -1;
        if (d < 0 || n >= 4u) {
            return false;
        }
        v = v << 4 | (uint32_t)d;
    }
    *out = (uint16_t)v;
    return true;
}

/* "1.18", "1.18.2": up to three numbers, nothing else. */
static bool version3(const char *s, uint16_t v[3])
{
    v[0] = v[1] = v[2] = 0u;
    size_t part = 0u;
    bool digit = false;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p >= '0' && *p <= '9') {
            const uint32_t n = (uint32_t)v[part] * 10u + (uint32_t)(*p - '0');
            if (n > 0xFFFFu) {
                return false;
            }
            v[part] = (uint16_t)n;
            digit = true;
        } else if (*p == '.' && digit && part < 2u) {
            part++;
            digit = false;
        } else {
            return false;
        }
    }
    return digit;
}

static int compare(const uint16_t a[3], const uint16_t b[3])
{
    for (size_t i = 0; i < 3u; i++) {
        if (a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
    }
    return 0;
}

static bool sha256_of(const char *s, uint8_t out[32])
{
    if (strncmp(s, "sha256:", 7u) != 0 || strlen(s) != 7u + 64u) {
        return false;
    }
    for (size_t i = 0; i < 32u; i++) {
        unsigned b = 0u;
        for (size_t j = 0; j < 2u; j++) {
            const char c = s[7u + 2u * i + j];
            const int d = c >= '0' && c <= '9'   ? c - '0'
                          : c >= 'a' && c <= 'f' ? c - 'a' + 10
                          : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                 : -1;
            if (d < 0) {
                return false;
            }
            b = b << 4 | (unsigned)d;
        }
        out[i] = (uint8_t)b;
    }
    return true;
}

/* One update object: into *u; false when it isn't one the panel would offer. */
static bool read_update(pj_reader_t *r, const char *region, bool include_beta,
                        const uint16_t current[3], zwfc_update_t *u, uint16_t v[3])
{
    memset(u, 0, sizeof(*u));
    bool has_version = false, has_norm = false, downgrade = true, has_downgrade = false;
    bool channel_ok = false, region_ok = true, target0 = false, bad_file = false;
    uint16_t norm[3] = {0u, 0u, 0u};
    char key[24];
    if (!pj_enter_obj(r)) {
        return false;
    }
    while (pj_next_member(r, key, sizeof key)) {
        if (strcmp(key, "version") == 0) {
            has_version = pj_get_str(r, u->version, sizeof u->version) && version3(u->version, v);
        } else if (strcmp(key, "normalizedVersion") == 0) {
            char s[24];
            has_norm = pj_get_str(r, s, sizeof s) && version3(s, norm);
        } else if (strcmp(key, "changelog") == 0) {
            /* a longer one is left out: the page points at the maker's notes then */
            if (!pj_get_str(r, u->changelog, sizeof u->changelog)) {
                u->changelog[0] = 0;
                u->changelog_cut = true;
            }
        } else if (strcmp(key, "channel") == 0) {
            char s[12];
            if (pj_get_str(r, s, sizeof s)) {
                u->beta = strcmp(s, "beta") == 0;
                channel_ok = strcmp(s, "stable") == 0 || (u->beta && include_beta);
            }
        } else if (strcmp(key, "downgrade") == 0) {
            has_downgrade = pj_get_bool(r, &downgrade);
        } else if (strcmp(key, "region") == 0) {
            char s[32];
            region_ok = pj_get_str(r, s, sizeof s) && region != NULL && strcmp(s, region) == 0;
        } else if (strcmp(key, "files") == 0) {
            if (!pj_enter_arr(r)) {
                return false;
            }
            while (pj_next_element(r)) {
                zwfc_file_t file;
                memset(&file, 0, sizeof file);
                bool has_url = false, has_sha = false, has_target = false;
                if (!pj_enter_obj(r)) {
                    return false;
                }
                while (pj_next_member(r, key, sizeof key)) {
                    char s[ZWFC_URL_MAX];
                    int64_t t = 0;
                    if (strcmp(key, "target") == 0) {
                        has_target = pj_get_int(r, &t) && t >= 0 && t <= 255;
                        file.target = (uint8_t)(has_target ? t : 0);
                    } else if (strcmp(key, "url") == 0) {
                        has_url = pj_get_str(r, file.url, sizeof file.url) &&
                                  strncmp(file.url, "https://", 8u) == 0;
                    } else if (strcmp(key, "integrity") == 0) {
                        has_sha = pj_get_str(r, s, sizeof s) && sha256_of(s, file.sha256);
                    } else {
                        pj_skip(r);
                    }
                }
                if (has_url && has_sha && has_target && u->file_count < ZWFC_FILES_MAX) {
                    target0 = target0 || file.target == 0u;
                    u->files[u->file_count++] = file;
                } else if (!has_url || !has_sha || !has_target) {
                    bad_file = true; /* a file it can't verify: nothing of this update */
                }
            }
        } else {
            pj_skip(r);
        }
    }
    if (r->error || !has_version || !has_downgrade || !channel_ok || !region_ok || !target0 ||
        bad_file) {
        return false;
    }
    if (has_norm) {
        memcpy(v, norm, sizeof norm);
    }
    u->major = v[0];
    u->minor = v[1];
    u->patch = v[2];
    return !downgrade && compare(v, current) > 0; /* newer, by its word and by ours */
}

zwfc_verdict_t zwfc_best(const char *json, size_t len, const zwfc_device_t *dev, const char *region,
                         bool include_beta, zwfc_update_t *out)
{
    if (json == NULL || dev == NULL || out == NULL) {
        return ZWFC_BAD_ANSWER;
    }
    memset(out, 0, sizeof(*out));
    const uint16_t current[3] = {dev->fw_major, dev->fw_minor, 0u};
    pj_reader_t r;
    pj_read_init(&r, json, len);
    if (!pj_enter_arr(&r)) {
        return ZWFC_BAD_ANSWER;
    }
    bool known = false, found = false;
    uint16_t best[3] = {0u, 0u, 0u};
    while (pj_next_element(&r)) {
        if (!pj_enter_obj(&r)) {
            return ZWFC_BAD_ANSWER;
        }
        uint16_t m = 0u, t = 0u, p = 0u;
        bool has_m = false, has_t = false, has_p = false, mine = false;
        char key[24];
        while (pj_next_member(&r, key, sizeof key)) {
            char s[16];
            if (strcmp(key, "manufacturerId") == 0) {
                has_m = pj_get_str(&r, s, sizeof s) && hex16(s, &m);
            } else if (strcmp(key, "productType") == 0) {
                has_t = pj_get_str(&r, s, sizeof s) && hex16(s, &t);
            } else if (strcmp(key, "productId") == 0) {
                has_p = pj_get_str(&r, s, sizeof s) && hex16(s, &p);
            } else if (strcmp(key, "updates") == 0) {
                /* the IDs come first in the service's answer; checked again at the end */
                mine = has_m && has_t && has_p && m == dev->manufacturer &&
                       t == dev->product_type && p == dev->product_id;
                if (!pj_enter_arr(&r)) {
                    return ZWFC_BAD_ANSWER;
                }
                while (pj_next_element(&r)) {
                    zwfc_update_t u;
                    uint16_t v[3];
                    if (read_update(&r, region, include_beta, current, &u, v) && mine &&
                        (!found || compare(v, best) > 0)) {
                        *out = u;
                        memcpy(best, v, sizeof best);
                        found = true;
                    }
                    if (r.error) {
                        return ZWFC_BAD_ANSWER;
                    }
                }
            } else {
                pj_skip(&r);
            }
        }
        known = known || (has_m && has_t && has_p && m == dev->manufacturer &&
                          t == dev->product_type && p == dev->product_id);
    }
    if (r.error || !pj_finished(&r)) {
        return ZWFC_BAD_ANSWER;
    }
    if (!known) {
        memset(out, 0, sizeof(*out));
        return ZWFC_UNKNOWN_DEVICE;
    }
    return found ? ZWFC_UPDATE : ZWFC_UP_TO_DATE;
}
