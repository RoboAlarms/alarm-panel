/*
 * zw_fwjob.c - a device's software update up to the transfer. See zw_fwjob.h.
 */
#include "zwave_zones/zw_fwjob.h"

#include <stdio.h>
#include <string.h>

#include "alarm_proto/proto_link.h"
#include "zwave_proto/zw_fwfile.h"

void zwfj_init(zwfj_t *j)
{
    memset(j, 0, sizeof(*j));
}

bool zwfj_device_of(const zw_node_record_t *rec, zwfc_device_t *out)
{
    memset(out, 0, sizeof(*out));
    if (rec == NULL || !rec->has_identity || !rec->has_versions) {
        return false;
    }
    out->manufacturer = rec->manufacturer_id;
    out->product_type = rec->product_type;
    out->product_id = rec->product_id;
    out->fw_major = rec->fw_major;
    out->fw_minor = rec->fw_minor;
    return true;
}

size_t zwfj_check_radio(zwfj_t *j, const zw_module_facts_t *f, const char *region, char *body,
                        size_t cap)
{
    if (f == NULL || f->manufacturer == 0u || (f->firmware_major == 0u && f->firmware_minor == 0u)) {
        return 0u;
    }
    zwfc_device_t dev;
    memset(&dev, 0, sizeof dev);
    dev.manufacturer = f->manufacturer;
    dev.product_type = f->product_type;
    dev.product_id = f->product_id;
    dev.fw_major = f->firmware_major;
    dev.fw_minor = f->firmware_minor;
    dev.has_sdk = f->protocol_major != 0u;
    dev.sdk_major = f->protocol_major;
    dev.sdk_minor = f->protocol_minor;
    dev.sdk_patch = f->protocol_patch;
    const size_t n = zwfc_request(&dev, region, body, cap);
    if (n == 0u) {
        return 0u;
    }
    zwfj_init(j);
    j->state = UI_ZW_FW_CHECKING;
    j->dev = dev; /* node 0, generation 0: the radio */
    j->radio = true;
    return n;
}

const char *zwfj_region(bool chosen_set, uint8_t chosen, const zw_module_facts_t *f)
{
    if (chosen_set) {
        return zwfc_region(chosen);
    }
    return f != NULL && f->region_known ? zwfc_region(f->region) : NULL;
}

static void fail(zwfj_t *j, uint8_t state, const char *words)
{
    j->state = state;
    snprintf(j->words, sizeof j->words, "%s", words);
}

size_t zwfj_check_begin(zwfj_t *j, const zw_node_record_t *rec, const char *region, char *body,
                        size_t cap)
{
    zwfc_device_t dev;
    if (!zwfj_device_of(rec, &dev)) {
        return 0u;
    }
    const size_t n = zwfc_request(&dev, region, body, cap);
    if (n == 0u) {
        return 0u;
    }
    zwfj_init(j);
    j->state = UI_ZW_FW_CHECKING;
    j->node = rec->node_id;
    j->generation = rec->generation;
    j->dev = dev;
    return n;
}

/* The change notes for the page: one line, cut to fit ("..." when cut). */
static void notes(zwfj_t *j)
{
    char *w = j->words;
    const size_t room = sizeof j->words - 4u; /* "..." and its end */
    size_t k = 0u;
    bool space = false;
    bool cut = false;
    for (const char *p = j->update.changelog; *p != '\0'; p++) {
        if (*p == '\n' || *p == '\r' || *p == '\t' || *p == ' ') {
            space = k > 0u;
            continue;
        }
        if (k + (space ? 2u : 1u) > room) {
            cut = true;
            break;
        }
        if (space) {
            w[k++] = ' ';
        }
        space = false;
        w[k++] = *p;
    }
    if (cut || j->update.changelog_cut) {
        memcpy(&w[k], "...", 4u);
    } else {
        w[k] = '\0';
    }
}

void zwfj_check_answer(zwfj_t *j, int http_status, const char *body, size_t len,
                       const char *region)
{
    if (j->state != UI_ZW_FW_CHECKING) {
        return; /* not asked, or asked again for another device since */
    }
    if (http_status == 0) {
        fail(j, UI_ZW_FW_CHECK_FAILED,
             "The panel couldn't reach the update service. Check its internet connection.");
        return;
    }
    if (http_status != 200) {
        char w[96];
        snprintf(w, sizeof w, "The update service answered with an error (%d). Try again later.",
                 http_status);
        fail(j, UI_ZW_FW_CHECK_FAILED, w);
        return;
    }
    const zwfc_verdict_t verdict = zwfc_best(body, len, &j->dev, region, false, &j->update);
    switch (verdict) {
    case ZWFC_UPDATE:
        if (j->update.file_count != 1u || j->update.files[0].target != 0u) {
            char w[96];
            snprintf(w, sizeof w, "Version %.20s is out, but it updates parts the panel can't "
                                  "update yet.", j->update.version);
            fail(j, UI_ZW_FW_CHECK_FAILED, w);
            return;
        }
        j->state = UI_ZW_FW_AVAILABLE;
        notes(j);
        return;
    case ZWFC_UP_TO_DATE:
    case ZWFC_UNKNOWN_DEVICE:
        if (j->radio && zwfc_radio_builtin(&j->dev, region, &j->update)) {
            j->state = UI_ZW_FW_AVAILABLE; /* Zooz's file the service doesn't offer this radio */
            notes(j);
            return;
        }
        fail(j, verdict == ZWFC_UP_TO_DATE ? UI_ZW_FW_UP_TO_DATE : UI_ZW_FW_UNKNOWN, "");
        return;
    default:
        fail(j, UI_ZW_FW_CHECK_FAILED, "The update service's answer couldn't be read. Try again "
                                       "later.");
        return;
    }
}

const char *zwfj_download_begin(zwfj_t *j, uint16_t node, uint16_t generation)
{
    if (j->state != UI_ZW_FW_AVAILABLE || j->node != node || j->generation != generation) {
        return NULL;
    }
    j->state = UI_ZW_FW_DOWNLOADING;
    j->words[0] = '\0';
    return j->update.files[0].url;
}

bool zwfj_cancel(zwfj_t *j)
{
    if (j->state != UI_ZW_FW_DOWNLOADING) {
        return false;
    }
    j->state = UI_ZW_FW_AVAILABLE;
    notes(j);
    return true;
}

void zwfj_download_failed(zwfj_t *j, int http_status)
{
    if (j->state != UI_ZW_FW_DOWNLOADING) {
        return;
    }
    char w[96];
    if (http_status == 0) {
        snprintf(w, sizeof w, "Its file couldn't be downloaded. Check the internet connection.");
    } else {
        snprintf(w, sizeof w, "Its file couldn't be downloaded (%d). Try again later.",
                 http_status);
    }
    fail(j, UI_ZW_FW_FILE_FAILED, w);
}

/* The URL's last path part, for the file's kind. */
static const char *file_name(const char *url)
{
    const char *name = url;
    for (const char *p = url; *p != '\0' && *p != '?' && *p != '#'; p++) {
        if (*p == '/') {
            name = p + 1;
        }
    }
    return name;
}

size_t zwfj_file(zwfj_t *j, const uint8_t *file, size_t len, uint8_t *out, size_t cap)
{
    if (j->state != UI_ZW_FW_DOWNLOADING) {
        return 0u;
    }
    uint8_t digest[32];
    proto_sha256(file, len, digest);
    if (len == 0u || memcmp(digest, j->update.files[0].sha256, sizeof digest) != 0) {
        fail(j, UI_ZW_FW_FILE_FAILED,
             "Its file isn't the one the update service promised. Nothing was sent.");
        return 0u;
    }
    char name[64];
    const char *base = file_name(j->update.files[0].url);
    size_t k = 0u;
    for (; base[k] != '\0' && base[k] != '?' && base[k] != '#' && k + 1u < sizeof name; k++) {
        name[k] = base[k];
    }
    name[k] = '\0';
    size_t n = 0u;
    switch (zw_fwfile_guess(name, file, len)) {
    case ZW_FWFILE_GECKO:
    case ZW_FWFILE_BINARY:
        if (len > cap) {
            fail(j, UI_ZW_FW_FILE_FAILED, "Its file is too big for the panel. Nothing was sent.");
            return 0u;
        }
        if (out != file) {
            memmove(out, file, len);
        }
        n = len;
        break;
    case ZW_FWFILE_HEX:
        n = zw_fwfile_hex_to_bin(file, len, out, cap);
        if (n == 0u) {
            fail(j, UI_ZW_FW_FILE_FAILED,
                 "Its file is damaged or too big for the panel. Nothing was sent.");
            return 0u;
        }
        break;
    default:
        fail(j, UI_ZW_FW_FILE_FAILED,
             "Its file is in a form the panel can't read. Nothing was sent.");
        return 0u;
    }
    return n;
}

void zwfj_started(zwfj_t *j)
{
    zwfj_init(j);
}

void zwfj_not_started(zwfj_t *j, ui_zw_req_result_t why)
{
    switch (why) {
    case UI_ZW_REQ_LOW_BATTERY:
        fail(j, UI_ZW_FW_FILE_FAILED, "Not started: its battery is too low. Replace it first.");
        break;
    case UI_ZW_REQ_BUSY:
        fail(j, UI_ZW_FW_FILE_FAILED, "Not started: the radio was busy. Try again when it's done.");
        break;
    case UI_ZW_REQ_NO_RADIO:
        fail(j, UI_ZW_FW_FILE_FAILED, "Not started: the Z-Wave radio isn't working.");
        break;
    case UI_ZW_REQ_NOT_ALLOWED:
        fail(j, UI_ZW_FW_FILE_FAILED, "Not started: installer mode ended, or something is armed.");
        break;
    default:
        fail(j, UI_ZW_FW_FILE_FAILED, "Not started: it can't take an update now.");
        break;
    }
}

void zwfj_page(const zwfj_t *j, uint16_t node, uint16_t generation, ui_zw_device_page_t *page)
{
    if (j->state == UI_ZW_FW_NONE || j->node != node || j->generation != generation) {
        return;
    }
    page->fw.check = j->state;
    snprintf(page->fw.version, sizeof page->fw.version, "%s", j->update.version);
    page->fw.beta = j->update.beta;
    snprintf(page->fw.words, sizeof page->fw.words, "%s", j->words);
}
